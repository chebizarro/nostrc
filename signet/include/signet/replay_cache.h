/* SPDX-License-Identifier: MIT
 *
 * replay_cache.h - Replay protection for Signet.
 *
 * This module tracks seen request identifiers (e.g., Nostr event IDs, nonces)
 * to prevent replay attacks. It also enforces created_at skew limits.
 *
 * In-memory rolling window with configurable TTL and clock skew tolerance.
 */

#ifndef SIGNET_REPLAY_CACHE_H
#define SIGNET_REPLAY_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * SignetReplayCache:
 * Opaque in-memory replay-protection cache.
 *
 * Since: 1.0
 */
typedef struct SignetReplayCache SignetReplayCache;

/**
 * SignetReplayResult:
 * @SIGNET_REPLAY_OK: signet replay ok
 * @SIGNET_REPLAY_DUPLICATE: signet replay duplicate
 * @SIGNET_REPLAY_TOO_OLD: signet replay too old
 * @SIGNET_REPLAY_TOO_FAR_IN_FUTURE: signet replay too far in future
 * @SIGNET_REPLAY_INVALID: signet replay invalid
 *
 * Replay validation result codes.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_REPLAY_OK = 0,
  SIGNET_REPLAY_DUPLICATE = 1,
  SIGNET_REPLAY_TOO_OLD = 2,
  SIGNET_REPLAY_TOO_FAR_IN_FUTURE = 3,
  /* Malformed input (missing/empty/overlong event id, or invalid timestamp
   * while skew validation is enabled). Callers must fail closed on this. */
  SIGNET_REPLAY_INVALID = 4,
  /* refuse_when_full only: no room without dropping an unexpired id (the
   * cache, or the caller's scope, is full). Callers must fail closed. */
  SIGNET_REPLAY_FULL = 5
} SignetReplayResult;

/**
 * SignetReplayCacheConfig:
 * @max_entries: maximum number of cache entries.
 * @ttl_seconds: entry time-to-live in seconds. When @skew_seconds is set the
 *   effective TTL is at least 2 * @skew_seconds, so an id stays cached for as
 *   long as its created_at can pass the skew check.
 * @skew_seconds: accepted clock skew in seconds.
 * @max_entries_per_scope: with signet_replay_check_and_mark_scoped(), the most
 *   unexpired ids one scope may hold (0: no per-scope bound). A full scope is
 *   refused (SIGNET_REPLAY_FULL), never evicted.
 * @refuse_when_full: when the cache is full, refuse (SIGNET_REPLAY_FULL)
 *   instead of evicting the oldest unexpired id. With a skew window this
 *   makes "an id seen once never executes again" unconditional: an evicted
 *   id could otherwise be replayed while its created_at is still fresh.
 *
 * Configuration for replay cache bounds and skew checks.
 *
 * Since: 1.0
 */
typedef struct {
  size_t max_entries;
  uint32_t ttl_seconds;
  uint32_t skew_seconds;
  size_t max_entries_per_scope;
  bool refuse_when_full;
} SignetReplayCacheConfig;

/* Create replay cache. Returns NULL on OOM. */
/**
 * signet_replay_cache_new:
 * @cfg: (nullable): configuration to use
 *
 * Create replay cache. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetReplayCache *signet_replay_cache_new(const SignetReplayCacheConfig *cfg);

/* Free cache. Safe on NULL. */
/**
 * signet_replay_cache_free:
 * @c: (nullable): a #SignetReplayCache
 *
 * Free cache. Safe on NULL.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_replay_cache_free(SignetReplayCache *c);

/* Check if event_id has been seen within TTL and, if OK, mark it as seen.
 *
 * event_id_hex is treated as an opaque string key (expected to be a Nostr event id hex).
 * event_created_at and now are unix seconds.
 */
/**
 * signet_replay_check_and_mark:
 * @c: (not nullable): a #SignetReplayCache
 * @event_id_hex: (nullable): event id hex
 * @event_created_at: event created at
 * @now: current Unix time in seconds
 *
 * Check if event_id has been seen within TTL and, if OK, mark it as seen.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetReplayResult signet_replay_check_and_mark(SignetReplayCache *c,
                                                const char *event_id_hex,
                                                int64_t event_created_at,
                                                int64_t now);

/**
 * signet_replay_check_and_mark_scoped:
 * @c: (not nullable): a #SignetReplayCache
 * @scope: (nullable): quota scope, e.g. the authenticated sender pubkey
 * @event_id_hex: (nullable): event id hex
 * @event_created_at: event created at
 * @now: current Unix time in seconds
 *
 * As signet_replay_check_and_mark(), charging the id to @scope's
 * max_entries_per_scope quota. Ids are still unique across scopes.
 *
 * Returns: the replay verdict
 */
SignetReplayResult signet_replay_check_and_mark_scoped(SignetReplayCache *c,
                                                       const char *scope,
                                                       const char *event_id_hex,
                                                       int64_t event_created_at,
                                                       int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_REPLAY_CACHE_H */
