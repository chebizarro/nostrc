#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>

typedef struct {
  SoupServer *server;
  GPtrArray *connections;
  gchar *url;
  guint reqs;
  guint timeout_source;
  gboolean timed_out;
} WireRelay;

static void
on_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
           GBytes *message, gpointer data)
{
  (void)connection;
  WireRelay *relay = data;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  if (type == SOUP_WEBSOCKET_DATA_TEXT && length >= 6 &&
      memcmp(bytes, "[\"REQ\"", 6) == 0)
    relay->reqs++;
}

static void
on_websocket(SoupServer *server, SoupServerMessage *message,
             const char *path, SoupWebsocketConnection *connection,
             gpointer data)
{
  (void)server;
  (void)message;
  (void)path;
  WireRelay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(on_message), relay);
}

static void
relay_init(WireRelay *relay)
{
  relay->server = soup_server_new(NULL, NULL);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  soup_server_add_websocket_handler(relay->server, "/relay", NULL, NULL,
                                    on_websocket, relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, 0,
                                         SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  GSList *uris = soup_server_get_uris(relay->server);
  g_assert_nonnull(uris);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/relay",
                                g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static void
relay_clear(WireRelay *relay)
{
  for (guint i = 0; i < relay->connections->len; i++)
    soup_websocket_connection_close(g_ptr_array_index(relay->connections, i),
                                     SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  g_ptr_array_unref(relay->connections);
  soup_server_disconnect(relay->server);
  g_object_unref(relay->server);
  g_free(relay->url);
}

static gboolean
expire(gpointer data)
{
  WireRelay *relay = data;
  relay->timed_out = TRUE;
  relay->timeout_source = 0;
  return G_SOURCE_REMOVE;
}

static void
wait_for_reqs(WireRelay *relay, guint count)
{
  relay->timed_out = FALSE;
  relay->timeout_source = g_timeout_add_seconds(5, expire, relay);
  while (relay->reqs < count && !relay->timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (relay->timeout_source)
    g_source_remove(relay->timeout_source);
  relay->timeout_source = 0;
  g_assert_cmpuint(relay->reqs, ==, count);
}

static void
test_wire_destinations(void)
{
  WireRelay inbox = {0}, late = {0}, group = {0};
  relay_init(&inbox);
  relay_init(&late);
  relay_init(&group);
  NostrFilters *inbox_filters = nostr_filters_new();
  NostrFilter *inbox_filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(inbox_filters, inbox_filter));
  nostr_filter_free(inbox_filter);
  NostrFilters *group_filters = nostr_filters_new();
  NostrFilter *group_filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(group_filters, group_filter));
  nostr_filter_free(group_filter);
  GhRelayScope *inbox_scope = gh_relay_scope_new(1, inbox_filters, NULL, NULL);
  GhRelayScope *group_scope = gh_relay_scope_new(1, group_filters, NULL, NULL);
  g_assert_true(gh_relay_scope_add_url(inbox_scope, inbox.url, NULL));
  g_assert_true(gh_relay_scope_add_url(group_scope, group.url, NULL));
  gh_relay_scope_start(inbox_scope);
  gh_relay_scope_start(group_scope);
  wait_for_reqs(&inbox, 1);
  wait_for_reqs(&group, 1);
  g_assert_cmpuint(late.reqs, ==, 0);
  g_assert_true(gh_relay_scope_add_url(inbox_scope, late.url, NULL));
  wait_for_reqs(&late, 1);
  g_assert_cmpuint(inbox.reqs, ==, 1);
  g_assert_cmpuint(group.reqs, ==, 1);
  gh_relay_scope_cancel(inbox_scope);
  gh_relay_scope_cancel(group_scope);
  gh_relay_scope_unref(inbox_scope);
  gh_relay_scope_unref(group_scope);
  relay_clear(&inbox);
  relay_clear(&late);
  relay_clear(&group);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay/wire-destinations", test_wire_destinations);
  return g_test_run();
}
