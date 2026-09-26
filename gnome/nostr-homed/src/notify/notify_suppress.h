/*
 * notify_suppress — generation-guard primitive for the notify daemon.
 *
 * The daemon suppresses notifications whenever GNostr's session-bus name
 * `org.gnostr.Client` is present (the user is looking at the client, so
 * the notification is redundant / annoying). Per §3.3 D5 revised (Finding
 * 12), "subscribes to nothing" is not enough because an EVENT callback,
 * metadata lookup, or a coalesce timer may already be in flight when the
 * name-owner-changed edge fires. We use a monotonic **generation counter**
 * that is bumped on both edges (name-appear AND name-vanish) so any
 * callback carrying an older generation cheaply detects the transition
 * and drops silently.
 *
 * Concurrency contract:
 *   - `nsn_guard_bump()` is called from the main thread only, from the
 *     GBus name-owner-changed watcher and from the resumption-grace timer.
 *   - `nsn_guard_current()` and `nsn_guard_check()` may be called from any
 *     thread — they are lock-free reads on a memory-orderly counter.
 *   - Callbacks that yield (async metadata lookup, coalesce timer expiry,
 *     the final `send_notification`) must snapshot the current generation
 *     at their entry via `nsn_guard_current()` and compare via
 *     `nsn_guard_check()` before their next side effect. A mismatch means
 *     the guard bumped since the callback was queued; the callback returns
 *     without side effects.
 */
#ifndef NOSTR_NOTIFY_SUPPRESS_H
#define NOSTR_NOTIFY_SUPPRESS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  /*
   * Monotonic generation counter. Even/odd meaning is NOT encoded — the
   * guard treats any bump as a state change. Producers snapshot at entry,
   * consumers re-check at every yield boundary.
   */
  _Atomic uint64_t generation;

  /*
   * Whether GNostr is currently visible on the session bus. Written by
   * the main thread only, read by anyone as a coarse hint (the real
   * cross-thread contract is the generation counter). If this is 1 the
   * daemon must NOT emit new notifications regardless of generation.
   */
  _Atomic int suppressed;
} NostrNotifySuppressGuard;

static inline void nsn_guard_init(NostrNotifySuppressGuard *g) {
  atomic_init(&g->generation, 1);
  atomic_init(&g->suppressed, 0);
}

/*
 * Bump the generation counter. Any callback snapshotted before this returns
 * will observe the new value on its next `nsn_guard_check()` and drop.
 * Returns the new generation.
 */
static inline uint64_t nsn_guard_bump(NostrNotifySuppressGuard *g) {
  return atomic_fetch_add_explicit(&g->generation, 1, memory_order_release) + 1;
}

/*
 * Snapshot the current generation. Callbacks call this once at entry.
 */
static inline uint64_t nsn_guard_current(const NostrNotifySuppressGuard *g) {
  return atomic_load_explicit(&g->generation, memory_order_acquire);
}

/*
 * Check whether the snapshot matches the current generation AND the
 * daemon is not in suppressed state. Returns true iff the caller may
 * proceed with side effects.
 */
static inline bool nsn_guard_check(const NostrNotifySuppressGuard *g,
                                   uint64_t snapshot) {
  if (atomic_load_explicit(&g->suppressed, memory_order_acquire)) return false;
  return snapshot == atomic_load_explicit(&g->generation, memory_order_acquire);
}

static inline void nsn_guard_set_suppressed(NostrNotifySuppressGuard *g,
                                            bool suppressed) {
  atomic_store_explicit(&g->suppressed, suppressed ? 1 : 0,
                        memory_order_release);
}

static inline bool nsn_guard_is_suppressed(const NostrNotifySuppressGuard *g) {
  return atomic_load_explicit(&g->suppressed, memory_order_acquire) != 0;
}

#endif /* NOSTR_NOTIFY_SUPPRESS_H */
