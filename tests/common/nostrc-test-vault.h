/* SPDX-License-Identifier: MIT */
#ifndef VAULT_TEST_HELPERS_H
#define VAULT_TEST_HELPERS_H
#include <vault/vault.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <json.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    uint8_t *bytes;
    size_t len;
    uint8_t hash[32];
} blob;
typedef struct {
    uint8_t sk[4][32], pub[4][32], writers[2][32], readers[2][32], key[32];
    char secret[4][65];
    porthome_vault_info vault;
    porthome_node_entry entries[2];
    porthome_node_chunk chunks[2];
    porthome_dir_node node;
    vault_ring ring;
    blob store[128];
    size_t stored;
    unsigned reads, puts;
    int corrupt, fail_put;
} world;
static inline size_t store_blob(world *w, const uint8_t *bytes, size_t len, const uint8_t hash[32])
{
    assert(w->stored < 128);
    size_t i = w->stored++;
    w->store[i].bytes = malloc(len);
    assert(w->store[i].bytes);
    memcpy(w->store[i].bytes, bytes, len);
    w->store[i].len = len;
    memcpy(w->store[i].hash, hash, 32);
    return i;
}
static inline int read_blob(void *ctx, const uint8_t hash[32], uint8_t **out, size_t *len)
{
    world *w = ctx;
    ++w->reads;
    *out = NULL;
    *len = 0;
    for (size_t i = 0; i < w->stored; ++i)
        if (!memcmp(hash, w->store[i].hash, 32)) {
            *out = malloc(w->store[i].len);
            assert(*out);
            memcpy(*out, w->store[i].bytes, w->store[i].len);
            *len = w->store[i].len;
            if (w->corrupt)
                (*out)[0] ^= 1;
            return 0;
        }
    return VAULT_ERR_IO;
}
static inline int put_blob(void *ctx, const uint8_t *bytes, size_t len, const uint8_t hash[32])
{
    world *w = ctx;
    ++w->puts;
    uint8_t actual[32];
    assert(porthome_sha256(bytes, len, actual) == 0 && !memcmp(actual, hash, 32));
    store_blob(w, bytes, len, hash);
    return w->fail_put ? VAULT_ERR_IO : 0;
}
static inline porthome_node_chunk chunk(world *w, const uint8_t key[32], uint64_t epoch,
                                        const uint8_t *pt, size_t len)
{
    porthome_node_chunk c = {.chunk_key_id = epoch, .size_plain=len};
    uint8_t *b = NULL;
    size_t n = 0;
    assert(porthome_encrypt_chunk_v2(key, pt, len, &b, &n, c.sha256) == 0);
    c.size_sealed = n;
    store_blob(w, b, n, c.sha256);
    free(b);
    return c;
}
static inline void world_init(world *w)
{
    memset(w, 0, sizeof(*w));
    nostr_json_init();
    for (size_t i = 0; i < 4; ++i) {
        w->sk[i][31] = (uint8_t)(i + 1);
        porthome_hex64(w->sk[i], w->secret[i]);
        char *p = nostr_key_get_public(w->secret[i]);
        assert(p && porthome_from_hex64(p, w->pub[i]) == 0);
        free(p);
    }
    memcpy(w->writers[0], w->pub[0], 32);
    memcpy(w->writers[1], w->pub[1], 32);
    memcpy(w->readers[0], w->pub[1], 32);
    memcpy(w->readers[1], w->pub[2], 32);
    uint8_t seed[32] = {7};
    assert(porthome_root_node_key(seed, w->key) == 0);
    assert(vault_ring_init(&w->ring, 1, w->key) == 0);
    memcpy(w->vault.owner, w->pub[0], 32);
    w->vault.vault_id[0] = 0x21;
    w->vault.writers = w->writers;
    w->vault.writers_len = 2;
    w->vault.readers = w->readers;
    w->vault.readers_len = 2;
    w->vault.grants_rev = 8;
    const uint8_t data0[] = {'h', 'e', 'l', 'l', 'o', 0, 0}, data1[] = {'b', 'y', 't', 'e', 's'};
    w->chunks[0] = chunk(w, w->key, 1, data0, sizeof(data0));
    w->chunks[1] = chunk(w, w->key, 1, data1, sizeof(data1));
    for (size_t i = 0; i < 2; ++i) {
        memset(w->entries[i].name_enc, 'a' + (int)i, 48);
        strcpy(w->entries[i].name_plain,i ? "other-file" : "root-file");
        w->entries[i].kind = PORTHOME_KIND_FILE;
        w->entries[i].mode = 0600;
        w->entries[i].mtime_ns = 1;
        w->entries[i].size_plain = i ? sizeof(data1) : sizeof(data0);
        w->entries[i].chunks = &w->chunks[i];
        w->entries[i].chunks_len = 1;
    }
    w->node.node_id[0] = 0x31;
    w->node.epoch = 1;
    w->node.vault = &w->vault;
    w->node.entries = w->entries;
    w->node.entries_len = 2;
}
static inline void world_dispose(world *w)
{
    for (size_t i = 0; i < w->stored; ++i)
        free(w->store[i].bytes);
    vault_ring_dispose(&w->ring);
    memset(w, 0, sizeof(*w));
    nostr_json_cleanup();
}
static inline void seal_node(world *w, const porthome_dir_node *node, uint8_t hash[32])
{
    uint8_t *b = NULL;
    size_t n = 0;
    assert(porthome_dir_node_seal(node, w->key, &b, &n, hash) == 0);
    store_blob(w, b, n, hash);
    free(b);
}
static inline vault_record *record_make(world *w, size_t author, uint64_t seq, uint64_t rev,
                                        const char *value, porthome_head_parent *parents,
                                        size_t parents_len, int policy_change)
{
    porthome_node_entry entries[2] = {w->entries[0], w->entries[1]};
    porthome_node_chunk changed;
    if (value) {
        changed = chunk(w, w->key, 1, (const uint8_t *)value, strlen(value));
        entries[0].chunks = &changed;
        entries[0].size_plain = strlen(value);
    } else
        entries[0] = entries[1];
    porthome_vault_info v = w->vault;
    v.grants_rev = rev;
    if (policy_change == 1)
        v.writers_len = 1;
    if (policy_change == 2) {
        v.writers = &w->writers[1];
        v.writers_len = 1;
    }
    porthome_dir_node node = w->node;
    node.vault = &v;
    node.entries = entries;
    node.entries_len = value ? 2 : 1;
    uint8_t *sealed = NULL, hash[32];
    size_t len = 0;
    assert(porthome_dir_node_seal(&node, w->key, &sealed, &len, hash) == 0);
    store_blob(w, sealed, len, hash);
    porthome_head h = {.epoch = 1,
                       .seq = seq,
                       .grants_rev = rev,
                       .written_at = seq,
                       .parents = parents,
                       .parents_len = parents_len};
    memcpy(h.vault_id, v.vault_id, 32);
    memcpy(h.root_manifest, hash, 32);
    NostrEvent *event = NULL;
    assert(porthome_head_event_new(&h, w->key, w->pub[author], &event) == 0);
    assert(nostr_event_sign(event, w->secret[author]) == 0);
    porthome_ring_t ring = vault_ring_view(&w->ring);
    vault_manifest_object object = {sealed, len, ring, 1};
    vault_record *r = NULL;
    assert(vault_record_open(event, v.vault_id, &ring, 1, &object, 1, &r) == 0);
    free(sealed);
    nostr_event_free(event);
    return r;
}
static inline porthome_head_parent parent_of(const vault_record *r, const uint8_t writer[32])
{
    porthome_head_parent p = {.seq = vault_record_head(r)->seq};
    memcpy(p.writer, writer, 32);
    memcpy(p.root_manifest, vault_record_head(r)->root_manifest, 32);
    return p;
}
#endif
