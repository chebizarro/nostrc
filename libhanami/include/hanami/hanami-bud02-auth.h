/*
 * hanami-bud02-auth.h - BUD-02 Blossom authorization (kind 24242)
 *
 * SPDX-License-Identifier: MIT
 *
 * Blossom uses kind 24242 events for upload/delete authorization.
 * This is structurally similar to NIP-98 (kind 27235) but uses different
 * tags: [t, action], [x, sha256], [expiration, unix_ts].
 *
 * @see https://github.com/hzrd149/blossom/blob/master/buds/02.md
 */

#ifndef HANAMI_BUD02_AUTH_H
#define HANAMI_BUD02_AUTH_H

#include "hanami-types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations from libnostr */
struct _NostrEvent;
typedef struct _NostrEvent NostrEvent;

/* =========================================================================
 * Constants
 * ========================================================================= */

/** BUD-02 event kind */
#define HANAMI_BUD02_KIND 24242

/** Default expiration window in seconds (5 minutes) */
#define HANAMI_BUD02_DEFAULT_EXPIRATION 300

/* =========================================================================
 * Types
 * ========================================================================= */

/**
 * hanami_bud02_action_t:
 * BUD-02 action types corresponding to the "t" tag value.
 */
typedef enum {
    HANAMI_BUD02_ACTION_UPLOAD = 0,
    HANAMI_BUD02_ACTION_DELETE,
    HANAMI_BUD02_ACTION_GET,
    HANAMI_BUD02_ACTION_LIST,
    HANAMI_BUD02_ACTION_MIRROR,
} hanami_bud02_action_t;

/**
 * hanami_bud02_result_t:
 * Result codes for BUD-02 operations.
 */
typedef enum {
    HANAMI_BUD02_OK = 0,
    HANAMI_BUD02_ERR_NULL_PARAM      = -1,
    HANAMI_BUD02_ERR_ALLOC           = -2,
    HANAMI_BUD02_ERR_INVALID_KIND    = -3,
    HANAMI_BUD02_ERR_EXPIRED         = -4,
    HANAMI_BUD02_ERR_ACTION_MISMATCH = -5,
    HANAMI_BUD02_ERR_HASH_MISMATCH   = -6,
    HANAMI_BUD02_ERR_SIGNATURE       = -7,
    HANAMI_BUD02_ERR_MISSING_TAG     = -8,
    HANAMI_BUD02_ERR_ENCODE          = -9,
    HANAMI_BUD02_ERR_DECODE          = -10,
    HANAMI_BUD02_ERR_INVALID_HEADER  = -11,
    HANAMI_BUD02_ERR_INVALID_ACTION  = -12,
    HANAMI_BUD02_ERR_PUBKEY_MISMATCH = -13,
    HANAMI_BUD02_ERR_MISSING_EXPIRATION = -14,
} hanami_bud02_result_t;

/**
 * hanami_bud02_validate_options_t:
 * Optional validation parameters.
 */
typedef struct {
    /** Expected SHA-256 blob hash (hex, 64 chars), or NULL to skip check */
    const char *expected_sha256;
    /** Expected pubkey (hex, 64 chars), or NULL to skip check */
    const char *expected_pubkey;
    /** If nonzero, override "now" for expiration check (for testing) */
    int64_t     now_override;
} hanami_bud02_validate_options_t;

/* =========================================================================
 * Event creation
 * ========================================================================= */

/**
 * hanami_bud02_create_auth_event:
 * @action: the BUD-02 action (upload, delete, get, list, mirror)
 * @sha256_hex: (nullable): SHA-256 hash of the blob (64-char hex), required
 *              for upload/delete, optional for get/list
 * @expiration: expiration unix timestamp (0 = use default: now + 300s)
 * @server_url: (nullable): optional server URL to include as "server" tag
 *
 * Create an unsigned kind 24242 event with BUD-02 tags.
 * Caller must sign with nostr_event_sign() before use.
 *
 * Returns: newly allocated NostrEvent, or NULL on error.
 *          Caller must free with nostr_event_free().
 */
NostrEvent *hanami_bud02_create_auth_event(hanami_bud02_action_t action,
                                           const char *sha256_hex,
                                           int64_t expiration,
                                           const char *server_url);

/* =========================================================================
 * Header creation/parsing (Nostr base64 scheme)
 * ========================================================================= */

/**
 * hanami_bud02_create_auth_header:
 * @event: signed kind 24242 event
 *
 * Serialize the event to JSON, base64-encode, and return "Nostr <base64>".
 *
 * Returns: newly allocated header string, or NULL on error. Caller frees.
 */
char *hanami_bud02_create_auth_header(const NostrEvent *event);

/**
 * hanami_bud02_parse_auth_header:
 * @header: "Nostr <base64>" header value
 * @out_event: (out): parsed event on success
 *
 * Returns: HANAMI_BUD02_OK on success
 */
hanami_bud02_result_t hanami_bud02_parse_auth_header(const char *header,
                                                     NostrEvent **out_event);

/* =========================================================================
 * Validation
 * ========================================================================= */

/**
 * hanami_bud02_validate_auth_event:
 * @event: kind 24242 event to validate
 * @expected_action: the action the server expects
 * @options: (nullable): optional validation parameters
 *
 * Validates:
 * 1. Kind is 24242
 * 2. "t" tag matches expected action
 * 3. "expiration" tag exists and is in the future (mandatory)
 * 4. Optional: "x" tag matches expected SHA-256
 * 5. Signature is valid
 * 6. Optional: pubkey matches expected pubkey
 *
 * Returns: HANAMI_BUD02_OK on success, specific error code otherwise
 */
hanami_bud02_result_t hanami_bud02_validate_auth_event(
    const NostrEvent *event,
    hanami_bud02_action_t expected_action,
    const hanami_bud02_validate_options_t *options);

/* =========================================================================
 * Accessors
 * ========================================================================= */

/** Get the "t" (action) tag value, or NULL if missing */
const char *hanami_bud02_get_action(const NostrEvent *event);

/** Get the "x" (SHA-256 hash) tag value, or NULL if missing */
const char *hanami_bud02_get_hash(const NostrEvent *event);

/** Get the "expiration" tag value as int64, or 0 if missing */
int64_t hanami_bud02_get_expiration(const NostrEvent *event);

/** Get the "server" tag value, or NULL if missing */
const char *hanami_bud02_get_server(const NostrEvent *event);

/* =========================================================================
 * Helpers
 * ========================================================================= */

/**
 * hanami_bud02_action_str:
 * @action: action enum value
 *
 * Returns: string name ("upload", "delete", etc.) or NULL for invalid
 */
const char *hanami_bud02_action_str(hanami_bud02_action_t action);

/**
 * hanami_bud02_action_from_str:
 * @str: action string
 * @out: (out): action enum on success
 *
 * Returns: HANAMI_BUD02_OK on success, HANAMI_BUD02_ERR_INVALID_ACTION otherwise
 */
hanami_bud02_result_t hanami_bud02_action_from_str(const char *str,
                                                    hanami_bud02_action_t *out);

/**
 * hanami_bud02_strerror:
 * @result: result code
 *
 * Returns: human-readable error string (static, do not free)
 */
const char *hanami_bud02_strerror(hanami_bud02_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_BUD02_AUTH_H */
