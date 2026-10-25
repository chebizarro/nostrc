/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/tree.h>
#include <nostr-event.h>
typedef struct {
    const vault_manifest_object *objects;
    size_t count;
    uint8_t (*hashes)[32], *seen;
    vault_record *record;
} inventory;
static int path_compare(const void *a, const void *b)
{
    return strcmp(((const vault_path *)a)->path, ((const vault_path *)b)->path);
}
static int path_add(vault_record *r, const char *prefix, const porthome_node_entry *entry)
{
    size_t n = strlen(prefix), len = n + (n ? 1 : 0) + 48;
    if (len > PORTHOME_MAX_PATH_LEN || r->paths_len == PORTHOME_MAX_ENTRIES)
        return PORTHOME_ERR_TOO_LARGE;
    if (r->paths_len == r->paths_cap) {
        size_t cap = r->paths_cap ? r->paths_cap * 2 : 32;
        if (cap > PORTHOME_MAX_ENTRIES)
            cap = PORTHOME_MAX_ENTRIES;
        vault_path *p = realloc(r->paths, cap * sizeof(*p));
        if (!p)
            return VAULT_ERR_OOM;
        r->paths = p;
        r->paths_cap = cap;
    }
    vault_path *p = &r->paths[r->paths_len];
    memset(p, 0, sizeof(*p));
    p->path = malloc(len + 1);
    if (!p->path)
        return VAULT_ERR_OOM;
    memcpy(p->path, prefix, n);
    if (n)
        p->path[n++] = '/';
    memcpy(p->path + n, entry->name_enc, 49);
    porthome_dir_node one = {.epoch = 1, .entries = (porthome_node_entry *)entry, .entries_len = 1};
    uint8_t *bytes = NULL;
    size_t z = 0;
    int rc = porthome_dir_node_encode(&one, &bytes, &z);
    if (!rc)
        rc = porthome_sha256(bytes, z, p->digest);
    if (bytes)
        OPENSSL_cleanse(bytes, z);
    free(bytes);
    if (rc) {
        free(p->path);
        p->path = NULL;
        return rc;
    }
    ++r->paths_len;
    return 0;
}
static int walk(inventory *s, const uint8_t hash[32], const uint8_t *node_id, uint64_t epoch,
                const porthome_ring_t *root_ring, const char *prefix, unsigned depth)
{
    if (depth > 128)
        return PORTHOME_ERR_TOO_LARGE;
    size_t index = 0;
    while (index < s->count && memcmp(s->hashes[index], hash, 32))
        ++index;
    if (index == s->count)
        return VAULT_ERR_INCOMPLETE;
    if (s->seen[index])
        return VAULT_ERR_CONFLICT;
    s->seen[index] = 1;
    const vault_manifest_object *o = &s->objects[index];
    if (o->epoch != epoch)
        return VAULT_ERR_EPOCH;
    porthome_dir_node *n = NULL;
    int rc = porthome_dir_node_open(root_ring ? root_ring : &o->ring, epoch, o->sealed,
                                    o->sealed_len, &n);
    if (rc)
        return rc;
    if (node_id && memcmp(n->node_id, node_id, 32)) {
        porthome_dir_node_dispose(n);
        free(n);
        return VAULT_ERR_ARG;
    }
    vault_record_manifest *kept = &s->record->manifests[s->record->manifests_len++];
    kept->node = n;
    memcpy(kept->address, hash, 32);
    if (!depth)
        s->record->root = n;
    for (size_t i = 0; i < n->entries_len; ++i) {
        porthome_node_entry *e = &n->entries[i];
        rc = path_add(s->record, prefix, e);
        if (rc)
            goto done;
        if (e->kind == PORTHOME_KIND_DIR) {
            /* paths may realloc during recursion; retain an independent prefix. */
            char *path = strdup(s->record->paths[s->record->paths_len - 1].path);
            if (!path) {
                rc = VAULT_ERR_OOM;
                goto done;
            }
            rc = walk(s, e->manifest_sha256, e->node_id, e->child_epoch, NULL, path, depth + 1);
            free(path);
            if (rc)
                goto done;
        }
    }
done:
    s->seen[index] = 2;
    return rc;
}
void vault_record_free(vault_record *r)
{
    if (!r)
        return;
    if (r->head) {
        porthome_head_dispose(r->head);
        free(r->head);
    }
    for (size_t i = 0; i < r->manifests_len; ++i) {
        porthome_dir_node_dispose(r->manifests[i].node);
        free(r->manifests[i].node);
    }
    free(r->manifests);
    for (size_t i = 0; i < r->paths_len; ++i)
        free(r->paths[i].path);
    free(r->paths);
    OPENSSL_cleanse(r, sizeof(*r));
    free(r);
}
size_t vault_record_manifest_count(const vault_record *r) { return r ? r->manifests_len : 0; }
const porthome_dir_node *vault_record_manifest_at(const vault_record *r, size_t i, const uint8_t **address)
{
    if (address) *address = NULL;
    if (!r || i >= r->manifests_len) return NULL;
    if (address) *address = r->manifests[i].address;
    return r->manifests[i].node;
}
const porthome_head *vault_record_head(const vault_record *r) { return r ? r->head : NULL; }
const porthome_dir_node *vault_record_root(const vault_record *r) { return r ? r->root : NULL; }
int vault_record_open(const NostrEvent *event, const uint8_t vault_id[32],
                      const porthome_ring_t *ring, uint64_t epoch,
                      const vault_manifest_object *objects, size_t count, vault_record **out)
{
    if (out)
        *out = NULL;
    if (!out || !event || !vault_id || (count && !objects))
        return VAULT_ERR_ARG;
    if (count > VAULT_MAX_OBJECTS)
        return PORTHOME_ERR_TOO_LARGE;
    vault_record *r = calloc(1, sizeof(*r));
    if (!r)
        return VAULT_ERR_OOM;
    int rc = porthome_head_event_open(event, vault_id, ring, epoch, &r->head);
    if (rc)
        goto done;
    if (porthome_from_hex64(event->pubkey, r->writer)) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    rc = porthome_head_encode(r->head, &body, &body_len);
    if (!rc)
        rc = porthome_sha256(body, body_len, r->head_hash);
    if (body)
        OPENSSL_cleanse(body, body_len);
    free(body);
    if (rc)
        goto done;
    if (!count) {
        rc = VAULT_ERR_INCOMPLETE;
        goto done;
    }
    inventory s = {.objects = objects, .count = count, .record = r};
    s.hashes = calloc(count, 32);
    s.seen = calloc(count, 1);
    if (!s.hashes || !s.seen) {
        rc = VAULT_ERR_OOM;
        free(s.hashes);
        free(s.seen);
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!objects[i].sealed ||
            objects[i].sealed_len > PORTHOME_MAX_CBOR_BYTES + PORTHOME_SEAL_OVERHEAD) {
            rc = VAULT_ERR_ARG;
            break;
        }
        rc = porthome_sha256(objects[i].sealed, objects[i].sealed_len, s.hashes[i]);
        if (rc)
            break;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(s.hashes[j], s.hashes[i], 32)) {
                rc = VAULT_ERR_ARG;
                break;
            }
        if (rc)
            break;
    }
    if (!rc) {
        r->manifests = calloc(count, sizeof(*r->manifests));
        if (!r->manifests) rc = VAULT_ERR_OOM;
    }
    if (!rc)
        rc = walk(&s, r->head->root_manifest, NULL, epoch, ring, "", 0);
    free(s.hashes);
    free(s.seen);
    if (rc)
        goto done;
    if (!r->root->vault || memcmp(r->root->vault->vault_id, vault_id, 32) ||
        r->root->vault->grants_rev != r->head->grants_rev) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    qsort(r->paths, r->paths_len, sizeof(*r->paths), path_compare);
    *out = r;
    return 0;
done:
    vault_record_free(r);
    return rc;
}
