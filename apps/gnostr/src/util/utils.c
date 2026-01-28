#include "utils.h"

#ifdef HAVE_SOUP3

/* Shared SoupSession singleton - avoids TLS cleanup issues on macOS */
static SoupSession *s_shared_session = NULL;
static GMutex s_session_mutex;

SoupSession *gnostr_get_shared_soup_session(void) {
  g_mutex_lock(&s_session_mutex);
  
  if (!s_shared_session) {
    /* Create session with conservative connection limits to avoid
     * overwhelming the TLS stack on macOS (libgnutls issues) */
    s_shared_session = soup_session_new_with_options(
      "max-conns", 10,           /* Total max connections */
      "max-conns-per-host", 2,   /* Max per host */
      "timeout", 30,             /* 30 second timeout */
      NULL);
    
    g_debug("gnostr: Created shared SoupSession with conservative limits");
  }
  
  g_mutex_unlock(&s_session_mutex);
  return s_shared_session;
}

void gnostr_cleanup_shared_soup_session(void) {
  g_mutex_lock(&s_session_mutex);
  
  if (s_shared_session) {
    /* Cancel any pending requests before cleanup */
    soup_session_abort(s_shared_session);
    g_clear_object(&s_shared_session);
    g_debug("gnostr: Cleaned up shared SoupSession");
  }
  
  g_mutex_unlock(&s_session_mutex);
}

#endif /* HAVE_SOUP3 */

gboolean str_has_prefix_http(const char *s) {
  return s && (g_str_has_prefix(s, "http://") || g_str_has_prefix(s, "https://"));
}