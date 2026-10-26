/*
 * hanami-types.h - Shared types for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * libhanami: Blossom filesystem extension & tools for libgit2
 */

#ifndef HANAMI_TYPES_H
#define HANAMI_TYPES_H

#include "hanami-export.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <git2/types.h>
#include <blossom/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

typedef blossom_error_t hanami_error_t;
#define HANAMI_OK BLOSSOM_OK
#define HANAMI_ERR_NOMEM BLOSSOM_ERR_NOMEM
#define HANAMI_ERR_INVALID_ARG BLOSSOM_ERR_INVALID_ARG
#define HANAMI_ERR_NOT_FOUND BLOSSOM_ERR_NOT_FOUND
#define HANAMI_ERR_NETWORK BLOSSOM_ERR_NETWORK
#define HANAMI_ERR_AUTH BLOSSOM_ERR_AUTH
#define HANAMI_ERR_HASH_MISMATCH BLOSSOM_ERR_HASH_MISMATCH
#define HANAMI_ERR_INDEX ((hanami_error_t)-7)
#define HANAMI_ERR_LIBGIT2 ((hanami_error_t)-8)
#define HANAMI_ERR_NOSTR BLOSSOM_ERR_NOSTR
#define HANAMI_ERR_CONFIG ((hanami_error_t)-10)
#define HANAMI_ERR_IO BLOSSOM_ERR_IO
#define HANAMI_ERR_BLOSSOM BLOSSOM_ERR_BLOSSOM
#define HANAMI_ERR_UPLOAD BLOSSOM_ERR_UPLOAD
#define HANAMI_ERR_TIMEOUT BLOSSOM_ERR_TIMEOUT

/**
 * hanami_strerror:
 * @err: error code
 *
 * Returns: (transfer none): human-readable error message (static string)
 */
HANAMI_API const char *hanami_strerror(hanami_error_t err);

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
    uint32_t chunk_key_id;   /**< Key id for the shared porthome index row (0 for raw objects) */
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
typedef blossom_sign_event_fn hanami_sign_event_fn;
typedef blossom_signer_t hanami_signer_t;

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_TYPES_H */
