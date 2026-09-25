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
#include "hanami-server-capability.h"
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
 * @out_json: (out): raw JSON response (caller frees)
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

/* =========================================================================
 * Batch upload (nostrc-xeby)
 * ========================================================================= */

/**
 * hanami_blossom_blob_t:
 * One blob in a batch upload. All fields are borrowed — the caller owns
 * the storage and must keep it valid for the duration of the call.
 */
typedef struct {
    const char    *sha256_hex; /**< 64-char lowercase hex, NUL-terminated */
    const uint8_t *bytes;      /**< body bytes */
    size_t         len;        /**< body length */
} hanami_blossom_blob_t;

/**
 * hanami_blossom_batch_result_t:
 * Outcome of one PUT in a batch.
 */
typedef struct {
    long   http_status;                 /**< HTTP status of the PUT (0 on transport error) */
    hanami_error_t error;               /**< HANAMI_OK on success, else per-blob failure */
    /** Constant string describing the failure class for diagnostics:
     *   - NULL             on success
     *   - "batch-fell-back" if the batch header failed with 401 and we
     *                       retried this blob with a per-blob header
     *   - "network"        on transport error
     *   - "auth"           on 401/403 even after per-blob fallback
     *   - "http-4xx"       on other 4xx
     *   - "http-5xx"       on 5xx
     *   The pointer is static — callers must NOT free. */
    const char *error_class;
} hanami_blossom_batch_result_t;

/**
 * hanami_blossom_upload_batch:
 * @client: client handle (signer required)
 * @blobs: array of @count blobs
 * @count: number of blobs; MUST be > 0
 * @out_results: (out): array of length @count; caller sizes; filled in
 *                order corresponding to @blobs
 *
 * PUT each blob to `${endpoint}/${sha256_hex}` with a SHARED
 * `Authorization: Nostr <b64>` header derived from a single kind-24242
 * event listing all N x-tags. This amortises one NIP-46 sign_event
 * round-trip across the whole batch.
 *
 * Server-capability policy (session-scoped cache lives on the client):
 *   1. If the server has been observed to reject batch (batch_ok=NO,
 *      typically from a prior 401 in this session), the call transparently
 *      uses per-blob auth for every blob — no batch header is minted.
 *   2. If the server is UNKNOWN, tries the batch header first. On the
 *      first 401 during PUT, marks batch_ok=NO for the session and falls
 *      back to per-blob auth for the REMAINDER of this call. Results
 *      for blobs that recovered under per-blob auth carry
 *      error_class="batch-fell-back".
 *
 * A pre-connect probe (hanami_server_probe_capabilities) may be invoked
 * automatically on the first batch call for a (client, endpoint) unless
 * the environment variable NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1 is
 * set — in that case the "try batch, fall back on 401" heuristic applies.
 *
 * Returns: HANAMI_OK iff every blob has result.error == HANAMI_OK;
 *          HANAMI_ERR_INVALID_ARG if arguments are bogus;
 *          HANAMI_ERR_AUTH if there is no signer;
 *          otherwise the error of the LAST failed blob (per-blob outcomes
 *          are always in out_results).
 */
hanami_error_t hanami_blossom_upload_batch(hanami_blossom_client_t *client,
                                           const hanami_blossom_blob_t *blobs,
                                           size_t count,
                                           hanami_blossom_batch_result_t *out_results);

/**
 * hanami_blossom_get_capabilities:
 * @client: client handle
 *
 * Returns: pointer to the client's per-endpoint capability record
 *          (borrowed; owned by the client). Never NULL for a valid client.
 *          The record is mutable only through the batch upload / probe
 *          APIs; callers may read fields directly but MUST NOT mutate.
 */
const hanami_server_capabilities_t *
hanami_blossom_get_capabilities(hanami_blossom_client_t *client);

/* =========================================================================
 * Per-server capability probe (nostrc-ypn2)
 * ========================================================================= */

/**
 * hanami_server_probe_capabilities:
 * @client: client handle (signer required)
 *
 * Runs the D8-derived capability probe against the client's endpoint:
 *
 *   (a) Reachability: BUD-01 HEAD on a random 64-hex hash (4xx is fine —
 *       it means the server answered).
 *   (b) server-tag tolerance: upload a 1 KiB session-random blob with a
 *       kind-24242 event that carries a "server" tag AND one x-tag. 2xx
 *       -> server_tag_ok=YES; auth-related 4xx -> NO (with a retry
 *       without the server tag to distinguish from other failures).
 *   (c) batch-viability: upload two 1 KiB session-random blobs sharing a
 *       kind-24242 event with x=[h1, h2]. Both 2xx -> batch_ok=YES;
 *       any 401 -> NO; other 4xx (e.g. 415 content-policy) -> UNKNOWN.
 *   (d) strict x-binding: upload blob H2 under an event with x=[H1] only.
 *       401 -> strict_x_binding=YES; 2xx or non-auth 4xx -> NO.
 *
 * The probe uses an EPHEMERAL secp256k1 keypair (unique per call) rather
 * than the client's real signer, so production blobs cannot be confused
 * with probe traffic. All probe blobs are 1 KiB of session-random bytes.
 * The probe is idempotent modulo blob storage — cleanup is best-effort.
 *
 * The result is stored in the client's capability cache and can be read
 * with hanami_blossom_get_capabilities().
 *
 * Total budget: at most 4 uploads + optional deletes per call. Sequential.
 *
 * Returns: HANAMI_OK if the probe RAN (regardless of the flag values);
 *          HANAMI_ERR_NETWORK if reachability failed;
 *          HANAMI_ERR_AUTH if no signer helper is available for the probe.
 *
 * Note: this call bypasses the client's signer — it does not require the
 * caller-supplied NIP-46 bunker etc. The probe signs its own events with
 * a throwaway ephemeral key held only in process memory.
 */
hanami_error_t hanami_server_probe_capabilities(hanami_blossom_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_BLOSSOM_CLIENT_H */
