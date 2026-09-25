/*
 * hanami-server-capability.h - Session-scoped per-server Blossom capability cache
 *
 * SPDX-License-Identifier: MIT
 *
 * Tracks per-server capability flags that let hanami_blossom_upload_batch()
 * (and future callers) make correct per-server choices without probing on
 * every upload:
 *
 *   - batch_ok        — a shared kind-24242 auth event with N x-tags is
 *                       accepted for N sequential PUTs (definitive: any 401
 *                       during a batch flips this to NO for the session)
 *   - server_tag_ok   — the server accepts a kind-24242 event with a
 *                       ["server", <url>] tag. blossom.band, notably,
 *                       rejects every URL form we tried (yo44 report).
 *   - strict_x_binding — the server enforces "PUT hash MUST be listed in
 *                       the auth event's x-tags" (401 on mismatch). Some
 *                       servers are permissive.
 *
 * Storage is per hanami_blossom_client_t: in-memory, session-scoped, never
 * persisted. Cache entries are keyed by exact endpoint URL (as passed to
 * the client at construction) — one entry per client.
 *
 * Follow-ups:
 *   - nostrc-xeby (this module + batch upload)
 *   - nostrc-ypn2 (probe wiring)
 *
 * Evidence: docs/reviews/porthome-blossom-batch-auth-2026-09-24.md.
 */

#ifndef HANAMI_SERVER_CAPABILITY_H
#define HANAMI_SERVER_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * hanami_capability_state_t:
 * Tri-state flag. UNKNOWN means "no observation yet — default to trying".
 */
typedef enum {
    HANAMI_CAP_UNKNOWN = 0,
    HANAMI_CAP_YES     = 1,
    HANAMI_CAP_NO      = 2,
} hanami_capability_state_t;

/**
 * Maximum length of a Content-Type string (including trailing NUL).
 * Long enough for e.g. "application/vnd.blossom.v1+octet-stream" plus
 * a handful of parameters; a value longer than this is rejected.
 */
#define HANAMI_PREFERRED_CT_MAX 64

/**
 * hanami_server_capabilities_t:
 * Per-server capability record. All fields default to UNKNOWN / 0 until
 * a probe or observed 401 flips them.
 *
 * preferred_content_type: session-scoped per-server override of the
 * upload Content-Type header (nostrc-bpum). Empty string ("") means
 * "use client default", which is itself either the env-var override
 * NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE or the compile-time default
 * HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE (application/octet-stream). Set
 * this when a probe or an observed 415 tells us a specific server
 * prefers a non-default Content-Type. Length is bounded by
 * HANAMI_PREFERRED_CT_MAX (including the trailing NUL).
 */
typedef struct {
    hanami_capability_state_t batch_ok;
    hanami_capability_state_t server_tag_ok;
    hanami_capability_state_t strict_x_binding;
    /**
     * raw_random_ok: YES if a plain random-bytes upload was accepted
     * (2xx) by this server, NO if it was rejected 4xx by a body-sniffer
     * (typically 415 "unsupported media type"). See nostrc-bpum +
     * docs/reviews/porthome-blossom-content-type-2026-09-25.md §2.
     */
    hanami_capability_state_t raw_random_ok;
    /**
     * png_shim_ok: YES if an upload wrapped in the deterministic
     * hanami-blossom-shim PNG prefix (see hanami-blossom-shim.h) was
     * accepted (2xx) by this server. Together with raw_random_ok this
     * tells the pusher whether it needs the shim on this endpoint.
     * Empirical validation against blossom.band / blossom.primal.net
     * is a follow-up item (nostrc-bpum probe extension) — the flag is
     * shipped UNKNOWN by default and flipped by an explicit probe.
     */
    hanami_capability_state_t png_shim_ok;
    /** Unix timestamp of last probe (0 = never probed). */
    int64_t last_probe_ts;
    /** Unix timestamp of last observed 401 (0 = none). */
    int64_t last_401_ts;
    /** True if reachability probe (BUD-01 HEAD/GET) succeeded. */
    bool reachable;
    /** Per-server upload Content-Type override; "" = use client default. */
    char preferred_content_type[HANAMI_PREFERRED_CT_MAX];
} hanami_server_capabilities_t;

/**
 * hanami_server_capabilities_init:
 * Initialise all fields to UNKNOWN / 0 / false.
 */
void hanami_server_capabilities_init(hanami_server_capabilities_t *caps);

/**
 * hanami_capability_state_str:
 * Human-readable state name for logging/diagnostics.
 */
const char *hanami_capability_state_str(hanami_capability_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_SERVER_CAPABILITY_H */
