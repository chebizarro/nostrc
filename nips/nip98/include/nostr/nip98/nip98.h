/**
 * @file nip98.h
 * @brief NIP-98: HTTP Auth - Nostr event-based HTTP authentication
 *
 * This NIP defines an ephemeral event used to authorize requests to HTTP
 * servers using Nostr events. A kind 27235 event is used with tags for
 * URL and HTTP method, optionally including a payload hash for request bodies.
 *
 * @see https://github.com/nostr-protocol/nips/blob/master/98.md
 */

#ifndef NOSTR_NIP98_H
#define NOSTR_NIP98_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct _NostrEvent;
typedef struct _NostrEvent NostrEvent;

/* NIP-98 HTTP Auth Event Kind */
#define NOSTR_NIP98_KIND 27235

/* Default time window for validation (seconds) */
#define NOSTR_NIP98_DEFAULT_TIME_WINDOW 60

/**
 * @brief Result codes for NIP-98 operations
 */
typedef enum {
    NOSTR_NIP98_OK = 0,
    NOSTR_NIP98_ERR_NULL_PARAM = -1,
    NOSTR_NIP98_ERR_ALLOC = -2,
    NOSTR_NIP98_ERR_INVALID_KIND = -3,
    NOSTR_NIP98_ERR_TIMESTAMP_EXPIRED = -4,
    NOSTR_NIP98_ERR_URL_MISMATCH = -5,
    NOSTR_NIP98_ERR_METHOD_MISMATCH = -6,
    NOSTR_NIP98_ERR_PAYLOAD_MISMATCH = -7,
    NOSTR_NIP98_ERR_SIGNATURE_INVALID = -8,
    NOSTR_NIP98_ERR_MISSING_TAG = -9,
    NOSTR_NIP98_ERR_ENCODE = -10,
    NOSTR_NIP98_ERR_DECODE = -11,
    NOSTR_NIP98_ERR_INVALID_HEADER = -12
} NostrNip98Result;

/**
 * @brief Validation options for NIP-98 auth events
 */
typedef struct {
    /** Time window in seconds for timestamp validation (default: 60) */
    int time_window_seconds;
    /** Expected payload SHA256 hash (hex, 64 chars), or NULL to skip check */
    const char *expected_payload_hash;
} NostrNip98ValidateOptions;

/**
 * @brief Create a NIP-98 HTTP auth event (kind 27235)
 *
 * Creates an unsigned event with the required tags for HTTP authentication.
 * The event must be signed before use with nostr_event_sign().
 *
 * @param url Absolute URL being accessed (required)
 * @param method HTTP method (GET, POST, PUT, DELETE, etc.) (required)
 * @param payload_sha256_hex Optional SHA256 hash of request body as hex string (64 chars), or NULL
 * @return Newly allocated NostrEvent, or NULL on error. Caller must free with nostr_event_free().
 *
 * @example
 * NostrEvent *auth = nostr_nip98_create_auth_event(
 *     "https://example.com/upload",
 *     "PUT",
 *     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
 * );
 * nostr_event_sign(auth, private_key);
 */
NostrEvent *nostr_nip98_create_auth_event(const char *url,
                                          const char *method,
                                          const char *payload_sha256_hex);

/**
 * @brief Create Authorization header value from a signed NIP-98 event
 *
 * Serializes the event to JSON, base64 encodes it, and prepends "Nostr ".
 * The returned string is suitable for use as an HTTP Authorization header value.
 *
 * @param event Signed NIP-98 event (required)
 * @return Newly allocated string "Nostr <base64>", or NULL on error. Caller must free().
 *
 * @example
 * char *header = nostr_nip98_create_auth_header(auth);
 * // header = "Nostr eyJpZCI6Ii..."
 * curl_easy_setopt(curl, CURLOPT_HTTPHEADER, "Authorization: <header>");
 * free(header);
 */
char *nostr_nip98_create_auth_header(const NostrEvent *event);

/**
 * @brief Parse Authorization header and extract NIP-98 event
 *
 * Parses an HTTP Authorization header value with "Nostr" scheme,
 * decodes the base64 event, and returns the deserialized event.
 *
 * @param header Authorization header value (e.g., "Nostr eyJ...")
 * @param out_event Output pointer for the parsed event
 * @return NOSTR_NIP98_OK on success, error code otherwise
 */
NostrNip98Result nostr_nip98_parse_auth_header(const char *header,
                                                NostrEvent **out_event);

/**
 * @brief Validate a NIP-98 auth event
 *
 * Performs all required NIP-98 validation checks:
 * 1. Kind must be 27235
 * 2. Timestamp must be within time window (default 60 seconds)
 * 3. URL tag must match expected URL
 * 4. Method tag must match expected method
 * 5. Signature must be valid
 * 6. Optionally verify payload hash matches
 *
 * @param event NIP-98 event to validate
 * @param expected_url Expected absolute URL
 * @param expected_method Expected HTTP method
 * @param options Optional validation options (can be NULL for defaults)
 * @return NOSTR_NIP98_OK on success, specific error code otherwise
 *
 * @example
 * NostrNip98ValidateOptions opts = {
 *     .time_window_seconds = 120,
 *     .expected_payload_hash = body_sha256_hex
 * };
 * NostrNip98Result result = nostr_nip98_validate_auth_event(
 *     event, "https://example.com/upload", "PUT", &opts
 * );
 * if (result != NOSTR_NIP98_OK) {
 *     // Authentication failed
 * }
 */
NostrNip98Result nostr_nip98_validate_auth_event(const NostrEvent *event,
                                                  const char *expected_url,
                                                  const char *expected_method,
                                                  const NostrNip98ValidateOptions *options);

/**
 * @brief Get the URL from a NIP-98 auth event
 *
 * @param event NIP-98 event
 * @return URL string (internal pointer, do not free), or NULL if not found
 */
const char *nostr_nip98_get_url(const NostrEvent *event);

/**
 * @brief Get the HTTP method from a NIP-98 auth event
 *
 * @param event NIP-98 event
 * @return Method string (internal pointer, do not free), or NULL if not found
 */
const char *nostr_nip98_get_method(const NostrEvent *event);

/**
 * @brief Get the payload hash from a NIP-98 auth event
 *
 * @param event NIP-98 event
 * @return Payload hash hex string (internal pointer, do not free), or NULL if not present
 */
const char *nostr_nip98_get_payload_hash(const NostrEvent *event);

/**
 * @brief Get error message for a NIP-98 result code
 *
 * @param result Result code
 * @return Human-readable error message (static string, do not free)
 */
const char *nostr_nip98_strerror(NostrNip98Result result);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP98_H */
