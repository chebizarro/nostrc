/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/tree.h>

static size_t find_node(const vault_tree *t, const uint8_t id[32])
{
    for (size_t i = 0; i < t->count; ++i)
        if (!memcmp(t->nodes[i].node->node_id, id, 32))
            return i;
    return SIZE_MAX;
}
static int graph_visit(const vault_tree *t, size_t index, uint8_t *seen, unsigned depth)
{
    if (depth > 128 || (size_t)depth * 49 > PORTHOME_MAX_PATH_LEN)
        return PORTHOME_ERR_TOO_LARGE;
    if (seen[index])
        return VAULT_ERR_CONFLICT;
    seen[index] = 1;
    const porthome_dir_node *n = t->nodes[index].node;
    if (n->entries_len && ((size_t)depth + 1) * 49 - 1 > PORTHOME_MAX_PATH_LEN)
        return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < n->entries_len; ++i) {
        const porthome_node_entry *e = &n->entries[i];
        if (e->kind != PORTHOME_KIND_DIR)
            continue;
        size_t child = find_node(t, e->node_id);
        if (child == SIZE_MAX)
            return VAULT_ERR_INCOMPLETE;
        if (e->child_epoch != t->nodes[child].node->epoch)
            return VAULT_ERR_EPOCH;
        int rc = graph_visit(t, child, seen, depth + 1);
        if (rc)
            return rc;
    }
    return 0;
}
int vault_tree_validate(const vault_tree *t)
{
    if (!t || !t->count || !t->nodes || !t->nodes[0].node || !t->nodes[0].node->vault)
        return VAULT_ERR_ARG;
    if (t->count > VAULT_MAX_OBJECTS)
        return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < t->count; ++i) {
        const vault_owned_tree_node *v = &t->nodes[i];
        const porthome_ring_entry_t *key = vault_ring_latest(&v->ring);
        if (!v->node || !key || key->epoch != v->node->epoch)
            return VAULT_ERR_EPOCH;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(v->node->node_id, t->nodes[j].node->node_id, 32))
                return VAULT_ERR_CONFLICT;
        uint8_t *b = NULL;
        size_t len = 0;
        int rc = porthome_dir_node_encode(v->node, &b, &len);
        if (b)
            OPENSSL_cleanse(b, len);
        free(b);
        if (rc)
            return rc;
    }
    uint8_t *seen = calloc(t->count, 1);
    if (!seen)
        return VAULT_ERR_OOM;
    int rc = graph_visit(t, 0, seen, 0);
    if (!rc)
        for (size_t i = 0; i < t->count; ++i)
            if (!seen[i]) {
                rc = VAULT_ERR_ARG;
                break;
            }
    free(seen);
    return rc;
}
void vault_tree_free(vault_tree *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->count; ++i) {
        if (t->nodes[i].node) {
            porthome_dir_node_dispose(t->nodes[i].node);
            free(t->nodes[i].node);
        }
        vault_ring_dispose(&t->nodes[i].ring);
    }
    free(t->nodes);
    free(t);
}
int vault_tree_new(const vault_tree_node *nodes, size_t count, vault_tree **out)
{
    if (out)
        *out = NULL;
    if (!out || !nodes || !count)
        return VAULT_ERR_ARG;
    if (count > VAULT_MAX_OBJECTS)
        return PORTHOME_ERR_TOO_LARGE;
    vault_tree *t = calloc(1, sizeof(*t));
    if (!t)
        return VAULT_ERR_OOM;
    t->nodes = calloc(count, sizeof(*t->nodes));
    if (!t->nodes) {
        free(t);
        return VAULT_ERR_OOM;
    }
    t->count = count;
    int rc = 0;
    for (size_t i = 0; i < count; ++i) {
        rc = vault_node_clone(nodes[i].node, &t->nodes[i].node);
        if (!rc)
            rc = vault_ring_copy(&t->nodes[i].ring, &nodes[i].ring);
        if (rc)
            break;
    }
    if (!rc)
        rc = vault_tree_validate(t);
    if (rc) {
        vault_tree_free(t);
        return rc;
    }
    *out = t;
    return 0;
}
int vault_tree_from_record(const vault_record *r, vault_tree_ring_fn rings, void *ctx,
                           vault_tree **out)
{
    if (out)
        *out = NULL;
    if (!out || !r || !rings || !r->manifests_len)
        return VAULT_ERR_ARG;
    vault_tree_node *v = calloc(r->manifests_len, sizeof(*v));
    if (!v)
        return VAULT_ERR_OOM;
    int rc = 0;
    for (size_t i = 0; i < r->manifests_len; ++i) {
        v[i].node = r->manifests[i].node;
        rc = rings(ctx, v[i].node->node_id, v[i].node->epoch, &v[i].ring);
        if (rc)
            break;
    }
    if (!rc)
        rc = vault_tree_new(v, r->manifests_len, out);
    free(v);
    return rc;
}
size_t vault_tree_count(const vault_tree *t) { return t ? t->count : 0; }
const porthome_dir_node *vault_tree_node_at(const vault_tree *t, size_t i, porthome_ring_t *ring)
{
    if (ring)
        *ring = (porthome_ring_t){0};
    if (!t || i >= t->count)
        return NULL;
    if (ring)
        *ring = vault_ring_view(&t->nodes[i].ring);
    return t->nodes[i].node;
}
int vault_tree_add_member(vault_tree *t, const uint8_t actor[32], const uint8_t member[32],
                          vault_cap cap)
{
    int rc = vault_tree_validate(t);
    if (rc)
        return rc;
    if (!actor || !member || (cap != VAULT_CAP_READ && cap != VAULT_CAP_WRITE))
        return VAULT_ERR_ARG;
    porthome_vault_info *v = t->nodes[0].node->vault;
    if (memcmp(actor, v->owner, 32))
        return VAULT_ERR_AUTH;
    uint8_t (**keys)[32] = cap == VAULT_CAP_WRITE ? &v->writers : &v->readers;
    size_t *len = cap == VAULT_CAP_WRITE ? &v->writers_len : &v->readers_len;
    for (size_t i = 0; i < *len; ++i)
        if (!memcmp((*keys)[i], member, 32))
            return 0;
    if (*len == PORTHOME_VAULT_MAX_MEMBERS || v->grants_rev == UINT64_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    uint8_t (*p)[32] = malloc((*len + 1) * 32);
    if (!p)
        return VAULT_ERR_OOM;
    if (*len)
        memcpy(p, *keys, *len * 32);
    memcpy(p[*len], member, 32);
    free(*keys);
    *keys = p;
    ++*len;
    ++v->grants_rev;
    return 0;
}
static int clone_tree(const vault_tree *t, vault_tree **out)
{
    vault_tree_node *v = calloc(t->count, sizeof(*v));
    if (!v)
        return VAULT_ERR_OOM;
    for (size_t i = 0; i < t->count; ++i)
        v[i] = (vault_tree_node){t->nodes[i].node, vault_ring_view(&t->nodes[i].ring)};
    int rc = vault_tree_new(v, t->count, out);
    free(v);
    return rc;
}
int vault_tree_revoke(const vault_tree *old, const porthome_head *head, const uint8_t actor[32],
                      const uint8_t member[32], uint64_t when, vault_tree **out)
{
    if (out)
        *out = NULL;
    if (!out || !head || !actor || !member)
        return VAULT_ERR_ARG;
    int rc = vault_tree_validate(old);
    if (rc)
        return rc;
    if (head->has_ref_state)
        return VAULT_ERR_INCOMPLETE;
    for (size_t i = 1; i < old->count; ++i)
        if (old->nodes[i].node->vault)
            return VAULT_ERR_CONFLICT;
    vault_tree *t = NULL;
    rc = clone_tree(old, &t);
    if (rc)
        return rc;
    vault_revoke_result *r = NULL;
    rc = vault_revoke(actor, member, old->nodes[0].node, &old->nodes[0].ring, NULL, head, when, &r);
    if (rc)
        goto fail;
    porthome_dir_node_dispose(t->nodes[0].node);
    free(t->nodes[0].node);
    vault_ring_dispose(&t->nodes[0].ring);
    t->nodes[0].node = r->node;
    r->node = NULL;
    t->nodes[0].ring = r->ring;
    memset(&r->ring, 0, sizeof(r->ring));
    vault_revoke_result_free(r);
    r = NULL;
    for (size_t i = 1; i < t->count; ++i) {
        uint64_t epoch;
        rc = vault_ring_append_random(&t->nodes[i].ring, &epoch);
        if (rc)
            goto fail;
        t->nodes[i].node->epoch = epoch;
    }
    for (size_t i = 0; i < t->count; ++i) {
        porthome_dir_node *n = t->nodes[i].node;
        const uint8_t *key = vault_ring_latest(&t->nodes[i].ring)->node_key;
        for (size_t j = 0; j < n->entries_len; ++j) {
            porthome_node_entry *e = &n->entries[j];
            rc = porthome_encrypt_name_v2(key, e->name_plain, e->name_enc);
            if (rc)
                goto fail;
            if (e->kind == PORTHOME_KIND_DIR) {
                size_t child = find_node(t, e->node_id);
                if (child == SIZE_MAX) {
                    rc = VAULT_ERR_INCOMPLETE;
                    goto fail;
                }
                const porthome_ring_entry_t *fresh = vault_ring_latest(&t->nodes[child].ring);
                e->child_epoch = fresh->epoch;
                e->has_key_override = 1;
                memcpy(e->key_override, fresh->node_key, 32);
            }
        }
    }
    rc = vault_tree_validate(t);
    if (rc)
        goto fail;
    *out = t;
    return 0;
fail:
    vault_revoke_result_free(r);
    vault_tree_free(t);
    return rc;
}
static int seal_node(vault_tree *t, size_t index, const vault_blob_io *io, uint8_t hash[32])
{
    porthome_dir_node *n = t->nodes[index].node;
    for (size_t i = 0; i < n->entries_len; ++i)
        if (n->entries[i].kind == PORTHOME_KIND_DIR) {
            size_t child = find_node(t, n->entries[i].node_id);
            uint8_t child_hash[32];
            int rc = seal_node(t, child, io, child_hash);
            if (rc)
                return rc;
            memcpy(n->entries[i].manifest_sha256, child_hash, 32);
        }
    uint8_t *blob = NULL;
    size_t len = 0;
    int rc = porthome_dir_node_seal(n, vault_ring_latest(&t->nodes[index].ring)->node_key, &blob,
                                    &len, hash);
    if (!rc && io->put(io->ctx, blob, len, hash))
        rc = VAULT_ERR_IO;
    free(blob);
    return rc;
}
int vault_tree_seal(vault_tree *t, const vault_blob_io *io, uint8_t hash[32])
{
    if (hash)
        memset(hash, 0, 32);
    if (!io || !io->put || !hash)
        return VAULT_ERR_ARG;
    int rc = vault_tree_validate(t);
    if (rc)
        return rc;
    uint8_t result[32];
    rc = seal_node(t, 0, io, result);
    if (!rc)
        memcpy(hash, result, 32);
    return rc;
}
int vault_tree_head_new(const vault_tree *t, const porthome_head *previous, const uint8_t actor[32],
                        const uint8_t root[32], uint64_t when, porthome_head **out)
{
    if (out)
        *out = NULL;
    if (!out || !actor || !root || when > INT64_MAX)
        return VAULT_ERR_ARG;
    int rc = vault_tree_validate(t);
    if (rc)
        return rc;
    const porthome_dir_node *n = t->nodes[0].node;
    if (memcmp(actor, n->vault->owner, 32))
        return VAULT_ERR_AUTH;
    if (previous && previous->seq == UINT64_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    if (previous && previous->has_ref_state)
        return VAULT_ERR_INCOMPLETE;
    if (previous && (memcmp(previous->vault_id, n->vault->vault_id, 32) ||
                     previous->epoch > n->epoch || previous->grants_rev > n->vault->grants_rev))
        return VAULT_ERR_ARG;
    const uint8_t *key = vault_ring_latest(&t->nodes[0].ring)->node_key;
    uint8_t *blob = NULL, hash[32];
    size_t len = 0;
    rc = porthome_dir_node_seal(n, key, &blob, &len, hash);
    free(blob);
    if (rc)
        return rc;
    if (CRYPTO_memcmp(hash, root, 32))
        return PORTHOME_ERR_HASH_MISMATCH;
    porthome_head *h = calloc(1, sizeof(*h));
    if (!h)
        return VAULT_ERR_OOM;
    h->epoch = n->epoch;
    h->seq = previous ? previous->seq + 1 : 1;
    h->grants_rev = n->vault->grants_rev;
    h->written_at = when;
    memcpy(h->vault_id, n->vault->vault_id, 32);
    memcpy(h->root_manifest, root, 32);
    if (previous) {
        h->parents = calloc(1, sizeof(*h->parents));
        if (!h->parents) {
            free(h);
            return VAULT_ERR_OOM;
        }
        h->parents_len = 1;
        memcpy(h->parents[0].writer, actor, 32);
        h->parents[0].seq = previous->seq;
        memcpy(h->parents[0].root_manifest, previous->root_manifest, 32);
        h->has_key_override = previous->has_key_override || previous->epoch != n->epoch;
    }
    if (h->has_key_override)
        memcpy(h->key_override, key, 32);
    *out = h;
    return 0;
}
void vault_tree_reseal_result_free(vault_tree_reseal_result *r)
{
    if (!r)
        return;
    free(r->changes);
    free(r);
}
int vault_tree_reseal(vault_tree *t, int eager, uint64_t budget, const vault_blob_io *io,
                      vault_tree_reseal_result **out)
{
    if (out)
        *out = NULL;
    if (!out || !io)
        return VAULT_ERR_ARG;
    int rc = vault_tree_validate(t);
    if (rc)
        return rc;
    size_t count = 0;
    for (size_t i = 0; i < t->count; ++i)
        for (size_t j = 0; j < t->nodes[i].node->entries_len; ++j) {
            const porthome_node_entry *e = &t->nodes[i].node->entries[j];
            for (size_t k = 0; k < e->chunks_len; ++k)
                if (e->chunks[k].chunk_key_id != t->nodes[i].node->epoch)
                    ++count;
        }
    if (count > SIZE_MAX / sizeof(vault_reseal_item))
        return PORTHOME_ERR_TOO_LARGE;
    vault_tree_reseal_result *r = calloc(1, sizeof(*r));
    vault_reseal_item *items = count ? calloc(count, sizeof(*items)) : NULL;
    if (!r || (count && !items)) {
        free(r);
        free(items);
        return VAULT_ERR_OOM;
    }
    r->changes = count ? calloc(count, sizeof(*r->changes)) : NULL;
    if (count && !r->changes) {
        free(items);
        free(r);
        return VAULT_ERR_OOM;
    }
    size_t used = 0;
    for (size_t i = 0; i < t->count; ++i)
        for (size_t j = 0; j < t->nodes[i].node->entries_len; ++j) {
            porthome_node_entry *e = &t->nodes[i].node->entries[j];
            for (size_t k = 0; k < e->chunks_len; ++k)
                if (e->chunks[k].chunk_key_id != t->nodes[i].node->epoch) {
                    if (e->chunks[k].size_plain > SIZE_MAX) {
                        rc = PORTHOME_ERR_TOO_LARGE;
                        goto fail;
                    }
                    size_t length = (size_t)e->chunks[k].size_plain;
                    items[used] = (vault_reseal_item){t->nodes[i].node, &t->nodes[i].ring, j, k};
                    memcpy(r->changes[used].before, e->chunks[k].sha256, 32);
                    r->changes[used].size_plain = length;
                    r->changes[used].epoch = t->nodes[i].node->epoch;
                    ++used;
                }
        }
    vault_reseal_task *task = NULL;
    rc = vault_reseal_task_new(items, count, eager, &task);
    if (rc)
        goto fail;
    rc = vault_reseal_task_run(task, budget, io);
    r->completed = vault_reseal_task_completed(task);
    r->remaining = count - r->completed;
    r->reserved = vault_reseal_task_reserved(task);
    r->changes_len = r->completed;
    for (size_t i = 0; i < r->completed; ++i)
        memcpy(r->changes[i].after,
               items[i].node->entries[items[i].entry].chunks[items[i].chunk].sha256, 32);
    vault_reseal_task_free(task);
    free(items);
    *out = r;
    return rc;
fail:
    free(items);
    vault_tree_reseal_result_free(r);
    return rc;
}
