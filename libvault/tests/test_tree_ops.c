/* SPDX-License-Identifier: MIT */
#include "helpers.h"
#include <vault/tree.h>
typedef struct {
    uint8_t hash[32];
    uint64_t epoch;
    size_t length;
} length_entry;
typedef struct {
    length_entry entries[16];
    size_t count;
    int missing, wrong;
} lengths;
static int length_of(void *ctx, const uint8_t hash[32], uint64_t epoch, size_t *out)
{
    lengths *l = ctx;
    if (l->missing)
        return VAULT_ERR_INCOMPLETE;
    for (size_t i = 0; i < l->count; ++i)
        if (l->entries[i].epoch == epoch && !memcmp(l->entries[i].hash, hash, 32)) {
            *out = l->entries[i].length - (l->wrong ? 1 : 0);
            return 0;
        }
    return VAULT_ERR_INCOMPLETE;
}
static void changes(lengths *l, const vault_tree_reseal_result *r)
{
    for (size_t i = 0; i < r->changes_len; ++i) {
        assert(l->count < 16);
        length_entry *e = &l->entries[l->count++];
        memcpy(e->hash, r->changes[i].after, 32);
        e->epoch = r->changes[i].epoch;
        e->length = r->changes[i].size_plain;
    }
}
static int name_of(void *ctx, const uint8_t id[32], const char tag[49], const char **out)
{
    (void)ctx;
    (void)id;
    if (tag[0] == 'a')
        *out = "root-file";
    else if (tag[0] == 'b')
        *out = "directory";
    else if (tag[0] == 'c')
        *out = "child-file";
    else
        return VAULT_ERR_INCOMPLETE;
    return 0;
}
static int ring_of(void *ctx, const uint8_t id[32], uint64_t epoch, porthome_ring_t *out)
{
    const vault_tree *t = ctx;
    for (size_t i = 0; i < vault_tree_count(t); ++i) {
        porthome_ring_t ring;
        const porthome_dir_node *n = vault_tree_node_at(t, i, &ring);
        if (!memcmp(n->node_id, id, 32) && n->epoch == epoch) {
            *out = ring;
            return 0;
        }
    }
    return VAULT_ERR_EPOCH;
}
static vault_record *record_of(world *w, const vault_tree *t, const porthome_head *head)
{
    NostrEvent *event = NULL;
    porthome_ring_t root;
    vault_tree_node_at(t, 0, &root);
    assert(porthome_head_event_new(head, root.entries[root.count - 1].node_key, w->pub[0],
                                   &event) == 0);
    assert(nostr_event_sign(event, w->secret[0]) == 0);
    vault_manifest_object objects[2] = {0};
    uint8_t *blobs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        porthome_ring_t ring;
        const porthome_dir_node *n = vault_tree_node_at(t, i, &ring);
        size_t len;
        uint8_t hash[32];
        assert(porthome_dir_node_seal(n, ring.entries[ring.count - 1].node_key, &blobs[i], &len,
                                      hash) == 0);
        objects[i] = (vault_manifest_object){blobs[i], len, ring, n->epoch};
    }
    vault_record *r = NULL;
    assert(vault_record_open(event, w->vault.vault_id, &root, head->epoch, objects, 2, &r) == 0);
    nostr_event_free(event);
    free(blobs[0]);
    free(blobs[1]);
    return r;
}
int main(void)
{
    world w;
    world_init(&w);
    uint8_t child_id[32] = {0x33}, child_key[32];
    assert(porthome_child_node_key(w.key, child_id, NULL, child_key) == 0);
    vault_ring child_ring = {0};
    assert(vault_ring_init(&child_ring, 1, child_key) == 0);
    const uint8_t data[] = {'c', 'h', 'i', 'l', 'd', 0, 0};
    porthome_node_chunk child_chunk = chunk(&w, child_key, 1, data, sizeof(data));
    porthome_node_entry child_file = w.entries[0];
    memset(child_file.name_enc, 'c', 48);
    child_file.chunks = &child_chunk;
    porthome_dir_node child = {.epoch = 1, .entries = &child_file, .entries_len = 1};
    memcpy(child.node_id, child_id, 32);
    porthome_node_entry entries[2] = {w.entries[0], {.kind = PORTHOME_KIND_DIR, .child_epoch = 1}};
    memset(entries[1].name_enc, 'b', 48);
    memcpy(entries[1].node_id, child_id, 32);
    porthome_dir_node root = w.node;
    root.entries = entries;
    vault_tree_node input[] = {{&root, vault_ring_view(&w.ring)},
                               {&child, vault_ring_view(&child_ring)}};
    vault_tree *t = NULL;
    assert(vault_tree_new(input, 2, &t) == 0);
    vault_blob_io io = {&w, read_blob, put_blob};
    uint8_t hash[32];
    assert(vault_tree_seal(t, &io, hash) == 0);
    porthome_head *head = NULL;
    assert(vault_tree_head_new(t, NULL, w.pub[0], hash, 1, &head) == 0);
    NostrEvent *fetch_event = NULL;
    assert(porthome_head_event_new(head, w.key, w.pub[0], &fetch_event) == 0);
    assert(nostr_event_sign(fetch_event, w.secret[0]) == 0);
    vault_record *fetched = NULL;
    vault_tree *fetched_tree = NULL;
    porthome_ring_t root_ring = vault_ring_view(&w.ring);
    assert(vault_record_fetch(fetch_event, w.vault.vault_id, root.node_id, &root_ring, 1, NULL,
                              NULL, &io, &fetched, &fetched_tree) == 0);
    assert(vault_tree_count(fetched_tree) == 2);
    vault_record_free(fetched);
    vault_tree_free(fetched_tree);
    uint8_t wrong_id[32] = {0xff};
    assert(vault_record_fetch(fetch_event, w.vault.vault_id, wrong_id, &root_ring, 1, NULL, NULL,
                              &io, &fetched, &fetched_tree) == VAULT_ERR_ARG);
    assert(!fetched && !fetched_tree);
    nostr_event_free(fetch_event);
    vault_record *record = record_of(&w, t, head);
    assert(vault_record_manifest_count(record) == 2);
    const uint8_t *address = NULL;
    assert(vault_record_manifest_at(record, 0, &address) && !memcmp(address, hash, 32));
    assert(!vault_record_manifest_at(record, 2, &address) && !address);
    vault_tree *held = NULL;
    assert(vault_tree_from_record(record, ring_of, t, &held) == 0);
    lengths l = {.count = 2};
    memcpy(l.entries[0].hash, w.chunks[0].sha256, 32);
    l.entries[0].epoch = 1;
    l.entries[0].length = 7;
    memcpy(l.entries[1].hash, child_chunk.sha256, 32);
    l.entries[1].epoch = 1;
    l.entries[1].length = 7;
    vault_tree_check_result checked;
    assert(vault_tree_check(held, length_of, &l, &io, &checked) == 0 && checked.nodes == 2 &&
           checked.chunks == 2 && checked.size_plain == 14);
    l.wrong = 1;
    assert(vault_tree_check(held, length_of, &l, &io, &checked) == VAULT_ERR_ARG);
    l.wrong = 0;
    l.missing = 1;
    assert(vault_tree_check(held, length_of, &l, &io, &checked) == VAULT_ERR_INCOMPLETE);
    l.missing = 0;
    vault_tree *fresh = NULL;
    assert(vault_tree_revoke(held, head, w.pub[0], w.pub[1], 2, name_of, NULL, &fresh) == 0);
    porthome_ring_t r0, r1;
    const porthome_dir_node *n0 = vault_tree_node_at(fresh, 0, &r0),
                            *n1 = vault_tree_node_at(fresh, 1, &r1);
    assert(n0->epoch == 2 && n1->epoch == 2 && r0.count == 2 && r1.count == 2);
    assert(memcmp(r0.entries[1].node_key, w.key, 32) &&
           memcmp(r1.entries[1].node_key, child_key, 32));
    assert(n0->entries[1].has_key_override && n0->entries[1].child_epoch == 2 &&
           !memcmp(n0->entries[1].key_override, r1.entries[1].node_key, 32));
    assert(memcmp(n0->entries[0].name_enc, entries[0].name_enc, 48) &&
           memcmp(n1->entries[0].name_enc, child_file.name_enc, 48));
    assert(vault_tree_node_at(held, 0, NULL)->epoch == 1 && head->epoch == 1);
    vault_tree_node limited[] = {{n0, {r0.entries + 1, 1}}, {n1, {r1.entries + 1, 1}}};
    vault_tree *no_history = NULL;
    assert(vault_tree_new(limited, 2, &no_history) == 0);
    unsigned reads = w.reads;
    assert(vault_tree_check(no_history, length_of, &l, &io, &checked) == VAULT_ERR_EPOCH &&
           w.reads == reads);
    vault_tree_free(no_history);
    vault_tree_reseal_result *progress = NULL;
    assert(vault_tree_reseal(fresh, length_of, &l, 0, 122, &io, &progress) == VAULT_ERR_BUDGET);
    assert(progress->completed == 1 && progress->remaining == 1 && progress->reserved == 122);
    changes(&l, progress);
    vault_tree_reseal_result_free(progress);
    assert(vault_tree_reseal(fresh, length_of, &l, 0, 122, &io, &progress) == 0 &&
           progress->remaining == 0);
    changes(&l, progress);
    vault_tree_reseal_result_free(progress);
    assert(vault_tree_seal(fresh, &io, hash) == 0);
    porthome_head *new_head = NULL;
    assert(vault_tree_head_new(fresh, head, w.pub[0], hash, 2, &new_head) == 0 &&
           new_head->seq == 2 && new_head->has_key_override);
    vault_record *new_record = record_of(&w, fresh, new_head);
    assert(vault_tree_check(fresh, length_of, &l, &io, &checked) == 0 && checked.chunks == 2);
    vault_tree *eager = NULL;
    assert(vault_tree_revoke(held, head, w.pub[0], w.pub[1], 2, name_of, NULL, &eager) == 0);
    reads = w.reads;
    unsigned puts_before = w.puts;
    assert(vault_tree_reseal(eager, length_of, &l, 1, 243, &io, &progress) == VAULT_ERR_BUDGET &&
           !progress->completed && progress->remaining == 2 && w.reads == reads &&
           w.puts == puts_before);
    vault_tree_reseal_result_free(progress);
    vault_tree_free(eager);
    uint64_t rev = vault_tree_node_at(fresh, 0, NULL)->vault->grants_rev;
    assert(vault_tree_add_member(fresh, w.pub[2], w.pub[3], VAULT_CAP_WRITE) == VAULT_ERR_AUTH);
    assert(vault_tree_add_member(fresh, w.pub[0], w.pub[3], VAULT_CAP_WRITE) == 0 &&
           vault_tree_node_at(fresh, 0, NULL)->vault->grants_rev == rev + 1);
    assert(vault_tree_add_member(fresh, w.pub[0], w.pub[3], VAULT_CAP_WRITE) == 0 &&
           vault_tree_node_at(fresh, 0, NULL)->vault->grants_rev == rev + 1);
    /* Existing manifests can reference 4 MiB v1 chunks, and migrations can
     * preserve that size in a v2 envelope despite the new 1 MiB CDC bound. */
    size_t big_len = 4u * 1024u * 1024u;
    uint8_t *big = calloc(big_len, 1);
    assert(big);
    big[0] = 1;
    for (int version = 1; version <= 2; version++) {
        uint8_t *sealed = NULL;
        size_t sealed_len = 0;
        porthome_node_chunk legacy = {.chunk_key_id = 1};
        assert((version == 1 ? porthome_encrypt_chunk(w.key, big, big_len, &sealed, &sealed_len,
                                                      legacy.sha256)
                             : porthome_encrypt_chunk_v2(w.key, big, big_len, &sealed, &sealed_len,
                                                         legacy.sha256)) == 0);
        legacy.size_sealed = sealed_len;
        store_blob(&w, sealed, sealed_len, legacy.sha256);
        free(sealed);
        porthome_node_entry file = w.entries[0];
        file.chunks = &legacy;
        file.chunks_len = 1;
        file.size_plain = big_len;
        porthome_dir_node n = w.node;
        n.entries = &file;
        n.entries_len = 1;
        vault_tree_node row = {&n, vault_ring_view(&w.ring)};
        vault_tree *large = NULL;
        assert(vault_tree_new(&row, 1, &large) == 0);
        lengths sizes = {.count = 1};
        memcpy(sizes.entries[0].hash, legacy.sha256, 32);
        sizes.entries[0].epoch = 1;
        sizes.entries[0].length = big_len;
        assert(vault_tree_check(large, length_of, &sizes, &io, &checked) == 0 &&
               checked.size_plain == big_len);
        vault_tree_free(large);
    }
    free(big);
    vault_record_free(new_record);
    vault_record_free(record);
    porthome_head_dispose(head);
    free(head);
    porthome_head_dispose(new_head);
    free(new_head);
    vault_tree_free(fresh);
    vault_tree_free(held);
    vault_tree_free(t);
    vault_ring_dispose(&child_ring);
    world_dispose(&w);
    puts("OK verified tree, recursive rekey, lazy/eager, exact-epoch fsck");
    return 0;
}
