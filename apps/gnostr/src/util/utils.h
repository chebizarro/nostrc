#ifndef GNOSTR_UTIL_H
#define GNOSTR_UTIL_H

#include <glib.h>

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

gboolean str_has_prefix_http(const char *s);

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