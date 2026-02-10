#ifndef GNOSTR_UTIL_H
#define GNOSTR_UTIL_H

#include <glib.h>
#include <nostr-event.h>

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

#endif /* HAVE_SOUP3 */

#include <nostr_pool.h>

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

/**
 * gnostr_ensure_hex_pubkey:
 * @input: a pubkey in any format: 64-char hex, npub1..., or nprofile1...
 *
 * Defensive normalizer for pubkey strings. Accepts hex passthrough,
 * or decodes NIP-19 bech32 (npub1/nprofile1) to 64-char hex.
 *
 * Returns: (transfer full) (nullable): newly-allocated 64-char hex string,
 *   or %NULL if @input is invalid. Caller must g_free().
 */
gchar *gnostr_ensure_hex_pubkey(const char *input);

#endif /* GNOSTR_UTIL_H */