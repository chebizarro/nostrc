/* The network-mode guard of a build without G09 (nostrc-6v0i, charter P5):
 * with network-mode tor, or a mode the build does not know, no scope or
 * publish reaches the direct transport, so nothing is resolved or dialled.
 *
 *  - a recording direct transport counts every open (every open would be a
 *    local DNS lookup and a dial): zero in Tor mode and for .onion;
 *  - the G24-style tripwire: the real GNostrRelay transport and a listener
 *    on 127.0.0.1 that must never be dialled in Tor mode; leaving Tor mode
 *    dials it at once, so the tripwire is shown to work. */
#include "gh-relay-guard.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <stdlib.h>
#include <string.h>

#define SECRET "0000000000000000000000000000000000000000000000000000000000000001"
#define WEB "wss://relay.example.org/"
#define ONION "ws://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuv.onion/"

/* ---- helpers ---------------------------------------------------------------------- */

typedef struct {
  gboolean timed_out;
} Wait;

static gboolean
expire(gpointer data)
{
  ((Wait *)data)->timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

/* Iterates until *counter >= value; fails after 10 s. */
static void
wait_at_least(const guint *counter, guint value)
{
  Wait wait = { 0 };
  guint timer = g_timeout_add_seconds(10, expire, &wait);
  while (*counter < value && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(wait.timed_out);
  g_source_remove(timer);
}

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
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

static gchar *
signed_json(void)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "never sent");
  g_assert_cmpint(nostr_event_sign(event, SECRET), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

typedef struct {
  guint errors;
  guint disconnected;
  GHashTable *last_error; /* URL -> detail */
} ScopeSeen;

static void
on_scope(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  ScopeSeen *seen = data;
  if (update->notice == GH_RELAY_NOTICE_ERROR) {
    seen->errors++;
    g_hash_table_replace(seen->last_error, g_strdup(update->url), g_strdup(update->detail));
  } else if (update->notice == GH_RELAY_NOTICE_DISCONNECTED) {
    seen->disconnected++;
  }
}

static void
scope_seen_init(ScopeSeen *seen)
{
  memset(seen, 0, sizeof *seen);
  seen->last_error = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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

static GhRelayPublish *
start_publish(const gchar *url, PublishSeen *seen)
{
  g_autofree gchar *json = signed_json();
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new(1, json, on_publish_update, on_publish_done,
                                                 seen, &error);
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_add_url(publish, url, NULL));
  g_assert_true(gh_relay_publish_start(publish, &error));
  g_assert_no_error(error);
  return publish;
}

/* ---- the recording direct transport ------------------------------------------------ */

typedef struct {
  GPtrArray *opened; /* URLs the guard let through */
  guint closed;
} Direct;

static Direct direct;

static gpointer
direct_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
            gpointer data, GError **error)
{
  (void)scope;
  (void)filters;
  (void)data;
  (void)error;
  g_ptr_array_add(direct.opened, g_strdup(url));
  return g_strdup(url);
}

static gpointer
direct_publish_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
                    gpointer data, GError **error)
{
  (void)publish;
  (void)event_json;
  (void)data;
  (void)error;
  g_ptr_array_add(direct.opened, g_strdup(url));
  return g_strdup(url);
}

static void
direct_close(gpointer handle, gpointer data)
{
  (void)data;
  direct.closed++;
  g_free(handle);
}

static const GhRelayTransport direct_transport = { direct_open, direct_close };
static const GhRelayPublishTransport direct_publish_transport = { direct_publish_open,
                                                                  direct_close };

static void
direct_install(void)
{
  direct.opened = g_ptr_array_new_with_free_func(g_free);
  direct.closed = 0;
  gh_relay_guard_set_direct_transports(&direct_transport, NULL, &direct_publish_transport,
                                       NULL);
}

static void
direct_uninstall(void)
{
  gh_relay_guard_set_direct_transports(NULL, NULL, NULL, NULL);
  g_clear_pointer(&direct.opened, g_ptr_array_unref);
}

/* ---- tests ------------------------------------------------------------------------ */

static void
test_decision(void)
{
  g_assert_true(gh_relay_guard_mode_allowed("system"));
  g_assert_true(gh_relay_guard_mode_allowed("none"));
  static const gchar *refused[] = { "tor", "", "i2p", "System", NULL };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++)
    g_assert_false(gh_relay_guard_mode_allowed(refused[i]));

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_relay_guard_url_allowed("system", WEB, &error));
  g_assert_true(gh_relay_guard_url_allowed("none", "ws://127.0.0.1:7777/", &error));
  g_assert_no_error(error);

  g_assert_false(gh_relay_guard_url_allowed("tor", WEB, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpstr(error->message, ==, GH_RELAY_GUARD_REFUSED_MESSAGE);
  g_clear_error(&error);
  g_assert_false(gh_relay_guard_url_allowed("unknown-mode", WEB, &error));
  g_clear_error(&error);

  /* .onion in every mode, however it is spelled. */
  static const gchar *onions[] = { ONION, "wss://Example.ONION/", "wss://example.onion./",
                                   "ws://example.onion:80/relay" };
  static const gchar *modes[] = { "system", "none", "tor" };
  for (guint i = 0; i < G_N_ELEMENTS(onions); i++) {
    for (guint m = 0; m < G_N_ELEMENTS(modes); m++) {
      g_assert_false(gh_relay_guard_url_allowed(modes[m], onions[i], &error));
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
      g_assert_nonnull(strstr(error->message, ".onion"));
      g_clear_error(&error);
    }
  }
  /* "onion" as a label elsewhere is not a .onion name. */
  g_assert_true(gh_relay_guard_url_allowed("none", "wss://onion.example.org/", NULL));
  g_assert_true(gh_relay_guard_url_allowed("none", "wss://notonion/", NULL));
}

/* Tor mode: nothing reaches the direct transport. Leaving it connects the
 * refused scope URL; going back closes it; system <-> none changes nothing.
 * A .onion relay never reaches the direct transport. */
static void
test_tor_refuses(void)
{
  direct_install();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "tor");
  gh_relay_guard_install(settings);

  ScopeSeen seen;
  scope_seen_init(&seen);
  GhRelayScope *scope = gh_relay_scope_new(1, any_filters(), on_scope, &seen);
  g_assert_true(gh_relay_scope_add_url(scope, WEB, NULL));
  g_assert_true(gh_relay_scope_add_url(scope, ONION, NULL));
  gh_relay_scope_start(scope);
  wait_at_least(&seen.errors, 2);
  g_assert_cmpstr(g_hash_table_lookup(seen.last_error, WEB), ==,
                  GH_RELAY_GUARD_REFUSED_MESSAGE);
  g_assert_nonnull(strstr(g_hash_table_lookup(seen.last_error, ONION), ".onion"));

  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(WEB, &published);
  wait_at_least(&published.done, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_cmpstr(published.message, ==, GH_RELAY_GUARD_REFUSED_MESSAGE);
  gh_relay_publish_unref(publish);
  drain();
  g_assert_cmpuint(direct.opened->len, ==, 0);

  /* The user picks No Proxy: the refused URL connects, the .onion does not. */
  g_settings_set_string(settings, "network-mode", "none");
  drain();
  g_assert_cmpuint(direct.opened->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(direct.opened, 0), ==, WEB);
  /* system and none both connect directly: nothing to redo. */
  g_settings_set_string(settings, "network-mode", "system");
  drain();
  g_assert_cmpuint(direct.opened->len, ==, 1);
  g_assert_cmpuint(direct.closed, ==, 0);

  /* Back to Tor: the connection closes; the URL says so, then refuses. */
  guint errors = seen.errors;
  g_settings_set_string(settings, "network-mode", "tor");
  drain();
  g_assert_cmpuint(direct.closed, ==, 1);
  g_assert_cmpuint(seen.disconnected, ==, 1);
  g_assert_cmpuint(seen.errors, ==, errors + 1);
  g_assert_cmpstr(g_hash_table_lookup(seen.last_error, WEB), ==,
                  GH_RELAY_GUARD_REFUSED_MESSAGE);

  /* A publish opened in an allowed mode fails when Tor is chosen. */
  g_settings_set_string(settings, "network-mode", "none");
  drain();
  PublishSeen late = { 0 };
  publish = start_publish(WEB, &late);
  drain();
  guint opened = direct.opened->len;
  g_assert_cmpuint(late.done, ==, 0);
  g_settings_set_string(settings, "network-mode", "tor");
  wait_at_least(&late.done, 1);
  g_assert_cmpint(late.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_cmpuint(direct.opened->len, ==, opened);
  gh_relay_publish_unref(publish);

  for (guint i = 0; i < direct.opened->len; i++)
    g_assert_cmpstr(g_ptr_array_index(direct.opened, i), !=, ONION);

  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  drain();
  gh_relay_guard_install(NULL);
  g_settings_reset(settings, "network-mode");
  direct_uninstall();
  g_hash_table_unref(seen.last_error);
  g_free(published.message);
  g_free(late.message);
}

/* Once the guard is gone, a scope made for it refuses rather than guess. */
static void
test_uninstalled_refuses(void)
{
  direct_install();
  gh_relay_guard_install(NULL);
  ScopeSeen seen;
  scope_seen_init(&seen);
  GhRelayScope *scope = gh_relay_scope_new_with_transport(1, any_filters(),
                                                          &gh_relay_guard_transport, NULL,
                                                          on_scope, &seen);
  g_assert_true(gh_relay_scope_add_url(scope, WEB, NULL));
  gh_relay_scope_start(scope);
  wait_at_least(&seen.errors, 1);
  g_assert_nonnull(strstr(g_hash_table_lookup(seen.last_error, WEB), "not set up"));
  g_assert_cmpuint(direct.opened->len, ==, 0);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  direct_uninstall();
  g_hash_table_unref(seen.last_error);
}

/* ---- the tripwire ----------------------------------------------------------------- */

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
  g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

static void
tripwire_init(Tripwire *tripwire)
{
  tripwire->service = g_socket_service_new();
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, 0);
  g_autoptr(GSocketAddress) bound = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(tripwire->service), address,
                                              G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                              NULL, &bound, &error));
  g_assert_no_error(error);
  tripwire->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
  g_signal_connect(tripwire->service, "incoming", G_CALLBACK(on_tripped), tripwire);
  g_socket_service_start(tripwire->service);
}

static void
tripwire_clear(Tripwire *tripwire)
{
  g_signal_handlers_disconnect_by_data(tripwire->service, tripwire);
  g_socket_service_stop(tripwire->service);
  g_socket_listener_close(G_SOCKET_LISTENER(tripwire->service));
  g_clear_object(&tripwire->service);
}

/* The real GNostrRelay transport behind the guard. In Tor mode neither a
 * scope nor a publish dials the listener, through a reconnect's worth of
 * main-loop time; leaving Tor mode dials it, which shows the tripwire
 * would have caught a direct connection. */
static void
test_tripwire(void)
{
  Tripwire tripwire = { 0 };
  tripwire_init(&tripwire);
  g_autofree gchar *url = g_strdup_printf("ws://127.0.0.1:%u/", tripwire.port);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "tor");
  gh_relay_guard_install(settings);

  ScopeSeen seen;
  scope_seen_init(&seen);
  GhRelayScope *scope = gh_relay_scope_new(1, any_filters(), on_scope, &seen);
  g_assert_true(gh_relay_scope_add_url(scope, url, NULL));
  gh_relay_scope_start(scope);
  PublishSeen published = { 0 };
  GhRelayPublish *publish = start_publish(url, &published);
  wait_at_least(&seen.errors, 1);
  wait_at_least(&published.done, 1);
  g_assert_cmpint(published.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);

  /* A direct transport would have dialled by now; give it 2 s anyway. */
  Wait wait = { 0 };
  g_timeout_add(2000, expire, &wait);
  while (!wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(tripwire.accepted, ==, 0);

  /* Only a mode this build can use connects, and then directly. */
  g_settings_set_string(settings, "network-mode", "none");
  wait_at_least(&tripwire.accepted, 1);

  gh_relay_publish_unref(publish);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  drain();
  gh_relay_guard_install(NULL);
  g_settings_reset(settings, "network-mode");
  tripwire_clear(&tripwire);
  g_hash_table_unref(seen.last_error);
  g_free(published.message);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay-guard/decision", test_decision);
  g_test_add_func("/groundhog/relay-guard/tor-refuses", test_tor_refuses);
  g_test_add_func("/groundhog/relay-guard/uninstalled-refuses", test_uninstalled_refuses);
  g_test_add_func("/groundhog/relay-guard/tripwire", test_tripwire);
  return g_test_run();
}
