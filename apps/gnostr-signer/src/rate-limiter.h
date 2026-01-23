/* rate-limiter.h - Rate limiting for authentication attempts
 *
 * Provides rate limiting functionality to prevent brute force attacks
 * on authentication in gnostr-signer.
 *
 * Features:
 * - Per-client pubkey tracking for NIP-46 bunker authentication
 * - Configurable max attempts before lockout (default 5)
 * - Exponential backoff after failures (1s, 2s, 4s, 8s... up to 5 min)
 * - Lockout after N consecutive failures
 * - Reset on successful auth
 * - Persistent storage across app restarts
 * - Admin ability to clear rate limits
 * - GObject-based implementation with signal support
 *
 * Related to issue: nostrc-1g1
 */
#pragma once
#include <glib-object.h>

G_BEGIN_DECLS

/* Default rate limiting policy */
#define GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS     5
#define GN_RATE_LIMITER_DEFAULT_WINDOW_SECONDS   300   /* 5 minutes */
#define GN_RATE_LIMITER_DEFAULT_LOCKOUT_SECONDS  1     /* 1 second initial (exponential backoff) */
#define GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS      300   /* 5 minute max lockout */
#define GN_RATE_LIMITER_MAX_BACKOFF_MULTIPLIER   256   /* Cap at 256x (4+ minutes at 1s base) */

#define GN_TYPE_RATE_LIMITER (gn_rate_limiter_get_type())

G_DECLARE_FINAL_TYPE(GnRateLimiter, gn_rate_limiter, GN, RATE_LIMITER, GObject)

/**
 * GnRateLimitStatus:
 * @GN_RATE_LIMIT_ALLOWED: Request is allowed
 * @GN_RATE_LIMIT_BACKOFF: Request is rate limited, retry after backoff period
 * @GN_RATE_LIMIT_LOCKED_OUT: Client is locked out after too many failures
 *
 * Status returned when checking if a request is allowed.
 */
typedef enum {
  GN_RATE_LIMIT_ALLOWED,
  GN_RATE_LIMIT_BACKOFF,
  GN_RATE_LIMIT_LOCKED_OUT
} GnRateLimitStatus;

/**
 * GnClientRateLimitInfo:
 * @client_pubkey: The client's public key (hex string)
 * @failed_attempts: Number of consecutive failed attempts
 * @lockout_until: Unix timestamp when lockout expires (0 if not locked out)
 * @backoff_multiplier: Current exponential backoff multiplier
 * @last_attempt: Unix timestamp of last attempt
 *
 * Information about a client's rate limit state.
 */
typedef struct {
  gchar *client_pubkey;
  guint failed_attempts;
  gint64 lockout_until;
  guint backoff_multiplier;
  gint64 last_attempt;
} GnClientRateLimitInfo;

/**
 * gn_client_rate_limit_info_free:
 * @info: A #GnClientRateLimitInfo to free
 *
 * Frees a #GnClientRateLimitInfo structure.
 */
void gn_client_rate_limit_info_free(GnClientRateLimitInfo *info);

/**
 * gn_rate_limiter_new:
 * @max_attempts: Maximum number of failed attempts allowed before lockout
 * @window_seconds: Time window in seconds (unused for exponential backoff)
 * @lockout_seconds: Base lockout duration in seconds (for exponential backoff)
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
 * - 5 attempts before lockout
 * - 1 second base lockout with exponential backoff
 * - 5 minute maximum lockout
 *
 * Returns: (transfer full): A new #GnRateLimiter instance with default settings
 */
GnRateLimiter *gn_rate_limiter_new_default(void);

/**
 * gn_rate_limiter_check_allowed:
 * @self: A #GnRateLimiter
 *
 * Checks if an authentication attempt is currently allowed (global).
 * This does not record the attempt; use gn_rate_limiter_record_attempt()
 * after the attempt is made.
 *
 * Returns: %TRUE if an attempt is allowed, %FALSE if rate limited
 */
gboolean gn_rate_limiter_check_allowed(GnRateLimiter *self);

/**
 * gn_rate_limiter_check_client:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 * @remaining_seconds: (out) (optional): If rate limited, seconds until retry allowed
 *
 * Checks if an authentication attempt is allowed for a specific client.
 * This is the primary method for NIP-46 bunker authentication rate limiting.
 *
 * Returns: A #GnRateLimitStatus indicating if the request is allowed
 */
GnRateLimitStatus gn_rate_limiter_check_client(GnRateLimiter *self,
                                                const gchar *client_pubkey,
                                                guint *remaining_seconds);

/**
 * gn_rate_limiter_record_attempt:
 * @self: A #GnRateLimiter
 * @success: Whether the authentication attempt was successful
 *
 * Records an authentication attempt (global). If @success is %TRUE, the rate
 * limiter state is reset. If @success is %FALSE, the failed attempt
 * is recorded and may trigger a lockout if the limit is exceeded.
 *
 * If this attempt causes the rate limit to be exceeded, the
 * "rate-limit-exceeded" signal will be emitted.
 */
void gn_rate_limiter_record_attempt(GnRateLimiter *self, gboolean success);

/**
 * gn_rate_limiter_record_client_attempt:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 * @success: Whether the authentication attempt was successful
 *
 * Records an authentication attempt for a specific client. If @success is %TRUE,
 * the client's rate limit state is reset. If @success is %FALSE, the failed
 * attempt is recorded and may trigger a lockout.
 *
 * Emits "client-rate-limit-exceeded" signal if lockout is triggered.
 */
void gn_rate_limiter_record_client_attempt(GnRateLimiter *self,
                                            const gchar *client_pubkey,
                                            gboolean success);

/**
 * gn_rate_limiter_get_remaining_lockout:
 * @self: A #GnRateLimiter
 *
 * Gets the number of seconds remaining in the current lockout period (global).
 *
 * Returns: Seconds until retry is allowed, or 0 if not locked out
 */
guint gn_rate_limiter_get_remaining_lockout(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_client_remaining_lockout:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 *
 * Gets the number of seconds remaining in a client's lockout period.
 *
 * Returns: Seconds until retry is allowed, or 0 if not locked out
 */
guint gn_rate_limiter_get_client_remaining_lockout(GnRateLimiter *self,
                                                    const gchar *client_pubkey);

/**
 * gn_rate_limiter_get_attempts_remaining:
 * @self: A #GnRateLimiter
 *
 * Gets the number of attempts remaining before lockout (global).
 *
 * Returns: Number of attempts remaining, or 0 if currently locked out
 */
guint gn_rate_limiter_get_attempts_remaining(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_client_attempts_remaining:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 *
 * Gets the number of attempts remaining for a specific client.
 *
 * Returns: Number of attempts remaining, or 0 if currently locked out
 */
guint gn_rate_limiter_get_client_attempts_remaining(GnRateLimiter *self,
                                                     const gchar *client_pubkey);

/**
 * gn_rate_limiter_reset:
 * @self: A #GnRateLimiter
 *
 * Resets the rate limiter state (global), clearing all recorded attempts
 * and any active lockout. This is typically called after successful
 * authentication.
 */
void gn_rate_limiter_reset(GnRateLimiter *self);

/**
 * gn_rate_limiter_reset_client:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 *
 * Resets the rate limit state for a specific client. This is typically
 * called after successful authentication from that client.
 */
void gn_rate_limiter_reset_client(GnRateLimiter *self,
                                   const gchar *client_pubkey);

/**
 * gn_rate_limiter_clear_all_clients:
 * @self: A #GnRateLimiter
 *
 * Admin function: Clears rate limit state for all clients.
 * This should only be accessible to administrators.
 */
void gn_rate_limiter_clear_all_clients(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_lockout_multiplier:
 * @self: A #GnRateLimiter
 *
 * Gets the current lockout multiplier used for exponential backoff (global).
 * This increases each time the rate limit is exceeded.
 *
 * Returns: Current lockout multiplier (1, 2, 4, etc.)
 */
guint gn_rate_limiter_get_lockout_multiplier(GnRateLimiter *self);

/**
 * gn_rate_limiter_is_locked_out:
 * @self: A #GnRateLimiter
 *
 * Convenience function to check if currently in lockout state (global).
 *
 * Returns: %TRUE if locked out, %FALSE otherwise
 */
gboolean gn_rate_limiter_is_locked_out(GnRateLimiter *self);

/**
 * gn_rate_limiter_is_client_locked_out:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 *
 * Checks if a specific client is currently locked out.
 *
 * Returns: %TRUE if locked out, %FALSE otherwise
 */
gboolean gn_rate_limiter_is_client_locked_out(GnRateLimiter *self,
                                               const gchar *client_pubkey);

/**
 * gn_rate_limiter_get_client_info:
 * @self: A #GnRateLimiter
 * @client_pubkey: The client's public key (hex string)
 *
 * Gets detailed rate limit information for a specific client.
 *
 * Returns: (transfer full) (nullable): A #GnClientRateLimitInfo or %NULL if
 *          the client has no rate limit state. Free with gn_client_rate_limit_info_free().
 */
GnClientRateLimitInfo *gn_rate_limiter_get_client_info(GnRateLimiter *self,
                                                        const gchar *client_pubkey);

/**
 * gn_rate_limiter_list_clients:
 * @self: A #GnRateLimiter
 *
 * Admin function: Gets a list of all clients with rate limit state.
 *
 * Returns: (transfer full) (element-type GnClientRateLimitInfo): A #GPtrArray
 *          of #GnClientRateLimitInfo structures. Free with g_ptr_array_unref().
 */
GPtrArray *gn_rate_limiter_list_clients(GnRateLimiter *self);

/**
 * gn_rate_limiter_save:
 * @self: A #GnRateLimiter
 *
 * Persists the current rate limit state to disk.
 * This is automatically called after state changes.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_rate_limiter_save(GnRateLimiter *self);

/**
 * gn_rate_limiter_load:
 * @self: A #GnRateLimiter
 *
 * Loads rate limit state from disk.
 * This is automatically called on construction.
 *
 * Returns: %TRUE on success, %FALSE if no saved state exists
 */
gboolean gn_rate_limiter_load(GnRateLimiter *self);

/**
 * gn_rate_limiter_get_default:
 *
 * Gets the singleton default rate limiter instance. Creates it with
 * default policy settings if it doesn't exist.
 *
 * Returns: (transfer none): The default #GnRateLimiter instance
 */
GnRateLimiter *gn_rate_limiter_get_default(void);

/**
 * gn_rate_limiter_format_error_message:
 * @status: A #GnRateLimitStatus
 * @remaining_seconds: Seconds remaining until retry allowed
 *
 * Generates a user-friendly error message for rate limiting.
 *
 * Returns: (transfer full): A human-readable error message. Free with g_free().
 */
gchar *gn_rate_limiter_format_error_message(GnRateLimitStatus status,
                                             guint remaining_seconds);

G_END_DECLS
