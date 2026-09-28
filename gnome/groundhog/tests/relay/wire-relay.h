/* Real local NIP-01 relay for Groundhog relay wire tests: a libsoup
 * WebSocket server on 127.0.0.1 that answers every REQ with EOSE and hands
 * every EVENT frame to an optional hook. Waits iterate the default main
 * context; their deadlines are failure bounds only, never progress.
 *
 * With require_auth it is a NIP-42 relay: every connection gets its own
 * ["AUTH",<challenge>] as soon as it opens, and until that connection has
 * authenticated a REQ is answered with CLOSED "auth-required:" and an EVENT
 * with OK false "auth-required:". An ["AUTH",<event>] is accepted (OK true)
 * only if the event is a validly signed kind 22242 carrying that
 * connection's challenge and this relay's URL, and refuse_auth is unset. */
#ifndef GH_TEST_WIRE_RELAY_H
#define GH_TEST_WIRE_RELAY_H

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <nostr-event.h>
#include <nostr-tag.h>
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
  /* NIP-42 and failure modes */
  gboolean require_auth;
  gboolean refuse_auth;      /* answer every AUTH with OK false */
  gboolean close_on_event;   /* drop the socket on EVENT, before any OK */
  gboolean close_on_connect; /* drop the socket right after the upgrade */
  guint challenges_sent;
  guint auth_frames;
  guint auth_ok;
  guint closed_reqs;         /* REQs refused with CLOSED auth-required */
  guint refused_events;      /* EVENTs refused with OK false auth-required */
  GPtrArray *auth_pubkeys;   /* pubkey of every AUTH frame, in order */
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

static G_GNUC_UNUSED gboolean
wire_authed(SoupWebsocketConnection *connection)
{
  return g_object_get_data(G_OBJECT(connection), "authed") != NULL;
}

static G_GNUC_UNUSED gboolean
wire_tag_is(NostrTags *tags, const gchar *key, const gchar *value)
{
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) == 0)
      return g_strcmp0(nostr_tag_get_value(tag), value) == 0;
  }
  return FALSE;
}

/* ["AUTH",{...}]: the relay-side NIP-42 check. Fills id (65 bytes). */
static G_GNUC_UNUSED gboolean
wire_auth_valid(WireRelay *relay, SoupWebsocketConnection *connection,
                const gchar *text, gchar *id, gchar **pubkey)
{
  static const gchar prefix[] = "[\"AUTH\",";
  gsize length = strlen(text);
  if (length < sizeof prefix || text[length - 1] != ']')
    return FALSE;
  g_autofree gchar *json = g_strndup(text + sizeof prefix - 1,
                                     length - (sizeof prefix - 1) - 1);
  NostrEvent *event = nostr_event_new();
  const gchar *challenge = g_object_get_data(G_OBJECT(connection), "challenge");
  gboolean valid =
    nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(event) == 22242 &&
    wire_tag_is(nostr_event_get_tags(event), "challenge", challenge) &&
    wire_tag_is(nostr_event_get_tags(event), "relay", relay->url);
  *pubkey = g_strdup(nostr_event_get_pubkey(event));
  nostr_event_free(event);
  return valid;
}

static G_GNUC_UNUSED void
wire_send_ok(SoupWebsocketConnection *connection, const gchar *id,
             gboolean accepted, const gchar *message)
{
  g_autofree gchar *frame = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id,
                                            accepted ? "true" : "false", message);
  soup_websocket_connection_send_text(connection, frame);
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
    g_autofree gchar *reply = NULL;
    if (relay->require_auth && !wire_authed(connection)) {
      relay->closed_reqs++;
      reply = g_strdup_printf("[\"CLOSED\",\"%s\",\"auth-required: members only\"]",
                              sub_id);
    } else {
      reply = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
    }
    soup_websocket_connection_send_text(connection, reply);
  } else if (g_str_has_prefix(text, "[\"EVENT\"")) {
    g_autofree gchar *event_id = wire_event_frame_id(text);
    g_assert_nonnull(event_id);
    if (relay->close_on_event) {
      relay->events++;
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    } else if (relay->require_auth && !wire_authed(connection)) {
      relay->refused_events++;
      wire_send_ok(connection, event_id, FALSE, "auth-required: members only");
    } else {
      relay->events++;
      if (relay->on_event)
        relay->on_event(relay, connection, event_id, relay->on_event_data);
    }
  } else if (g_str_has_prefix(text, "[\"AUTH\"")) {
    relay->auth_frames++;
    gchar id[65] = {0};
    gchar *pubkey = NULL;
    gboolean valid = wire_auth_valid(relay, connection, text, id, &pubkey);
    g_assert_true(valid); /* Groundhog must never send an unverified AUTH */
    g_ptr_array_add(relay->auth_pubkeys, pubkey);
    if (relay->refuse_auth) {
      wire_send_ok(connection, id, FALSE, "restricted: not a member");
    } else {
      relay->auth_ok++;
      g_object_set_data(G_OBJECT(connection), "authed", GINT_TO_POINTER(1));
      wire_send_ok(connection, id, TRUE, "");
    }
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
  if (relay->close_on_connect) {
    soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    return;
  }
  if (relay->require_auth) {
    gchar *challenge = g_strdup_printf("challenge-%u-%p", ++relay->challenges_sent,
                                       (void *)connection);
    g_object_set_data_full(G_OBJECT(connection), "challenge", challenge, g_free);
    g_autofree gchar *frame = g_strdup_printf("[\"AUTH\",\"%s\"]", challenge);
    soup_websocket_connection_send_text(connection, frame);
  }
}

static G_GNUC_UNUSED void
relay_init_port(WireRelay *relay, guint16 port)
{
  relay->server = soup_server_new(NULL, NULL);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  if (!relay->auth_pubkeys)
    relay->auth_pubkeys = g_ptr_array_new_with_free_func(g_free);
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
  for (guint i = 0; i < relay->connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay->connections, i);
    /* The connection can outlive this (stack) relay during libsoup's close
     * handshake; its handlers must not write into a later test's frame. */
    g_signal_handlers_disconnect_by_data(connection, relay);
    if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }
  g_ptr_array_unref(relay->connections);
  g_clear_pointer(&relay->auth_pubkeys, g_ptr_array_unref);
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
