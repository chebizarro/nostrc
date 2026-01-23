/* rate-limiter.h - Rate limiting for authentication attempts
 *
 * Provides rate limiting functionality to prevent brute force attacks
 * on password/passphrase entry in gnostr-signer.
 *
 * Features:
 * - Configurable max attempts per time window
 * - Lockout period after max attempts exceeded
 * - Exponential backoff on repeated lockouts
 * - GObject-based implementation with signal support
 * - In-memory storage (cleared on app restart)
 *
 * Related to issue: nostrc-1g1
 */
#pragma once
#include <glib-object.h>

G_BEGIN_DECLS

/* Default rate limiting policy */
#define GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS     5
#define GN_RATE_LIMITER_DEFAULT_WINDOW_SECONDS   300  /* 5 minutes */
#define GN_RATE_LIMITER_DEFAULT_LOCKOUT_SECONDS  30
#define GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS      3600 /* 1 hour max */

#define GN_TYPE_RATE_LIMITER (gn_rate_limiter_get_type())

G_DECLARE_FINAL_TYPE(GnRateLimiter, gn_rate_limiter, GN, RATE_LIMITER, GObject)

/**
 * gn_rate_limiter_new:
 * @max_attempts: Maximum number of failed attempts allowed per window
 * @window_seconds: Time window in seconds during which attempts are counted
 * @lockout_seconds: Initial lockout duration in seconds after limit exceeded
 *
 * Creates a new rate limiter with the specified policy.
 *
 * Returns: (transfer full): A new #GnRateLimiter instance
 */
GnRateLimiter *gn_rate_limiter_new(guint max_attempts,
                                    guint window_seconds,
                                    guint lockout_seconds);

/**
 * gn_rate_limiter_new_default:
 *
 * Creates a new rate limiter with the default policy:
 * - 5 attempts per 5 minute window
 * - 30 second initial lockout
 *
 * Returns: (transfer full): A new #GnRateLimiter instance with default settings
 */
GnRateLimiter *gn_rate_limiter_new_default(void);

/**
 * gn_rate_limiter_check_allowed:
 * @self: A #GnRateLimiter
 *
 * Checks if an authentication attempt is currently allowed.
 * This does not record the attempt; use gn_rate_limiter_record_attempt()
 * after the attempt is made.
 *
 * Returns: %TRUE if an attempt is allowed, %FALSE if rate limited
 */
gboolean gn_rate_limiter_check_allowed(GnRateLimiter *self);

/**
 * gn_rate_limiter_record_attempt:
 * @self: A #GnRateLimiter
 * @success: Whether the authentication attempt was successful
 *
 * Records an authentication attempt. If @success is %TRUE, the rate
 * limiter state is reset. If @success is %FALSE, the failed attempt
 * is recorded and may trigger a lockout if the limit is exceeded.
 *
 * If this attempt causes the rate limit to be exceeded, the
 * "rate-limit-exceeded" signal will be emitted.
 */
void gn_rate_limiter_record_attempt(GnRateLimiter *self, gboolean success);

/**
 * gn_rate_limiter_get_remaining_lockout:
 * @self: A #GnRateLimiter
 *
 * Gets the number of seconds remaining in the current lockout period.
 *
 * Returns: Seconds until retry is allowed, or 0 if not locked out
 */
guint gn_rate_limiter_get_remaining_lockout(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_attempts_remaining:
 * @self: A #GnRateLimiter
 *
 * Gets the number of attempts remaining before lockout.
 *
 * Returns: Number of attempts remaining, or 0 if currently locked out
 */
guint gn_rate_limiter_get_attempts_remaining(GnRateLimiter *self);

/**
 * gn_rate_limiter_reset:
 * @self: A #GnRateLimiter
 *
 * Resets the rate limiter state, clearing all recorded attempts
 * and any active lockout. This is typically called after successful
 * authentication.
 */
void gn_rate_limiter_reset(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_lockout_multiplier:
 * @self: A #GnRateLimiter
 *
 * Gets the current lockout multiplier used for exponential backoff.
 * This increases each time the rate limit is exceeded.
 *
 * Returns: Current lockout multiplier (1, 2, 4, etc.)
 */
guint gn_rate_limiter_get_lockout_multiplier(GnRateLimiter *self);

/**
 * gn_rate_limiter_is_locked_out:
 * @self: A #GnRateLimiter
 *
 * Convenience function to check if currently in lockout state.
 *
 * Returns: %TRUE if locked out, %FALSE otherwise
 */
gboolean gn_rate_limiter_is_locked_out(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_default:
 *
 * Gets the singleton default rate limiter instance. Creates it with
 * default policy settings if it doesn't exist.
 *
 * Returns: (transfer none): The default #GnRateLimiter instance
 */
GnRateLimiter *gn_rate_limiter_get_default(void);

G_END_DECLS
