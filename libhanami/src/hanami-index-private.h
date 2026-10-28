/* SPDX-License-Identifier: MIT — private index helpers (cache file, meta, tx). */
#ifndef HANAMI_INDEX_PRIVATE_H
#define HANAMI_INDEX_PRIVATE_H
#include "hanami/hanami-index.h"

/* hanami_meta schema; unversioned (pre-0.6) databases are stamped on open,
 * newer versions are refused. */
#define HANAMI_INDEX_SCHEMA_VERSION 1
/* Meta key: newest manifest segment fully imported into this cache. */
#define HANAMI_INDEX_META_MANIFEST_HEAD "manifest_head"
/* Meta key: segments from manifest_head back to its checkpoint, inclusive. */
#define HANAMI_INDEX_META_MANIFEST_DEPTH "manifest_depth"

/* Open the index at an exact database file path (or ":memory:"). */
hanami_error_t hanami_index_open_file(hanami_index_t **out, const char *db_path);
/* HANAMI_ERR_NOT_FOUND when absent; caller frees *out. */
hanami_error_t hanami_index_meta_get(hanami_index_t *idx, const char *key, char **out);
hanami_error_t hanami_index_meta_set(hanami_index_t *idx, const char *key, const char *value);
/* Record that an OID is covered by the published manifest chain. */
hanami_error_t hanami_index_mark_published(hanami_index_t *idx, const char *git_oid_hex);
hanami_error_t hanami_index_unmark_published(hanami_index_t *idx, const char *git_oid_hex);
/* Sync bookkeeping (inside a transaction): note every OID of the chain read;
 * when the walk reached a checkpoint (the complete map), prune published
 * markers the new chain does not cover (it replaced an unrelated chain). */
hanami_error_t hanami_index_sync_begin(hanami_index_t *idx);
hanami_error_t hanami_index_sync_note(hanami_index_t *idx, const char *git_oid_hex);
hanami_error_t hanami_index_sync_prune(hanami_index_t *idx);
/* Rows ascending by OID: all, or only those not yet published. More than
 * max_rows fails with HANAMI_ERR_INDEX. Caller frees *rows. */
hanami_error_t hanami_index_collect(hanami_index_t *idx, bool unpublished_only,
    size_t max_rows, hanami_index_entry_t **rows, size_t *count);
hanami_error_t hanami_index_begin(hanami_index_t *idx);
hanami_error_t hanami_index_commit(hanami_index_t *idx);
void hanami_index_rollback(hanami_index_t *idx);
#endif
