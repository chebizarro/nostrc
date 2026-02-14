#include "utils.h"
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-filter.h"
#include "json.h"
#include <gio/gio.h>
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

/* gnostr_ensure_hex_pubkey moved to nostr-gobject/src/nostr_utils.c */

/* hq-gflmf: Shared async relay publish — moves connect+publish loops off
 * the main thread for all callers (bookmarks, pin_list, mute_list, etc.). */

typedef struct {
  NostrEvent *event;
  GPtrArray  *relay_urls;
  guint       success_count;
  guint       fail_count;
  GnostrRelayPublishDoneCallback callback;
  gpointer    user_data;
} RelayPublishWorkData;

static void
relay_publish_work_data_free(gpointer p)
{
  RelayPublishWorkData *d = (RelayPublishWorkData *)p;
  if (!d) return;
  if (d->event) nostr_event_free(d->event);
  if (d->relay_urls) g_ptr_array_free(d->relay_urls, TRUE);
  g_free(d);
}

static void
relay_publish_worker(GTask *task, gpointer source_object,
                     gpointer task_data, GCancellable *cancellable)
{
  (void)source_object; (void)cancellable;
  RelayPublishWorkData *d = (RelayPublishWorkData *)task_data;

  for (guint i = 0; i < d->relay_urls->len; i++) {
    const gchar *url = (const gchar *)g_ptr_array_index(d->relay_urls, i);
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) { d->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("publish_async: connect failed %s: %s", url,
              conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      d->fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, d->event, &pub_err)) {
      d->success_count++;
    } else {
      g_debug("publish_async: publish failed %s: %s", url,
              pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      d->fail_count++;
    }
    g_object_unref(relay);
  }

  g_task_return_boolean(task, d->success_count > 0);
}

static void
relay_publish_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object; (void)user_data;
  GTask *task = G_TASK(res);
  RelayPublishWorkData *d = g_task_get_task_data(task);
  GError *error = NULL;
  g_task_propagate_boolean(task, &error);
  g_clear_error(&error);

  if (d->callback) {
    d->callback(d->success_count, d->fail_count, d->user_data);
  }
}

void
gnostr_publish_to_relays_async(NostrEvent *event,
                                GPtrArray *relay_urls,
                                GnostrRelayPublishDoneCallback callback,
                                gpointer user_data)
{
  if (!event || !relay_urls || relay_urls->len == 0) {
    if (callback) callback(0, 0, user_data);
    if (event) nostr_event_free(event);
    if (relay_urls) g_ptr_array_free(relay_urls, TRUE);
    return;
  }

  RelayPublishWorkData *d = g_new0(RelayPublishWorkData, 1);
  d->event = event;
  d->relay_urls = relay_urls;
  d->callback = callback;
  d->user_data = user_data;

  GTask *task = g_task_new(NULL, NULL, relay_publish_done, NULL);
  g_task_set_task_data(task, d, relay_publish_work_data_free);
  g_task_run_in_thread(task, relay_publish_worker);
  g_object_unref(task);
}
