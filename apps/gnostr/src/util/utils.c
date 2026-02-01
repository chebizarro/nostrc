#include "utils.h"

#ifdef HAVE_SOUP3

/* Shared SoupSession singleton - avoids TLS cleanup issues on macOS */
static SoupSession *s_shared_session = NULL;
static GMutex s_session_mutex;
static gboolean s_session_shutdown = FALSE;

SoupSession *gnostr_get_shared_soup_session(void) {
  g_mutex_lock(&s_session_mutex);

  /* nostrc-b1vg: Prevent creating new session after shutdown to avoid
   * use-after-free in TLS certificate cleanup. Return NULL and let
   * callers handle gracefully (most already check for NULL). */
  if (s_session_shutdown) {
    g_mutex_unlock(&s_session_mutex);
    g_debug("gnostr: Rejecting soup session request after shutdown");
    return NULL;
  }

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

  /* Mark as shutdown BEFORE cleanup to prevent new requests */
  s_session_shutdown = TRUE;

  if (s_shared_session) {
    /* Cancel any pending requests before cleanup */
    soup_session_abort(s_shared_session);
    g_clear_object(&s_shared_session);
    g_debug("gnostr: Cleaned up shared SoupSession");
  }

  g_mutex_unlock(&s_session_mutex);
}

#endif /* HAVE_SOUP3 */

/* Shared GnostrSimplePool singleton for one-shot queries */
static GnostrSimplePool *s_shared_query_pool = NULL;
static GMutex s_query_pool_mutex;
static gboolean s_query_pool_shutdown = FALSE;

GnostrSimplePool *gnostr_get_shared_query_pool(void) {
  g_mutex_lock(&s_query_pool_mutex);

  /* nostrc-b1vg: Prevent creating new pool after shutdown */
  if (s_query_pool_shutdown) {
    g_mutex_unlock(&s_query_pool_mutex);
    g_debug("gnostr: Rejecting query pool request after shutdown");
    return NULL;
  }

  if (!s_shared_query_pool) {
    s_shared_query_pool = gnostr_simple_pool_new();
    g_debug("gnostr: Created shared query pool for relay queries");
  }

  g_mutex_unlock(&s_query_pool_mutex);
  return s_shared_query_pool;
}

void gnostr_cleanup_shared_query_pool(void) {
  g_mutex_lock(&s_query_pool_mutex);

  /* Mark as shutdown BEFORE cleanup */
  s_query_pool_shutdown = TRUE;

  if (s_shared_query_pool) {
    g_clear_object(&s_shared_query_pool);
    g_debug("gnostr: Cleaned up shared query pool");
  }

  g_mutex_unlock(&s_query_pool_mutex);
}

gboolean str_has_prefix_http(const char *s) {
  return s && (g_str_has_prefix(s, "http://") || g_str_has_prefix(s, "https://"));
}