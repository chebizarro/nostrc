/*
 * hanami-blossom-client.h - Blossom HTTP client (BUD-01/BUD-02)
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin wrapper around libcurl implementing all Blossom server endpoints.
 * Authenticated requests use kind 24242 Nostr events via the BUD-02 adapter.
 *
 * @see https://github.com/hzrd149/blossom/blob/master/buds/01.md
 * @see https://github.com/hzrd149/blossom/blob/master/buds/02.md
 */

#ifndef HANAMI_BLOSSOM_CLIENT_H
#define HANAMI_BLOSSOM_CLIENT_H

#include "hanami-types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque client handle */
typedef struct hanami_blossom_client hanami_blossom_client_t;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/**
 * hanami_blossom_client_opts_t:
 * Options for creating a Blossom client.
 */
typedef struct {
    const char *endpoint;       /**< Base URL, e.g. "https://blossom.example.com" */
    long timeout_seconds;       /**< Request timeout (default: 30) */
    const char *user_agent;     /**< User-Agent string (default: "libhanami/0.1") */
} hanami_blossom_client_opts_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * hanami_blossom_client_new:
 * @opts: client options (endpoint required, other fields optional/zeroed)
 * @signer: (nullable): signer for authenticated requests (upload/delete)
 * @out: (out): created client handle
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_blossom_client_new(const hanami_blossom_client_opts_t *opts,
                                         const hanami_signer_t *signer,
                                         hanami_blossom_client_t **out);

/**
 * hanami_blossom_client_free:
 * @client: (transfer full) (nullable): client to free
 */
void hanami_blossom_client_free(hanami_blossom_client_t *client);

/* =========================================================================
 * BUD-01: GET / HEAD (unauthenticated reads)
 * ========================================================================= */

/**
 * hanami_blossom_get:
 * @client: client handle
 * @sha256_hex: SHA-256 hash of the blob (64-char hex)
 * @out_data: (out): blob data (caller must free)
 * @out_len: (out): blob length
 *
 * GET /<sha256> — retrieve a blob by its content hash.
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_NOT_FOUND for 404
 */
hanami_error_t hanami_blossom_get(hanami_blossom_client_t *client,
                                  const char *sha256_hex,
                                  uint8_t **out_data,
                                  size_t *out_len);

/**
 * hanami_blossom_head:
 * @client: client handle
 * @sha256_hex: SHA-256 hash of the blob
 * @out_exists: (out): true if the blob exists on the server
 *
 * HEAD /<sha256> — check if a blob exists without downloading.
 *
 * Returns: HANAMI_OK on success (check out_exists for result)
 */
hanami_error_t hanami_blossom_head(hanami_blossom_client_t *client,
                                   const char *sha256_hex,
                                   bool *out_exists);

/* =========================================================================
 * BUD-02: PUT / DELETE (authenticated writes)
 * ========================================================================= */

/**
 * hanami_blossom_upload:
 * @client: client handle (signer required)
 * @data: blob data
 * @len: blob length
 * @sha256_hex: (nullable): pre-computed SHA-256 of the data, or NULL to
 *              compute automatically
 * @out_desc: (out) (nullable): blob descriptor returned by server (caller frees)
 *
 * PUT /upload — upload a blob with kind 24242 authorization.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_blossom_upload(hanami_blossom_client_t *client,
                                     const uint8_t *data,
                                     size_t len,
                                     const char *sha256_hex,
                                     hanami_blob_descriptor_t **out_desc);

/**
 * hanami_blossom_delete:
 * @client: client handle (signer required)
 * @sha256_hex: SHA-256 hash of the blob to delete
 *
 * DELETE /<sha256> — delete a blob with kind 24242 authorization.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_blossom_delete(hanami_blossom_client_t *client,
                                     const char *sha256_hex);

/* =========================================================================
 * BUD-01: LIST (unauthenticated)
 * ========================================================================= */

/**
 * hanami_blossom_list:
 * @client: client handle
 * @pubkey_hex: pubkey to list blobs for (64-char hex)
 * @out_json: (out): raw JSON response (caller frees). TODO: parse to descriptors.
 * @out_len: (out): response length
 *
 * GET /list/<pubkey> — list blobs uploaded by a pubkey.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_blossom_list(hanami_blossom_client_t *client,
                                   const char *pubkey_hex,
                                   char **out_json,
                                   size_t *out_len);

/* =========================================================================
 * BUD-04: MIRROR (authenticated)
 * ========================================================================= */

/**
 * hanami_blossom_mirror:
 * @client: client handle (signer required)
 * @source_url: URL of the blob to mirror
 * @out_desc: (out) (nullable): blob descriptor (caller frees)
 *
 * PUT /mirror — ask the server to fetch and store a blob from another URL.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_blossom_mirror(hanami_blossom_client_t *client,
                                     const char *source_url,
                                     hanami_blob_descriptor_t **out_desc);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_BLOSSOM_CLIENT_H */
