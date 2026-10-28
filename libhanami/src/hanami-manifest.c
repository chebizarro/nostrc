/*
 * hanami-manifest.c - plaintext object manifest codec and chain reader
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami-manifest.h"
#include "hanami-index-private.h"
#include "hanami-nostr-internal.h"
#include <git2/oid.h>
#include <git2/repository.h>
#include <git2/errors.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
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
    if (count) qsort(sorted, count, sizeof(*sorted), row_cmp);
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
    char *known = NULL, *known_depth = NULL;
    hanami_error_t err = hanami_index_meta_get(cache, HANAMI_INDEX_META_MANIFEST_HEAD, &known);
    if (err == HANAMI_ERR_NOT_FOUND) err = HANAMI_OK;
    if (err != HANAMI_OK) return err;
    if (known && strcmp(known, head) == 0) { free(known); return HANAMI_OK; }
    if (known && hanami_index_meta_get(cache, HANAMI_INDEX_META_MANIFEST_DEPTH, &known_depth) != HANAMI_OK)
        known_depth = NULL;
    if ((err = hanami_index_begin(cache)) != HANAMI_OK) { free(known); free(known_depth); return err; }
    err = hanami_index_sync_begin(cache);

    char cur[65];
    memcpy(cur, head, 65);
    size_t depth = 0;
    int64_t now = (int64_t)time(NULL);
    while (err == HANAMI_OK && cur[0] && !(known && strcmp(cur, known) == 0)) {
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
            if (!hanami_index_exists(cache, m.rows[i].git_oid)) {
                m.rows[i].timestamp = now;
                err = hanami_index_put(cache, &m.rows[i]);
            }
            if (err == HANAMI_OK) err = hanami_index_mark_published(cache, m.rows[i].git_oid);
            if (err == HANAMI_OK) err = hanami_index_sync_note(cache, m.rows[i].git_oid);
        }
        memcpy(cur, m.prev, 65);
        hanami_manifest_dispose(&m);
    }
    if (segments_read) *segments_read = depth;
    /* Chain length back to the checkpoint: walked segments, plus the cached
     * head's depth when the walk stopped there (unknown counts as full, so
     * the next writer checkpoints). */
    size_t total = depth;
    if (cur[0])
        total += known_depth ? (size_t)strtoul(known_depth, NULL, 10)
                             : HANAMI_MANIFEST_CHECKPOINT_LINKS;
    free(known); free(known_depth);
    /* Reached a checkpoint without meeting the cached head: this chain is the
     * complete map and may have replaced an unrelated one. Only its rows are
     * published now; others stay as unpublished local rows. */
    if (err == HANAMI_OK && !cur[0])
        err = hanami_index_sync_prune(cache);
    char depth_text[24];
    snprintf(depth_text, sizeof(depth_text), "%zu", total);
    if (err == HANAMI_OK)
        err = hanami_index_meta_set(cache, HANAMI_INDEX_META_MANIFEST_HEAD, head);
    if (err == HANAMI_OK)
        err = hanami_index_meta_set(cache, HANAMI_INDEX_META_MANIFEST_DEPTH, depth_text);
    if (err == HANAMI_OK)
        err = hanami_index_commit(cache);
    if (err != HANAMI_OK)
        hanami_index_rollback(cache);
    return err;
}

hanami_error_t hanami_repo_cache_open(git_repository *repo, hanami_index_t **out)
{
    if (!repo || !out) return HANAMI_ERR_INVALID_ARG;
    *out = NULL;
    const char *gitdir = git_repository_path(repo);
    size_t n = strlen(gitdir) + 32;
    char *dir = malloc(n), *file = malloc(n);
    if (!dir || !file) { free(dir); free(file); return HANAMI_ERR_NOMEM; }
    snprintf(dir, n, "%shanami", gitdir);
    snprintf(file, n, "%shanami/index.sqlite", gitdir);
    hanami_error_t err = HANAMI_OK;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) err = HANAMI_ERR_IO;
    if (err == HANAMI_OK) err = hanami_index_open_file(out, file);
    free(dir); free(file);
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

static hanami_error_t covered(const nip34_ref_t *refs, size_t n, const char *head,
                              hanami_index_t *cache)
{
    nip34_repo_state_t state = { .refs = (nip34_ref_t *)refs, .ref_count = n, .head = (char *)head };
    return hanami_manifest_check_coverage(&state, cache);
}

hanami_error_t hanami_manifest_publish_state(blossom_client_t *client, hanami_nostr_ctx_t *ctx,
    hanami_index_t *cache, const char *repo, const char *author,
    const nip34_ref_t *refs, size_t n, const char *head)
{
    if (!client || !ctx || !cache || !repo || !author) return HANAMI_ERR_INVALID_ARG;
    /* The 30618 lands in the signer's namespace; publishing for another
     * author would "succeed" where readers of that author never look. */
    const char *signer = hanami_nostr_ctx_signer_pubkey(ctx);
    if (!signer || strcmp(signer, author) != 0) {
        git_error_set(GIT_ERROR_INVALID, "hanami: signer %s cannot publish state for %s",
                      signer ? signer : "(none)", author);
        return HANAMI_ERR_AUTH;
    }

    /* Base: the currently published chain. Catch up first, so the delta is
     * relative to what readers see (base check, not compare-and-swap). */
    nip34_repo_state_t *published = NULL;
    char base[65] = {0};
    hanami_error_t err = hanami_nostr_fetch_state_manifest(ctx, repo, author, &published, base);
    nip34_repo_state_free(published);
    if (err == HANAMI_ERR_NOT_FOUND) { err = HANAMI_OK; base[0] = 0; }
    if (err != HANAMI_OK) {
        git_error_set(GIT_ERROR_NET, "hanami: cannot read published kind-30618 for %s: %s",
                      repo, hanami_strerror(err));
        return err;
    }
    size_t depth = 0;
    if (base[0]) {
        err = hanami_manifest_sync(client, cache, repo, base, NULL);
        char *text = NULL;
        if (err == HANAMI_OK &&
            hanami_index_meta_get(cache, HANAMI_INDEX_META_MANIFEST_DEPTH, &text) == HANAMI_OK)
            depth = (size_t)strtoul(text, NULL, 10);
        free(text);
        if (err != HANAMI_OK) {
            git_error_set(GIT_ERROR_INVALID, "hanami: published manifest %s for %s is "
                          "unavailable or invalid: %s", base, repo, hanami_strerror(err));
            return err;
        }
    }
    if (covered(refs, n, head, cache) != HANAMI_OK) {
        git_error_set(GIT_ERROR_INVALID, "hanami: refs for %s point at objects that were "
                      "not uploaded; refusing to publish an uncoverable state", repo);
        return HANAMI_ERR_INDEX;
    }

    /* No base (first push, or a 30618 republished without the tag) or a chain
     * at the link limit: write a checkpoint holding every row. */
    bool checkpoint = !base[0] || depth >= HANAMI_MANIFEST_CHECKPOINT_LINKS;
    hanami_index_entry_t *rows = NULL;
    size_t count = 0;
    err = hanami_index_collect(cache, !checkpoint, HANAMI_INDEX_SNAPSHOT_MAX_ROWS, &rows, &count);
    if (err != HANAMI_OK) {
        git_error_set(GIT_ERROR_INVALID, "hanami: manifest for %s would exceed %u rows",
                      repo, HANAMI_INDEX_SNAPSHOT_MAX_ROWS);
        return err;
    }
    char next[65];
    size_t next_depth = depth;
    if (!checkpoint && count == 0) {
        memcpy(next, base, 65); /* refs-only change: same chain head */
    } else {
        char *text = NULL; size_t len = 0;
        err = hanami_manifest_encode(repo, checkpoint ? NULL : base, rows, count, &text, &len);
        if (err == HANAMI_OK) err = blossom_client_address(client, (const uint8_t *)text, len, next);
        if (err == HANAMI_OK) err = blossom_client_put(client, (const uint8_t *)text, len, next, NULL);
        free(text);
        if (err != HANAMI_OK) {
            free(rows);
            git_error_set(GIT_ERROR_NET, "hanami: manifest upload for %s failed: %s",
                          repo, hanami_strerror(err));
            return err;
        }
        next_depth = checkpoint ? 1 : depth + 1;
    }

    /* State last, and only relay acceptance (OK true) counts. */
    err = hanami_nostr_publish_state_manifest(ctx, repo, refs, n, head, next);
    if (err != HANAMI_OK) {
        free(rows);
        git_error_set(GIT_ERROR_NET, "hanami: kind-30618 for %s was not accepted: %s",
                      repo, hanami_strerror(err));
        return err;
    }
    /* Only now are these rows covered by the published chain. A crash here is
     * repaired by the next sync from the published head. */
    char depth_text[24];
    snprintf(depth_text, sizeof(depth_text), "%zu", next_depth);
    err = hanami_index_begin(cache);
    for (size_t i = 0; err == HANAMI_OK && i < count; i++)
        err = hanami_index_mark_published(cache, rows[i].git_oid);
    if (err == HANAMI_OK) err = hanami_index_meta_set(cache, HANAMI_INDEX_META_MANIFEST_HEAD, next);
    if (err == HANAMI_OK) err = hanami_index_meta_set(cache, HANAMI_INDEX_META_MANIFEST_DEPTH, depth_text);
    if (err == HANAMI_OK) err = hanami_index_commit(cache);
    if (err != HANAMI_OK) hanami_index_rollback(cache);
    free(rows);
    /* The publication itself succeeded; a cache write failure only costs a
     * re-sync next time, so it does not turn an accepted push into failure. */
    return HANAMI_OK;
}
