/*
 * auth.h - BUD-02 Blossom authorization (kind 24242)
 *
 * SPDX-License-Identifier: MIT
 *
 * Blossom uses kind 24242 events for upload/delete authorization.
 * This is structurally similar to NIP-98 (kind 27235) but uses different
 * tags: [t, action], [x, sha256], [expiration, unix_ts].
 *
 * @see https://github.com/hzrd149/blossom/blob/master/buds/02.md
 */

#ifndef NOSTR_BUD02_AUTH_H
#define NOSTR_BUD02_AUTH_H

#include <nostr/bud02/export.h>
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
#define NOSTR_BUD02_KIND 24242

/** Default expiration window in seconds (5 minutes) */
#define NOSTR_BUD02_DEFAULT_EXPIRATION 300

/** Default batch-auth expiration window in seconds (2 minutes).
 *  Kept short because the batch header may be reused across N PUTs; a
 *  smaller window bounds the damage of a leaked header. */
#define NOSTR_BUD02_BATCH_DEFAULT_EXPIRATION 120

/** Hard cap on batch expiration window (15 minutes). Prevents callers
 *  from minting long-lived tokens by accident. */
#define NOSTR_BUD02_BATCH_MAX_EXPIRATION 900

/* =========================================================================
 * Types
 * ========================================================================= */

/**
 * nostr_bud02_action_t:
 * BUD-02 action types corresponding to the "t" tag value.
 */
typedef enum {
    NOSTR_BUD02_ACTION_UPLOAD = 0,
    NOSTR_BUD02_ACTION_DELETE,
    NOSTR_BUD02_ACTION_GET,
    NOSTR_BUD02_ACTION_LIST,
    NOSTR_BUD02_ACTION_MIRROR,
} nostr_bud02_action_t;

/**
 * nostr_bud02_result_t:
 * Result codes for BUD-02 operations.
 */
typedef enum {
    NOSTR_BUD02_OK = 0,
    NOSTR_BUD02_ERR_NULL_PARAM      = -1,
    NOSTR_BUD02_ERR_ALLOC           = -2,
    NOSTR_BUD02_ERR_INVALID_KIND    = -3,
    NOSTR_BUD02_ERR_EXPIRED         = -4,
    NOSTR_BUD02_ERR_ACTION_MISMATCH = -5,
    NOSTR_BUD02_ERR_HASH_MISMATCH   = -6,
    NOSTR_BUD02_ERR_SIGNATURE       = -7,
    NOSTR_BUD02_ERR_MISSING_TAG     = -8,
    NOSTR_BUD02_ERR_ENCODE          = -9,
    NOSTR_BUD02_ERR_DECODE          = -10,
    NOSTR_BUD02_ERR_INVALID_HEADER  = -11,
    NOSTR_BUD02_ERR_INVALID_ACTION  = -12,
    NOSTR_BUD02_ERR_PUBKEY_MISMATCH = -13,
    NOSTR_BUD02_ERR_MISSING_EXPIRATION = -14,
} nostr_bud02_result_t;

/**
 * nostr_bud02_validate_options_t:
 * Optional validation parameters.
 */
typedef struct {
    /** Expected SHA-256 blob hash (hex, 64 chars), or NULL to skip check */
    const char *expected_sha256;
    /** Expected pubkey (hex, 64 chars), or NULL to skip check */
    const char *expected_pubkey;
    /** If nonzero, override "now" for expiration check (for testing) */
    int64_t     now_override;
} nostr_bud02_validate_options_t;

/* =========================================================================
 * Event creation
 * ========================================================================= */

/**
 * nostr_bud02_create_auth_event:
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
NOSTR_BUD02_API NostrEvent *nostr_bud02_create_auth_event(nostr_bud02_action_t action,
                                           const char *sha256_hex,
                                           int64_t expiration,
                                           const char *server_url);

/**
 * nostr_bud02_create_batch_auth_event:
 * @action: the BUD-02 action (typically upload)
 * @sha256_hex_array: array of SHA-256 hashes (each 64-char hex), one per blob
 * @count: number of hashes in @sha256_hex_array; MUST be > 0
 * @expiration: expiration unix timestamp (0 = default: now + 120s;
 *              capped at now + 900s to bound token lifetime).
 * @server_url: (nullable): optional "server" tag. By default (NULL) this
 *              tag is OMITTED — some Blossom servers (e.g. blossom.band)
 *              reject any server-tag variant. Callers that know the
 *              server tolerates the tag (from a capability probe) may
 *              pass a URL; the default MUST remain "omit".
 *
 * Create an unsigned kind 24242 event with ONE ["t", <action>] tag,
 * ONE ["expiration", <ts>] tag, and N ["x", <hash_i>] tags — one per
 * blob in the batch. This single signed event is used as the shared
 * `Authorization:` header across all N PUTs in a batch upload.
 *
 * The batch semantics are BUD-02-compatible: servers whose auth layer
 * checks "sha256(request-body) is IN the event's x-tag list" (as
 * observed on blossom.sharegap.net, blossom.band without the server tag,
 * and blossom.primal.net; docs/reviews/porthome-blossom-batch-auth-2026-09-24.md)
 * accept this shape. Callers MUST be prepared for a per-server fallback
 * to one-x-per-event on a 401 (session-scoped negative cache lives in
 * the client's capability cache).
 *
 * Caller must sign with nostr_event_sign() before use (or pass the
 * event to the higher-level batch upload API which handles signing).
 *
 * Returns: newly allocated NostrEvent, or NULL on error (bad action,
 *          NULL/empty array, allocation failure). Caller must free with
 *          nostr_event_free().
 */
NOSTR_BUD02_API NostrEvent *nostr_bud02_create_batch_auth_event(nostr_bud02_action_t action,
                                                 const char *const *sha256_hex_array,
                                                 size_t count,
                                                 int64_t expiration,
                                                 const char *server_url);

/* =========================================================================
 * Header creation/parsing (Nostr base64 scheme)
 * ========================================================================= */

/**
 * nostr_bud02_create_auth_header:
 * @event: signed kind 24242 event
 *
 * Serialize the event to JSON, base64-encode, and return "Nostr <base64>".
 *
 * Returns: newly allocated header string, or NULL on error. Caller frees.
 */
NOSTR_BUD02_API char *nostr_bud02_create_auth_header(const NostrEvent *event);

/**
 * nostr_bud02_parse_auth_header:
 * @header: "Nostr <base64>" header value
 * @out_event: (out): parsed event on success
 *
 * Returns: NOSTR_BUD02_OK on success
 */
NOSTR_BUD02_API nostr_bud02_result_t nostr_bud02_parse_auth_header(const char *header,
                                                     NostrEvent **out_event);

/* =========================================================================
 * Validation
 * ========================================================================= */

/**
 * nostr_bud02_validate_auth_event:
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
 * Returns: NOSTR_BUD02_OK on success, specific error code otherwise
 */
NOSTR_BUD02_API nostr_bud02_result_t nostr_bud02_validate_auth_event(
    const NostrEvent *event,
    nostr_bud02_action_t expected_action,
    const nostr_bud02_validate_options_t *options);

/* =========================================================================
 * Accessors
 * ========================================================================= */

/** Get the "t" (action) tag value, or NULL if missing */
NOSTR_BUD02_API const char *nostr_bud02_get_action(const NostrEvent *event);

/** Get the "x" (SHA-256 hash) tag value, or NULL if missing */
NOSTR_BUD02_API const char *nostr_bud02_get_hash(const NostrEvent *event);

/** Get the "expiration" tag value as int64, or 0 if missing */
NOSTR_BUD02_API int64_t nostr_bud02_get_expiration(const NostrEvent *event);

/** Get the "server" tag value, or NULL if missing */
NOSTR_BUD02_API const char *nostr_bud02_get_server(const NostrEvent *event);

/* =========================================================================
 * Helpers
 * ========================================================================= */

/**
 * nostr_bud02_action_str:
 * @action: action enum value
 *
 * Returns: string name ("upload", "delete", etc.) or NULL for invalid
 */
NOSTR_BUD02_API const char *nostr_bud02_action_str(nostr_bud02_action_t action);

/**
 * nostr_bud02_action_from_str:
 * @str: action string
 * @out: (out): action enum on success
 *
 * Returns: NOSTR_BUD02_OK on success, NOSTR_BUD02_ERR_INVALID_ACTION otherwise
 */
NOSTR_BUD02_API nostr_bud02_result_t nostr_bud02_action_from_str(const char *str,
                                                    nostr_bud02_action_t *out);

/**
 * nostr_bud02_strerror:
 * @result: result code
 *
 * Returns: human-readable error string (static, do not free)
 */
NOSTR_BUD02_API const char *nostr_bud02_strerror(nostr_bud02_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_BUD02_AUTH_H */
