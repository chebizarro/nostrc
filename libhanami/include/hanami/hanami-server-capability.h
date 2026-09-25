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
 * hanami_server_capabilities_t:
 * Per-server capability record. All fields default to UNKNOWN / 0 until
 * a probe or observed 401 flips them.
 */
typedef struct {
    hanami_capability_state_t batch_ok;
    hanami_capability_state_t server_tag_ok;
    hanami_capability_state_t strict_x_binding;
    /** Unix timestamp of last probe (0 = never probed). */
    int64_t last_probe_ts;
    /** Unix timestamp of last observed 401 (0 = none). */
    int64_t last_401_ts;
    /** True if reachability probe (BUD-01 HEAD/GET) succeeded. */
    bool reachable;
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
