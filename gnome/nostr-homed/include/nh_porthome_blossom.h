/*
 * nh_porthome_blossom.h — thin content-addressed Blossom wrapper for Phase 1.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL. Do not deploy.
 *
 * A minimal wrapper around libhanami's hanami_blossom_client_* (BUD-01
 * GET/HEAD/LIST, BUD-02 PUT/DELETE, BUD-04 MIRROR — signed kind-24242
 * auth events). Adds:
 *
 *   - HTTPS-only endpoint acceptance (http:// / file:// / etc. refused)
 *   - size caps (both upload and download) enforced before hitting curl
 *   - retry + failover across a caller-supplied ordered server list
 *   - content-SHA256 verification of every fetch — a Blossom server that
 *     returns bytes whose hash disagrees with the requested address is
 *     treated as hostile and the next server is tried
 *
 * The wrapper carries no global state; the caller supplies the signer
 * (fits both a local-key signer and a NIP-46 signer without touching
 * this API).
 *
 * The wrapper does NOT hold homed keys, does not encrypt, does not
 * chunk. Sealing is the caller's job (nh_porthome_crypto.h). This layer
 * only speaks bytes-in / bytes-out addressed by SHA-256(bytes).
 *
 * SSRF: the design (§8.2) requires all Blossom fetches to be gated by
 * the same SSRF check the profile-image helper uses. The Phase-1
 * wrapper enforces the URL-syntax half of that gate here (HTTPS-only,
 * no user-info, no obvious loopback literal); the sockaddr-level gate
 * belongs in the unprivileged fetch helper (Phase 2).
 *
 * Bead: nostrc-h10m.
 */

#ifndef NH_PORTHOME_BLOSSOM_H
#define NH_PORTHOME_BLOSSOM_H

#include "nh_porthome_crypto.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* libhanami's signer type is an anonymous-struct typedef, so we cannot
 * forward-declare it without a name mismatch. Include the header. This
 * transitively pulls in <git2/types.h> — accepted: any caller of the
 * Phase-1 wrapper already has libhanami on its include path. */
#include <hanami/hanami-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reasonable defaults if the caller passes 0. */
#define NH_PORTHOME_BLOSSOM_DEFAULT_TIMEOUT_S      30
#define NH_PORTHOME_BLOSSOM_DEFAULT_MAX_RETRIES     3
#define NH_PORTHOME_BLOSSOM_DEFAULT_MAX_BLOB_BYTES (16u * 1024u * 1024u) /* 16 MiB */

typedef struct nh_porthome_blossom nh_porthome_blossom_t;

typedef struct {
    /* Ordered list of Blossom base URLs. Each MUST be https://; each
     * MUST end WITHOUT a trailing slash (libhanami convention). */
    const char *const *servers;
    size_t             n_servers;
    /* Per-request HTTP timeout. 0 -> default. */
    long   timeout_seconds;
    /* Maximum retries per (blob, server) before failing over to the next
     * server. 0 -> default. Retries use exponential backoff 1s/2s/4s. */
    int    max_retries;
    /* Hard byte cap on any single blob (upload or download). 0 -> default. */
    size_t max_blob_bytes;
} nh_porthome_blossom_opts_t;

/* Error codes. Positive values only appear as `bool` outputs; failures
 * return < 0 as nh_porthome_status extended below. */
typedef enum {
    NH_PORTHOME_BLOSSOM_OK              =  0,
    NH_PORTHOME_BLOSSOM_ERR_ARG         = -101,
    NH_PORTHOME_BLOSSOM_ERR_HTTPS_ONLY  = -102, /* an endpoint was not https:// */
    NH_PORTHOME_BLOSSOM_ERR_TOO_LARGE   = -103, /* size cap exceeded */
    NH_PORTHOME_BLOSSOM_ERR_HASH_MISMATCH = -104, /* fetched bytes did not match requested SHA-256 */
    NH_PORTHOME_BLOSSOM_ERR_NOT_FOUND   = -105,
    NH_PORTHOME_BLOSSOM_ERR_NETWORK     = -106, /* all servers exhausted */
    NH_PORTHOME_BLOSSOM_ERR_AUTH        = -107,
    NH_PORTHOME_BLOSSOM_ERR_OOM         = -108,
} nh_porthome_blossom_status;

/* Static URL sanity: returns 0 iff `url` is a syntactically-plausible
 * https://host[:port][/base] with no user-info, no fragment, no
 * whitespace, and no obviously-loopback literal (`https://127.` /
 * `https://[::1]` / `https://localhost`). Sockaddr-level SSRF must be
 * enforced by the unprivileged fetch helper — this is a cheap
 * pre-filter meant to catch typos and misconfiguration, not to defend
 * against DNS rebinding. */
int nh_porthome_blossom_url_ok(const char *url);

int nh_porthome_blossom_new(const nh_porthome_blossom_opts_t *opts,
                            const hanami_signer_t *signer,
                            nh_porthome_blossom_t **out);
void nh_porthome_blossom_free(nh_porthome_blossom_t *c);

/* Upload `data` (length `len`) to at least one server. Computes
 * SHA-256(data), verifies it against `expected_sha256_hex` if
 * non-NULL, then tries each configured server in order.
 * Writes the 64-hex-char + NUL address to `out_sha256_hex` on success.
 * A single-server success returns 0. */
int nh_porthome_blossom_upload(nh_porthome_blossom_t *c,
                               const uint8_t *data, size_t len,
                               const char *expected_sha256_hex,
                               char out_sha256_hex[65]);

/* Fetch by sha256; verifies content-hash before returning. On success,
 * caller frees *out_data with free(). */
int nh_porthome_blossom_fetch(nh_porthome_blossom_t *c,
                              const char *sha256_hex,
                              uint8_t **out_data, size_t *out_len);

/* HEAD across the server set; returns 0 with *out_exists=true iff at
 * least one server confirms the blob's presence. */
int nh_porthome_blossom_has(nh_porthome_blossom_t *c,
                            const char *sha256_hex,
                            bool *out_exists);

/* DELETE across every server; best-effort. Returns 0 iff every server
 * either returned OK or 404 (idempotent). */
int nh_porthome_blossom_delete(nh_porthome_blossom_t *c,
                               const char *sha256_hex);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_BLOSSOM_H */
