/* SPDX-License-Identifier: MIT — fixture inspection using the public SDK. */
#include "../vc.h"
#include <porthome/crypto.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <nostr-keys.h>
#include <time.h>
static int put(void *ctx, const uint8_t *b, size_t n, const uint8_t hash[32])
{
    return vc_cache_put(ctx, hash, b, n);
}
static int receive(void *ctx, NostrEvent **out)
{
    NostrEvent **e = ctx;
    *out = *e;
    *e = NULL;
    return *out ? 0 : VAULT_ERR_AGAIN;
}
int main(int argc, char **argv)
{
    assert(argc == 5);
    vc_app a = {0};
    a.options.state_path = argv[2];
    assert(vc_state_open(&a, 0) == 0);
    assert(vc_state_load(&a) == 0);
    if (!strcmp(argv[1], "grant")) {
        uint8_t sk[32];
        assert(porthome_from_hex64(argv[3], sk) == 0);
        FILE *f = fopen(argv[4], "rb");
        assert(f);
        char json[100000];
        size_t n = fread(json, 1, sizeof json - 1, f);
        assert(!ferror(f) && feof(f));
        fclose(f);
        json[n] = 0;
        NostrEvent *e = nostr_event_new();
        assert(nostr_event_deserialize(e, json) == 0);
        vault_nip59_transport transport = {&e, NULL, receive};
        vault_nip59_carrier *carrier = NULL;
        assert(vault_nip59_carrier_new(sk, &transport, &carrier) == 0);
        vault_grant *g = NULL;
        assert(vault_grant_receive(vault_nip59_carrier_interface(carrier), &g) == 0);
        porthome_ring_t ring;
        const porthome_dir_node *root = vault_tree_node_at(a.state.tree, 0, &ring);
        assert(g->ring.count == 1);
        assert(g->ring.entries[0].epoch == root->epoch);
        assert(!memcmp(g->ring.entries[0].node_key, ring.entries[ring.count - 1].node_key, 32));
        assert(!memcmp(g->vault_id, root->vault->vault_id, 32));
        assert(!memcmp(g->owner, root->vault->owner, 32));
        assert(!memcmp(g->node_id, root->node_id, 32));
        porthome_head *head = NULL;
        assert(porthome_head_event_open(a.state.heads[a.state.current].event, g->vault_id, &ring,
                                        root->epoch, &head) == 0);
        assert(!memcmp(g->root_manifest, head->root_manifest, 32));
        porthome_head_dispose(head);
        free(head);
        char *body = NULL;
        assert(vault_grant_encode(g, &body) == 0);
        puts(body);
        OPENSSL_clear_free(body, strlen(body));
        vault_grant_free(g);
        vault_nip59_carrier_free(carrier);
        nostr_event_free(e);
        OPENSSL_cleanse(sk, 32);
    } else if (!strcmp(argv[1], "fork") || !strcmp(argv[1], "prune") ||
               !strcmp(argv[1], "missing")) {
        porthome_ring_t ring;
        const porthome_dir_node *root = vault_tree_node_at(a.state.tree, 0, &ring);
        porthome_head *old = NULL;
        assert(porthome_head_event_open(a.state.heads[a.state.current].event, root->vault->vault_id,
                                        &ring, root->epoch, &old) == 0);
        uint8_t *bytes = NULL;
        size_t len = 0;
        porthome_dir_node *copy = NULL;
        assert(porthome_dir_node_encode(root, &bytes, &len) == 0);
        assert(porthome_dir_node_decode(bytes, len, &copy) == 0);
        OPENSSL_clear_free(bytes, len);
        assert(copy->entries_len);
        copy->entries[0].mode ^=
            !strcmp(argv[3], "0000000000000000000000000000000000000000000000000000000000000001")
                ? 0200
                : 0100;
        size_t count = vault_tree_count(a.state.tree);
        if (!strcmp(argv[1], "prune")) {
            size_t keep = 0;
            for (size_t i = 0; i < copy->entries_len; i++) {
                if (copy->entries[i].kind != PORTHOME_KIND_DIR)
                    copy->entries[keep++] = copy->entries[i];
            }
            copy->entries_len = keep;
            count = 1;
        }
        vault_tree_node *nodes = calloc(count, sizeof *nodes);
        assert(nodes);
        nodes[0] = (vault_tree_node){copy, ring};
        for (size_t i = 1; i < count; i++)
            nodes[i].node = vault_tree_node_at(a.state.tree, i, &nodes[i].ring);
        vault_tree *t = NULL;
        assert(vault_tree_new(nodes, count, &t) == 0);
        free(nodes);
        porthome_dir_node_dispose(copy);
        free(copy);
        uint8_t hash[32];
        vault_blob_io io = {&a, NULL, put};
        assert(vault_tree_seal(t, &io, hash) == 0);
        char *pubkey = nostr_key_get_public(argv[3]);
        assert(pubkey);
        uint8_t author[32];
        assert(porthome_from_hex64(pubkey, author) == 0);
        free(pubkey);
        porthome_head_parent parent = {.seq = old->seq};
        memcpy(parent.writer, root->vault->owner, 32);
        memcpy(parent.root_manifest, old->root_manifest, 32);
        if (!strcmp(argv[1], "missing"))
            parent.root_manifest[0] ^= 0xff;
        porthome_head h = {.epoch = root->epoch,
                           .seq = !memcmp(author, root->vault->owner, 32) ? old->seq + 1 : 1,
                           .grants_rev = root->vault->grants_rev,
                           .written_at = old->written_at + 1,
                           .parents = &parent,
                           .parents_len = 1};
        memcpy(h.vault_id, root->vault->vault_id, 32);
        memcpy(h.root_manifest, hash, 32);
        NostrEvent *event = NULL;
        assert(porthome_head_event_new(&h, ring.entries[ring.count - 1].node_key, author, &event) ==
               0);
        assert(nostr_event_sign(event, argv[3]) == 0);
        char *json = nostr_event_serialize(event);
        assert(json);
        FILE *f = fopen(argv[4], "wb");
        assert(f);
        assert(fwrite(json, 1, strlen(json), f) == strlen(json));
        assert(fclose(f) == 0);
        free(json);
        nostr_event_free(event);
        porthome_head_dispose(old);
        free(old);
        vault_tree_free(t);
    } else
        assert(0);
    vc_state_free(&a.state);
    return 0;
}
