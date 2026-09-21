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
 * Two backing modes are supported:
 *   - In-memory (nh_auth_ratelimit_new): counters live only in RAM; state is
 *     lost on broker restart. Clock is the caller-supplied now_ms passed into
 *     each call (typically the broker's monotonic clock).
 *   - Persistent (nh_auth_ratelimit_open_persistent): every mutation is upserted
 *     into a SQLite table so a broker restart preserves any active cooldown.
 *     Because the broker's monotonic clock resets across reboots, the
 *     persistent variant IGNORES the caller-supplied now_ms and uses its own
 *     wall-clock hook (default: time(NULL)*1000). The clock hook is injectable
 *     so tests can pin wall time deterministically. See the persistent-open
 *     comment for the full rationale.
 *
 * Follow-up on bucket B1 — beads nostrc-zcll.2.
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
 * Returns NULL on OOM or on an invalid config (max_failures == 0). The result
 * is an in-memory limiter; counters do NOT survive a broker restart. */
nh_auth_ratelimit *
nh_auth_ratelimit_new(const nh_auth_ratelimit_config *config);

/* Wall-clock hook used by the persistent limiter to timestamp mutations.
 * The hook must return milliseconds since some absolute epoch that is stable
 * across broker restarts (production uses time(NULL) * 1000). Tests inject a
 * counter they can advance deterministically. */
typedef uint64_t (*nh_auth_ratelimit_clock_fn)(void *ctx);

/* Opens (or creates) a persistent rate limiter backed by a SQLite database at
 * `path`. On success `*out` receives the limiter and existing state is loaded
 * from the database so any account still inside a cooldown at broker shutdown
 * remains locked out across the restart. Every mutation
 * (record_failure/reset/window-rollover/cooldown-arm) is persisted before the
 * call returns.
 *
 * Clock semantics: the persistent variant IGNORES the now_ms parameter of the
 * check/record/reset functions and reads its own clock hook. This is because
 * the broker's monotonic clock (CLOCK_MONOTONIC) resets across reboots, so
 * persisted timestamps must be expressed in a domain that survives — wall
 * clock (real time). Passing NULL for clock_fn selects the default hook,
 * which returns time(NULL) * 1000 (wall-clock milliseconds).
 *
 * `config` may be NULL to use the built-in defaults. `path` must be non-NULL
 * and non-empty. Returns 0 on success; -1 on invalid arguments, OOM, or a
 * SQLite failure (in which case *out is left untouched). */
int nh_auth_ratelimit_open_persistent(const char *path,
                                      const nh_auth_ratelimit_config *config,
                                      nh_auth_ratelimit_clock_fn clock_fn,
                                      void *clock_ctx,
                                      nh_auth_ratelimit **out);

void nh_auth_ratelimit_free(nh_auth_ratelimit *limiter);

/* Returns non-zero if the key is currently allowed to make a login attempt.
 * Zero means the key is inside a cooldown. Never modifies counters.
 *
 * For the in-memory variant, `now_ms` is used as the current time. For the
 * persistent variant, `now_ms` is ignored — see nh_auth_ratelimit_open_persistent. */
int nh_auth_ratelimit_check(nh_auth_ratelimit *limiter, const char *key,
                            uint64_t now_ms);

/* Records one failed proof for the key at now_ms. If the accumulated failures
 * inside the current window reach max_failures, the key enters cooldown until
 * now_ms + cooldown_ms. See nh_auth_ratelimit_open_persistent for the clock
 * semantics of the persistent variant. */
void nh_auth_ratelimit_record_failure(nh_auth_ratelimit *limiter,
                                      const char *key, uint64_t now_ms);

/* Successful proof: clears any counter/cooldown for this key. */
void nh_auth_ratelimit_reset(nh_auth_ratelimit *limiter, const char *key);

#endif /* NH_AUTH_RATELIMIT_H */
