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

#include <nostr_simple_pool.h>

/**
 * gnostr_get_shared_query_pool:
 *
 * Returns a shared GnostrSimplePool instance for one-shot relay queries.
 * Using a shared pool reduces connection churn and improves connection reuse.
 * 
 * This pool is intended for query_single_async operations. For long-lived
 * subscriptions (subscribe_many_async), widgets should use their own pool
 * to properly manage signal handlers.
 *
 * Returns: (transfer none): The shared GnostrSimplePool. Do NOT unref this.
 */
GnostrSimplePool *gnostr_get_shared_query_pool(void);

/**
 * gnostr_cleanup_shared_query_pool:
 *
 * Cleans up the shared query pool. Call this during app shutdown
 * AFTER all async operations have been cancelled.
 */
void gnostr_cleanup_shared_query_pool(void);

gboolean str_has_prefix_http(const char *s);

#endif /* GNOSTR_UTIL_H */