/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/tree.h>
#include <nostr-event.h>
typedef struct {
    vault_manifest_object *objects;
    vault_ring *rings;
    uint8_t (*ids)[32], (*hashes)[32];
    porthome_dir_node **decoded;
    size_t count, total;
    vault_tree_ring_fn held;
    void *held_ctx;
    const vault_blob_io *io;
} fetch_state;
static int selected_ring(const porthome_ring_t *held, uint64_t epoch, const uint8_t key[32],
                         vault_ring *out)
{
    size_t n = 0;
    if (held && held->count) {
        int rc = vault_ring_valid(held);
        if (rc)
            return rc;
        while (n < held->count && held->entries[n].epoch <= epoch)
            ++n;
    }
    int replace = n && held->entries[n - 1].epoch == epoch;
    size_t count = n + (replace ? 0 : 1);
    if (count > VAULT_MAX_EPOCHS)
        return PORTHOME_ERR_TOO_LARGE;
    porthome_ring_entry_t *p = calloc(count, sizeof(*p));
    if (!p)
        return VAULT_ERR_OOM;
    if (n)
        memcpy(p, held->entries, n * sizeof(*p));
    p[count - 1].epoch = epoch;
    memcpy(p[count - 1].node_key, key, 32);
    porthome_ring_t view = {p, count};
    int rc = vault_ring_copy(out, &view);
    OPENSSL_cleanse(p, count * sizeof(*p));
    free(p);
    return rc;
}
static int fetch_node(fetch_state *s, const uint8_t hash[32], const uint8_t id[32], uint64_t epoch,
                      const porthome_ring_t *ring, unsigned depth)
{
    if (depth > 128 || s->count == VAULT_MAX_OBJECTS)
        return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < s->count; ++i)
        if (!memcmp(s->hashes[i], hash, 32) || !memcmp(s->ids[i], id, 32))
            return VAULT_ERR_CONFLICT;
    size_t at = s->count++;
    memcpy(s->ids[at], id, 32);
    memcpy(s->hashes[at], hash, 32);
    int rc = vault_ring_copy(&s->rings[at], ring);
    if (rc)
        return rc;
    uint8_t *blob = NULL, actual[32];
    size_t len = 0;
    if (s->io->read(s->io->ctx, hash, &blob, &len)) {
        free(blob);
        return VAULT_ERR_IO;
    }
    if (!blob || len > PORTHOME_MAX_CBOR_BYTES + PORTHOME_SEAL_OVERHEAD ||
        len > 64u * 1024u * 1024u - s->total) {
        free(blob);
        return PORTHOME_ERR_TOO_LARGE;
    }
    s->total += len;
    s->objects[at] = (vault_manifest_object){blob, len, vault_ring_view(&s->rings[at]), epoch};
    rc = porthome_sha256(blob, len, actual);
    if (rc)
        return rc;
    if (CRYPTO_memcmp(hash, actual, 32))
        return PORTHOME_ERR_HASH_MISMATCH;
    rc = porthome_dir_node_open(ring, epoch, blob, len, &s->decoded[at]);
    if (rc)
        return rc;
    porthome_dir_node *node = s->decoded[at];
    if (memcmp(node->node_id, id, 32))
        return VAULT_ERR_ARG;
    const uint8_t *parent_key = NULL;
    for (size_t i = 0; i < ring->count; ++i)
        if (ring->entries[i].epoch == epoch)
            parent_key = ring->entries[i].node_key;
    if (!parent_key)
        return VAULT_ERR_EPOCH;
    for (size_t i = 0; i < node->entries_len; ++i) {
        const porthome_node_entry *e = &node->entries[i];
        if (e->kind != PORTHOME_KIND_DIR)
            continue;
        porthome_ring_t history = {0};
        if (s->held) {
            rc = s->held(s->held_ctx, e->node_id, e->child_epoch, &history);
            if (rc && rc != VAULT_ERR_EPOCH && rc != VAULT_ERR_INCOMPLETE)
                return rc;
            if (rc)
                history = (porthome_ring_t){0};
        }
        uint8_t child_key[32];
        int found = 0;
        if (e->has_key_override) {
            memcpy(child_key, e->key_override, 32);
            found = 1;
        }
        if (!found)
            for (size_t j = 0; j < history.count; ++j)
                if (history.entries[j].epoch == e->child_epoch) {
                    memcpy(child_key, history.entries[j].node_key, 32);
                    found = 1;
                    break;
                }
        if (!found) {
            rc = porthome_child_node_key(parent_key, e->node_id, NULL, child_key);
            if (rc) {
                OPENSSL_cleanse(child_key, 32);
                return rc;
            }
        }
        vault_ring child_ring = {0};
        rc = selected_ring(&history, e->child_epoch, child_key, &child_ring);
        OPENSSL_cleanse(child_key, 32);
        if (rc)
            return rc;
        porthome_ring_t view = vault_ring_view(&child_ring);
        rc = fetch_node(s, e->manifest_sha256, e->node_id, e->child_epoch, &view, depth + 1);
        vault_ring_dispose(&child_ring);
        if (rc)
            return rc;
    }
    return 0;
}
int vault_record_fetch(const NostrEvent *event, const uint8_t vault[32], const uint8_t root_id[32],
                       const porthome_ring_t *ring, uint64_t epoch, vault_tree_ring_fn held,
                       void *ctx, const vault_blob_io *io, vault_record **out, vault_tree **tree)
{
    if (out)
        *out = NULL;
    if (tree)
        *tree = NULL;
    if (!out || !tree || !event || !vault || !root_id || !ring || !io || !io->read)
        return VAULT_ERR_ARG;
    porthome_head *head = NULL;
    int rc = porthome_head_event_open(event, vault, ring, epoch, &head);
    if (rc)
        return rc;
    const uint8_t *key = NULL;
    for (size_t i = 0; i < ring->count; ++i)
        if (ring->entries[i].epoch == epoch)
            key = ring->entries[i].node_key;
    fetch_state s = {.held = held, .held_ctx = ctx, .io = io};
    s.objects = calloc(VAULT_MAX_OBJECTS, sizeof(*s.objects));
    s.rings = calloc(VAULT_MAX_OBJECTS, sizeof(*s.rings));
    s.ids = calloc(VAULT_MAX_OBJECTS, 32);
    s.hashes = calloc(VAULT_MAX_OBJECTS, 32);
    s.decoded = calloc(VAULT_MAX_OBJECTS, sizeof(*s.decoded));
    vault_ring selected = {0};
    if (!s.objects || !s.rings || !s.ids || !s.hashes || !s.decoded) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    rc = selected_ring(ring, epoch, key, &selected);
    if (rc)
        goto done;
    porthome_ring_t root = vault_ring_view(&selected);
    rc = fetch_node(&s, head->root_manifest, root_id, epoch, &root, 0);
    if (rc)
        goto done;
    rc = vault_record_open(event, vault, &root, epoch, s.objects, s.count, out);
    if (rc)
        goto done;
    vault_tree_node *nodes = calloc(s.count, sizeof(*nodes));
    if (!nodes) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    for (size_t i = 0; i < s.count; ++i)
        nodes[i] = (vault_tree_node){s.decoded[i], vault_ring_view(&s.rings[i])};
    rc = vault_tree_new(nodes, s.count, tree);
    free(nodes);
done:
    if (rc) {
        vault_record_free(*out);
        *out = NULL;
        vault_tree_free(*tree);
        *tree = NULL;
    }
    vault_ring_dispose(&selected);
    porthome_head_dispose(head);
    free(head);
    for (size_t i = 0; i < s.count; ++i) {
        if (s.objects)
            free((void *)s.objects[i].sealed);
        if (s.decoded && s.decoded[i]) {
            porthome_dir_node_dispose(s.decoded[i]);
            free(s.decoded[i]);
        }
        if (s.rings)
            vault_ring_dispose(&s.rings[i]);
    }
    free(s.objects);
    free(s.rings);
    free(s.ids);
    free(s.hashes);
    free(s.decoded);
    return rc;
}
