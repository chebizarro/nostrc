/* SPDX-License-Identifier: MIT — private index helpers (cache file, meta, tx). */
#ifndef HANAMI_INDEX_PRIVATE_H
#define HANAMI_INDEX_PRIVATE_H
#include "hanami/hanami-index.h"

/* hanami_meta schema; unversioned (pre-0.6) databases are stamped on open,
 * newer versions are refused. */
#define HANAMI_INDEX_SCHEMA_VERSION 1
/* Meta key: newest manifest segment fully imported into this cache. */
#define HANAMI_INDEX_META_MANIFEST_HEAD "manifest_head"

/* Open the index at an exact database file path (or ":memory:"). */
hanami_error_t hanami_index_open_file(hanami_index_t **out, const char *db_path);
/* HANAMI_ERR_NOT_FOUND when absent; caller frees *out. */
hanami_error_t hanami_index_meta_get(hanami_index_t *idx, const char *key, char **out);
hanami_error_t hanami_index_meta_set(hanami_index_t *idx, const char *key, const char *value);
hanami_error_t hanami_index_begin(hanami_index_t *idx);
hanami_error_t hanami_index_commit(hanami_index_t *idx);
void hanami_index_rollback(hanami_index_t *idx);
#endif
