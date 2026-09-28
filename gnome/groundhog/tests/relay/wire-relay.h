/* Real local NIP-01 relay for Groundhog relay wire tests: a libsoup
 * WebSocket server on 127.0.0.1 that answers every REQ with EOSE and hands
 * every EVENT frame to an optional hook. Waits iterate the default main
 * context; their deadlines are failure bounds only, never progress. */
#ifndef GH_TEST_WIRE_RELAY_H
#define GH_TEST_WIRE_RELAY_H

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <nostr-event.h>
#include <stdlib.h>
#include <string.h>

typedef struct _WireRelay WireRelay;
typedef void (*WireEventFunc)(WireRelay *relay,
                              SoupWebsocketConnection *connection,
                              const gchar *event_id, gpointer data);

struct _WireRelay {
  SoupServer *server;
  GPtrArray *connections;
  gchar *url;
  guint reqs;
  guint events;
  guint closed_sockets;
  WireEventFunc on_event;
  gpointer on_event_data;
};

typedef struct {
  gboolean timed_out;
} WaitState;

/* Returns the id of the signed event in an ["EVENT",{...}] frame, or NULL. */
static G_GNUC_UNUSED gchar *
wire_event_frame_id(const gchar *text)
{
  static const gchar prefix[] = "[\"EVENT\",";
  gsize length = strlen(text);
  if (!g_str_has_prefix(text, prefix) || length < sizeof prefix ||
      text[length - 1] != ']')
    return NULL;
  g_autofree gchar *json = g_strndup(text + sizeof prefix - 1,
                                     length - (sizeof prefix - 1) - 1);
  NostrEvent *event = nostr_event_new();
  gchar id[65];
  gboolean valid = nostr_event_deserialize_signed(event, json, NULL) ==
                     NOSTR_EVENT_VALIDATION_OK &&
                   nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  nostr_event_free(event);
  return valid ? g_strdup(id) : NULL;
}

static G_GNUC_UNUSED void
wire_on_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
                GBytes *message, gpointer data)
{
  WireRelay *relay = data;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  g_autofree gchar *text = g_strndup(bytes, length);
  if (g_str_has_prefix(text, "[\"REQ\"")) {
    relay->reqs++;
    const gchar *comma = strchr(text, ',');
    const gchar *start = comma ? strchr(comma, '"') : NULL;
    const gchar *end = start ? strchr(start + 1, '"') : NULL;
    g_assert_nonnull(end);
    gchar *sub_id = g_strndup(start + 1, end - start - 1);
    g_object_set_data_full(G_OBJECT(connection), "sub-id", sub_id, g_free);
    g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
    soup_websocket_connection_send_text(connection, eose);
  } else if (g_str_has_prefix(text, "[\"EVENT\"")) {
    g_autofree gchar *event_id = wire_event_frame_id(text);
    g_assert_nonnull(event_id);
    relay->events++;
    if (relay->on_event)
      relay->on_event(relay, connection, event_id, relay->on_event_data);
  }
}

static G_GNUC_UNUSED void
wire_on_socket_closed(SoupWebsocketConnection *connection, gpointer data)
{
  (void)connection;
  WireRelay *relay = data;
  relay->closed_sockets++;
}

static G_GNUC_UNUSED void
wire_on_websocket(SoupServer *server, SoupServerMessage *message,
                  const char *path, SoupWebsocketConnection *connection,
                  gpointer data)
{
  (void)server;
  (void)message;
  (void)path;
  WireRelay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(wire_on_message), relay);
  g_signal_connect(connection, "closed", G_CALLBACK(wire_on_socket_closed), relay);
}

static G_GNUC_UNUSED void
relay_init_port(WireRelay *relay, guint16 port)
{
  relay->server = soup_server_new(NULL, NULL);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  soup_server_add_websocket_handler(relay->server, "/relay", NULL, NULL,
                                    wire_on_websocket, relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, port,
                                         SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  GSList *uris = soup_server_get_uris(relay->server);
  g_assert_nonnull(uris);
  g_free(relay->url);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/relay",
                                g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static G_GNUC_UNUSED void
relay_init(WireRelay *relay)
{
  relay_init_port(relay, 0);
}

static G_GNUC_UNUSED void
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

/* A ws:// URL on a port that was free a moment ago and has no listener. */
static G_GNUC_UNUSED gchar *
unused_relay_url(guint16 *port_out)
{
  g_autoptr(GSocketListener) reservation = g_socket_listener_new();
  g_autoptr(GError) error = NULL;
  guint16 port = g_socket_listener_add_any_inet_port(reservation, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(port, >, 0);
  g_socket_listener_close(reservation);
  if (port_out)
    *port_out = port;
  return g_strdup_printf("ws://127.0.0.1:%u/relay", port);
}

static G_GNUC_UNUSED gboolean
expire(gpointer data)
{
  WaitState *wait = data;
  wait->timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

static G_GNUC_UNUSED void
wait_for_count(const guint *counter, guint count)
{
  WaitState wait = {0};
  guint timeout_source = g_timeout_add_seconds(18, expire, &wait);
  while (*counter < count && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!wait.timed_out)
    g_source_remove(timeout_source);
  g_assert_cmpuint(*counter, ==, count);
}

static G_GNUC_UNUSED void
wait_for_reqs(WireRelay *relay, guint count)
{
  wait_for_count(&relay->reqs, count);
}

static G_GNUC_UNUSED void
wait_for_close(WireRelay *relay, guint count)
{
  WaitState wait = {0};
  guint timeout_source = g_timeout_add(3000, expire, &wait);
  while (relay->closed_sockets < count && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!wait.timed_out)
    g_source_remove(timeout_source);
  g_assert_cmpuint(relay->closed_sockets, ==, count);
}

#endif
