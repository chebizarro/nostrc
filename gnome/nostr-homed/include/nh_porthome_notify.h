/*
 * nh_porthome_notify.h — unified desktop-notification seam for the
 * portable-home stack (syncd, fuse, provisioner).
 *
 * SPDX-License-Identifier: MIT
 *
 * The whole portable-home stack posts a small set of user-facing
 * notifications (sweep drops, conflict files, offline-read misses,
 * limited-mode provisioning). Before Phase 5 I1 each caller open-coded
 * its own fork(2)/execlp(notify-send) path — three copies with
 * subtly-different throttling and no coordination. This seam:
 *
 *   - Consolidates the fork+exec on notify-send behind a single API.
 *     libnotify is intentionally NOT linked; the base packages already
 *     depend on gnome-shell, which drags in `notify-send` via
 *     libnotify-bin. Failure to launch notify-send is silent.
 *
 *   - Adds per-(category, key) throttling with a 10 min default window.
 *     A "sweep dropped N blobs" fired ten times in five minutes surfaces
 *     ONCE to the desktop; the other nine are recorded in the status
 *     file (see nh_porthome_status.h) and dropped.
 *
 *   - Honours NOSTR_HOMED_PORTHOME_QUIET_HOURS ("HH:MM-HH:MM", local
 *     time; crossing midnight is supported). Inside the window the
 *     desktop pop is suppressed but the status-file record is written.
 *     A per-user override lives at $XDG_CONFIG_HOME/nostr-homed/
 *     quiet-hours (single line, same syntax) — the env var wins if set.
 *
 * Beads: nostrc-h10m.1 (Phase 5 I1). Parent nostrc-h10m.
 */

#ifndef NH_PORTHOME_NOTIFY_H
#define NH_PORTHOME_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed category enumeration. Callers MUST pick one — free-form
 * category strings break coordinated throttling. */
typedef enum {
    NH_NOTIFY_CAT_SWEEP        = 1, /* weekly HEAD-sweep drop/re-upload  */
    NH_NOTIFY_CAT_CONFLICT     = 2, /* pull reconcile emitted conflicts  */
    NH_NOTIFY_CAT_LIMITED_MODE = 3, /* provisioner produced limited home */
    NH_NOTIFY_CAT_OFFLINE_MISS = 4, /* fuse read miss with no network    */
    NH_NOTIFY_CAT_PROVISION    = 5, /* generic provisioner status flip   */
} nh_notify_category;

/* Opaque notifier context. Holds the throttle ring and the resolved
 * quiet-hours window. Thread-safety: intended for use from a single
 * thread per instance. */
typedef struct nh_porthome_notifier nh_porthome_notifier;

/* Backend hook — replaces the built-in fork+exec(notify-send) call.
 * Return value is ignored (fire-and-forget). Provide via
 * nh_porthome_notifier_set_backend() for tests. */
typedef void (*nh_notify_backend_fn)(void *ud,
                                     const char *app,
                                     const char *icon,
                                     const char *summary,
                                     const char *body);

/* Clock hook (Unix seconds, local). Provide via
 * nh_porthome_notifier_set_clock() to make the throttle deterministic
 * under test. NULL / default → time(NULL) via gettimeofday. */
typedef int64_t (*nh_notify_clock_fn)(void *ud);

/* Recorded-notification callback (see nh_porthome_status.h — the CLI
 * / status merger persists these into porthome-status.json). Called
 * even when the desktop pop was suppressed (throttled or quiet
 * hours), so the status file mirrors every attempted notification. */
typedef void (*nh_notify_record_fn)(void *ud,
                                    int64_t epoch_secs,
                                    nh_notify_category cat,
                                    const char *key,
                                    const char *summary,
                                    const char *body,
                                    bool delivered);

/* Construct. state_dir may be NULL — in that case per-user quiet-hours
 * config is looked up under $XDG_CONFIG_HOME. Never returns NULL:
 * allocation failure aborts (matches the rest of the syncd code
 * base, which treats OOM as fatal). */
nh_porthome_notifier *nh_porthome_notifier_new(void);

/* Free. Safe on NULL. */
void nh_porthome_notifier_free(nh_porthome_notifier *n);

/* Configure the throttle window in seconds. 0 disables throttling
 * (every fire delivers). Default at construction is 600 (10 min). */
void nh_porthome_notifier_set_window(nh_porthome_notifier *n,
                                     uint32_t seconds);

/* Set the quiet-hours window in local time. Two integers in [0,1439]
 * or (-1,-1) to disable. If end < start the window wraps midnight.
 * Overrides both the env var and the on-disk config for the lifetime
 * of the notifier (used by tests). */
void nh_porthome_notifier_set_quiet_hours(nh_porthome_notifier *n,
                                          int start_minutes,
                                          int end_minutes);

/* Parse "HH:MM-HH:MM" into two minute-of-day integers. Returns 0 on
 * success, -1 on parse error. Exported so the CLI can validate
 * `--quiet-hours-set` input before writing the config file. */
int nh_porthome_notify_parse_hhmm(const char *s,
                                  int *out_start_min,
                                  int *out_end_min);

/* Test seams. Any NULL argument restores the default. */
void nh_porthome_notifier_set_backend(nh_porthome_notifier *n,
                                      nh_notify_backend_fn fn,
                                      void *ud);
void nh_porthome_notifier_set_clock(nh_porthome_notifier *n,
                                    nh_notify_clock_fn fn,
                                    void *ud);
void nh_porthome_notifier_set_record(nh_porthome_notifier *n,
                                     nh_notify_record_fn fn,
                                     void *ud);

/* Post a notification. `key` is the throttle scope within `cat`
 * (typically an object hash prefix, a rel-path, or a short slug —
 * pick something stable so repeated events collapse). NULL key is
 * equivalent to "" and shares a single throttle bucket per category.
 *
 * Return codes (informational; callers usually ignore):
 *   1  = delivered to desktop
 *   0  = suppressed (throttle or quiet hours) — recorded via record_fn
 *  -1  = internal error (bad args); no side effects
 */
int nh_porthome_notify(nh_porthome_notifier *n,
                       const char *app,
                       const char *icon,
                       const char *summary,
                       const char *body,
                       nh_notify_category cat,
                       const char *key);

/* Human-readable slug for a category (stable — used as a JSON key in
 * porthome-status.json and as the throttle-bucket label). Never
 * returns NULL. Unknown categories map to "other". */
const char *nh_porthome_notify_category_slug(nh_notify_category cat);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_NOTIFY_H */
