#ifndef GNOSTR_UTIL_H
#define GNOSTR_UTIL_H

#include <glib.h>
#include <gio/gio.h>

typedef struct _NostrEvent NostrEvent;

#ifndef GNOSTR_MEDIA_FETCH_INTENT_DEFINED
#define GNOSTR_MEDIA_FETCH_INTENT_DEFINED
typedef enum {
  GNOSTR_MEDIA_FETCH_AUTOMATIC = 0,
  GNOSTR_MEDIA_FETCH_USER_INITIATED
} GnostrMediaFetchIntent;
#endif

typedef enum {
  GNOSTR_MEDIA_POLICY_ERROR_INVALID_URL,
  GNOSTR_MEDIA_POLICY_ERROR_CREDENTIALS,
  GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_PORT,
  GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS,
  GNOSTR_MEDIA_POLICY_ERROR_RESOLUTION,
  GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_REDIRECT
} GnostrMediaPolicyError;

#define GNOSTR_MEDIA_POLICY_ERROR (gnostr_media_policy_error_quark())
GQuark gnostr_media_policy_error_quark(void);

/* Validates HTTP(S) syntax, rejects URL credentials, and requires every
 * currently resolved destination address to be globally routable. */
gboolean gnostr_media_url_is_safe(const char *url, GError **error);

/* Redirects are limited to the same origin and must independently pass the
 * destination policy. Relative locations are resolved against @from_url. */
gboolean gnostr_media_redirect_is_safe(const char *from_url,
                                       const char *location,
                                       char **out_url,
                                       GError **error);

/* Automatic fetches require the opt-in setting. Explicit activation is allowed
 * independently, but never bypasses URL/address policy. */
gboolean gnostr_media_fetch_intent_is_allowed(GnostrMediaFetchIntent intent);

#ifndef GNOSTR_MEDIA_POLICY_TESTING
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>

/**
 * gnostr_get_shared_soup_session:
 *
 * Returns a shared SoupSession instance for HTTP requests.
 * Using a shared session avoids TLS cleanup issues on macOS where
 * libgnutls can crash when many sessions are destroyed rapidly.
 * 
 * The session has conservative connection limits to avoid overwhelming
 * the TLS stack.
 *
 * Returns: (transfer none): The shared SoupSession. Do NOT unref this.
 */
SoupSession *gnostr_get_shared_soup_session(void);

/**
 * gnostr_cleanup_shared_soup_session:
 *
 * Cleans up the shared SoupSession. Call this during app shutdown
 * AFTER all async operations have been cancelled.
 */
void gnostr_cleanup_shared_soup_session(void);

/**
 * gnostr_is_remote_media_allowed:
 *
 * Checks the "load-remote-media" GSettings key to determine if automatic
 * remote media fetching is permitted. Returns FALSE if the setting is
 * disabled or the schema is unavailable.
 *
 * All automatic media paths, including avatars, must use this policy.
 *
 * Returns: TRUE if remote media loading is allowed
 */
gboolean gnostr_is_remote_media_allowed(void);

#endif /* HAVE_SOUP3 */

#include <nostr-gobject-1.0/nostr_pool.h>

/**
 * gnostr_get_shared_query_pool:
 *
 * Returns a shared GNostrPool instance for one-shot relay queries.
 * Using a shared pool reduces connection churn and improves connection reuse.
 *
 * Callers must sync relays on the pool before querying:
 *   gnostr_pool_sync_relays(pool, urls, count);
 *
 * For long-lived subscriptions, widgets should create their own pool.
 *
 * Returns: (transfer none): The shared GNostrPool. Do NOT unref this.
 */
GNostrPool *gnostr_get_shared_query_pool(void);

/**
 * gnostr_cleanup_shared_query_pool:
 *
 * Cleans up the shared query pool. Call this during app shutdown
 * AFTER all async operations have been cancelled.
 */
void gnostr_cleanup_shared_query_pool(void);

/**
 * gnostr_pool_wire_ndb:
 * @pool: a #GNostrPool to configure
 *
 * Wires a pool with nostrdb cache-first query and event sink callbacks.
 * After calling this, the pool checks nostrdb before hitting the network
 * and auto-persists all relay results to nostrdb.
 *
 * Safe to call multiple times — idempotent.
 */
void gnostr_pool_wire_ndb(GNostrPool *pool);

gboolean str_has_prefix_http(const char *s);

/**
 * GnostrRelayPublishDoneCallback:
 * @success_count: number of relays that accepted the event
 * @fail_count: number of relays that failed
 * @user_data: user data passed to gnostr_publish_to_relays_async()
 *
 * Called on the main thread when the async publish completes.
 */
typedef void (*GnostrRelayPublishDoneCallback)(guint success_count,
                                                guint fail_count,
                                                gpointer user_data);

/**
 * gnostr_publish_to_relays_async:
 * @event: (transfer full): a signed NostrEvent to publish
 * @relay_urls: (transfer full): a GPtrArray of relay URL strings
 * @callback: (nullable): completion callback (runs on main thread)
 * @user_data: user data for @callback
 *
 * Publishes @event to each relay in @relay_urls on a background thread.
 * Takes ownership of both @event and @relay_urls.
 */
void gnostr_publish_to_relays_async(NostrEvent *event,
                                     GPtrArray *relay_urls,
                                     GnostrRelayPublishDoneCallback callback,
                                     gpointer user_data);

/* gnostr_ensure_hex_pubkey moved to nostr-gobject */
#include <nostr-gobject-1.0/nostr_utils.h>

#endif /* !GNOSTR_MEDIA_POLICY_TESTING */
#endif /* GNOSTR_UTIL_H */
