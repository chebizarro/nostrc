#include "utils.h"
#ifndef GNOSTR_MEDIA_POLICY_TESTING
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-filter.h"
#include "json.h"
#endif
#include <gio/gio.h>
#include <string.h>

GQuark
gnostr_media_policy_error_quark(void)
{
  return g_quark_from_static_string("gnostr-media-policy-error-quark");
}

static gboolean
inet_address_is_public(GInetAddress *address)
{
  if (!address ||
      g_inet_address_get_is_any(address) ||
      g_inet_address_get_is_loopback(address) ||
      g_inet_address_get_is_link_local(address) ||
      g_inet_address_get_is_site_local(address) ||
      g_inet_address_get_is_multicast(address))
    return FALSE;

  const guint8 *bytes = g_inet_address_to_bytes(address);
  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4) {
    return !(
      bytes[0] == 0 ||
      bytes[0] == 10 ||
      bytes[0] == 127 ||
      (bytes[0] == 100 && (bytes[1] & 0xc0) == 0x40) ||
      (bytes[0] == 169 && bytes[1] == 254) ||
      (bytes[0] == 172 && (bytes[1] & 0xf0) == 16) ||
      (bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 0) ||
      (bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 2) ||
      (bytes[0] == 192 && bytes[1] == 88 && bytes[2] == 99) ||
      (bytes[0] == 192 && bytes[1] == 168) ||
      (bytes[0] == 198 && (bytes[1] == 18 || bytes[1] == 19)) ||
      (bytes[0] == 198 && bytes[1] == 51 && bytes[2] == 100) ||
      (bytes[0] == 203 && bytes[1] == 0 && bytes[2] == 113) ||
      bytes[0] >= 224);
  }

  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6) {
    if ((bytes[0] & 0xfe) == 0xfc ||
        (bytes[0] == 0x20 && bytes[1] == 0x01 &&
         (bytes[2] & 0xfe) == 0x00) ||
        (bytes[0] == 0x20 && bytes[1] == 0x01 &&
         bytes[2] == 0x0d && bytes[3] == 0xb8) ||
        (bytes[0] == 0x20 && bytes[1] == 0x02) ||
        (bytes[0] == 0x3f && bytes[1] == 0xff &&
         (bytes[2] & 0xf0) == 0x00))
      return FALSE;
    static const guint8 v4_mapped_prefix[12] =
      { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (memcmp(bytes, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0) {
      g_autoptr(GInetAddress) mapped =
        g_inet_address_new_from_bytes(bytes + 12, G_SOCKET_FAMILY_IPV4);
      return inet_address_is_public(mapped);
    }
    /* Only IPv6 global unicast (2000::/3) is eligible. */
    return (bytes[0] & 0xe0) == 0x20;
  }
  return FALSE;
}

gboolean
gnostr_media_url_is_safe(const char *url, GError **error)
{
  if (!url || !*url) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_INVALID_URL,
                        "Media URL is empty");
    return FALSE;
  }

  g_autoptr(GError) parse_error = NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, &parse_error);
  const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const char *host = uri ? g_uri_get_host(uri) : NULL;
  if (!uri || !scheme || !host || !*host ||
      (g_ascii_strcasecmp(scheme, "http") != 0 &&
       g_ascii_strcasecmp(scheme, "https") != 0)) {
    g_set_error(error, GNOSTR_MEDIA_POLICY_ERROR,
                GNOSTR_MEDIA_POLICY_ERROR_INVALID_URL,
                "Media URL must be an absolute HTTP(S) URL: %s",
                parse_error ? parse_error->message : "invalid URL");
    return FALSE;
  }

  if (g_uri_get_userinfo(uri) != NULL) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_CREDENTIALS,
                        "Credentials in media URLs are not allowed");
    return FALSE;
  }

  gint port = g_uri_get_port(uri);
  if (port != -1 && port != 80 && port != 443) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_PORT,
                        "Media URLs may only use ports 80 or 443");
    return FALSE;
  }

  if (g_ascii_strcasecmp(host, "localhost") == 0 ||
      g_str_has_suffix(host, ".localhost")) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS,
                        "Local media destinations are not allowed");
    return FALSE;
  }

  g_autoptr(GInetAddress) literal = g_inet_address_new_from_string(host);
  if (literal) {
    if (inet_address_is_public(literal))
      return TRUE;
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS,
                        "Private, local, or reserved media destination");
    return FALSE;
  }

  g_autoptr(GResolver) resolver = g_resolver_get_default();
  g_autoptr(GError) resolve_error = NULL;
  GList *addresses = g_resolver_lookup_by_name(resolver, host, NULL,
                                               &resolve_error);
  if (!addresses) {
    g_set_error(error, GNOSTR_MEDIA_POLICY_ERROR,
                GNOSTR_MEDIA_POLICY_ERROR_RESOLUTION,
                "Could not resolve media host: %s",
                resolve_error ? resolve_error->message : "unknown error");
    return FALSE;
  }

  gboolean safe = TRUE;
  for (GList *it = addresses; it; it = it->next) {
    if (!inet_address_is_public(G_INET_ADDRESS(it->data))) {
      safe = FALSE;
      break;
    }
  }
  g_resolver_free_addresses(addresses);
  if (!safe) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_ADDRESS,
                        "Media host resolves to a private, local, or reserved address");
  }
  return safe;
}

static gint
uri_effective_port(GUri *uri)
{
  gint port = g_uri_get_port(uri);
  if (port >= 0)
    return port;
  return g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 ? 443 : 80;
}

gboolean
gnostr_media_redirect_is_safe(const char *from_url,
                              const char *location,
                              char **out_url,
                              GError **error)
{
  if (out_url)
    *out_url = NULL;
  if (!from_url || !location || !*location) {
    g_set_error_literal(error, GNOSTR_MEDIA_POLICY_ERROR,
                        GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_REDIRECT,
                        "Redirect is missing a source or destination");
    return FALSE;
  }

  g_autoptr(GError) resolve_error = NULL;
  g_autofree char *resolved =
    g_uri_resolve_relative(from_url, location, G_URI_FLAGS_NONE, &resolve_error);
  g_autoptr(GUri) from = g_uri_parse(from_url, G_URI_FLAGS_NONE, NULL);
  g_autoptr(GUri) to = resolved
    ? g_uri_parse(resolved, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!from || !to ||
      !g_uri_get_scheme(from) || !g_uri_get_scheme(to) ||
      !g_uri_get_host(from) || !g_uri_get_host(to) ||
      g_ascii_strcasecmp(g_uri_get_scheme(from), g_uri_get_scheme(to)) != 0 ||
      g_ascii_strcasecmp(g_uri_get_host(from), g_uri_get_host(to)) != 0 ||
      uri_effective_port(from) != uri_effective_port(to)) {
    g_set_error(error, GNOSTR_MEDIA_POLICY_ERROR,
                GNOSTR_MEDIA_POLICY_ERROR_UNSAFE_REDIRECT,
                "Cross-origin media redirect is not allowed%s%s",
                resolve_error ? ": " : "",
                resolve_error ? resolve_error->message : "");
    return FALSE;
  }

  if (!gnostr_media_url_is_safe(resolved, error))
    return FALSE;
  if (out_url)
    *out_url = g_steal_pointer(&resolved);
  return TRUE;
}

#ifndef GNOSTR_MEDIA_POLICY_TESTING
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

  SoupSession *session = g_object_ref(s_shared_session);
  g_mutex_unlock(&s_session_mutex);
  return session;
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

/* nostrc-jvdv.2: Centralised privacy gate for remote media fetching.
 * Reads the "load-remote-media" boolean from org.gnostr.Client GSettings.
 * Returns FALSE if the setting is disabled or the schema is unavailable. */
#define GNOSTR_CLIENT_SCHEMA_ID "org.gnostr.Client"
#define GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY "load-remote-media"

static GSettings *s_remote_media_settings;
static gint s_remote_media_allowed;
static gsize s_remote_media_settings_initialized;

static void
on_remote_media_setting_changed(GSettings *settings,
                                gchar *key,
                                gpointer user_data)
{
  (void)key;
  (void)user_data;
  g_atomic_int_set(&s_remote_media_allowed,
                   g_settings_get_boolean(settings,
                                          GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY));
}

static void
ensure_remote_media_settings(void)
{
  if (!g_once_init_enter(&s_remote_media_settings_initialized))
    return;

  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    g_autoptr(GSettingsSchema) schema =
      g_settings_schema_source_lookup(source, GNOSTR_CLIENT_SCHEMA_ID, TRUE);
    if (schema &&
        g_settings_schema_has_key(schema, GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY)) {
      s_remote_media_settings = g_settings_new_full(schema, NULL, NULL);
      g_atomic_int_set(&s_remote_media_allowed,
                       g_settings_get_boolean(
                         s_remote_media_settings,
                         GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY));
      g_signal_connect(s_remote_media_settings,
                       "changed::" GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY,
                       G_CALLBACK(on_remote_media_setting_changed), NULL);
    } else {
      g_debug("Remote media: GSettings schema/key unavailable; blocking");
    }
  }

  g_once_init_leave(&s_remote_media_settings_initialized, 1);
}

gboolean
gnostr_is_remote_media_allowed(void)
{
  ensure_remote_media_settings();
  /* GSettings reads are thread-safe and authoritative even if the singleton
   * was first touched from a worker whose thread-default context is not
   * iterated. The changed signal keeps the cached fast value synchronized for
   * observers, while this policy check cannot become stale. */
  if (s_remote_media_settings)
    return g_settings_get_boolean(s_remote_media_settings,
                                  GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY);
  return g_atomic_int_get(&s_remote_media_allowed) != 0;
}

#endif /* HAVE_SOUP3 */

gboolean
gnostr_media_fetch_intent_is_allowed(GnostrMediaFetchIntent intent)
{
  if (intent == GNOSTR_MEDIA_FETCH_USER_INITIATED)
    return TRUE;
#ifdef HAVE_SOUP3
  return intent == GNOSTR_MEDIA_FETCH_AUTOMATIC &&
         gnostr_is_remote_media_allowed();
#else
  return FALSE;
#endif
}

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
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10, NULL);
    if (rc != 0 || !txn) {
        g_string_free(json, TRUE);
        return NULL;
    }

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_query(txn, json->str, &results, &count, NULL);
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
    g_autoptr(GNostrRelay) relay = gnostr_relay_new(url);
    if (!relay) { d->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("publish_async: connect failed %s: %s", url,
              conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
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

#endif /* !GNOSTR_MEDIA_POLICY_TESTING */
