/*
 * auth_porthome_fetch.h — broker-side spawner for the nostr-home-fetch
 * unprivileged helper. Bead nostrc-9k4g.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Compiled only when NH_AUTH_BROKER_ENABLE_PORTHOME is
 * defined.
 *
 * This module owns the fork+exec of the helper binary, feeds it the
 * JSON control payload on stdin, drains its progress lines on stdout,
 * and returns a terminal status mapped from the helper's exit code.
 *
 * Coordination with bead nostrc-ww50 (subprocess sandbox): the SPAWN
 * point in this module is the seam where ww50's fork+drop-privs wrapper
 * plugs in. The helper is a separate executable so ww50 can wrap the
 * spawn (setresuid/setresgid before execve, seccomp filter etc.)
 * without touching this file's caller. */

#ifndef NH_AUTH_PORTHOME_FETCH_H
#define NH_AUTH_PORTHOME_FETCH_H

#include <stddef.h>
#include <stdint.h>

#include "auth_porthome.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Terminal state after the helper reports. Map to a
 * nh_auth_porthome_job_state at the caller. */
typedef enum {
    NH_PORTHOME_FETCH_RES_OK           = 0,
    /* NETWORK_FAIL / DECODE_FAIL / SIZE_CAP / TIMEOUT — recoverable,
     * caller MUST route to LIMITED and never touch the existing home. */
    NH_PORTHOME_FETCH_RES_LIMITED      = 1,
    /* DECRYPT_FAIL / INTERNAL / helper crash — hard failure. */
    NH_PORTHOME_FETCH_RES_FAILED       = 2,
    /* Helper binary missing or NH_PORTHOME_FETCH_HELPER=off env. Caller
     * must treat this as LIMITED (silent fallback), NOT as failure. */
    NH_PORTHOME_FETCH_RES_UNAVAILABLE  = 3,
} nh_auth_porthome_fetch_result;

typedef struct nh_auth_porthome_fetch_args {
    /* All strings are borrowed; the caller owns their lifetimes. */
    const char *account_pubkey_hex;
    const char *home_root_id_hex;
    const char *home_key_hex;
    const char *d_tag;
    const char *const *relays;
    size_t             relays_count;
    const char *const *blossom_servers;
    size_t             blossom_servers_count;
    uint64_t bandwidth_cap_bytes;
    uint32_t per_file_timeout_sec;
    uint64_t max_total_bytes;
    uint32_t relay_timeout_ms;
    int      allow_insecure;
    /* Materialisation target: an openat-safe dirfd (preferred) OR a
     * filesystem path. Exactly ONE must be >=0 / non-NULL. */
    int         staging_fd;
    const char *staging_dir;
    /* Wall-clock cap for the whole helper run (milliseconds). If the
     * helper doesn't finish in this window it is SIGKILL'd. 0 → 300 s
     * default. */
    uint32_t total_timeout_ms;
} nh_auth_porthome_fetch_args;

typedef struct nh_auth_porthome_fetch_progress_cb {
    /* Called for each parsed progress line. `bytes`/`files` are
     * monotone within one run. `phase` is the enum from
     * porthome_fetch_ctl.h. Return non-zero to request cancellation. */
    int (*fn)(void *ctx, uint64_t bytes, uint64_t files, int phase);
    void *ctx;
} nh_auth_porthome_fetch_progress_cb;

/* Spawn the helper and drive it to completion. Returns one of
 * nh_auth_porthome_fetch_result. On OK the staging fd/dir contains the
 * materialised home tree; on LIMITED/FAILED the caller must NOT install
 * or touch the existing home.
 *
 * `exit_code_out` (optional) receives the helper's numeric exit code
 * for logging. */
nh_auth_porthome_fetch_result
nh_auth_porthome_fetch_spawn(const nh_auth_porthome_fetch_args *args,
                             const nh_auth_porthome_fetch_progress_cb *cb,
                             int *exit_code_out);

/* Resolve the helper binary path. Returns a static string on success or
 * NULL if not runnable. Precedence:
 *   1. environment NH_PORTHOME_FETCH_HELPER — full path
 *   2. /usr/libexec/nostr-homed/nostr-home-fetch (installed)
 *   3. build-time default from CMake (test seam)
 * The returned pointer is either into env or a static string. */
const char *nh_auth_porthome_fetch_helper_path(void);

/* Test seam: override the helper path for the current process. Passing
 * NULL restores the discovery precedence above. */
void nh_auth_porthome_fetch_set_helper_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_PORTHOME_FETCH_H */
