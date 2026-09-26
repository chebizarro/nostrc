/* nostr-publish-transport.c - Relay transport interface + backends
 *
 * SPDX-License-Identifier: MIT
 *
 * Two backends live in this file:
 *
 *   1. Real libsoup 3 WebSocket client (SoupSession +
 *      soup_session_websocket_connect_async). Handles wss:// TLS
 *      transparently via GnuTLS, parses inbound NIP-01 envelopes into
 *      routing dispatches (primary listener + optional OK callback +
 *      optional NIP-42 AUTH callback), and drives exponential reconnect
 *      backoff (60 s → 60 min cap, doubled per failed attempt) on any
 *      unclean close. WebSocket-layer keepalive is delegated to libsoup;
 *      the nostr-layer PING frame is deferred (relays that require it
 *      are rare — nostrc-lq12 owns the follow-up).
 *
 *   2. An in-process fixture used by tests. It records every outbound
 *      frame in FIFO order and lets the test deliver EVENT/EOSE/OK
 *      frames back through the listener without a socket.
 *
 * The vtable approach keeps consumers (nostr-dav's relay sync, the
 * publisher) free of ifdefs and lets their tests run under valgrind
 * without a network stack. Moved from gnome/nostr-dav (nd-relay-transport.c).
 */

#include "nostr-publish-transport.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

/* Reconnect backoff — matches nostr-dav's relay-sync policy so the two
 * layers evolve together. Kept private here because reconnect is a transport
 * concern; the sync layer treats each connect/disconnect edge as a state
 * callback and does not itself schedule retries. */
#define NP_WS_BACKOFF_INITIAL_SEC 60
#define NP_WS_BACKOFF_MAX_SEC     (60 * 60)

typedef struct {
  gboolean (*connect_async)   (NostrPublishTransport *self);
  gboolean (*send_frame)      (NostrPublishTransport *self,
                               const gchar      *frame_json,
                               GError          **error);
  void     (*disconnect)      (NostrPublishTransport *self);
  gboolean (*is_connected)    (NostrPublishTransport *self);
  void     (*finalize)        (NostrPublishTransport *self);
} NostrPublishTransportOps;

struct _NostrPublishTransport {
  int                            ref_count;
  const NostrPublishTransportOps     *ops;
  gchar                         *url;

  NostrPublishTransportListener       listener;
  gpointer                       listener_data;
  NostrPublishTransportStateCallback  state_cb;
  gpointer                       state_data;

  NostrPublishTransportOkCallback     ok_cb;
  gpointer                       ok_data;

  NostrPublishTransportAuthCallback   auth_cb;
  gpointer                       auth_data;

  /* Backend-specific state: owned by the ops table, released by
   * ops->finalize(). Only the fixture backend surfaces this at the
   * public boundary. */
  gboolean                       is_fixture;
  gboolean                       fx_connected;
  gpointer                       backend_state;
};

typedef struct {
  GPtrArray *sent;   /* char* frames, FIFO */
} NostrPublishFixtureState;

/* ---- Envelope dispatch (shared by both backends when they receive a
 * text message) --------------------------------------------------------
 *
 * Splits a `["<CMD>", …]` envelope into a fast-path routing hint plus
 * (for OK/AUTH) the structured arguments the specialised callbacks care
 * about. The primary listener always fires. Malformed envelopes are
 * dropped with a debug warning rather than closing the socket — a peer
 * sending garbage should not disrupt the well-formed traffic sharing the
 * connection. */

static void
dispatch_envelope(NostrPublishTransport *self, const gchar *text)
{
  if (text == NULL) return;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, text, -1, &err)) {
    g_debug("nostr-publish-transport(%s): malformed envelope dropped: %s",
            self->url, err ? err->message : "parse error");
    g_clear_error(&err);
    return;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY) {
    g_debug("nostr-publish-transport(%s): envelope is not a JSON array",
            self->url);
    return;
  }
  JsonArray *arr = json_node_get_array(root);
  if (json_array_get_length(arr) < 1)
    return;
  const gchar *cmd = json_array_get_string_element(arr, 0);
  if (cmd == NULL)
    return;

  /* Fire the primary listener first so the sync layer sees every
   * envelope in-order regardless of what the specialised callbacks do. */
  if (self->listener != NULL)
    self->listener(self, cmd, text, self->listener_data);

  if (g_str_equal(cmd, "OK")) {
    /* ["OK", <event_id>, <accepted>, <message>] */
    if (self->ok_cb == NULL)
      return;
    if (json_array_get_length(arr) < 3)
      return;
    const gchar *event_id = json_array_get_string_element(arr, 1);
    if (event_id == NULL) return;
    JsonNode *ok_node = json_array_get_element(arr, 2);
    gboolean accepted = FALSE;
    if (ok_node != NULL && JSON_NODE_TYPE(ok_node) == JSON_NODE_VALUE) {
      GType vt = json_node_get_value_type(ok_node);
      if (vt == G_TYPE_BOOLEAN)
        accepted = json_node_get_boolean(ok_node);
      else if (vt == G_TYPE_INT64)
        accepted = json_node_get_int(ok_node) != 0;
    }
    const gchar *reason = NULL;
    if (json_array_get_length(arr) >= 4)
      reason = json_array_get_string_element(arr, 3);
    self->ok_cb(self, event_id, accepted, reason, self->ok_data);
    return;
  }

  if (g_str_equal(cmd, "AUTH")) {
    /* ["AUTH", <challenge>] server → client */
    if (self->auth_cb == NULL)
      return;
    if (json_array_get_length(arr) < 2)
      return;
    const gchar *challenge = json_array_get_string_element(arr, 1);
    if (challenge == NULL || *challenge == '\0')
      return;
    g_autofree gchar *signed_auth =
      self->auth_cb(self, challenge, self->auth_data);
    if (signed_auth == NULL)
      return;
    /* Emit ["AUTH", <signed_event>] back on the same connection. */
    g_autofree gchar *frame = g_strdup_printf("[\"AUTH\",%s]", signed_auth);
    GError *send_err = NULL;
    if (!self->ops->send_frame(self, frame, &send_err)) {
      g_debug("nostr-publish-transport(%s): AUTH reply failed: %s",
              self->url,
              send_err ? send_err->message : "unknown");
      g_clear_error(&send_err);
    }
    return;
  }

  /* EVENT / EOSE / CLOSED / NOTICE / PING: primary listener already
   * delivered — nothing structured to extract here. */
}

/* ---- Fixture backend ---- */

static gboolean
fx_connect_async(NostrPublishTransport *self)
{
  self->fx_connected = TRUE;
  if (self->state_cb != NULL)
    self->state_cb(self, TRUE, NULL, self->state_data);
  return TRUE;
}

static gboolean
fx_send_frame(NostrPublishTransport *self, const gchar *frame_json, GError **error)
{
  if (!self->fx_connected) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "fixture transport is not connected");
    return FALSE;
  }
  NostrPublishFixtureState *fx = self->backend_state;
  g_ptr_array_add(fx->sent, g_strdup(frame_json));
  return TRUE;
}

static void
fx_disconnect(NostrPublishTransport *self)
{
  gboolean was_connected = self->fx_connected;
  self->fx_connected = FALSE;
  if (was_connected && self->state_cb != NULL)
    self->state_cb(self, FALSE, NULL, self->state_data);
}

static gboolean
fx_is_connected(NostrPublishTransport *self)
{
  return self->fx_connected;
}

static void
fx_finalize(NostrPublishTransport *self)
{
  NostrPublishFixtureState *fx = self->backend_state;
  if (fx != NULL) {
    g_clear_pointer(&fx->sent, g_ptr_array_unref);
    g_free(fx);
    self->backend_state = NULL;
  }
}

static const NostrPublishTransportOps FIXTURE_OPS = {
  .connect_async = fx_connect_async,
  .send_frame    = fx_send_frame,
  .disconnect    = fx_disconnect,
  .is_connected  = fx_is_connected,
  .finalize      = fx_finalize,
};

/* ---- libsoup 3 WebSocket backend ------------------------------------
 *
 * Lifecycle:
 *
 *   connect_async() → arms a SoupSession call; on completion the
 *     "message"/"closed"/"error" signals wire up dispatch + reconnect.
 *
 *   send_frame() → soup_websocket_connection_send_text() when the
 *     connection is OPEN; buffered NOT_CONNECTED otherwise (caller's
 *     responsibility to retry via its own backoff — the sync layer's
 *     REQ resend does this, and the publisher's outbox row stays
 *     pending).
 *
 *   disconnect() → cancels any in-flight connect, closes the socket
 *     with a normal close code, and cancels the reconnect timer. Kept
 *     idempotent so tear_down + finalize is safe.
 *
 *   the "closed" signal → schedules a reconnect on the app's main
 *     context using exponential backoff. `want_connected` distinguishes
 *     "operator asked to disconnect" (no reconnect) from "peer or
 *     network dropped us" (reconnect).
 */

typedef struct {
  NostrPublishTransport         *owner;  /* weak — we live inside its ops table */
  SoupSession              *session;
  SoupWebsocketConnection  *conn;
  GCancellable             *cancellable;

  gulong                    sig_message;
  gulong                    sig_closed;
  gulong                    sig_error;

  guint                     reconnect_source_id;
  guint                     backoff_sec;
  gboolean                  want_connected;
  gboolean                  connecting;   /* an async connect is in flight */
  gchar                    *socket_path;  /* non-NULL: dial this AF_UNIX path */
} NostrPublishWebsocket;

static void ws_start_connect(NostrPublishTransport *self);
static void ws_schedule_reconnect(NostrPublishTransport *self, gboolean reset_backoff);

static void
ws_clear_conn_signals(NostrPublishWebsocket *ws)
{
  if (ws->conn == NULL)
    return;
  if (ws->sig_message != 0) {
    g_signal_handler_disconnect(ws->conn, ws->sig_message);
    ws->sig_message = 0;
  }
  if (ws->sig_closed != 0) {
    g_signal_handler_disconnect(ws->conn, ws->sig_closed);
    ws->sig_closed = 0;
  }
  if (ws->sig_error != 0) {
    g_signal_handler_disconnect(ws->conn, ws->sig_error);
    ws->sig_error = 0;
  }
}

static void
ws_drop_conn(NostrPublishWebsocket *ws)
{
  if (ws->conn == NULL)
    return;
  ws_clear_conn_signals(ws);
  /* Trigger a clean CLOSE if the peer is still connected. */
  if (soup_websocket_connection_get_state(ws->conn) ==
      SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_close(ws->conn,
                                    SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  g_clear_object(&ws->conn);
}

/* ---- WebSocket signal handlers ---- */

static void
on_ws_message(SoupWebsocketConnection *conn,
              gint                     type,
              GBytes                  *message,
              gpointer                 user_data)
{
  (void)conn;
  NostrPublishTransport *self = user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) {
    /* NIP-01 is text; drop binary frames rather than misinterpret. */
    return;
  }
  gsize size = 0;
  const char *data = g_bytes_get_data(message, &size);
  if (data == NULL || size == 0)
    return;
  /* SoupWebsocketConnection guarantees NUL termination for TEXT frames. */
  dispatch_envelope(self, data);
}

static void
on_ws_closed(SoupWebsocketConnection *conn, gpointer user_data)
{
  (void)conn;
  NostrPublishTransport *self = user_data;
  NostrPublishWebsocket *ws = self->backend_state;

  ws_clear_conn_signals(ws);
  g_clear_object(&ws->conn);

  if (self->state_cb != NULL)
    self->state_cb(self, FALSE, NULL, self->state_data);

  if (ws->want_connected)
    ws_schedule_reconnect(self, FALSE);
}

static void
on_ws_error(SoupWebsocketConnection *conn, GError *error, gpointer user_data)
{
  (void)conn;
  NostrPublishTransport *self = user_data;
  g_debug("nostr-publish-transport(%s): websocket error: %s",
          self->url,
          error ? error->message : "unknown");
  /* The "closed" signal follows for an unrecoverable error; the reconnect
   * path is scheduled there so we don't double-schedule. */
}

/* ---- Connect completion ---- */

static void
on_ws_connect_finish(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  NostrPublishTransport *self = user_data;
  NostrPublishWebsocket *ws = self->backend_state;

  ws->connecting = FALSE;

  GError *err = NULL;
  SoupWebsocketConnection *conn =
    soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &err);
  if (conn == NULL) {
    /* NB: the async op holds a ref on the transport for the duration of
     * the connect; on failure we drop it here after handling. */
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error(&err);
      nostr_publish_transport_unref(self);
      return;
    }

    g_debug("nostr-publish-transport(%s): connect failed: %s",
            self->url, err ? err->message : "unknown");
    if (self->state_cb != NULL)
      self->state_cb(self, FALSE, err, self->state_data);
    g_clear_error(&err);

    if (ws->want_connected)
      ws_schedule_reconnect(self, FALSE);
    nostr_publish_transport_unref(self);
    return;
  }

  ws->conn = conn;
  ws->backoff_sec = NP_WS_BACKOFF_INITIAL_SEC;   /* success → reset */

  ws->sig_message = g_signal_connect(conn, "message",
                                     G_CALLBACK(on_ws_message), self);
  ws->sig_closed  = g_signal_connect(conn, "closed",
                                     G_CALLBACK(on_ws_closed), self);
  ws->sig_error   = g_signal_connect(conn, "error",
                                     G_CALLBACK(on_ws_error), self);

  if (self->state_cb != NULL)
    self->state_cb(self, TRUE, NULL, self->state_data);

  nostr_publish_transport_unref(self);
}

static void
ws_start_connect(NostrPublishTransport *self)
{
  NostrPublishWebsocket *ws = self->backend_state;
  if (ws->connecting || ws->conn != NULL)
    return;

  if (ws->session == NULL) {
    /* Session attaches its GSources to the current thread-default main
     * context, which for the daemon is the GApplication's default. */
    if (ws->socket_path != NULL) {
      /* WebSocket over a Unix-domain socket (the per-user session relay
       * at $XDG_RUNTIME_DIR/nostr/relay.sock). The URL still drives the
       * HTTP handshake; the connectable replaces DNS + TCP. */
      g_autoptr(GSocketAddress) addr =
        g_unix_socket_address_new(ws->socket_path);
      ws->session = soup_session_new_with_options("remote-connectable",
                                                  addr, NULL);
    } else {
      ws->session = soup_session_new();
    }
  }
  if (ws->cancellable == NULL)
    ws->cancellable = g_cancellable_new();

  SoupMessage *msg = soup_message_new("GET", self->url);
  if (msg == NULL) {
    GError *err = NULL;
    g_set_error(&err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "invalid relay URL: %s", self->url);
    if (self->state_cb != NULL)
      self->state_cb(self, FALSE, err, self->state_data);
    g_clear_error(&err);
    if (ws->want_connected)
      ws_schedule_reconnect(self, FALSE);
    return;
  }

  ws->connecting = TRUE;
  /* Hold a ref until on_ws_connect_finish runs — the async op needs the
   * transport alive even if the caller unrefs mid-connect. */
  nostr_publish_transport_ref(self);
  soup_session_websocket_connect_async(ws->session, msg,
                                       NULL,           /* origin */
                                       NULL,           /* protocols */
                                       G_PRIORITY_DEFAULT,
                                       ws->cancellable,
                                       on_ws_connect_finish, self);
  g_object_unref(msg);
}

static gboolean
on_reconnect_tick(gpointer user_data)
{
  NostrPublishTransport *self = user_data;
  NostrPublishWebsocket *ws = self->backend_state;
  ws->reconnect_source_id = 0;
  if (ws->want_connected)
    ws_start_connect(self);
  return G_SOURCE_REMOVE;
}

static void
ws_schedule_reconnect(NostrPublishTransport *self, gboolean reset_backoff)
{
  NostrPublishWebsocket *ws = self->backend_state;
  if (ws->reconnect_source_id != 0)
    return;   /* already armed */
  if (reset_backoff)
    ws->backoff_sec = NP_WS_BACKOFF_INITIAL_SEC;

  guint delay = ws->backoff_sec;
  /* Double for the NEXT attempt (this arms with the current value). */
  guint next = ws->backoff_sec * 2u;
  if (next > (guint)NP_WS_BACKOFF_MAX_SEC)
    next = (guint)NP_WS_BACKOFF_MAX_SEC;
  ws->backoff_sec = next;

  ws->reconnect_source_id =
    g_timeout_add_seconds(delay, on_reconnect_tick, self);
}

/* ---- Ops table ---- */

static gboolean
ws_connect_async(NostrPublishTransport *self)
{
  NostrPublishWebsocket *ws = self->backend_state;
  ws->want_connected = TRUE;
  if (ws->backoff_sec == 0)
    ws->backoff_sec = NP_WS_BACKOFF_INITIAL_SEC;
  ws_start_connect(self);
  return TRUE;
}

static gboolean
ws_send_frame(NostrPublishTransport *self, const gchar *frame_json, GError **error)
{
  NostrPublishWebsocket *ws = self->backend_state;
  if (ws->conn == NULL ||
      soup_websocket_connection_get_state(ws->conn) !=
        SOUP_WEBSOCKET_STATE_OPEN) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                "nostr-publish-transport(%s): not connected", self->url);
    return FALSE;
  }
  soup_websocket_connection_send_text(ws->conn, frame_json);
  return TRUE;
}

static void
ws_disconnect(NostrPublishTransport *self)
{
  NostrPublishWebsocket *ws = self->backend_state;
  ws->want_connected = FALSE;
  if (ws->reconnect_source_id != 0) {
    g_source_remove(ws->reconnect_source_id);
    ws->reconnect_source_id = 0;
  }
  if (ws->cancellable != NULL)
    g_cancellable_cancel(ws->cancellable);
  ws_drop_conn(ws);
}

static gboolean
ws_is_connected(NostrPublishTransport *self)
{
  NostrPublishWebsocket *ws = self->backend_state;
  if (ws->conn == NULL)
    return FALSE;
  return soup_websocket_connection_get_state(ws->conn) ==
         SOUP_WEBSOCKET_STATE_OPEN;
}

static void
ws_finalize(NostrPublishTransport *self)
{
  NostrPublishWebsocket *ws = self->backend_state;
  if (ws == NULL)
    return;
  if (ws->reconnect_source_id != 0) {
    g_source_remove(ws->reconnect_source_id);
    ws->reconnect_source_id = 0;
  }
  if (ws->cancellable != NULL)
    g_cancellable_cancel(ws->cancellable);
  ws_drop_conn(ws);
  g_clear_object(&ws->cancellable);
  g_clear_object(&ws->session);
  g_free(ws->socket_path);
  g_free(ws);
  self->backend_state = NULL;
}

static const NostrPublishTransportOps WEBSOCKET_OPS = {
  .connect_async = ws_connect_async,
  .send_frame    = ws_send_frame,
  .disconnect    = ws_disconnect,
  .is_connected  = ws_is_connected,
  .finalize      = ws_finalize,
};

/* ---- Public API ---- */

static NostrPublishTransport *
new_common(const gchar *url, const NostrPublishTransportOps *ops)
{
  NostrPublishTransport *self = g_new0(NostrPublishTransport, 1);
  self->ref_count = 1;
  self->ops       = ops;
  self->url       = g_strdup(url);
  return self;
}

NostrPublishTransport *
nostr_publish_transport_new_websocket(const gchar *url)
{
  g_return_val_if_fail(url != NULL, NULL);
  NostrPublishTransport *self = new_common(url, &WEBSOCKET_OPS);
  NostrPublishWebsocket *ws = g_new0(NostrPublishWebsocket, 1);
  ws->owner       = self;
  ws->backoff_sec = NP_WS_BACKOFF_INITIAL_SEC;
  self->backend_state = ws;
  return self;
}

NostrPublishTransport *
nostr_publish_transport_new_websocket_unix(const gchar *url,
                                           const gchar *socket_path)
{
  g_return_val_if_fail(socket_path != NULL && *socket_path != '\0', NULL);
  NostrPublishTransport *self = nostr_publish_transport_new_websocket(url);
  if (self != NULL) {
    NostrPublishWebsocket *ws = self->backend_state;
    ws->socket_path = g_strdup(socket_path);
  }
  return self;
}

NostrPublishTransport *
nostr_publish_transport_factory_websocket(const gchar *relay_url,
                                          gpointer     user_data)
{
  (void)user_data;
  return nostr_publish_transport_new_websocket(relay_url);
}

NostrPublishTransport *
nostr_publish_transport_new_fixture(const gchar *url)
{
  g_return_val_if_fail(url != NULL, NULL);
  NostrPublishTransport *self = new_common(url, &FIXTURE_OPS);
  self->is_fixture = TRUE;
  NostrPublishFixtureState *fx = g_new0(NostrPublishFixtureState, 1);
  fx->sent = g_ptr_array_new_with_free_func(g_free);
  self->backend_state = fx;
  return self;
}

void
nostr_publish_transport_set_listener(NostrPublishTransport         *self,
                                NostrPublishTransportListener  listener,
                                gpointer                  user_data)
{
  g_return_if_fail(self != NULL);
  self->listener      = listener;
  self->listener_data = user_data;
}

void
nostr_publish_transport_set_state_callback(NostrPublishTransport              *self,
                                      NostrPublishTransportStateCallback  cb,
                                      gpointer                       user_data)
{
  g_return_if_fail(self != NULL);
  self->state_cb   = cb;
  self->state_data = user_data;
}

void
nostr_publish_transport_set_ok_callback(NostrPublishTransport          *self,
                                   NostrPublishTransportOkCallback cb,
                                   gpointer                   user_data)
{
  g_return_if_fail(self != NULL);
  self->ok_cb   = cb;
  self->ok_data = user_data;
}

void
nostr_publish_transport_set_auth_callback(NostrPublishTransport            *self,
                                     NostrPublishTransportAuthCallback cb,
                                     gpointer                     user_data)
{
  g_return_if_fail(self != NULL);
  self->auth_cb   = cb;
  self->auth_data = user_data;
}

void
nostr_publish_transport_connect_async(NostrPublishTransport *self)
{
  g_return_if_fail(self != NULL);
  self->ops->connect_async(self);
}

gboolean
nostr_publish_transport_send_frame(NostrPublishTransport *self,
                              const gchar      *frame_json,
                              GError          **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(frame_json != NULL, FALSE);
  return self->ops->send_frame(self, frame_json, error);
}

void
nostr_publish_transport_disconnect(NostrPublishTransport *self)
{
  g_return_if_fail(self != NULL);
  self->ops->disconnect(self);
}

gboolean
nostr_publish_transport_is_connected(NostrPublishTransport *self)
{
  g_return_val_if_fail(self != NULL, FALSE);
  return self->ops->is_connected(self);
}

const gchar *
nostr_publish_transport_get_url(NostrPublishTransport *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->url;
}

NostrPublishTransport *
nostr_publish_transport_ref(NostrPublishTransport *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_int_inc(&self->ref_count);
  return self;
}

void
nostr_publish_transport_unref(NostrPublishTransport *self)
{
  if (self == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&self->ref_count))
    return;
  if (self->ops->finalize != NULL)
    self->ops->finalize(self);
  g_free(self->url);
  g_free(self);
}

/* ---- Fixture-only helpers ---- */

void
nostr_publish_transport_fixture_deliver_frame(NostrPublishTransport *self,
                                         const gchar      *kind_hint,
                                         const gchar      *envelope_json)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(self->is_fixture);
  g_return_if_fail(envelope_json != NULL);
  /* Test fixture: route through the same dispatcher production uses so
   * OK/AUTH callbacks fire in tests too. `kind_hint` is preserved as a
   * fast-path hint for the primary listener; the dispatcher recomputes
   * the command internally and ignores the hint. */
  (void)kind_hint;
  dispatch_envelope(self, envelope_json);
}

void
nostr_publish_transport_fixture_set_state(NostrPublishTransport *self,
                                     gboolean          connected,
                                     GError           *error)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(self->is_fixture);
  self->fx_connected = connected;
  if (self->state_cb != NULL)
    self->state_cb(self, connected, error, self->state_data);
}

gchar **
nostr_publish_transport_fixture_take_sent(NostrPublishTransport *self,
                                     gsize            *out_len)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(self->is_fixture, NULL);

  NostrPublishFixtureState *fx = self->backend_state;
  gsize n = fx->sent->len;
  gchar **frames = g_new0(gchar *, n + 1);
  for (gsize i = 0; i < n; i++) {
    /* Steal each element so g_ptr_array_set_size() below does not
     * double-free through the array's free_func. */
    frames[i] = g_ptr_array_index(fx->sent, i);
    g_ptr_array_index(fx->sent, i) = NULL;
  }
  g_ptr_array_set_size(fx->sent, 0);
  if (out_len != NULL)
    *out_len = n;
  return frames;
}
