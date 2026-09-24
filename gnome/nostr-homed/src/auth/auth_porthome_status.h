/*
 * auth_porthome_status.h — broker-side "provisioner" key writer for the
 * unified porthome-status.json (Phase 5, bead nostrc-3o91).
 *
 * SPDX-License-Identifier: MIT
 *
 * The syncd (Phase 5 I3) and the FUSE overlay (h10m.1.1) contribute a
 * "syncd" and "fuse" key each to $XDG_STATE_HOME/nostr-homed/
 * porthome-status.json. This module is the missing "provisioner"
 * writer: PROVISION_HOME transitions in auth_porthome.c call these
 * setters and the emit function drops the current body under the
 * account's per-user status file (owned by acct.uid).
 *
 * Design notes:
 *  - The broker runs as root; per-user status lives under the account's
 *    home dir. We resolve the path via nh_identity_account.home (never
 *    getpwuid_r — that path recurses into libnss_nostr) and best-effort
 *    chown the resulting file to acct.uid so the user session's syncd
 *    / CLI can read it after login.
 *  - Body fields (v1, stable): state | last_state | last_provisioned_ts
 *    | last_error_class | chunks_pending | chunks_done. All hand-
 *    serialised through the porthome-common escape/write helper — NO
 *    jansson dep on the broker side.
 *  - Compiled only when NH_AUTH_BROKER_ENABLE_PORTHOME is defined.
 *    Otherwise every public function is a silent no-op so the runtime
 *    keeps its link closure headless-pure.
 *  - Also lazily owns a nh_porthome_notifier so PROVISION category
 *    lifecycle events can be posted through the shared throttle
 *    window. Root-side notify-send typically has no user DBus so the
 *    desktop pop is a no-op — but the throttle+record seam is still
 *    exercised, matching the syncd + fuse pattern.
 */

#ifndef NH_AUTH_PORTHOME_STATUS_H
#define NH_AUTH_PORTHOME_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provisioner state slugs, surfaced through status.provisioner.state. */
typedef enum {
    NH_PROV_ST_IDLE       = 0,
    NH_PROV_ST_PREPARING,
    NH_PROV_ST_FETCHING,
    NH_PROV_ST_VERIFYING,
    NH_PROV_ST_PUBLISHING,
    NH_PROV_ST_DONE,
    NH_PROV_ST_ERROR,
} nh_provisioner_state;

/* Opaque per-job status writer. Owns the current field snapshot; every
 * setter takes the internal mutex. Safe from multiple threads (broker
 * uses one thread today but this keeps a future concurrent broker
 * safe). NULL-safe on every setter. */
typedef struct nh_provisioner_status_writer nh_provisioner_status_writer;

nh_provisioner_status_writer *nh_provisioner_status_writer_new(void);
void nh_provisioner_status_writer_free(nh_provisioner_status_writer *w);

/* Field mutators. NULL writer is a no-op. */
void nh_provisioner_status_set_state(nh_provisioner_status_writer *w,
                                     nh_provisioner_state s);
void nh_provisioner_status_set_last_provisioned_ts(
    nh_provisioner_status_writer *w, int64_t epoch_secs);
void nh_provisioner_status_set_last_error_class(
    nh_provisioner_status_writer *w, const char *class_slug);
void nh_provisioner_status_set_chunks(nh_provisioner_status_writer *w,
                                      uint32_t done, uint32_t pending);

/* Emit the current snapshot into the per-user porthome-status.json
 * under `homedir` (which must be an absolute path such as "/home/x"),
 * merged as the "provisioner" key. Best-effort chowns file, lockfile,
 * and the .../nostr-homed dir to uid:gid so the user's syncd + CLI
 * can read them.
 *
 * `homedir_override` when non-NULL takes precedence over `homedir` —
 * used by tests to redirect writes to a tmpdir without a real user.
 *
 * Returns 0 on write success (chown failures are warn-only), -errno on
 * hard I/O failure. */
int nh_provisioner_status_emit(nh_provisioner_status_writer *w,
                               const char *homedir,
                               uid_t uid, gid_t gid,
                               const char *homedir_override);

/* Post the shared PROVISION notification for a state transition.
 * `event_slug` is one of "start" / "done" / "error:<class>". Throttled
 * by the notifier's 10 min per-(cat,key) window. NULL writer is a
 * no-op. This is a thin convenience over the lazily-initialised
 * broker-scoped nh_porthome_notifier. */
void nh_provisioner_notify(nh_provisioner_status_writer *w,
                           const char *event_slug,
                           const char *summary,
                           const char *body);

/* Category slug for state (stable — used inside the JSON body and
 * for logs). Never returns NULL; unknown -> "idle". */
const char *nh_provisioner_state_slug(nh_provisioner_state s);

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_PORTHOME_STATUS_H */
