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

gboolean str_has_prefix_http(const char *s);

#endif /* GNOSTR_UTIL_H */