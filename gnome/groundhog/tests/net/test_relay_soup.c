/* G09 network modes and Tor, fail closed (privacy charter §4.2, §4.3, §9.2
 * NT-5..NT-9, PT-5). Everything is local:
 *  - H3, the SOCKS5 fixture (socks5-fixture.h), in front of
 *  - H2, the wire relay (tests/relay/wire-relay.h), and a loopback HTTP server
 *    for NIP-05 and NIP-11;
 *  - a tripwire listener that no test may ever dial in Tor mode;
 *  - a GResolver installed as GLib's default that records every local DNS
 *    lookup: in Tor mode there must be none (remote DNS through the proxy).
 * Waits iterate the main context; their deadlines are failure bounds only. */
#include "gh-net-http.h"
#include "gh-net-session.h"
#include "gh-nip05.h"
#include "gh-nip11.h"
#include "gh-relay-net.h"
#include "gh-relay-soup.h"
#include "socks5-fixture.h"
#include "wire-relay.h"

#define ONION "groundhogtestrelayqw4yvkpvrbwe5wtamgzhhdrhm5k2ww7m3tgdwbzd.onion"
#define SECRET "0000000000000000000000000000000000000000000000000000000000000001"
#define HEX_BOB "b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0"
#define RELAY_SELF "5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f5e1f"

/* ---- the DNS tripwire ----------------------------------------------------------- */

static GPtrArray *dns_names; /* every name anything asked the local resolver for */

#define TRIP_TYPE_RESOLVER (trip_resolver_get_type())
G_DECLARE_FINAL_TYPE(TripResolver, trip_resolver, TRIP, RESOLVER, GResolver)
struct _TripResolver {
  GResolver parent_instance;
};
G_DEFINE_FINAL_TYPE(TripResolver, trip_resolver, G_TYPE_RESOLVER)

/* nostrc-qi5e: a public-looking name that resolves to a loopback address
 * (DNS rebinding); every other name is refused. Both are recorded. */
#define REBIND_HOST "rebind.groundhog.test"

static GList *
trip_lookup_flags(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                  GCancellable *cancellable, GError **error)
{
  (void)resolver;
  (void)cancellable;
  g_ptr_array_add(dns_names, g_strdup(name));
  if (g_str_equal(name, REBIND_HOST) && !(flags & G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY))
    return g_list_append(NULL, g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4));
  g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "tripwire: %s", name);
  return NULL;
}

static GList *
trip_lookup(GResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
  return trip_lookup_flags(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                           error);
}

static void
trip_lookup_flags_async(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new(resolver, cancellable, callback, data);
  GError *error = NULL;
  GList *addresses = trip_lookup_flags(resolver, name, flags, cancellable, &error);
  if (addresses)
    g_task_return_pointer(task, addresses, (GDestroyNotify)g_resolver_free_addresses);
  else
    g_task_return_error(task, error);
}

static void
trip_lookup_async(GResolver *resolver, const gchar *name, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  trip_lookup_flags_async(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                          callback, data);
}

static GList *
trip_lookup_finish(GResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static GList *
trip_records(GResolver *resolver, const gchar *rrname, GResolverRecordType type,
             GCancellable *cancellable, GError **error)
{
  (void)resolver;
  (void)type;
  (void)cancellable;
  g_ptr_array_add(dns_names, g_strdup(rrname));
  g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "tripwire: %s", rrname);
  return NULL;
}

static void
trip_records_async(GResolver *resolver, const gchar *rrname, GResolverRecordType type,
                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new(resolver, cancellable, callback, data);
  GError *error = NULL;
  trip_records(resolver, rrname, type, cancellable, &error);
  g_task_return_error(task, error);
}

static void
trip_resolver_class_init(TripResolverClass *klass)
{
  GResolverClass *resolver = G_RESOLVER_CLASS(klass);
  resolver->lookup_by_name = trip_lookup;
  resolver->lookup_by_name_async = trip_lookup_async;
  resolver->lookup_by_name_finish = trip_lookup_finish;
  resolver->lookup_by_name_with_flags = trip_lookup_flags;
  resolver->lookup_by_name_with_flags_async = trip_lookup_flags_async;
  resolver->lookup_by_name_with_flags_finish = trip_lookup_finish;
  resolver->lookup_records = trip_records;
  resolver->lookup_records_async = trip_records_async;
  resolver->lookup_records_finish = trip_lookup_finish;
}

static void
trip_resolver_init(TripResolver *self)
{
  (void)self;
}

/* ---- the tripwire listener ------------------------------------------------------ */

typedef struct {
  GSocketService *service;
  guint16 port;
  guint accepted;
} Tripwire;

static gboolean
on_tripped(GSocketService *service, GSocketConnection *connection, GObject *source,
           gpointer data)
{
  (void)service;
  (void)source;
  ((Tripwire *)data)->accepted++;
  (void)g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

static void
tripwire_init(Tripwire *tripwire)
{
  tripwire->service = g_socket_service_new();
  g_autoptr(GError) error = NULL;
  tripwire->port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(tripwire->service),
                                                       NULL, &error);
  g_assert_no_error(error);
  g_signal_connect(tripwire->service, "incoming", G_CALLBACK(on_tripped), tripwire);
  g_socket_service_start(tripwire->service);
}

static void
tripwire_clear(Tripwire *tripwire)
{
  g_signal_handlers_disconnect_by_data(tripwire->service, tripwire);
  g_socket_service_stop(tripwire->service);
  /* The cancelled accept leaves before its socket closes. */
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_socket_listener_close(G_SOCKET_LISTENER(tripwire->service));
  g_clear_object(&tripwire->service);
}

/* ---- helpers --------------------------------------------------------------------- */

typedef gboolean (*Condition)(gpointer data);

static void
spin_until(Condition condition, gpointer data)
{
  WaitState wait = { 0 };
  guint bound = g_timeout_add_seconds(20, expire, &wait);
  while (!condition(data) && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!wait.timed_out)
    g_source_remove(bound);
  g_assert_true(condition(data));
}

/* Lets the main loop run for a while, e.g. through a reconnect backoff, to
 * show that something does NOT happen; never a progress signal. */
static void
run_for(guint milliseconds)
{
  WaitState wait = { 0 };
  g_timeout_add(milliseconds, expire, &wait);
  while (!wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
}

static guint16
url_port(const gchar *url)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  return (guint16)g_uri_get_port(uri);
}

static gboolean
have_tls(void)
{
  return g_tls_backend_supports_tls(g_tls_backend_get_default());
}

static gchar *
closed_address(void)
{
  guint16 port = 0;
  g_autofree gchar *url = unused_relay_url(&port);
  return g_strdup_printf("127.0.0.1:%u", port);
}

static gchar *
signed_json(const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, content);
  g_assert_cmpint(nostr_event_sign(event, SECRET), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

static NostrFilters *
any_filters(void)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  return filters;
}

static gboolean
is_hex(const gchar *text, gsize length)
{
  if (!text || strlen(text) != length)
    return FALSE;
  for (const gchar *p = text; *p; p++)
    if (!g_ascii_isxdigit(*p) || g_ascii_isupper(*p))
      return FALSE;
  return TRUE;
}

typedef struct {
  guint errors;
  guint eoses;
  guint disconnected;
  gchar *last_error;
} ScopeSeen;

static void
on_scope(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  ScopeSeen *seen = data;
  if (update->notice == GH_RELAY_NOTICE_ERROR) {
    seen->errors++;
    g_free(seen->last_error);
    seen->last_error = g_strdup(update->detail);
  } else if (update->notice == GH_RELAY_NOTICE_EOSE) {
    seen->eoses++;
  } else if (update->notice == GH_RELAY_NOTICE_DISCONNECTED) {
    seen->disconnected++;
  }
}

typedef struct {
  guint done;
  GhRelayPublishOutcome outcome;
  gchar *message;
} PublishSeen;

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  (void)publish;
  PublishSeen *seen = data;
  seen->outcome = result->outcome;
  g_free(seen->message);
  seen->message = g_strdup(result->message);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)publish;
  (void)summary;
  ((PublishSeen *)data)->done++;
}

static GhRelayScope *
open_scope(guint64 generation, const gchar *url, ScopeSeen *seen)
{
  GhRelayScope *scope = gh_relay_scope_new(generation, any_filters(), on_scope, seen);
  g_assert_true(gh_relay_scope_add_url(scope, url, NULL));
  gh_relay_scope_start(scope);
  return scope;
}

static GhRelayPublish *
start_publish(const gchar *url, const gchar *content, PublishSeen *seen)
{
  g_autofree gchar *json = signed_json(content);
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new(3, json, on_publish_update, on_publish_done,
                                                 seen, &error);
  g_assert_no_error(error);
  gh_relay_publish_set_deadline(publish, 15);
  g_assert_true(gh_relay_publish_add_url(publish, url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  return publish;
}

static gchar *
onion_url(const gchar *scheme, guint16 port, const gchar *path)
{
  return g_strdup_printf("%s://" ONION ":%u%s", scheme, port, path);
}

typedef struct {
  GhNetSession *session;
  GhNetTorState state;
} TorWait;

static gboolean
tor_state_is(gpointer data)
{
  TorWait *wait = data;
  return gh_net_session_get_tor_state(wait->session) == wait->state;
}

static void
wait_tor_state(GhNetSession *session, GhNetTorState state)
{
  TorWait wait = { session, state };
  spin_until(tor_state_is, &wait);
}

static GhNetSession *
install_session(GhNetMode mode, const gchar *tor_address)
{
  GhNetSession *session = gh_net_session_new(NULL);
  gh_net_session_set_mode(session, mode, tor_address);
  gh_relay_net_install(session);
  return session;
}

static void
uninstall_session(GhNetSession *session)
{
  gh_relay_net_install(NULL);
  g_object_unref(session);
  /* Let queued reports and closes run before the next test. */
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static gboolean
count_at_least(gpointer data)
{
  const guint *const *pair = data;
  return *pair[0] >= *pair[1];
}

static void
wait_at_least(const guint *counter, guint count)
{
  const guint *pair[] = { counter, &count };
  spin_until(count_at_least, pair);
}

/* ---- NT-9 and the pure rules ------------------------------------------------------ */

static gchar **
lookup(GProxyResolver *resolver, const gchar *uri)
{
  g_autoptr(GError) error = NULL;
  gchar **proxies = g_proxy_resolver_lookup(resolver, uri, NULL, &error);
  g_assert_no_error(error);
  return proxies;
}

static void
test_modes(void)
{
  g_assert_cmpint(gh_net_mode_from_string("system"), ==, GH_NET_MODE_SYSTEM);
  g_assert_cmpint(gh_net_mode_from_string("none"), ==, GH_NET_MODE_NONE);
  g_assert_cmpint(gh_net_mode_from_string("tor"), ==, GH_NET_MODE_TOR);
  /* Fail closed: anything unknown is the strictest mode. */
  g_assert_cmpint(gh_net_mode_from_string("bogus"), ==, GH_NET_MODE_TOR);
  g_assert_cmpint(gh_net_mode_from_string(NULL), ==, GH_NET_MODE_TOR);

  g_autoptr(GhNetSession) session = gh_net_session_new(NULL);
  g_assert_cmpint(gh_net_session_get_mode(session), ==, GH_NET_MODE_SYSTEM);
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_OFF);

  /* System: GLib's default resolver, i.e. the desktop proxy settings. */
  g_autoptr(GProxyResolver) system = gh_net_session_dup_resolver(session, "x", NULL);
  g_assert_true(system == g_proxy_resolver_get_default());

  /* No Proxy: direct for everything. */
  gh_net_session_set_mode(session, GH_NET_MODE_NONE, NULL);
  g_autoptr(GProxyResolver) none = gh_net_session_dup_resolver(session, "x", NULL);
  g_auto(GStrv) direct = lookup(none, "wss://relay.example.org/");
  g_assert_cmpuint(g_strv_length(direct), ==, 1);
  g_assert_cmpstr(direct[0], ==, "direct://");

  /* Tor: one SOCKS5 proxy for every URI, credentials per label, never
   * direct. */
  guint64 serial = gh_net_session_get_serial(session);
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, "127.0.0.1:9050");
  g_assert_cmpuint(gh_net_session_get_serial(session), ==, serial + 1);
  g_autoptr(GProxyResolver) tor_a = gh_net_session_dup_resolver(session, "1/scope/a", NULL);
  g_autoptr(GProxyResolver) tor_a2 = gh_net_session_dup_resolver(session, "1/scope/a", NULL);
  g_autoptr(GProxyResolver) tor_b = gh_net_session_dup_resolver(session, "1/scope/b", NULL);
  const gchar *uris[] = { "wss://relay.example.org/", "https://example.com/x",
                          "ws://" ONION "/", "http://127.0.0.1:1/" };
  g_autofree gchar *first = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(uris); i++) {
    g_auto(GStrv) a = lookup(tor_a, uris[i]);
    g_auto(GStrv) a2 = lookup(tor_a2, uris[i]);
    g_auto(GStrv) b = lookup(tor_b, uris[i]);
    g_assert_cmpuint(g_strv_length(a), ==, 1);
    g_assert_cmpuint(g_strv_length(b), ==, 1);
    g_assert_true(g_str_has_prefix(a[0], "socks5://"));
    g_assert_true(g_str_has_suffix(a[0], "@127.0.0.1:9050"));
    g_assert_cmpstr(a[0], ==, a2[0]); /* same label: same circuit */
    g_assert_cmpstr(a[0], !=, b[0]);  /* another label: another circuit */
    if (!first)
      first = g_strdup(a[0]);
    g_assert_cmpstr(a[0], ==, first);
  }
  g_autofree gchar *user = gh_net_isolation_username("1/scope/a");
  g_assert_true(is_hex(user, 16));
  g_autofree gchar *expected = g_strdup_printf("socks5://%s:", user);
  g_assert_true(g_str_has_prefix(first, expected));
  /* A random label for each fresh connection. */
  g_autoptr(GProxyResolver) fresh1 = gh_net_proxy_resolver_new(GH_NET_MODE_TOR, "127.0.0.1:9050",
                                                               NULL, NULL);
  g_autoptr(GProxyResolver) fresh2 = gh_net_proxy_resolver_new(GH_NET_MODE_TOR, "127.0.0.1:9050",
                                                               NULL, NULL);
  g_auto(GStrv) f1 = lookup(fresh1, uris[0]);
  g_auto(GStrv) f2 = lookup(fresh2, uris[0]);
  g_assert_cmpstr(f1[0], !=, f2[0]);
  /* IPv6 proxies are bracketed; a bad address never means "direct". */
  g_autoptr(GProxyResolver) six = gh_net_proxy_resolver_new(GH_NET_MODE_TOR, "[::1]:9150",
                                                            "l", NULL);
  g_auto(GStrv) s6 = lookup(six, uris[0]);
  g_assert_true(g_str_has_suffix(s6[0], "@[::1]:9150"));
  const gchar *bad[] = { "", "nonsense:", "socks5://127.0.0.1:9050", "user@host:1", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(gh_net_proxy_resolver_new(GH_NET_MODE_TOR, bad[i], "l", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  g_autoptr(GError) null_error = NULL;
  g_assert_null(gh_net_proxy_resolver_new(GH_NET_MODE_TOR, NULL, "l", &null_error));
  g_assert_error(null_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

/* PT-5 and NT-8: the relay URL policy, before anything is resolved. */
static void
test_url_policy(void)
{
  static const struct {
    const gchar *url;
    gboolean direct;
    gboolean tor;
  } cases[] = {
    { "wss://relay.example.org", TRUE, TRUE },
    { "wss://" ONION, FALSE, TRUE },
    { "ws://" ONION ":8080/relay", FALSE, TRUE },
    { "ws://WWW." "GROUNDHOGTESTRELAYQW4YVKPVRBWE5WTAMGZHHDRHM5K2WW7M3TGDWBZD.ONION", FALSE, TRUE },
    { "ws://relay.example.org", FALSE, FALSE },
    { "ws://127.0.0.1:7777/relay", TRUE, TRUE },
    { "ws://[::1]:7777", TRUE, TRUE },
    { "ws://localhost:7777", TRUE, TRUE },
    { "https://relay.example.org", FALSE, FALSE },
    { "wss://", FALSE, FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_assert_cmpint(gh_net_relay_url_allowed(GH_NET_MODE_SYSTEM, cases[i].url, NULL), ==,
                    cases[i].direct);
    g_assert_cmpint(gh_net_relay_url_allowed(GH_NET_MODE_NONE, cases[i].url, NULL), ==,
                    cases[i].direct);
    g_assert_cmpint(gh_net_relay_url_allowed(GH_NET_MODE_TOR, cases[i].url, NULL), ==,
                    cases[i].tor);
  }
  g_assert_true(gh_net_host_is_onion("abc.onion"));
  g_assert_true(gh_net_host_is_onion("abc.ONION."));
  g_assert_false(gh_net_host_is_onion(".onion"));
  g_assert_false(gh_net_host_is_onion("onion"));
  g_assert_false(gh_net_host_is_onion("abc.onion.example.org"));

  /* Through the dispatcher in a direct mode: a .onion or plaintext relay is
   * refused at open, with no connection and no lookup. */
  GhNetSession *session = install_session(GH_NET_MODE_NONE, NULL);
  ScopeSeen seen = { 0 };
  g_autofree gchar *onion = onion_url("ws", 7777, "/relay");
  GhRelayScope *scope = gh_relay_scope_new(1, any_filters(), on_scope, &seen);
  g_assert_true(gh_relay_scope_add_url(scope, onion, NULL));
  g_assert_true(gh_relay_scope_add_url(scope, "ws://relay.example.org/", NULL));
  gh_relay_scope_start(scope);
  g_assert_cmpuint(seen.errors, ==, 2); /* refused synchronously */
  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(onion, "onion outside tor", &published);
  g_assert_cmpuint(published.done, ==, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_nonnull(strstr(published.message, "Tor"));
  gh_relay_publish_unref(publish);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  g_assert_cmpuint(dns_names->len, ==, 0);
  uninstall_session(session);
  g_free(seen.last_error);
  g_free(published.message);
}

/* ---- NT-5 Tor routing --------------------------------------------------------------- */

typedef struct {
  SoupServer *server;
  guint16 port;
  guint hits;
  SoupServerMessage *held; /* a /hold request not answered yet (weak) */
} Http;

static void
on_http(SoupServer *server, SoupServerMessage *message, const char *path, GHashTable *query,
        gpointer data)
{
  (void)server;
  (void)query;
  Http *http = data;
  http->hits++;
  if (g_str_equal(path, "/.well-known/nostr.json")) {
    static const gchar doc[] = "{\"names\":{\"bob\":\"" HEX_BOB "\"}}";
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC, doc,
                                     strlen(doc));
  } else if (g_str_equal(path, "/hold")) {
    /* Unanswered until release_held(). */
    http->held = message;
    g_object_add_weak_pointer(G_OBJECT(message), (gpointer *)&http->held);
    soup_server_message_pause(message);
  } else if (g_str_equal(path, "/relay")) {
    static const gchar doc[] = "{\"name\":\"test\",\"self\":\"" RELAY_SELF "\"}";
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/nostr+json", SOUP_MEMORY_STATIC,
                                     doc, strlen(doc));
  } else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
  }
}

static void
http_init(Http *http)
{
  http->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(http->server, NULL, on_http, http, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(http->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  GSList *uris = soup_server_get_uris(http->server);
  http->port = (guint16)g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static gboolean
http_holding(gpointer data)
{
  return ((Http *)data)->held != NULL;
}

/* Answers the held request, if its connection is still there. */
static void
release_held(Http *http)
{
  if (!http->held)
    return;
  SoupServerMessage *message = http->held;
  g_object_remove_weak_pointer(G_OBJECT(message), (gpointer *)&http->held);
  http->held = NULL;
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC, "{}", 2);
  soup_server_message_unpause(message);
}

static void
http_clear(Http *http)
{
  release_held(http);
  soup_server_disconnect(http->server);
  g_clear_object(&http->server);
}

typedef struct {
  gboolean done;
  gchar *value;
  GError *error;
} Fetched;

static void
on_nip05(GObject *source, GAsyncResult *result, gpointer data)
{
  Fetched *fetched = data;
  fetched->value = gh_nip05_lookup_finish(GH_NIP05(source), result, &fetched->error);
  fetched->done = TRUE;
}

static void
on_nip11(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Fetched *fetched = data;
  fetched->value = gh_nip11_fetch_relay_key_finish(result, &fetched->error);
  fetched->done = TRUE;
}

static void
on_get(GObject *source, GAsyncResult *result, gpointer data)
{
  Fetched *fetched = data;
  g_autoptr(GBytes) body = gh_net_http_get_finish(GH_NET_HTTP(source), result, &fetched->error);
  fetched->value = body ? g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body)) : NULL;
  fetched->done = TRUE;
}

static gboolean
fetched_done(gpointer data)
{
  return ((Fetched *)data)->done;
}

static void
fetched_clear(Fetched *fetched)
{
  g_clear_pointer(&fetched->value, g_free);
  g_clear_error(&fetched->error);
  fetched->done = FALSE;
}

static void
assert_all_remote_dns(Socks5Fixture *socks)
{
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, >, 0);
  for (guint i = 0; i < requests->len; i++) {
    Socks5Request *request = g_ptr_array_index(requests, i);
    g_assert_cmpuint(request->atyp, ==, 0x03); /* a host name, not an address */
    g_assert_cmpuint(request->method, ==, 0x02);
    g_assert_true(is_hex(request->username, 16));
    g_assert_true(is_hex(request->password, 16));
  }
}

/* Every relay connection (scope and publish) and every HTTP request (NIP-05,
 * NIP-11, any https fetch) reaches H3 with ATYP 0x03 and the right host, the
 * relay and server see only the proxy's connections, and nothing is resolved
 * locally. */
static void
test_tor_routing(void)
{
  Socks5Fixture *socks = socks5_fixture_new();
  GhNetSession *session = install_session(GH_NET_MODE_TOR, socks5_fixture_address(socks));
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  guint16 relay_port = url_port(relay.url);
  g_autofree gchar *url = onion_url("ws", relay_port, "/relay");

  /* A scope's REQ and a publish's EVENT, both through the proxy. */
  ScopeSeen seen = { 0 };
  GhRelayScope *scope = open_scope(1, url, &seen);
  wait_for_count(&seen.eoses, 1);
  g_assert_cmpuint(relay.reqs, ==, 1);
  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(url, "through tor", &published);
  wait_for_count(&published.done, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpuint(relay.events, ==, 1);
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 2);
  for (guint i = 0; i < requests->len; i++) {
    Socks5Request *request = g_ptr_array_index(requests, i);
    g_assert_cmpstr(request->host, ==, ONION);
    g_assert_cmpuint(request->port, ==, relay_port);
  }
  wait_tor_state(session, GH_NET_TOR_READY);

  /* A clear-web relay: its name goes to the proxy, never to the local DNS.
   * (Nothing listens behind it, so the dial fails; it stays in Tor.) */
  ScopeSeen web_seen = { 0 };
  Socks5Request *last = NULL;
  if (have_tls()) {
    GhRelayScope *web = open_scope(1, "wss://relay.example.org:9/", &web_seen);
    wait_at_least(&web_seen.errors, 1);
    last = g_ptr_array_index(requests, requests->len - 1);
    g_assert_cmpstr(last->host, ==, "relay.example.org");
    g_assert_cmpuint(last->port, ==, 9);
    gh_relay_scope_cancel(web);
    gh_relay_scope_unref(web);
  }

  /* NIP-05 and NIP-11 through GhNetHttp in Tor mode. */
  Http http = { 0 };
  http_init(&http);
  socks5_fixture_set_domain_port(socks, http.port);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "tor");
  g_settings_set_string(settings, "tor-socks-address", socks5_fixture_address(socks));
  g_autoptr(GhNip05) nip05 = gh_nip05_new(settings, NULL, NULL);
  Fetched fetched = { 0 };
  guint before = requests->len;
  gh_nip05_lookup_async(nip05, "bob@" ONION, NULL, on_nip05, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_no_error(fetched.error);
  g_assert_cmpstr(fetched.value, ==, HEX_BOB);
  g_assert_cmpuint(requests->len, ==, before + 1);
  last = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(last->host, ==, ONION);
  g_assert_cmpuint(last->port, ==, 80);
  fetched_clear(&fetched);

  g_autoptr(GhNetHttp) net = gh_net_http_new(settings);
  g_autofree gchar *relay_doc = onion_url("ws", 4444, "/relay");
  gh_nip11_fetch_relay_key_async(net, relay_doc, NULL, on_nip11, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_no_error(fetched.error);
  g_assert_cmpstr(fetched.value, ==, RELAY_SELF);
  last = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(last->host, ==, ONION);
  g_assert_cmpuint(last->port, ==, 4444);
  fetched_clear(&fetched);

  /* An https lookup: the host name reaches the proxy, which then refuses
   * the connection. */
  if (have_tls()) {
    socks5_fixture_set_refuse(socks, TRUE);
    gh_nip05_lookup_async(nip05, "alice@nip05.example.org", NULL, on_nip05, &fetched);
    spin_until(fetched_done, &fetched);
    g_assert_nonnull(fetched.error);
    last = g_ptr_array_index(requests, requests->len - 1);
    g_assert_cmpstr(last->host, ==, "nip05.example.org");
    g_assert_cmpuint(last->port, ==, 443);
    fetched_clear(&fetched);
  }
  g_assert_cmpuint(http.hits, ==, 2); /* only through the proxy */

  /* Each HTTP request has its own credentials (a fresh circuit). */
  Socks5Request *nip05_request = g_ptr_array_index(requests, before);
  Socks5Request *nip11_request = g_ptr_array_index(requests, before + 1);
  g_assert_cmpstr(nip05_request->username, !=, nip11_request->username);

  assert_all_remote_dns(socks);
  g_assert_cmpuint(dns_names->len, ==, 0);

  gh_relay_publish_unref(publish);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");
  http_clear(&http);
  relay_clear(&relay);
  uninstall_session(session);
  socks5_fixture_free(socks);
  g_free(seen.last_error);
  g_free(web_seen.last_error);
  g_free(published.message);
}

/* ---- a mode change ends HTTP requests of the old mode (W16 review #10) ---------------- */

/* As the relay dispatcher closes every old connection, a mode change ends
 * every GhNetHttp request made in another mode: a direct request whose
 * server has not answered yet never completes once the user picks Tor. It
 * fails with G_IO_ERROR_CONNECTION_CLOSED (callers take CANCELLED for their
 * own cancellation and would say nothing). A request of the mode chosen is
 * not touched, and the caller's cancellation is still CANCELLED. */
static void
test_http_mode_switch(void)
{
  Http http = { 0 };
  http_init(&http);
  Socks5Fixture *socks = socks5_fixture_new();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "none");
  g_settings_set_string(settings, "tor-socks-address", socks5_fixture_address(socks));
  g_autoptr(GhNetHttp) net = gh_net_http_new(settings);
  g_autofree gchar *hold = g_strdup_printf("http://127.0.0.1:%u/hold", http.port);
  Fetched fetched = { 0 };

  /* Direct, then Tor is chosen: the request ends, and the server's late
   * answer reaches nobody. */
  gh_net_http_get_async(net, hold, 1024, NULL, on_get, &fetched);
  spin_until(http_holding, &http);
  g_settings_set_string(settings, "network-mode", "tor");
  spin_until(fetched_done, &fetched);
  g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  g_assert_null(fetched.value);
  release_held(&http);
  run_for(50);
  g_assert_null(fetched.value);
  fetched_clear(&fetched);

  /* Through Tor, then No Proxy is chosen: the same. */
  gh_net_http_get_async(net, hold, 1024, NULL, on_get, &fetched);
  spin_until(http_holding, &http);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 1);
  g_settings_set_string(settings, "network-mode", "none");
  spin_until(fetched_done, &fetched);
  g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  release_held(&http);
  fetched_clear(&fetched);

  /* A change notice for the mode requests were made in leaves them be. */
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  gh_net_http_get_async(net, hold, 1024, cancellable, on_get, &fetched);
  spin_until(http_holding, &http);
  g_signal_emit_by_name(settings, "changed::network-mode", "network-mode");
  run_for(50);
  g_assert_false(fetched.done);
  g_cancellable_cancel(cancellable);
  spin_until(fetched_done, &fetched);
  g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  fetched_clear(&fetched);

  g_clear_object(&net);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");
  http_clear(&http);
  socks5_fixture_free(socks);
  g_assert_cmpuint(dns_names->len, ==, 0);
}

/* The owner may go before its request (a service disposed during a NIP-11
 * or NIP-05 fetch): the request keeps it alive, and cancelling afterwards
 * frees it cleanly. GTask drops its source object before its task data, so
 * a borrowed owner pointer was a use-after-free (W16 addendum N1; the
 * sanitizer job runs this suite). */
static void
test_http_owner_dropped_in_flight(void)
{
  Http http = { 0 };
  http_init(&http);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "none");
  GhNetHttp *net = gh_net_http_new(settings);
  g_object_add_weak_pointer(G_OBJECT(net), (gpointer *)&net);
  g_autofree gchar *hold = g_strdup_printf("http://127.0.0.1:%u/hold", http.port);
  Fetched fetched = { 0 };
  g_autoptr(GCancellable) cancellable = g_cancellable_new();

  gh_net_http_get_async(net, hold, 1024, cancellable, on_get, &fetched);
  spin_until(http_holding, &http);
  g_object_unref(net);
  g_assert_nonnull(net); /* the request holds it */
  g_cancellable_cancel(cancellable);
  spin_until(fetched_done, &fetched);
  g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  fetched_clear(&fetched);
  run_for(50);
  g_assert_null(net); /* and lets it go with the request */

  release_held(&http);
  g_settings_reset(settings, "network-mode");
  http_clear(&http);
}

/* ---- NT-7 Tor fail-closed ------------------------------------------------------------ */

/* The SOCKS port is closed: no relay connection, no HTTP connection, no
 * DNS lookup, "Can't reach Tor"; retries keep trying the proxy only. Only a
 * mode change lets anything connect directly. */
static void
test_tor_fail_closed(void)
{
  Tripwire tripwire = { 0 };
  tripwire_init(&tripwire);
  g_autofree gchar *nowhere = closed_address();
  GhNetSession *session = install_session(GH_NET_MODE_TOR, nowhere);
  wait_tor_state(session, GH_NET_TOR_UNREACHABLE);

  g_autofree gchar *trip_url = g_strdup_printf("ws://127.0.0.1:%u/relay", tripwire.port);
  ScopeSeen seen = { 0 }, web_seen = { 0 };
  GhRelayScope *scope = open_scope(1, trip_url, &seen);
  GhRelayScope *web = open_scope(1, "wss://relay.example.org/", &web_seen);
  wait_at_least(&seen.errors, 1);
  wait_at_least(&web_seen.errors, 1);
  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(trip_url, "never sent", &published);
  wait_for_count(&published.done, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);

  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "tor");
  g_settings_set_string(settings, "tor-socks-address", nowhere);
  g_autoptr(GhNetHttp) net = gh_net_http_new(settings);
  Fetched fetched = { 0 };
  g_autofree gchar *trip_http = g_strdup_printf("http://127.0.0.1:%u/x", tripwire.port);
  gh_net_http_get_async(net, trip_http, 1024, NULL, on_get, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_nonnull(fetched.error);
  fetched_clear(&fetched);
  g_autoptr(GhNip05) nip05 = gh_nip05_new(settings, NULL, NULL);
  gh_nip05_lookup_async(nip05, "bob@example.org", NULL, on_nip05, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_nonnull(fetched.error);
  fetched_clear(&fetched);
  /* An unusable Tor address is an error too, never "direct". */
  g_settings_set_string(settings, "tor-socks-address", "not an address");
  gh_net_http_get_async(net, trip_http, 1024, NULL, on_get, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  fetched_clear(&fetched);

  /* Through at least one reconnect backoff (≤ 1.5 s): still nothing. */
  guint errors = seen.errors;
  wait_at_least(&seen.errors, errors + 1);
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_UNREACHABLE);
  g_assert_cmpuint(tripwire.accepted, ==, 0);
  g_assert_cmpuint(dns_names->len, ==, 0);
  g_assert_cmpuint(seen.eoses, ==, 0);

  /* The user leaves Tor mode: only now does the scope connect directly.
   * (The clear-web scope goes first: directly it would ask the real DNS.) */
  gh_relay_scope_cancel(web);
  gh_relay_scope_unref(web);
  gh_net_session_set_mode(session, GH_NET_MODE_NONE, NULL);
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_OFF);
  wait_at_least(&tripwire.accepted, 1);

  gh_relay_publish_unref(publish);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");
  uninstall_session(session);
  tripwire_clear(&tripwire);
  g_free(seen.last_error);
  g_free(web_seen.last_error);
  g_free(published.message);
}

/* The reachability check itself: a SOCKS5 greeting, never a CONNECT. */
static void
test_tor_probe(void)
{
  Socks5Fixture *socks = socks5_fixture_new();
  g_autoptr(GhNetSession) session = gh_net_session_new(NULL);
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, socks5_fixture_address(socks));
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_CHECKING);
  wait_tor_state(session, GH_NET_TOR_READY);
  g_assert_cmpuint(socks5_fixture_greetings(socks), ==, 1);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 0);
  /* A failed Tor connection re-checks. */
  gh_net_session_report(session, FALSE);
  wait_tor_state(session, GH_NET_TOR_CHECKING);
  wait_tor_state(session, GH_NET_TOR_READY);
  g_assert_cmpuint(socks5_fixture_greetings(socks), ==, 2);
  /* A report from another mode's serial is ignored. */
  gh_net_session_set_mode(session, GH_NET_MODE_NONE, NULL);
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_OFF);
  gh_net_session_report(session, FALSE);
  run_for(50);
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_OFF);
  g_assert_cmpuint(socks5_fixture_greetings(socks), ==, 2);
  socks5_fixture_free(socks);
  g_autofree gchar *nowhere = closed_address();
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, nowhere);
  wait_tor_state(session, GH_NET_TOR_UNREACHABLE);
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, "not an address");
  g_assert_cmpint(gh_net_session_get_tor_state(session), ==, GH_NET_TOR_UNREACHABLE);
}

/* ---- mode switches ------------------------------------------------------------------- */

static void
hold_event(WireRelay *relay, SoupWebsocketConnection *connection, const gchar *event_id,
           gpointer data)
{
  (void)relay;
  (void)connection;
  (void)event_id;
  (void)data; /* sink: never answers, so the publish waits for its OK */
}

/* Switching the mode closes every connection made in the old one before
 * the scope reconnects in the new one; a publish still waiting for its OK
 * fails instead of finishing on the old path. */
static void
test_mode_switch(void)
{
  Socks5Fixture *socks = socks5_fixture_new();
  GhNetSession *session = install_session(GH_NET_MODE_NONE, NULL);
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  WireRelay sink = { 0 };
  relay_init(&sink);
  sink.on_event = hold_event;

  ScopeSeen seen = { 0 };
  GhRelayScope *scope = open_scope(1, relay.url, &seen);
  wait_for_count(&seen.eoses, 1);
  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(sink.url, "held", &published);
  wait_for_count(&sink.events, 1);
  g_assert_cmpuint(relay.connections->len, ==, 1);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 0);

  /* Direct -> Tor: the direct sockets close; the scope comes back through
   * the proxy, the publish fails. */
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, socks5_fixture_address(socks));
  wait_for_count(&published.done, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_nonnull(strstr(published.message, "mode changed"));
  wait_at_least(&relay.closed_sockets, 1);
  wait_for_count(&seen.eoses, 2);
  g_assert_cmpuint(seen.disconnected, ==, 1);
  g_assert_cmpuint(relay.connections->len, ==, 2);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 1);
  Socks5Request *request = g_ptr_array_index(socks5_fixture_requests(socks), 0);
  g_assert_cmpuint(request->atyp, ==, 0x01); /* an IP literal URL stays one */
  g_assert_cmpstr(request->host, ==, "127.0.0.1");

  /* Tor -> direct: the proxied socket closes, the scope reconnects
   * directly, the proxy sees nothing more. */
  gh_net_session_set_mode(session, GH_NET_MODE_NONE, NULL);
  wait_at_least(&relay.closed_sockets, 2);
  wait_for_count(&seen.eoses, 3);
  g_assert_cmpuint(seen.disconnected, ==, 2);
  g_assert_cmpuint(relay.connections->len, ==, 3);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 1);

  /* A mode that refuses the URL (Tor with an unusable address) leaves the
   * scope disconnected with an error, and connects nothing. */
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, "not an address");
  wait_at_least(&relay.closed_sockets, 3);
  wait_at_least(&seen.errors, 1);
  g_assert_cmpuint(relay.connections->len, ==, 3);

  gh_relay_publish_unref(publish);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  relay_clear(&sink);
  uninstall_session(session);
  socks5_fixture_free(socks);
  g_free(seen.last_error);
  g_free(published.message);
}

/* ---- NT-6 isolation credentials -------------------------------------------------------- */

static const gchar *
username_at(Socks5Fixture *socks, guint index)
{
  return ((Socks5Request *)g_ptr_array_index(socks5_fixture_requests(socks), index))->username;
}

static void
test_isolation(void)
{
  Socks5Fixture *socks = socks5_fixture_new();
  GhNetSession *session = install_session(GH_NET_MODE_TOR, socks5_fixture_address(socks));
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  guint16 relay_port = url_port(relay.url);
  g_autofree gchar *url = onion_url("ws", relay_port, "/relay");

  /* One scope: its reconnect keeps its username (and so its circuit). */
  ScopeSeen first = { 0 };
  GhRelayScope *inbox = open_scope(1, url, &first);
  wait_for_count(&first.eoses, 1);
  soup_websocket_connection_close(g_ptr_array_index(relay.connections, 0),
                                  SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
  wait_for_count(&first.eoses, 2);
  g_assert_cmpuint(socks5_fixture_requests(socks)->len, ==, 2);
  g_assert_cmpstr(username_at(socks, 0), ==, username_at(socks, 1));

  /* Another scope of the same account, a scope of the next account, and
   * two publishes: each its own username. */
  ScopeSeen second = { 0 }, next = { 0 };
  GhRelayScope *discovery = open_scope(1, url, &second);
  wait_for_count(&second.eoses, 1);
  GhRelayScope *next_account = open_scope(2, url, &next);
  wait_for_count(&next.eoses, 1);
  PublishSeen p1 = { 0 }, p2 = { 0 };
  GhRelayPublish *publish1 = start_publish(url, "one", &p1);
  GhRelayPublish *publish2 = start_publish(url, "two", &p2);
  wait_for_count(&p1.done, 1);
  wait_for_count(&p2.done, 1);
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 6);
  g_autoptr(GHashTable) distinct = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 1; i < requests->len; i++) {
    Socks5Request *request = g_ptr_array_index(requests, i);
    g_assert_true(is_hex(request->username, 16));
    /* No URL or key reaches the Tor daemon. */
    g_assert_null(strstr(url, request->username));
    g_assert_null(strstr(SECRET, request->username));
    g_assert_null(strstr(request->username, "onion"));
    g_hash_table_add(distinct, request->username);
  }
  g_assert_cmpuint(g_hash_table_size(distinct), ==, 5);

  gh_relay_publish_unref(publish1);
  gh_relay_publish_unref(publish2);
  GhRelayScope *scopes[] = { inbox, discovery, next_account };
  for (guint i = 0; i < G_N_ELEMENTS(scopes); i++) {
    gh_relay_scope_cancel(scopes[i]);
    gh_relay_scope_unref(scopes[i]);
  }
  relay_clear(&relay);
  uninstall_session(session);
  socks5_fixture_free(socks);
  g_free(first.last_error);
  g_free(second.last_error);
  g_free(next.last_error);
  g_free(p1.message);
  g_free(p2.message);
}

/* Without an installed session the dispatcher refuses rather than guess. */
static void
test_no_session_refuses(void)
{
  ScopeSeen seen = { 0 };
  GhRelayScope *scope = gh_relay_scope_new_with_transport(1, any_filters(),
                                                          &gh_relay_net_transport, NULL,
                                                          on_scope, &seen);
  g_assert_true(gh_relay_scope_add_url(scope, "wss://relay.example.org/", NULL));
  gh_relay_scope_start(scope);
  g_assert_cmpuint(seen.errors, ==, 1);
  gh_relay_scope_unref(scope);
  g_free(seen.last_error);
}

/* ---- nostrc-qi5e: downloads reach public addresses only ------------------------------- */

/* gh_net_address_is_public(), including every IPv6 form carrying an IPv4
 * address, which the embedded address decides. */
static void
test_public_address(void)
{
  static const gchar *const refused[] = {
    "127.0.0.1", "10.1.2.3", "172.16.0.1", "192.168.1.1", "169.254.169.254", "100.64.0.1",
    "0.0.0.0", "0.1.2.3", "192.0.0.8", "198.18.0.1", "224.0.0.1", "240.0.0.1",
    "255.255.255.255", "::", "::1", "fe80::1", "fd00::1", "fc00::1", "fec0::1", "ff02::1",
    "100::1", "2001:db8::1",
    /* IPv4-mapped, IPv4-compatible, SIIT */
    "::ffff:127.0.0.1", "::ffff:192.168.1.1", "::127.0.0.1", "::10.0.0.1", "::0.0.0.2",
    "::ffff:0:127.0.0.1", "::ffff:0:10.1.2.3",
    /* NAT64 64:ff9b::/96 */
    "64:ff9b::127.0.0.1", "64:ff9b::7f00:1", "64:ff9b::192.168.1.1", "64:ff9b::100.64.0.1",
    /* local NAT64 64:ff9b:1::/48: 127.0.0.1 and 10.1.2.3 as /96, 127.0.0.1 as
     * /64 (whose /96 reading, 1.0.0.0, is public), 10.0.0.1 as /48; and the
     * rest of 64:ff9b::/32 */
    "64:ff9b:1::7f00:1", "64:ff9b:1::a01:203", "64:ff9b:1:0:7f:0:100:0", "64:ff9b:1:a00:0:100::",
    "64:ff9b:2::808:808",
    /* 6to4 of 127.0.0.1, 192.168.1.1, 10.1.2.3 */
    "2002:7f00:1::1", "2002:c0a8:101::", "2002:a01:203::1",
    /* Teredo: client 127.0.0.1 (inverted 80ff:fffe); server 10.0.0.1 */
    "2001:0:4136:e378:8000:63bf:80ff:fffe", "2001:0:a00:1::f7f7:f7f7",
  };
  static const gchar *const allowed[] = {
    "8.8.8.8", "1.1.1.1", "93.184.216.34", "2606:4700:4700::1111", "2a00:1450:4001:80b::200e",
    "::ffff:8.8.8.8", "64:ff9b::8.8.8.8", "64:ff9b:1::808:808", "2002:808:808::1",
    "2001:0:4136:e378:8000:63bf:f7f7:f7f7",
  };
  g_autoptr(GString) wrong = g_string_new(NULL);
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string(refused[i]);
    g_assert_nonnull(address);
    if (gh_net_address_is_public(address))
      g_string_append_printf(wrong, " %s (taken for public)", refused[i]);
  }
  for (guint i = 0; i < G_N_ELEMENTS(allowed); i++) {
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string(allowed[i]);
    g_assert_nonnull(address);
    if (!gh_net_address_is_public(address))
      g_string_append_printf(wrong, " %s (refused)", allowed[i]);
  }
  g_assert_cmpstr(wrong->str, ==, "");
}

static gboolean
proxy_is_direct(GProxyResolver *resolver, const gchar *uri)
{
  g_auto(GStrv) proxies = g_proxy_resolver_lookup(resolver, uri, NULL, NULL);
  return proxies && proxies[0] && g_str_equal(proxies[0], "direct://") && !proxies[1];
}

/* DNS rebinding: a public-looking name the resolver maps to 127.0.0.1. An
 * ordinary request connects there (the control); a public-only one is
 * refused where the connection is made, and nothing connects, in No Proxy
 * and (direct) System mode, as are loopback literals. In Tor mode the proxy
 * resolves the name: no local lookup, the name goes to the proxy. */
static void
test_public_only(void)
{
  Tripwire listener = { 0 }; /* here the connection counter, dialled on purpose */
  tripwire_init(&listener);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "none");
  g_autoptr(GhNetHttp) net = gh_net_http_new(settings);
  g_autofree gchar *url = g_strdup_printf("https://" REBIND_HOST ":%u/blob", listener.port);
  Fetched fetched = { 0 };

  gh_net_http_get_async(net, url, 1024, NULL, on_get, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_nonnull(fetched.error); /* no TLS there */
  fetched_clear(&fetched);
  wait_at_least(&listener.accepted, 1);
  g_assert_cmpuint(listener.accepted, ==, 1);

  const gchar *const modes[] = { "none", "system" };
  for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
    if (g_str_equal(modes[i], "system") &&
        !proxy_is_direct(g_proxy_resolver_get_default(), url)) {
      g_test_message("System mode uses a proxy here: its public-only check is the proxy's");
      continue;
    }
    g_settings_set_string(settings, "network-mode", modes[i]);
    g_autofree gchar *literal = g_strdup_printf("https://127.0.0.1:%u/blob", listener.port);
    g_autofree gchar *mapped = g_strdup_printf("https://[::ffff:127.0.0.1]:%u/blob",
                                               listener.port);
    const gchar *const urls[] = { url, literal, mapped };
    for (guint j = 0; j < G_N_ELEMENTS(urls); j++) {
      gh_net_http_get_public_async(net, urls[j], NULL, 1024, NULL, on_get, &fetched);
      spin_until(fetched_done, &fetched);
      g_test_message("%s, public only: %s", modes[i], urls[j]);
      g_assert_error(fetched.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
      fetched_clear(&fetched);
    }
  }
  run_for(100);
  g_assert_cmpuint(listener.accepted, ==, 1);

  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, listener.port);
  g_settings_set_string(settings, "tor-socks-address", socks5_fixture_address(socks));
  g_settings_set_string(settings, "network-mode", "tor");
  const guint looked_up = dns_names->len;
  gh_net_http_get_public_async(net, url, NULL, 1024, NULL, on_get, &fetched);
  spin_until(fetched_done, &fetched);
  g_assert_nonnull(fetched.error); /* no TLS there either */
  fetched_clear(&fetched);
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 1);
  Socks5Request *request = g_ptr_array_index(requests, 0);
  g_assert_cmpuint(request->atyp, ==, 0x03);
  g_assert_cmpstr(request->host, ==, REBIND_HOST);
  g_assert_cmpuint(dns_names->len, ==, looked_up);

  g_clear_object(&net);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");
  socks5_fixture_free(socks);
  tripwire_clear(&listener);
  g_ptr_array_set_size(dns_names, 0); /* the rebinding name's lookups, on purpose */
}

/* ---- nostrc-qp24.91: the public-only enumerator, lookup order by lookup order ------ */

/* A resolver that holds every asynchronous lookup until the test releases
 * it, so the IPv6 and IPv4 answers of a GNetworkAddress enumeration arrive
 * in the order the test chooses. IPv6 lookups fail; IPv4 lookups of
 * MIXED_HOST give private and public addresses, of LOCAL_HOST only
 * 127.0.0.1. Synchronous lookups answer at once. */
#define MIXED_HOST "mixed.groundhog.test"
#define LOCAL_HOST "local.groundhog.test"

#define HELD_TYPE_RESOLVER (held_resolver_get_type())
G_DECLARE_FINAL_TYPE(HeldResolver, held_resolver, HELD, RESOLVER, GResolver)
struct _HeldResolver {
  GResolver parent_instance;
  GPtrArray *pending;  /* GTask, a held lookup; its task data: the flags */
  guint completed;     /* held lookups whose callbacks have run */
};
G_DEFINE_FINAL_TYPE(HeldResolver, held_resolver, G_TYPE_RESOLVER)

static GList *
held_answer(const gchar *name, GResolverNameLookupFlags flags, GError **error)
{
  if (flags & G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY) {
    g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "no IPv6 for %s", name);
    return NULL;
  }
  static const gchar *const mixed[] = { "127.0.0.1", "8.8.8.8", "10.1.2.3", "1.1.1.1" };
  guint count = g_str_equal(name, MIXED_HOST) ? G_N_ELEMENTS(mixed)
                : g_str_equal(name, LOCAL_HOST) ? 1 : 0;
  if (!count) {
    g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "no address for %s", name);
    return NULL;
  }
  GList *addresses = NULL;
  for (guint i = 0; i < count; i++)
    addresses = g_list_append(addresses, g_inet_address_new_from_string(mixed[i]));
  return addresses;
}

static GList *
held_lookup_flags(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                  GCancellable *cancellable, GError **error)
{
  (void)resolver;
  (void)cancellable;
  return held_answer(name, flags, error);
}

static GList *
held_lookup(GResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
  return held_lookup_flags(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                           error);
}

static void
held_lookup_flags_async(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  g_task_set_task_data(task, GUINT_TO_POINTER(flags), NULL);
  g_object_set_data_full(G_OBJECT(task), "name", g_strdup(name), g_free);
  g_ptr_array_add(HELD_RESOLVER(resolver)->pending, task);
}

static void
held_lookup_async(GResolver *resolver, const gchar *name, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  held_lookup_flags_async(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                          callback, data);
}

static GList *
held_lookup_finish(GResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
on_held_completed(GObject *task, GParamSpec *pspec, gpointer data)
{
  (void)task;
  (void)pspec;
  HELD_RESOLVER(data)->completed++;
}

/* Answers the held lookup for IPv6 (or else for IPv4); FALSE when none is
 * held. */
static gboolean
held_release(HeldResolver *self, gboolean ipv6)
{
  for (guint i = 0; i < self->pending->len; i++) {
    GTask *task = g_ptr_array_index(self->pending, i);
    GResolverNameLookupFlags flags = GPOINTER_TO_UINT(g_task_get_task_data(task));
    if (!!(flags & G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY) != !!ipv6)
      continue;
    g_ptr_array_remove_index(self->pending, i);
    g_signal_connect(task, "notify::completed", G_CALLBACK(on_held_completed), self);
    GError *error = NULL;
    GList *addresses = held_answer(g_object_get_data(G_OBJECT(task), "name"), flags, &error);
    if (addresses)
      g_task_return_pointer(task, addresses, (GDestroyNotify)g_resolver_free_addresses);
    else
      g_task_return_error(task, error);
    g_object_unref(task);
    return TRUE;
  }
  return FALSE;
}

static void
held_resolver_finalize(GObject *object)
{
  g_assert_cmpuint(HELD_RESOLVER(object)->pending->len, ==, 0);
  g_ptr_array_unref(HELD_RESOLVER(object)->pending);
  G_OBJECT_CLASS(held_resolver_parent_class)->finalize(object);
}

static void
held_resolver_class_init(HeldResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = held_resolver_finalize;
  GResolverClass *resolver = G_RESOLVER_CLASS(klass);
  resolver->lookup_by_name = held_lookup;
  resolver->lookup_by_name_async = held_lookup_async;
  resolver->lookup_by_name_finish = held_lookup_finish;
  resolver->lookup_by_name_with_flags = held_lookup_flags;
  resolver->lookup_by_name_with_flags_async = held_lookup_flags_async;
  resolver->lookup_by_name_with_flags_finish = held_lookup_finish;
}

static void
held_resolver_init(HeldResolver *self)
{
  self->pending = g_ptr_array_new();
}

typedef struct {
  gboolean done;
  GSocketAddress *address;
  GError *error;
} Next;

static void
on_next(GObject *source, GAsyncResult *result, gpointer data)
{
  Next *next = data;
  next->address = g_socket_address_enumerator_next_finish(G_SOCKET_ADDRESS_ENUMERATOR(source),
                                                          result, &next->error);
  next->done = TRUE;
}

typedef struct {
  HeldResolver *resolver;
  gboolean ipv6_first;
  guint released;
  Next *next;
} Pump;

/* Releases the held lookups one at a time, in the chosen order, each only
 * after the previous one's callback has run; TRUE once next is done. */
static gboolean
pump(gpointer data)
{
  Pump *p = data;
  if (!p->next->done && p->released < 2 && p->resolver->completed == p->released &&
      held_release(p->resolver, p->released == 0 ? p->ipv6_first : !p->ipv6_first))
    p->released++;
  return p->next->done;
}

/* Every address the asynchronous enumeration of host yields, as text, and
 * the error it ends with. */
static gchar *
enumerate_async(HeldResolver *resolver, const gchar *host, gboolean ipv6_first, GError **error)
{
  g_autoptr(GSocketConnectable) address = gh_net_public_address_new(host, 443, "https");
  g_autoptr(GSocketAddressEnumerator) enumerator = g_socket_connectable_enumerate(address);
  Pump p = { resolver, ipv6_first, 0, NULL };
  resolver->completed = 0;
  g_autoptr(GString) out = g_string_new(NULL);
  for (;;) {
    Next next = { 0 };
    p.next = &next;
    g_socket_address_enumerator_next_async(enumerator, NULL, on_next, &next);
    spin_until(pump, &p);
    if (!next.address) {
      if (next.error)
        g_propagate_error(error, next.error);
      break;
    }
    g_autofree gchar *text = g_inet_address_to_string(
      g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(next.address)));
    g_string_append_printf(out, "%s%s", out->len ? " " : "", text);
    g_object_unref(next.address);
  }
  /* Both lookups were made and answered, in the chosen order. */
  g_assert_cmpuint(p.released, ==, 2);
  return g_strdup(out->str);
}

/* The public-only enumerator (gh_net_public_address_new()) refuses a private
 * address and asks its GNetworkAddress enumerator for the next one. Here that
 * happens with the IPv6 lookup failing before the IPv4 one answers, and after,
 * for a name with public addresses among private ones and for a name with
 * only 127.0.0.1. The refusal's continuation crashed on x86_64 Linux: it
 * passed g_steal_pointer(&task) and g_task_get_cancellable(task) in one
 * argument list, and GCC evaluated the steal first (nostrc-qp24.91). */
static void
test_public_enumerator(void)
{
  g_autoptr(GResolver) previous = g_resolver_get_default();
  g_autoptr(HeldResolver) resolver = g_object_new(HELD_TYPE_RESOLVER, NULL);
  g_resolver_set_default(G_RESOLVER(resolver));

  for (guint order = 0; order < 2; order++) {
    gboolean ipv6_first = order == 0;
    g_test_message("IPv6 answer %s the IPv4 one", ipv6_first ? "before" : "after");
    GError *error = NULL;
    g_autofree gchar *mixed = enumerate_async(resolver, MIXED_HOST, ipv6_first, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mixed, ==, "8.8.8.8 1.1.1.1");
    g_autofree gchar *local = enumerate_async(resolver, LOCAL_HOST, ipv6_first, &error);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_cmpstr(local, ==, "");
  }

  /* The synchronous enumeration filters the same way. */
  const gchar *const hosts[] = { MIXED_HOST, LOCAL_HOST };
  const gchar *const expected[] = { "8.8.8.8 1.1.1.1", "" };
  for (guint i = 0; i < G_N_ELEMENTS(hosts); i++) {
    g_autoptr(GSocketConnectable) address = gh_net_public_address_new(hosts[i], 443, "https");
    g_autoptr(GSocketAddressEnumerator) enumerator = g_socket_connectable_enumerate(address);
    g_autoptr(GString) out = g_string_new(NULL);
    g_autoptr(GError) error = NULL;
    GSocketAddress *next;
    while ((next = g_socket_address_enumerator_next(enumerator, NULL, &error))) {
      g_autofree gchar *text = g_inet_address_to_string(
        g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(next)));
      g_string_append_printf(out, "%s%s", out->len ? " " : "", text);
      g_object_unref(next);
    }
    g_assert_cmpstr(out->str, ==, expected[i]);
    if (i == 0)
      g_assert_no_error(error);
    else
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  }

  g_resolver_set_default(previous);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  dns_names = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GResolver) tripwire = g_object_new(TRIP_TYPE_RESOLVER, NULL);
  g_resolver_set_default(tripwire);
  g_test_add_func("/groundhog/net/modes", test_modes);
  g_test_add_func("/groundhog/net/url-policy", test_url_policy);
  g_test_add_func("/groundhog/net/tor-routing", test_tor_routing);
  g_test_add_func("/groundhog/net/http-mode-switch", test_http_mode_switch);
  g_test_add_func("/groundhog/net/http-owner-dropped-in-flight", test_http_owner_dropped_in_flight);
  g_test_add_func("/groundhog/net/tor-fail-closed", test_tor_fail_closed);
  g_test_add_func("/groundhog/net/tor-probe", test_tor_probe);
  g_test_add_func("/groundhog/net/mode-switch", test_mode_switch);
  g_test_add_func("/groundhog/net/isolation", test_isolation);
  g_test_add_func("/groundhog/net/no-session-refuses", test_no_session_refuses);
  g_test_add_func("/groundhog/net/public-address", test_public_address);
  g_test_add_func("/groundhog/net/public-only", test_public_only);
  g_test_add_func("/groundhog/net/public-enumerator", test_public_enumerator);
  int status = g_test_run();
  g_ptr_array_unref(dns_names);
  return status;
}
