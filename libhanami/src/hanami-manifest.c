/*
 * hanami-manifest.c - plaintext object manifest codec and chain reader
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami-manifest.h"
#include "hanami-index-private.h"
#include <git2/oid.h>
#include <git2/types.h>
#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool hanami_hex_valid(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

bool hanami_oid_hex_valid(const char *s)
{
    /* This build's libgit2 uses classic SHA-1. Never truncate a SHA-256 OID. */
    return hanami_hex_valid(s, GIT_OID_SHA1_HEXSIZE);
}

bool hanami_object_type_valid(long long type)
{
    return type >= GIT_OBJECT_COMMIT && type <= GIT_OBJECT_TAG;
}

void hanami_manifest_dispose(hanami_manifest *m)
{
    if (!m) return;
    free(m->rows);
    memset(m, 0, sizeof(*m));
}

static int row_cmp(const void *a, const void *b)
{
    return strcmp(((const hanami_index_entry_t *)a)->git_oid,
                  ((const hanami_index_entry_t *)b)->git_oid);
}

hanami_error_t hanami_manifest_encode(const char *repo, const char *prev,
    const hanami_index_entry_t *rows, size_t count, char **out, size_t *len)
{
    if (!repo || !out || !len || (count && !rows)) return HANAMI_ERR_INVALID_ARG;
    *out = NULL; *len = 0;
    if (count > HANAMI_INDEX_SNAPSHOT_MAX_ROWS || (prev && !hanami_hex_valid(prev, 64)))
        return HANAMI_ERR_INDEX;
    hanami_index_entry_t *sorted = count ? malloc(count * sizeof(*sorted)) : NULL;
    if (count && !sorted) return HANAMI_ERR_NOMEM;
    if (count) memcpy(sorted, rows, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), row_cmp);
    hanami_error_t err = HANAMI_ERR_NOMEM;
    json_t *root = json_object(), *array = json_array();
    if (!root || !array) goto done;
    err = HANAMI_ERR_INDEX;
    for (size_t i = 0; i < count; i++) {
        const hanami_index_entry_t *r = &sorted[i];
        if (!hanami_oid_hex_valid(r->git_oid) || !hanami_hex_valid(r->blossom_hash, 64) ||
            !hanami_object_type_valid(r->type) || (uint64_t)r->size > INT64_MAX ||
            (i && strcmp(sorted[i - 1].git_oid, r->git_oid) >= 0))
            goto done;
        json_t *row = json_pack("{s:s,s:s,s:I,s:i}", "git_oid", r->git_oid,
            "sha256", r->blossom_hash, "size", (json_int_t)r->size, "type", (int)r->type);
        if (!row || json_array_append_new(array, row) < 0) { err = HANAMI_ERR_NOMEM; goto done; }
    }
    if (json_object_set_new(root, "v", json_integer(HANAMI_MANIFEST_VERSION)) < 0 ||
        json_object_set_new(root, "repo", json_string(repo)) < 0 ||
        json_object_set_new(root, "prev", prev ? json_string(prev) : json_null()) < 0 ||
        json_object_set(root, "rows", array) < 0) { err = HANAMI_ERR_NOMEM; goto done; }
    char *text = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
    if (!text) { err = HANAMI_ERR_NOMEM; goto done; }
    if (strlen(text) > HANAMI_INDEX_SNAPSHOT_MAX_BYTES) { free(text); goto done; }
    *out = text; *len = strlen(text);
    err = HANAMI_OK;
done:
    json_decref(array); json_decref(root); free(sorted);
    return err;
}

static const char *exact_string(json_t *value)
{
    const char *s = json_string_value(value);
    return s && json_string_length(value) == strlen(s) ? s : NULL;
}

hanami_error_t hanami_manifest_decode(const uint8_t *data, size_t len,
    const char *repo, hanami_manifest *out)
{
    if (!data || !repo || !out) return HANAMI_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (len > HANAMI_INDEX_SNAPSHOT_MAX_BYTES) return HANAMI_ERR_INDEX;
    json_t *root = json_loadb((const char *)data, len, JSON_REJECT_DUPLICATES, NULL);
    json_t *v = json_object_get(root, "v"), *jr = json_object_get(root, "repo");
    json_t *prev = json_object_get(root, "prev"), *rows = json_object_get(root, "rows");
    const char *prev_hex = json_is_null(prev) ? NULL : exact_string(prev);
    const char *got_repo = exact_string(jr);
    if (!json_is_object(root) || json_object_size(root) != 4 ||
        !json_is_integer(v) || json_integer_value(v) != HANAMI_MANIFEST_VERSION ||
        !got_repo || strcmp(got_repo, repo) != 0 ||
        !(json_is_null(prev) || hanami_hex_valid(prev_hex, 64)) ||
        !json_is_array(rows) || json_array_size(rows) > HANAMI_INDEX_SNAPSHOT_MAX_ROWS) {
        json_decref(root);
        return HANAMI_ERR_INDEX;
    }
    size_t n = json_array_size(rows);
    out->rows = n ? calloc(n, sizeof(*out->rows)) : NULL;
    if (n && !out->rows) { json_decref(root); return HANAMI_ERR_NOMEM; }
    if (prev_hex) memcpy(out->prev, prev_hex, 65);
    size_t i; json_t *row;
    json_array_foreach(rows, i, row) {
        const char *oid = exact_string(json_object_get(row, "git_oid"));
        const char *sha = exact_string(json_object_get(row, "sha256"));
        json_t *type = json_object_get(row, "type"), *size = json_object_get(row, "size");
        if (!json_is_object(row) || json_object_size(row) != 4 ||
            !hanami_oid_hex_valid(oid) || !hanami_hex_valid(sha, 64) ||
            !json_is_integer(type) || !hanami_object_type_valid(json_integer_value(type)) ||
            !json_is_integer(size) || json_integer_value(size) < 0 ||
            (uint64_t)json_integer_value(size) > SIZE_MAX ||
            (i && strcmp(out->rows[i - 1].git_oid, oid) >= 0)) {
            hanami_manifest_dispose(out);
            json_decref(root);
            return HANAMI_ERR_INDEX;
        }
        hanami_index_entry_t *e = &out->rows[i];
        memcpy(e->git_oid, oid, strlen(oid) + 1);
        memcpy(e->blossom_hash, sha, 65);
        e->type = (git_object_t)json_integer_value(type);
        e->size = (size_t)json_integer_value(size);
    }
    out->count = n;
    json_decref(root);
    return HANAMI_OK;
}

hanami_error_t hanami_manifest_sync(blossom_client_t *client, hanami_index_t *cache,
    const char *repo, const char *head, size_t *segments_read)
{
    if (segments_read) *segments_read = 0;
    if (!client || !cache || !repo || !hanami_hex_valid(head, 64))
        return HANAMI_ERR_INVALID_ARG;
    char *known = NULL;
    hanami_error_t err = hanami_index_meta_get(cache, HANAMI_INDEX_META_MANIFEST_HEAD, &known);
    if (err == HANAMI_ERR_NOT_FOUND) err = HANAMI_OK;
    if (err != HANAMI_OK) return err;
    if (known && strcmp(known, head) == 0) { free(known); return HANAMI_OK; }
    if ((err = hanami_index_begin(cache)) != HANAMI_OK) { free(known); return err; }

    char cur[65];
    memcpy(cur, head, 65);
    size_t depth = 0;
    int64_t now = (int64_t)time(NULL);
    while (cur[0] && !(known && strcmp(cur, known) == 0)) {
        if (++depth > HANAMI_MANIFEST_MAX_DEPTH) { err = HANAMI_ERR_INDEX; break; }
        uint8_t *data = NULL; size_t len = 0;
        /* libblossom verifies the wire address before returning content. */
        err = blossom_client_get_content(client, cur, &data, &len);
        if (err != HANAMI_OK) break;
        hanami_manifest m;
        err = hanami_manifest_decode(data, len, repo, &m);
        free(data);
        if (err != HANAMI_OK) break;
        for (size_t i = 0; err == HANAMI_OK && i < m.count; i++) {
            if (hanami_index_exists(cache, m.rows[i].git_oid)) continue;
            m.rows[i].timestamp = now;
            err = hanami_index_put(cache, &m.rows[i]);
        }
        memcpy(cur, m.prev, 65);
        hanami_manifest_dispose(&m);
    }
    if (segments_read) *segments_read = depth;
    free(known);
    if (err == HANAMI_OK)
        err = hanami_index_meta_set(cache, HANAMI_INDEX_META_MANIFEST_HEAD, head);
    if (err == HANAMI_OK)
        err = hanami_index_commit(cache);
    if (err != HANAMI_OK)
        hanami_index_rollback(cache);
    return err;
}

static bool target_covered(const char *target, hanami_index_t *cache)
{
    if (!target || strncmp(target, "ref: ", 5) == 0) return true;
    return hanami_oid_hex_valid(target) && hanami_index_exists(cache, target);
}

hanami_error_t hanami_manifest_check_coverage(const nip34_repo_state_t *state,
    hanami_index_t *cache)
{
    if (!state || !cache) return HANAMI_ERR_INVALID_ARG;
    for (size_t i = 0; i < state->ref_count; i++)
        if (!target_covered(state->refs[i].target, cache)) return HANAMI_ERR_INDEX;
    return target_covered(state->head, cache) ? HANAMI_OK : HANAMI_ERR_INDEX;
}
