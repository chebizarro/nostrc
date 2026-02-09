#include "utils.h"
#include "nostr_nip19.h"
#include "../storage_ndb.h"
#include "nostr-filter.h"
#include "json.h"
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
    /* nostrc-bnr1: Increased max-conns-per-host from 2 to 6 to reduce
     * connection starvation. With 12 concurrent avatar fetches all targeting
     * the same CDN host (nostr.build, void.cat, etc.), only 2 slots caused
     * banner requests to queue for a long time. 6 matches Chrome's default. */
    s_shared_session = soup_session_new_with_options(
      "max-conns", 24,           /* Total max connections */
      "max-conns-per-host", 6,   /* Max per host (was 2) */
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

/* Cache query adapter: check nostrdb before hitting the network.
 * Serializes NostrFilters to JSON and queries storage_ndb.
 * Thread-safe — called from GTask worker thread. */
static GPtrArray *
ndb_cache_query(NostrFilters *filters, gpointer user_data G_GNUC_UNUSED)
{
    if (!filters || filters->count == 0)
        return NULL;

    /* Build JSON array of serialized filters: [filter1, filter2, ...] */
    GString *json = g_string_new("[");
    for (size_t i = 0; i < filters->count; i++) {
        char *fj = nostr_filter_serialize(&filters->filters[i]);
        if (!fj) continue;
        if (i > 0) g_string_append_c(json, ',');
        g_string_append(json, fj);
        free(fj);
    }
    g_string_append_c(json, ']');

    /* Query nostrdb */
    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_string_free(json, TRUE);
        return NULL;
    }

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_query(txn, json->str, &results, &count);
    g_string_free(json, TRUE);
    storage_ndb_end_query(txn);

    if (rc != 0 || count <= 0) {
        if (results)
            storage_ndb_free_results(results, count);
        return NULL;
    }

    /* Convert to GPtrArray of owned JSON strings */
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < count; i++) {
        if (results[i])
            g_ptr_array_add(out, g_strdup(results[i]));
    }
    storage_ndb_free_results(results, count);

    return out;
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
    /* Check nostrdb cache before hitting the network */
    gnostr_pool_set_cache_query(s_shared_query_pool, ndb_cache_query, NULL, NULL);
    /* Auto-persist all fetched events to nostrdb */
    gnostr_pool_set_event_sink(s_shared_query_pool, ndb_event_sink, NULL, NULL);
    g_debug("gnostr: Created shared GNostrPool with nostrdb cache + event sink");
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

void gnostr_pool_wire_ndb(GNostrPool *pool) {
  g_return_if_fail(pool != NULL);
  gnostr_pool_set_cache_query(pool, ndb_cache_query, NULL, NULL);
  gnostr_pool_set_event_sink(pool, ndb_event_sink, NULL, NULL);
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