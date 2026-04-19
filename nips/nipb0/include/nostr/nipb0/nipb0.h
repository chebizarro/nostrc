#ifndef NIPS_NIPB0_NOSTR_NIPB0_NIPB0_H
#define NIPS_NIPB0_NOSTR_NIPB0_NIPB0_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-B0 / Blossom: Blob Storage on Media Servers
 *
 * Protocol layer for the Blossom blob storage protocol.
 * Provides URL building, auth event creation (kind 24242),
 * and blob descriptor JSON parsing.
 *
 * HTTP transport is NOT included — callers bring their own
 * HTTP client (libcurl, libsoup, etc.) and use this library
 * to build requests and parse responses.
 *
 * Auth workflow:
 *   1. Create auth event: nostr_blossom_create_auth()
 *   2. Sign it:           nostr_event_sign(ev, privkey)
 *   3. Build header:      nostr_nip98_create_auth_header(ev)
 *   4. Set HTTP header:   Authorization: <header>
 */

/** Blossom auth event kind */
#define NOSTR_BLOSSOM_AUTH_KIND 24242

/** Default auth expiration in seconds */
#define NOSTR_BLOSSOM_DEFAULT_EXPIRATION 60

/**
 * Blossom operation types for auth events.
 */
typedef enum {
    NOSTR_BLOSSOM_OP_UPLOAD,   /**< PUT /upload */
    NOSTR_BLOSSOM_OP_GET,      /**< GET /<hash> */
    NOSTR_BLOSSOM_OP_LIST,     /**< GET /list/<pubkey> */
    NOSTR_BLOSSOM_OP_DELETE,   /**< DELETE /<hash> */
} NostrBlossomOp;

/**
 * Blob descriptor — parsed from server JSON responses.
 * All string pointers are heap-allocated. Free with
 * nostr_blossom_descriptor_free().
 */
typedef struct {
    char *url;          /**< Blob URL */
    char *sha256;       /**< SHA-256 hex hash */
    int64_t size;       /**< Size in bytes */
    char *content_type; /**< MIME type (nullable) */
    int64_t uploaded;   /**< Upload timestamp (unix) */
} NostrBlossomBlobDescriptor;

/* ---- URL building ---- */

/**
 * Build upload URL: <server>/upload
 * Caller must free().
 */
char *nostr_blossom_url_upload(const char *server);

/**
 * Build download URL: <server>/<hash>
 * Caller must free().
 */
char *nostr_blossom_url_download(const char *server, const char *hash);

/**
 * Build check (HEAD) URL: <server>/<hash>
 * Same as download URL. Caller must free().
 */
char *nostr_blossom_url_check(const char *server, const char *hash);

/**
 * Build list URL: <server>/list/<pubkey_hex>
 * Caller must free().
 */
char *nostr_blossom_url_list(const char *server, const char *pubkey_hex);

/**
 * Build delete URL: <server>/<hash>
 * Same as download URL. Caller must free().
 */
char *nostr_blossom_url_delete(const char *server, const char *hash);

/**
 * Build mirror URL: <server>/mirror
 * Caller must free().
 */
char *nostr_blossom_url_mirror(const char *server);

/* ---- Auth event creation ---- */

/**
 * nostr_blossom_create_auth:
 * @op: Operation type (upload, get, list, delete)
 * @hash: SHA-256 hex hash for the blob (NULL for list)
 * @expiration_secs: Seconds until expiration (0 = default 60s)
 *
 * Creates an unsigned kind 24242 auth event with appropriate
 * "t" (operation), "x" (hash), and "expiration" tags.
 *
 * Caller must sign the event, then use nostr_nip98_create_auth_header()
 * to build the HTTP Authorization header value.
 *
 * Returns: (transfer full) (nullable): new event, or NULL on error.
 *          Caller must free with nostr_event_free().
 */
NostrEvent *nostr_blossom_create_auth(NostrBlossomOp op,
                                       const char *hash,
                                       int expiration_secs);

/* ---- Blob descriptor parsing ---- */

/**
 * Parse a single blob descriptor from JSON.
 * Caller must free with nostr_blossom_descriptor_free().
 *
 * Returns: 0 on success, -EINVAL on bad input
 */
int nostr_blossom_parse_descriptor(const char *json,
                                    NostrBlossomBlobDescriptor *out);

/**
 * Parse a JSON array of blob descriptors (from /list response).
 * Fills up to max_entries elements. Sets *out_count to actual count.
 * Caller must free each entry with nostr_blossom_descriptor_free().
 *
 * Returns: 0 on success, -EINVAL on bad input
 */
int nostr_blossom_parse_descriptor_list(const char *json,
                                         NostrBlossomBlobDescriptor *out,
                                         size_t max_entries,
                                         size_t *out_count);

/**
 * Free heap-allocated strings in a blob descriptor.
 * Does NOT free the struct itself.
 */
void nostr_blossom_descriptor_free(NostrBlossomBlobDescriptor *desc);

/* ---- Utilities ---- */

/**
 * Build mirror request body JSON: {"url":"<remote_url>"}
 * Caller must free().
 */
char *nostr_blossom_mirror_body(const char *remote_url);

/**
 * Extract hash from a blob URL.
 * Takes the last path segment and strips any file extension.
 * e.g. "https://cdn.example.com/abc123.png" → "abc123"
 * Caller must free().
 */
char *nostr_blossom_extract_hash(const char *url);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIPB0_NOSTR_NIPB0_NIPB0_H */
