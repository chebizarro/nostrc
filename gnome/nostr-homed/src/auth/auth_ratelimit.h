#ifndef NH_AUTH_RATELIMIT_H
#define NH_AUTH_RATELIMIT_H

/* Per-account failed-proof throttle for the broker.
 *
 * Policy (fixed window with cooldown):
 *   - Count failed proofs per key within a sliding window (window_ms).
 *   - Once max_failures failed proofs land inside the current window, the key
 *     enters a hard cooldown that lasts cooldown_ms from the moment of the
 *     triggering failure. During cooldown BEGIN_LOGIN / SUBMIT_UNLOCK for
 *     that key return NH_AUTH_RESULT_RATE_LIMITED without touching the SM
 *     or the provider.
 *   - A successful proof clears the counter and any active cooldown.
 *
 * This module is intentionally single-threaded — the broker owns one instance
 * per broker and drives it from its event loop. All clock reads are supplied
 * by the caller (the broker uses its injectable clock seam) so tests can pin
 * or advance monotonic time deterministically.
 *
 * In-memory only in this increment; persistent-across-restart rate limits are
 * a follow-up (see beads nostrc-zcll.2 hand-off notes).
 */

#include <stddef.h>
#include <stdint.h>

typedef struct nh_auth_ratelimit nh_auth_ratelimit;

typedef struct nh_auth_ratelimit_config {
  unsigned max_failures; /* Failed proofs allowed inside one window. */
  uint64_t window_ms;    /* Duration of the failure-counting window. */
  uint64_t cooldown_ms;  /* Duration of the lockout once the budget trips. */
} nh_auth_ratelimit_config;

/* Reasonable defaults: 5 failures per minute triggers a one-minute cooldown. */
#define NH_AUTH_RATELIMIT_DEFAULT_MAX_FAILURES 5u
#define NH_AUTH_RATELIMIT_DEFAULT_WINDOW_MS (60u * 1000u)
#define NH_AUTH_RATELIMIT_DEFAULT_COOLDOWN_MS (60u * 1000u)

void nh_auth_ratelimit_config_defaults(nh_auth_ratelimit_config *out);

/* Creates a rate limiter with the given policy. Passing NULL uses defaults.
 * Returns NULL on OOM or on an invalid config (max_failures == 0). */
nh_auth_ratelimit *
nh_auth_ratelimit_new(const nh_auth_ratelimit_config *config);
void nh_auth_ratelimit_free(nh_auth_ratelimit *limiter);

/* Returns non-zero if the key is currently allowed to make a login attempt.
 * Zero means the key is inside a cooldown. Never modifies counters. */
int nh_auth_ratelimit_check(nh_auth_ratelimit *limiter, const char *key,
                            uint64_t now_ms);

/* Records one failed proof for the key at now_ms. If the accumulated failures
 * inside the current window reach max_failures, the key enters cooldown until
 * now_ms + cooldown_ms. */
void nh_auth_ratelimit_record_failure(nh_auth_ratelimit *limiter,
                                      const char *key, uint64_t now_ms);

/* Successful proof: clears any counter/cooldown for this key. */
void nh_auth_ratelimit_reset(nh_auth_ratelimit *limiter, const char *key);

#endif /* NH_AUTH_RATELIMIT_H */
