/**
 * GnostrProfileService - Centralized profile fetching service with automatic batching
 *
 * This service provides a centralized way for all widgets to request profile data.
 * It automatically batches requests, deduplicates them, and manages the debounce timing.
 *
 * Architecture:
 * 1. Widgets call gnostr_profile_service_request() with a pubkey and callback
 * 2. Requests are queued and deduplicated internally
 * 3. After 150ms debounce, all queued pubkeys are batch-fetched
 * 4. First checks nostrdb cache for immediate results
 * 5. Network fetch for cache misses via gnostr_pool_query_async (kind-0 filter with authors)
 * 6. Fetched profiles are stored to nostrdb
 * 7. All pending callbacks for each pubkey are notified when profile arrives
 */

#ifndef GNOSTR_PROFILE_SERVICE_H
#define GNOSTR_PROFILE_SERVICE_H

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

/* Include profile provider for GnostrProfileMeta definition */
#include "../ui/gnostr-profile-provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback signature for profile requests.
 * @param pubkey_hex  The pubkey that was requested (64-char hex)
 * @param meta        The profile metadata (may be NULL if not found)
 *                    Caller MUST NOT free this - it is owned by the service
 * @param user_data   User data passed to request function
 */
typedef void (*GnostrProfileServiceCallback)(const char *pubkey_hex,
                                              const GnostrProfileMeta *meta,
                                              gpointer user_data);

/* Get the default (singleton) profile service instance.
 * The service is created on first call and persists for the application lifetime.
 * Thread-safe.
 *
 * @return The singleton service instance (never NULL after first call)
 */
gpointer gnostr_profile_service_get_default(void);

/* Request profile data for a pubkey.
 * The callback will be invoked on the main thread when the profile is available.
 * If the profile is already in the cache, the callback may be invoked immediately
 * (synchronously) or in the next main loop iteration.
 *
 * Multiple requests for the same pubkey are automatically deduplicated.
 * All registered callbacks will be notified when the profile arrives.
 *
 * @param service     The profile service instance (from gnostr_profile_service_get_default())
 * @param pubkey_hex  64-character hex pubkey to fetch
 * @param callback    Callback to invoke when profile is ready (may be NULL)
 * @param user_data   User data for callback
 */
void gnostr_profile_service_request(gpointer service,
                                     const char *pubkey_hex,
                                     GnostrProfileServiceCallback callback,
                                     gpointer user_data);

/* Cancel all pending callbacks for a specific user_data.
 * Useful when a widget is being destroyed and needs to cancel its pending requests.
 * Does not cancel the fetch itself, just removes the callbacks.
 *
 * @param service     The profile service instance
 * @param user_data   The user_data to match for cancellation
 * @return Number of callbacks cancelled
 */
guint gnostr_profile_service_cancel_for_user_data(gpointer service, gpointer user_data);

/* Set the relay URLs to use for fetching profiles.
 * Must be called before any requests will actually fetch from network.
 * Typically called after login when relay list is known.
 *
 * @param service    The profile service instance
 * @param urls       Array of relay URL strings
 * @param url_count  Number of URLs in array
 */
void gnostr_profile_service_set_relays(gpointer service,
                                        const char **urls,
                                        size_t url_count);

/* Set the debounce delay in milliseconds.
 * Default is 150ms. Requests are batched during this window.
 *
 * @param service     The profile service instance
 * @param debounce_ms Debounce delay in milliseconds
 */
void gnostr_profile_service_set_debounce(gpointer service, guint debounce_ms);

/* Get the simple pool used by the service for testing/debugging.
 * Returns the internal GNostrPool instance.
 *
 * @param service  The profile service instance
 * @return The GNostrPool instance (do not unref)
 */
gpointer gnostr_profile_service_get_pool(gpointer service);

/* Set an external simple pool to use instead of creating one internally.
 * Useful for sharing a pool with the main window.
 *
 * @param service  The profile service instance
 * @param pool     The GNostrPool to use (service takes a ref)
 */
void gnostr_profile_service_set_pool(gpointer service, gpointer pool);

/* Service statistics for monitoring */
typedef struct {
  guint64 requests;         /* Total requests received */
  guint64 cache_hits;       /* Requests served from cache */
  guint64 network_fetches;  /* Number of network fetch batches */
  guint64 profiles_fetched; /* Profiles received from network */
  guint64 callbacks_fired;  /* Total callbacks invoked */
  guint pending_requests;   /* Currently pending pubkeys */
  guint pending_callbacks;  /* Currently pending callbacks */
} GnostrProfileServiceStats;

/* Get service statistics.
 *
 * @param service  The profile service instance
 * @param stats    Output struct for statistics
 */
void gnostr_profile_service_get_stats(gpointer service, GnostrProfileServiceStats *stats);

/* Shutdown the profile service and free all resources.
 * Safe to call multiple times. After this call, gnostr_profile_service_get_default()
 * will create a new instance.
 */
void gnostr_profile_service_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GNOSTR_PROFILE_SERVICE_H */
