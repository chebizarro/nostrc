/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_reader.h"
#include <json.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
struct nh_vault_reader {
    nh_vault_session *disk;
    vault_tree *tree;
    vault_record *record;
    char *path;
};
static int below(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(path, prefix, n) && (!path[n] || path[n] == '/');
}
static json_t *find_snapshot(json_t *doc, const char *id) {
    json_t *row;
    size_t i;
    json_array_foreach(json_object_get(doc, "retained"), i, row) {
        const char *wire = json_string_value(json_object_get(row, "event"));
        NostrEvent *event = wire ? nostr_event_new() : NULL;
        int match =
            event && !nostr_event_deserialize(event, wire) && event->id && !strcmp(event->id, id);
        nostr_event_free(event);
        if (match)
            return row;
    }
    return NULL;
}
static int open_one(const nh_syncd_vault_config_entry *cfg, const char *state,
                    const uint8_t key[32], nh_syncd_cache *cache, const vault_blob_io *public_io,
                    const char *path, uint64_t generation, uint64_t size, const char *const *chunks,
                    size_t count, nh_vault_reader **out) {
    nh_vault_reader *r = calloc(1, sizeof *r);
    if (!r)
        return VAULT_ERR_OOM;
    int rc = nh_vault_session_load(state, cfg->vault_id, key, &r->disk);
    if (rc) {
        nh_vault_reader_close(r);
        return below(path, cfg->path) ? rc : 1;
    }
    r->disk->cache = cache;
    /* Staging is never a FUSE source, and private mode never uses remote I/O. */
    if (r->disk->objects >= 0) {
        close(r->disk->objects);
        r->disk->objects = -1;
    }
    if (json_is_false(json_object_get(r->disk->doc, "cache_only")) && public_io)
        r->disk->remote = *public_io;
    json_t *snapshot = NULL, *value;
    const char *gkey;
    uint64_t best = 0;
    if (below(path, cfg->path)) {
        json_object_foreach(json_object_get(r->disk->doc, "views"), gkey, value) {
            char *end = NULL;
            errno = 0;
            unsigned long long g = strtoull(gkey, &end, 10);
            if (errno || !*gkey || *end) {
                rc = VAULT_ERR_ARG;
                break;
            }
            if (g <= generation && g >= best) {
                best = g;
                snapshot = value;
            }
        }
        r->path = strdup(path + strlen(cfg->path) + (path[strlen(cfg->path)] == '/'));
    }
    size_t i;
    json_t *row;
    uint64_t conflict_gen = 0;
    size_t longest = 0;
    json_array_foreach(json_object_get(r->disk->doc, "conflicts"), i, row) {
        const char *dest = json_string_value(json_object_get(row, "destination"));
        json_int_t g = json_integer_value(json_object_get(row, "generation"));
        if (!dest || !below(path, dest) || g <= 0 || (uint64_t)g > generation ||
            strlen(dest) < longest || (strlen(dest) == longest && (uint64_t)g < conflict_gen))
            continue;
        const char *id = json_string_value(json_object_get(row, "head"));
        const char *original = json_string_value(json_object_get(row, "path"));
        if (!id || !original || !below(original, cfg->path)) {
            rc = VAULT_ERR_ARG;
            break;
        }
        snapshot = find_snapshot(r->disk->doc, id);
        const char *relative = original + strlen(cfg->path) + (original[strlen(cfg->path)] == '/');
        const char *suffix = path + strlen(dest);
        size_t n = strlen(relative) + strlen(suffix) + 1;
        free(r->path);
        r->path = malloc(n);
        if (r->path)
            snprintf(r->path, n, "%s%s", relative, suffix);
        conflict_gen = (uint64_t)g;
        longest = strlen(dest);
    }
    if (!rc && !snapshot)
        rc = below(path, cfg->path) || longest ? VAULT_ERR_INCOMPLETE : 1;
    if (!rc && !r->path)
        rc = VAULT_ERR_OOM;
    vault_grant *grant = NULL;
    const char *wire = json_string_value(json_object_get(r->disk->doc, "grant"));
    if (!rc)
        rc = wire ? vault_grant_decode(wire, &grant) : VAULT_ERR_INCOMPLETE;
    if (!rc)
        rc = nh_vault_snapshot_open(r->disk, grant, snapshot, &r->record, &r->tree);
    vault_grant_free(grant);
    const porthome_node_entry *entry = NULL;
    porthome_ring_t ring;
    if (!rc)
        rc = nh_syncd_vault_lookup(r->tree, r->path, 0, &entry, &ring);
    if (!rc && (entry->kind != PORTHOME_KIND_FILE || entry->size_plain != size ||
                entry->chunks_len != count))
        rc = VAULT_ERR_AUTH;
    for (size_t k = 0; !rc && k < count; ++k) {
        char hex[65];
        porthome_hex64(entry->chunks[k].sha256, hex);
        if (strcmp(hex, chunks[k]))
            rc = VAULT_ERR_AUTH;
    }
    if (rc) {
        nh_vault_reader_close(r);
        return rc;
    }
    *out = r;
    return 0;
}
int nh_vault_reader_open(const nh_syncd_vault_config *cfg, const char *state, const uint8_t key[32],
                         nh_syncd_cache *cache, const vault_blob_io *public_io, const char *path,
                         uint64_t generation, uint64_t size, const char *const *chunks,
                         size_t count, nh_vault_reader **out) {
    *out = NULL;
    for (size_t i = 0; cfg && i < cfg->len; ++i) {
        if (!below(path, cfg->vaults[i].path) && !nh_syncd_vault_conflict_path(path))
            continue;
        int rc = open_one(&cfg->vaults[i], state, key, cache, public_io, path, generation, size,
                          chunks, count, out);
        if (rc != 1)
            return rc;
    }
    return nh_syncd_vault_conflict_path(path) ? VAULT_ERR_INCOMPLETE : 1;
}
int nh_vault_reader_read(nh_vault_reader *r, uint64_t offset, uint8_t *out, size_t cap, size_t *n) {
    return nh_syncd_vault_read(r->tree, r->path, offset, out, cap, &r->disk->io, n);
}
void nh_vault_reader_close(nh_vault_reader *r) {
    if (!r)
        return;
    vault_record_free(r->record);
    vault_tree_free(r->tree);
    nh_vault_session_close(r->disk);
    free(r->path);
    free(r);
}
