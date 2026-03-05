/* SPDX-License-Identifier: MIT
 *
 * replay_cache.h - Replay protection for Signet.
 *
 * This module tracks seen request identifiers (e.g., Nostr event IDs, nonces)
 * to prevent replay attacks. It also enforces created_at skew limits.
 *
 * Phase 1: minimal in-memory implementation.
 */

#ifndef SIGNET_REPLAY_CACHE_H
#define SIGNET_REPLAY_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct SignetReplayCache SignetReplayCache;

typedef enum {
  SIGNET_REPLAY_OK = 0,
  SIGNET_REPLAY_DUPLICATE = 1,
  SIGNET_REPLAY_TOO_OLD = 2,
  SIGNET_REPLAY_TOO_FAR_IN_FUTURE = 3
} SignetReplayResult;

typedef struct {
  size_t max_entries;
  uint32_t ttl_seconds;
  uint32_t skew_seconds;
} SignetReplayCacheConfig;

/* Create replay cache. Returns NULL on OOM. */
SignetReplayCache *signet_replay_cache_new(const SignetReplayCacheConfig *cfg);

/* Free cache. Safe on NULL. */
void signet_replay_cache_free(SignetReplayCache *c);

/* Check if event_id has been seen within TTL and, if OK, mark it as seen.
 *
 * event_id_hex is treated as an opaque string key (expected to be a Nostr event id hex).
 * event_created_at and now are unix seconds.
 */
SignetReplayResult signet_replay_check_and_mark(SignetReplayCache *c,
                                                const char *event_id_hex,
                                                int64_t event_created_at,
                                                int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_REPLAY_CACHE_H */