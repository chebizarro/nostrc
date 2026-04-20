/*
 * hanami-types.h - Shared types for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * libhanami: Blossom filesystem extension & tools for libgit2
 */

#ifndef HANAMI_TYPES_H
#define HANAMI_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <git2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

typedef enum {
    HANAMI_OK = 0,
    HANAMI_ERR_NOMEM = -1,
    HANAMI_ERR_INVALID_ARG = -2,
    HANAMI_ERR_NOT_FOUND = -3,
    HANAMI_ERR_NETWORK = -4,
    HANAMI_ERR_AUTH = -5,
    HANAMI_ERR_HASH_MISMATCH = -6,
    HANAMI_ERR_INDEX = -7,
    HANAMI_ERR_LIBGIT2 = -8,
    HANAMI_ERR_NOSTR = -9,
    HANAMI_ERR_CONFIG = -10,
    HANAMI_ERR_IO = -11,
    HANAMI_ERR_BLOSSOM = -12,
    HANAMI_ERR_UPLOAD = -13,
    HANAMI_ERR_TIMEOUT = -14,
} hanami_error_t;

/**
 * hanami_strerror:
 * @err: error code
 *
 * Returns: (transfer none): human-readable error message (static string)
 */
const char *hanami_strerror(hanami_error_t err);

/* =========================================================================
 * OID ↔ Blossom hash index entry
 * ========================================================================= */

/**
 * hanami_index_entry:
 *
 * Maps a Git object ID to its Blossom SHA-256 content hash.
 */
typedef struct {
    char git_oid[65];       /**< Hex SHA-1 (40 chars) or SHA-256 (64 chars) + NUL */
    char blossom_hash[65];  /**< Hex SHA-256 of raw content + NUL */
    git_object_t type;      /**< blob, tree, commit, or tag */
    size_t size;            /**< Object size in bytes */
    int64_t timestamp;      /**< Last access time (Unix epoch seconds) */
} hanami_index_entry_t;

/* Configuration — see hanami/hanami-config.h for the full API */
typedef struct hanami_config hanami_config_t;

/* =========================================================================
 * Ref state (for NIP-34 kind 30618)
 * ========================================================================= */

/**
 * hanami_ref_entry_t:
 *
 * A single ref → commit mapping in a repository state announcement.
 */
typedef struct {
    const char *refname;    /**< e.g. "refs/heads/main" or "HEAD" */
    const char *target;     /**< Commit hex OID, or "ref: refs/heads/main" for symbolic */
} hanami_ref_entry_t;

/* =========================================================================
 * Blossom blob descriptor (returned from PUT /upload)
 * ========================================================================= */

/**
 * hanami_blob_descriptor_t:
 *
 * Descriptor returned by a Blossom server after upload.
 */
typedef struct {
    char sha256[65];    /**< SHA-256 hex of the blob */
    size_t size;        /**< Blob size in bytes */
    char *url;          /**< Full URL to retrieve the blob (owned, may be NULL) */
    char *mime_type;    /**< MIME type (owned, may be NULL) */
    int64_t uploaded;   /**< Upload timestamp (Unix epoch) */
} hanami_blob_descriptor_t;

/**
 * hanami_blob_descriptor_free:
 * @desc: descriptor to free (nullable)
 */
void hanami_blob_descriptor_free(hanami_blob_descriptor_t *desc);

/* =========================================================================
 * Signer callback
 * ========================================================================= */

/**
 * hanami_sign_event_fn:
 * @event_json: JSON string of the unsigned event
 * @out_signed_json: (out): caller must free() the returned signed JSON string
 * @user_data: opaque user context
 *
 * Callback to sign a Nostr event. The implementation should parse the event
 * JSON, compute the event id, sign it, and return the complete signed JSON.
 *
 * Returns: HANAMI_OK on success, error code on failure
 */
typedef hanami_error_t (*hanami_sign_event_fn)(const char *event_json,
                                               char **out_signed_json,
                                               void *user_data);

/**
 * hanami_signer_t:
 *
 * Signer context providing identity and signing capabilities.
 */
typedef struct {
    const char *pubkey;             /**< Hex public key (64 chars) */
    hanami_sign_event_fn sign;      /**< Signing callback */
    void *user_data;                /**< Opaque context for the signer */
} hanami_signer_t;

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_TYPES_H */
