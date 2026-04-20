/*
 * hanami-index.h - OID ↔ Blossom SHA-256 mapping index
 *
 * SPDX-License-Identifier: MIT
 *
 * Git computes OIDs as SHA-1 or SHA-256 of (type_header + content).
 * Blossom uses SHA-256 of raw content only. These never match.
 * This index maintains the bidirectional mapping.
 */

#ifndef HANAMI_INDEX_H
#define HANAMI_INDEX_H

#include "hanami-types.h"
#include <git2/oid.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque index handle */
typedef struct hanami_index hanami_index_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * hanami_index_open:
 * @out: (out): created index handle
 * @path: directory for the index database file
 * @backend: "sqlite" or "lmdb" (NULL defaults to "sqlite")
 *
 * Open or create the OID mapping index at the given path.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_index_open(hanami_index_t **out,
                                 const char *path,
                                 const char *backend);

/**
 * hanami_index_close:
 * @idx: (transfer full) (nullable): index to close and free
 */
void hanami_index_close(hanami_index_t *idx);

/* =========================================================================
 * CRUD operations
 * ========================================================================= */

/**
 * hanami_index_put:
 * @idx: index handle
 * @entry: entry to store (git_oid and blossom_hash must be set)
 *
 * Insert or update a mapping. If the git_oid already exists, it is updated.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_index_put(hanami_index_t *idx,
                                const hanami_index_entry_t *entry);

/**
 * hanami_index_get_by_oid:
 * @idx: index handle
 * @git_oid_hex: hex Git OID to look up (40 or 64 chars)
 * @out: (out): filled on success
 *
 * Look up a mapping by Git object ID.
 *
 * Returns: HANAMI_OK if found, HANAMI_ERR_NOT_FOUND if absent
 */
hanami_error_t hanami_index_get_by_oid(hanami_index_t *idx,
                                       const char *git_oid_hex,
                                       hanami_index_entry_t *out);

/**
 * hanami_index_get_by_blossom:
 * @idx: index handle
 * @blossom_hash_hex: hex SHA-256 of raw content (64 chars)
 * @out: (out): filled on success (returns first match)
 *
 * Look up a mapping by Blossom content hash.
 *
 * Returns: HANAMI_OK if found, HANAMI_ERR_NOT_FOUND if absent
 */
hanami_error_t hanami_index_get_by_blossom(hanami_index_t *idx,
                                           const char *blossom_hash_hex,
                                           hanami_index_entry_t *out);

/**
 * hanami_index_exists:
 * @idx: index handle
 * @git_oid_hex: hex Git OID
 *
 * Returns: true if the OID is in the index
 */
bool hanami_index_exists(hanami_index_t *idx, const char *git_oid_hex);

/**
 * hanami_index_delete:
 * @idx: index handle
 * @git_oid_hex: hex Git OID to remove
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_NOT_FOUND if absent
 */
hanami_error_t hanami_index_delete(hanami_index_t *idx,
                                   const char *git_oid_hex);

/**
 * hanami_index_count:
 * @idx: index handle
 *
 * Returns: number of entries in the index
 */
size_t hanami_index_count(hanami_index_t *idx);

/* =========================================================================
 * Hash computation helpers
 * ========================================================================= */

/**
 * hanami_hash_blossom:
 * @data: raw content bytes
 * @len: length of data
 * @out_hex: (out): 65-byte buffer for hex SHA-256 + NUL
 *
 * Compute the Blossom SHA-256 hash of raw content.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_hash_blossom(const void *data, size_t len,
                                   char out_hex[65]);

/**
 * hanami_hash_git_sha1:
 * @data: raw object content
 * @len: length of data
 * @type: Git object type (blob, tree, commit, tag)
 * @out_hex: (out): 41-byte buffer for hex SHA-1 + NUL
 *
 * Compute the Git SHA-1 OID: SHA-1("type len\0" + data).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_hash_git_sha1(const void *data, size_t len,
                                    git_object_t type,
                                    char out_hex[41]);

/**
 * hanami_hash_git_sha256:
 * @data: raw object content
 * @len: length of data
 * @type: Git object type (blob, tree, commit, tag)
 * @out_hex: (out): 65-byte buffer for hex SHA-256 + NUL
 *
 * Compute the Git SHA-256 OID: SHA-256("type len\0" + data).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_hash_git_sha256(const void *data, size_t len,
                                      git_object_t type,
                                      char out_hex[65]);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_INDEX_H */
