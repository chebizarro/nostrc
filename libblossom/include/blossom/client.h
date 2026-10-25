/* Blossom BUD-01/02/04 synchronous libcurl client. SPDX-License-Identifier: MIT */
#ifndef BLOSSOM_CLIENT_H
#define BLOSSOM_CLIENT_H
#include <blossom/types.h>
#include <blossom/capability.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BLOSSOM_UPLOAD_CONTENT_TYPE "application/octet-stream"
#define BLOSSOM_UPLOAD_CT_ENV "NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE"
#define BLOSSOM_DEFAULT_MAX_BLOB_BYTES (16u * 1024u * 1024u)
typedef struct blossom_client blossom_client_t;
typedef struct {
    const char *endpoint; /* HTTPS base URL; no credentials, query or fragment. */
    long timeout_seconds; /* Zero selects 30 seconds. */
    const char *user_agent;
    size_t max_blob_bytes; /* Zero selects 16 MiB; applied during reception. */
    /* Explicit local-test transport only: HTTP and loopback/private sockets.
     * Never enable for untrusted endpoints. Redirects remain disabled. */
    bool allow_insecure;
} blossom_client_opts_t;
typedef struct {
    const char *sha256_hex; /* Expected wire hash; NULL computes it. */
    const uint8_t *bytes;
    size_t len;
} blossom_blob_t;
typedef struct {
    long http_status;
    blossom_error_t error;
    const char *error_class; /* Static string; do not free. */
} blossom_batch_result_t;
/* Handles are single-thread-owned. Signer pointers remain borrowed. */
BLOSSOM_API blossom_error_t blossom_client_new(const blossom_client_opts_t *, const blossom_signer_t *, blossom_client_t **);
BLOSSOM_API void blossom_client_free(blossom_client_t *);
/* GET returns hash-verified WIRE bytes, including any PNG prefix. Use the
 * pure blossom_shim_unwrap after verification when decoding sealed content. */
BLOSSOM_API blossom_error_t blossom_client_get(blossom_client_t *, const char *, uint8_t **, size_t *);
/* Content view for consumers such as Git ODBs; verifies wire hash before unwrapping. */
BLOSSOM_API blossom_error_t blossom_client_get_content(blossom_client_t *, const char *, uint8_t **, size_t *);
/* Compute the probe-selected wire address before indexing/HEAD/upload. */
BLOSSOM_API blossom_error_t blossom_client_address(blossom_client_t *, const uint8_t *, size_t, char out[65]);
BLOSSOM_API blossom_error_t blossom_client_head(blossom_client_t *, const char *, bool *);
/* PUT probes first, wraps if raw=NO and png=YES, then hashes/signs the wire
 * representation. expected_hash, when non-NULL, must match that representation.
 * The returned descriptor's hash/size are the verified locally sent bytes. */
BLOSSOM_API blossom_error_t blossom_client_put(blossom_client_t *, const uint8_t *, size_t, const char *, blossom_blob_descriptor_t **);
/* Shared multi-x auth only after a positive probe; 401 falls back to single-x. */
BLOSSOM_API blossom_error_t blossom_client_put_batch(blossom_client_t *, const blossom_blob_t *, size_t, blossom_batch_result_t *);
BLOSSOM_API blossom_error_t blossom_client_mirror(blossom_client_t *, const char *, blossom_blob_descriptor_t **);
BLOSSOM_API blossom_error_t blossom_client_list(blossom_client_t *, const char *, char **, size_t *);
BLOSSOM_API blossom_error_t blossom_client_delete(blossom_client_t *, const char *);
BLOSSOM_API const blossom_server_capabilities_t *blossom_client_get_capabilities(blossom_client_t *);
BLOSSOM_API blossom_error_t blossom_client_set_upload_content_type(blossom_client_t *, const char *);
/* Cached for 24h per normalized endpoint. Server-info is read without the
 * caller's identity; empirical upload probes use a fresh ephemeral identity.
 * Private servers may refuse that identity: upload flags then stay UNKNOWN. */
BLOSSOM_API blossom_error_t blossom_probe_capabilities(blossom_client_t *);
BLOSSOM_API blossom_error_t blossom_hash_blossom(const uint8_t *, size_t, char out_hex[65]);
/* Syntax gate; actual destination sockaddr is checked again at connection. */
BLOSSOM_API blossom_error_t blossom_url_check(const char *url, bool allow_insecure);
#ifdef __cplusplus
}
#endif
#endif
