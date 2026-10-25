/* Multi-server replication/failover over the one Blossom transport.
 * No keys, encryption, chunking, homed or git dependencies. SPDX-License-Identifier: MIT */
#ifndef BLOSSOM_POOL_H
#define BLOSSOM_POOL_H
#include <blossom/client.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BLOSSOM_POOL_DEFAULT_TIMEOUT_S 30
#define BLOSSOM_POOL_DEFAULT_MAX_RETRIES 3
#define BLOSSOM_POOL_DEFAULT_MAX_BLOB_BYTES BLOSSOM_DEFAULT_MAX_BLOB_BYTES
typedef struct blossom_pool blossom_pool_t;
typedef struct {
    const char *const *servers; /* HTTPS URLs, no trailing slash. Copied. */
    size_t n_servers;
    long timeout_seconds;
    int max_retries; /* Zero selects 3; exponential 1s/2s/4s backoff. */
    size_t max_blob_bytes; /* Zero selects 16 MiB. */
    bool allow_insecure; /* Explicit local-test transport only. */
} blossom_pool_opts_t;
typedef enum {
    BLOSSOM_POOL_OK = 0,
    BLOSSOM_POOL_ERR_ARG = -101,
    BLOSSOM_POOL_ERR_HTTPS_ONLY = -102,
    BLOSSOM_POOL_ERR_TOO_LARGE = -103,
    BLOSSOM_POOL_ERR_HASH_MISMATCH = -104,
    BLOSSOM_POOL_ERR_NOT_FOUND = -105,
    BLOSSOM_POOL_ERR_NETWORK = -106,
    BLOSSOM_POOL_ERR_AUTH = -107,
    BLOSSOM_POOL_ERR_OOM = -108
} blossom_pool_status;
typedef struct {
    const uint8_t *bytes; /* Borrowed; never redirected or mutated on return. */
    size_t len;
    const char *expected_sha256_hex; /* Optional expected WIRE address. */
    char sha256_hex[65]; /* Filled after uniform representation preparation. */
} blossom_pool_batch_blob_t;
typedef struct {
    const char *server_url; /* Borrowed from the pool. */
    size_t chunks_uploaded, bytes_uploaded, chunks_failed;
    int batch_fell_back;
} blossom_pool_batch_per_server_t;
BLOSSOM_API int blossom_pool_url_ok(const char *);
/* Signer and its callbacks remain borrowed for the handle's lifetime. */
BLOSSOM_API int blossom_pool_new(const blossom_pool_opts_t *, const blossom_signer_t *, blossom_pool_t **);
BLOSSOM_API void blossom_pool_free(blossom_pool_t *);
/* Probe all destinations to choose ONE wire representation, before hashing.
 * PUT to every destination; success requires at least one acceptance per blob.
 * Higher replication requirements belong to callers using per-server stats. */
BLOSSOM_API int blossom_pool_upload(blossom_pool_t *, const uint8_t *, size_t, const char *, char out[65]);
BLOSSOM_API int blossom_pool_upload_batch(blossom_pool_t *, blossom_pool_batch_blob_t *, size_t, blossom_pool_batch_per_server_t *);
/* GET returns bounded, hash-verified wire bytes; caller frees the buffer. */
BLOSSOM_API int blossom_pool_fetch(blossom_pool_t *, const char *, uint8_t **, size_t *);
BLOSSOM_API int blossom_pool_has(blossom_pool_t *, const char *, bool *);
/* Idempotent: all servers must acknowledge deletion or report 404. */
BLOSSOM_API int blossom_pool_delete(blossom_pool_t *, const char *);
#ifdef __cplusplus
}
#endif
#endif
