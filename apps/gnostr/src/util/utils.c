#include "utils.h"
#include "nostr_nip19.h"
#include "../storage_ndb.h"
#include <string.h>

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

/* Event sink adapter: persists relay query results to nostrdb automatically */
static void
ndb_event_sink(GPtrArray *jsons, gpointer user_data G_GNUC_UNUSED)
{
    storage_ndb_ingest_events_async(jsons); /* takes ownership */
}

/* Shared GNostrPool singleton for one-shot queries (hq-r248b) */
static GNostrPool *s_shared_query_pool = NULL;
static GMutex s_query_pool_mutex;
static gboolean s_query_pool_shutdown = FALSE;

GNostrPool *gnostr_get_shared_query_pool(void) {
  g_mutex_lock(&s_query_pool_mutex);

  /* nostrc-b1vg: Prevent creating new pool after shutdown */
  if (s_query_pool_shutdown) {
    g_mutex_unlock(&s_query_pool_mutex);
    g_debug("gnostr: Rejecting query pool request after shutdown");
    return NULL;
  }

  if (!s_shared_query_pool) {
    s_shared_query_pool = gnostr_pool_new();
    /* Auto-persist all fetched events to nostrdb */
    gnostr_pool_set_event_sink(s_shared_query_pool, ndb_event_sink, NULL, NULL);
    g_debug("gnostr: Created shared GNostrPool with nostrdb event sink");
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

/* Validate a 64-char hex string (only 0-9, a-f, A-F) */
static gboolean is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (int i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

gchar *gnostr_ensure_hex_pubkey(const char *input) {
  if (!input || !*input) return NULL;

  /* Fast path: already 64-char hex */
  if (is_hex64(input))
    return g_strdup(input);

  /* Bech32 path: npub1... or nprofile1... */
  if (g_str_has_prefix(input, "npub1") || g_str_has_prefix(input, "nprofile1")) {
    GError *error = NULL;
    GNostrNip19 *nip19 = gnostr_nip19_decode(input, &error);
    if (!nip19) {
      g_warning("gnostr_ensure_hex_pubkey: failed to decode '%.*s...': %s",
                10, input, error ? error->message : "unknown");
      g_clear_error(&error);
      return NULL;
    }
    const gchar *hex = gnostr_nip19_get_pubkey(nip19);
    gchar *result = hex ? g_strdup(hex) : NULL;
    g_object_unref(nip19);
    return result;
  }

  /* Unknown format */
  g_warning("gnostr_ensure_hex_pubkey: unrecognized format '%.*s...' (len=%zu)",
            10, input, strlen(input));
  return NULL;
}