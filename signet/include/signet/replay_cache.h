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
  SIGNET_REPLAY_INVALID = 4
} SignetReplayResult;

/**
 * SignetReplayCacheConfig:
 * @max_entries: maximum number of cache entries.
 * @ttl_seconds: entry time-to-live in seconds.
 * @skew_seconds: accepted clock skew in seconds.
 *
 * Configuration for replay cache bounds and skew checks.
 *
 * Since: 1.0
 */
typedef struct {
  size_t max_entries;
  uint32_t ttl_seconds;
  uint32_t skew_seconds;
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

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_REPLAY_CACHE_H */
