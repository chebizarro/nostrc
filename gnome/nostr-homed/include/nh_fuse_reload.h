/*
 * nh_fuse_reload.h — inotify-driven snapshot reload for the FUSE mount.
 *
 * SPDX-License-Identifier: MIT
 *
 * Design: docs/designs/nostrfs-porthome-overlay.md §7.3 F4.
 *
 * The mount's namespace table (nh_fuse_table) is loaded from
 * snapshot.json at start. Without a watcher, a syncd-published
 * generation N+1 is invisible to the mount until unmount+remount —
 * confirmed live in docs/reviews/porthome-fuse-live-2026-09-24.md
 * case (f) and tracked as bead nostrc-plo4.
 *
 * This module owns:
 *   - inotify watch on the state_dir (NOT the file — atomic-rename
 *     writers destroy the file's inode). We filter on the exact
 *     basename ("snapshot.json").
 *   - debounce: a burst of events (temp file writes, rename, etc.)
 *     collapses into a single reload after `debounce_ms` (250 ms
 *     default). This tames syncd's atomic-rename storms.
 *   - reload path: caller-supplied `load_fn` parses a new namespace
 *     table; `swap_fn` atomically installs it. On failure, `fail_fn`
 *     fires — the caller keeps the old table live and surfaces a
 *     LIMITED_MODE notification. NEVER unmounts.
 *   - shutdown: `nh_fuse_reload_free` wakes any blocked watcher
 *     thread via a self-pipe and joins it.
 *
 * Per-open GENERATION binding is INDEPENDENT of this module: the
 * FUSE handlers copy chunks_hex and generation into the fh at open()
 * time, so an already-open FD keeps serving the old generation
 * regardless of table swaps. This module MUST NOT reach into
 * open fhs.
 *
 * Threading: the module owns a background pthread. The swap runs
 * on that thread; the caller is responsible for serialising the
 * table swap against readers (typically via a pthread_rwlock the
 * caller wraps around `swap_fn`).
 *
 * Escape hatch: NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY=1 disables the
 * watcher (module still constructs — nh_fuse_reload_fd() returns -1
 * and no background thread is spawned). Useful on filesystems that
 * don't implement inotify (eCryptfs, some overlayfs stacks).
 *
 * Beads: nostrc-plo4, parent nostrc-h10m.
 */
#ifndef NH_FUSE_RELOAD_H
#define NH_FUSE_RELOAD_H

#include "nh_fuse_table.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loader — called by the reload thread when the debounce window
 * closes. Must be side-effect free on failure (do not partially
 * consume state). Returns 0 on success (with *out_new_table set to
 * the newly-parsed table, ownership transferred to the caller of
 * swap_fn), or a negative errno on failure. */
typedef int  (*nh_fuse_reload_load_fn)(void *ud,
                                       const char *state_dir,
                                       nh_fuse_table **out_new_table);

/* Swap — install `new_table` and free the old one. The caller's
 * implementation MUST serialise this against readers (rwlock or
 * equivalent). The reload thread has already released internal
 * state; this is the only place ownership of `new_table` transfers. */
typedef void (*nh_fuse_reload_swap_fn)(void *ud,
                                       nh_fuse_table *new_table);

/* Failure hook — fires when load_fn returns non-zero. Caller
 * typically emits a throttled LIMITED_MODE notification and logs.
 * The old table remains live; this module makes no policy
 * decisions on failure. */
typedef void (*nh_fuse_reload_fail_fn)(void *ud, int rc);

/* Clock seam — Unix milliseconds (monotonic). NULL / 0 return uses
 * CLOCK_MONOTONIC by default. Exported for tests. */
typedef int64_t (*nh_fuse_reload_clock_fn)(void *ud);

typedef struct {
    /* Directory containing snapshot.json. Must exist. */
    const char             *state_dir;
    /* Basename to filter on (default "snapshot.json"). */
    const char             *watch_basename;
    /* Debounce window in ms (default 250). */
    long                    debounce_ms;
    /* Disable inotify entirely (env override
     * NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY=1 forces this on). */
    bool                    disable_inotify;
    /* Callbacks. load_fn + swap_fn are required unless
     * disable_inotify is true. */
    nh_fuse_reload_load_fn  load_fn;
    void                   *load_ud;
    nh_fuse_reload_swap_fn  swap_fn;
    void                   *swap_ud;
    nh_fuse_reload_fail_fn  fail_fn;
    void                   *fail_ud;
    /* Test seam. */
    nh_fuse_reload_clock_fn clock_fn;
    void                   *clock_ud;
} nh_fuse_reload_cfg;

typedef struct nh_fuse_reload nh_fuse_reload_t;

/* Construct. Returns 0 on success and starts the watcher thread
 * (unless disable_inotify or the env override is set). Returns a
 * negative errno on failure. Even on inotify open failure we return
 * 0 with a disabled watcher — the mount must still serve reads. */
int  nh_fuse_reload_new (const nh_fuse_reload_cfg *cfg,
                         nh_fuse_reload_t **out);

/* Signal the background thread to exit, join it, close fds, free.
 * Safe on NULL. */
void nh_fuse_reload_free(nh_fuse_reload_t *r);

/* Inotify fd for callers that want to fold it into their own poll
 * loop. Returns -1 when the watcher is disabled. Ownership stays
 * with the module. */
int  nh_fuse_reload_fd(const nh_fuse_reload_t *r);

/* Test seam: bypass the background thread and drive one reload
 * cycle synchronously. Reads any pending inotify events, evaluates
 * the debounce window against the (test) clock, and if actionable
 * invokes load_fn + swap_fn (or fail_fn). Returns:
 *    1 = swap performed
 *    0 = nothing to do (debounce not elapsed / no events)
 *   <0 = fail_fn was invoked (load_fn error propagated)
 *
 * Callers doing the whole cycle synchronously should ALSO call
 * nh_fuse_reload_mark_event() first if they aren't using inotify. */
int  nh_fuse_reload_tick_now(nh_fuse_reload_t *r);

/* Test seam: mark an event as arrived at the current clock time. */
void nh_fuse_reload_mark_event(nh_fuse_reload_t *r);

/* Introspection for tests / status. Returns the count of successful
 * reloads (swap invocations) performed by this module. */
unsigned nh_fuse_reload_swap_count(const nh_fuse_reload_t *r);
unsigned nh_fuse_reload_fail_count(const nh_fuse_reload_t *r);

#ifdef __cplusplus
}
#endif
#endif /* NH_FUSE_RELOAD_H */
