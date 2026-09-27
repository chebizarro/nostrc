/* test_relay_transport_libsoup.c - Real libsoup WebSocket transport
 *
 * SPDX-License-Identifier: MIT
 *
 * Spins up an in-process SoupServer, exposes a WebSocket handler that
 * either echoes NIP-01 envelopes or drives NIP-42 AUTH, and points a
 * real nd_relay_transport_new_websocket() client at it. Scenarios:
 *
 *   1. connect → send REQ → receive EVENT + EOSE + OK → disconnect
 *      cleanly. Asserts each envelope routes through the primary
 *      listener and the OK callback in the expected order.
 *   2. NIP-42 AUTH: server pushes ["AUTH", <challenge>] on connect; the
 *      client's auth callback signs an event and the server sees the
 *      returning ["AUTH", …] envelope on its side.
 *   3. NdRelaySync end to end (nostrc-wr3t): the sync layer itself sends
 *      the REQ over the real transport as soon as it connects, and the
 *      event the server answers with is folded into the calendar store.
 *      Before wr3t nothing sent a REQ, so this could only pass against
 *      the fixture.
 *
 * The test runs client + server on the same thread-default main context
 * and blocks on GAsyncQueue signals until each stage settles, so it
 * exercises the real async paths without spawning worker threads.
 */

#include "nd-relay-transport.h"
#include "nd-relay-sync.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-store-db.h"
#include "nd-test-harness.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib.h>

#include <string.h>

/* ---- Server-side handler state ---- */

typedef enum {
  MODE_ECHO_REQ,      /* wait for a REQ, then push EVENT + EOSE + OK */
  MODE_AUTH_FIRST,    /* immediately push AUTH, then echo REQ path */
  MODE_SYNC,          /* REQ -> a foldable kind-31923 EVENT + EOSE */
} ServerMode;

typedef struct {
  ServerMode                mode;
  SoupWebsocketConnection  *conn;
  GAsyncQueue              *server_events;   /* char*, freed by consumer */
  GMainLoop                *loop;
  gboolean                  saw_client_auth;
  gulong                    sig_msg;
  gulong                    sig_closed;
} ServerCtx;

static void
push_event(ServerCtx *sctx, const gchar *tag)
{
  g_async_queue_push(sctx->server_events, g_strdup(tag));
}

/* Fires when the client sends a frame. In echo mode we look for a REQ
 * and reply with EVENT+EOSE+OK. In auth mode we also match a client
 * AUTH reply and record it. */
static void
on_server_message(SoupWebsocketConnection *conn,
                  gint                     type,
                  GBytes                  *message,
                  gpointer                 user_data)
{
  ServerCtx *sctx = user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;

  gsize size = 0;
  const char *data = g_bytes_get_data(message, &size);
  if (data == NULL) return;
  g_autofree gchar *text = g_strndup(data, size);
  push_event(sctx, g_strdup_printf("recv:%s", text));

  if (sctx->mode == MODE_SYNC && g_str_has_prefix(text, "[\"REQ\",\"nd-sync\",")) {
    const gchar *event_json =
      "{\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
      "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"kind\":31923,\"created_at\":1710000000,\"content\":\"from relay\","
      "\"tags\":[[\"d\",\"wire-uid\"],[\"title\",\"Over the wire\"],"
      "[\"start\",\"1710003600\"],[\"end\",\"1710007200\"]],\"sig\":\"deadbeef\"}";
    g_autofree gchar *event_env =
      g_strdup_printf("[\"EVENT\",\"nd-sync\",%s]", event_json);
    soup_websocket_connection_send_text(conn, event_env);
    soup_websocket_connection_send_text(conn, "[\"EOSE\",\"nd-sync\"]");
    return;
  }

  if (g_str_has_prefix(text, "[\"REQ\"")) {
    /* Reply with EVENT (bare event object), EOSE, OK for a fabricated
     * event id "aaaa..." (any 64-hex will do — the client is not
     * verifying sig in this test). */
    const gchar *event_json =
      "{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"kind\":31923,\"created_at\":1710000000,\"content\":\"hi\","
      "\"tags\":[[\"d\",\"uid-1\"]],\"sig\":\"deadbeef\"}";
    g_autofree gchar *event_env =
      g_strdup_printf("[\"EVENT\",\"sub-1\",%s]", event_json);
    soup_websocket_connection_send_text(conn, event_env);
    soup_websocket_connection_send_text(conn, "[\"EOSE\",\"sub-1\"]");
    return;
  }

  if (g_str_has_prefix(text, "[\"EVENT\"")) {
    /* Client published an EVENT. Extract its id (simple scan; robust
     * enough for test payloads) and reply with an OK. */
    const gchar *needle = strstr(text, "\"id\":\"");
    if (needle != NULL) {
      needle += strlen("\"id\":\"");
      const gchar *end = strchr(needle, '"');
      if (end != NULL) {
        g_autofree gchar *id = g_strndup(needle, end - needle);
        g_autofree gchar *ok =
          g_strdup_printf("[\"OK\",\"%s\",true,\"\"]", id);
        soup_websocket_connection_send_text(conn, ok);
      }
    }
    return;
  }

  if (g_str_has_prefix(text, "[\"AUTH\"")) {
    sctx->saw_client_auth = TRUE;
    push_event(sctx, g_strdup("client-auth"));
    /* Kick the main loop so the waiting assertion below can proceed. */
    if (sctx->loop != NULL)
      g_main_loop_quit(sctx->loop);
    return;
  }
}

static void
on_server_closed(SoupWebsocketConnection *conn, gpointer user_data)
{
  (void)conn;
  ServerCtx *sctx = user_data;
  push_event(sctx, g_strdup("server-closed"));
}

static void
server_websocket_cb(SoupServer              *server,
                    SoupServerMessage       *msg,
                    const char              *path,
                    SoupWebsocketConnection *connection,
                    gpointer                 user_data)
{
  (void)server; (void)msg; (void)path;
  ServerCtx *sctx = user_data;
  sctx->conn = g_object_ref(connection);
  sctx->sig_msg = g_signal_connect(connection, "message",
                                   G_CALLBACK(on_server_message), sctx);
  sctx->sig_closed = g_signal_connect(connection, "closed",
                                      G_CALLBACK(on_server_closed), sctx);
  push_event(sctx, g_strdup("server-connected"));

  if (sctx->mode == MODE_AUTH_FIRST) {
    /* Immediately push an AUTH challenge. */
    soup_websocket_connection_send_text(connection,
      "[\"AUTH\",\"challenge-abc-123\"]");
  }
}

/* ---- Client-side listener + callbacks ---- */

typedef struct {
  GAsyncQueue *client_events;    /* char*, freed by consumer */
  GMainLoop   *loop;
  guint        state_transitions;
  gboolean     last_state_connected;
  guint        ok_calls;
  gchar       *last_ok_event_id;
  gboolean     last_ok_accepted;
  gchar       *last_ok_reason;
  guint        auth_calls;
  gchar       *last_auth_challenge;
} ClientCtx;

static void
client_listener(NdRelayTransport *t,
                const gchar      *kind_hint,
                const gchar      *envelope_json,
                gpointer          user_data)
{
  (void)t;
  ClientCtx *c = user_data;
  g_async_queue_push(c->client_events,
    g_strdup_printf("%s:%s", kind_hint ? kind_hint : "?",
                    envelope_json ? envelope_json : ""));
  /* When we see EOSE, quit the loop so the test can advance. */
  if (kind_hint != NULL && g_str_equal(kind_hint, "EOSE") && c->loop != NULL)
    g_main_loop_quit(c->loop);
}

static void
client_state_cb(NdRelayTransport *t, gboolean connected,
                const GError *error, gpointer user_data)
{
  (void)t; (void)error;
  ClientCtx *c = user_data;
  c->state_transitions++;
  c->last_state_connected = connected;
  if (connected && c->loop != NULL)
    g_main_loop_quit(c->loop);
}

static void
client_ok_cb(NdRelayTransport *t, const gchar *event_id,
             gboolean accepted, const gchar *reason, gpointer user_data)
{
  (void)t;
  ClientCtx *c = user_data;
  c->ok_calls++;
  g_free(c->last_ok_event_id);
  c->last_ok_event_id = g_strdup(event_id ? event_id : "");
  c->last_ok_accepted = accepted;
  g_free(c->last_ok_reason);
  c->last_ok_reason = g_strdup(reason ? reason : "");
  if (c->loop != NULL)
    g_main_loop_quit(c->loop);
}

static gchar *
client_auth_cb(NdRelayTransport *t, const gchar *challenge, gpointer user_data)
{
  (void)t;
  ClientCtx *c = user_data;
  c->auth_calls++;
  g_free(c->last_auth_challenge);
  c->last_auth_challenge = g_strdup(challenge);
  /* Return a valid-looking signed event JSON. */
  return g_strdup(
    "{\"id\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
    "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"kind\":22242,\"created_at\":1710000000,\"content\":\"\","
    "\"tags\":[[\"challenge\",\"challenge-abc-123\"]],\"sig\":\"deadbeef\"}");
}

/* ---- Server helpers ---- */

static gchar *
make_ws_url(SoupServer *server)
{
  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  GUri *uri = uris->data;
  gchar *url = g_strdup_printf("ws://127.0.0.1:%d/", g_uri_get_port(uri));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  return url;
}

static SoupServer *
start_server(ServerCtx *sctx, gchar **out_url)
{
  SoupServer *server = soup_server_new("server-header", "nd-test/1.0", NULL);
  soup_server_add_websocket_handler(server, "/", NULL, NULL,
                                    server_websocket_cb, sctx, NULL);
  GError *err = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                         SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error(err);
  *out_url = make_ws_url(server);
  return server;
}

/* Runs the current thread-default main context until @condition returns
 * TRUE or @budget_ms passes. Returns TRUE if the condition met. */
static gboolean
wait_for(gboolean (*condition)(gpointer), gpointer data, gint budget_ms)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)budget_ms * 1000;
  while (!condition(data)) {
    if (g_get_monotonic_time() >= deadline)
      return FALSE;
    g_main_context_iteration(NULL, FALSE);
    /* Yield briefly so socket + timer events actually make progress. */
    g_usleep(2 * 1000);
  }
  return TRUE;
}

static gboolean
have_ok(gpointer data) { return ((ClientCtx *)data)->ok_calls > 0; }

static gboolean
have_connected(gpointer data)
{
  ClientCtx *c = data;
  return c->state_transitions > 0 && c->last_state_connected;
}

static gboolean
have_client_auth(gpointer data) { return ((ClientCtx *)data)->auth_calls > 0; }

/* ---- Scenario 1: REQ → EVENT/EOSE/OK round-trip ---- */

static void
test_req_event_ok_roundtrip(void)
{
  ServerCtx sctx = { .mode = MODE_ECHO_REQ };
  sctx.server_events = g_async_queue_new_full(g_free);

  ClientCtx cctx = {0};
  cctx.client_events = g_async_queue_new_full(g_free);

  g_autofree gchar *url = NULL;
  SoupServer *server = start_server(&sctx, &url);

  NdRelayTransport *t = nd_relay_transport_new_websocket(url);
  nd_relay_transport_set_listener(t, client_listener, &cctx);
  nd_relay_transport_set_state_callback(t, client_state_cb, &cctx);
  nd_relay_transport_set_ok_callback(t, client_ok_cb, &cctx);

  nd_relay_transport_connect_async(t);

  g_assert_true(wait_for(have_connected, &cctx, 5000));
  g_assert_true(nd_relay_transport_is_connected(t));

  /* Send a REQ; server will push EVENT + EOSE. */
  GError *err = NULL;
  g_assert_true(nd_relay_transport_send_frame(t,
    "[\"REQ\",\"sub-1\",{\"kinds\":[31923],\"authors\":[\"11\"]}]", &err));
  g_assert_no_error(err);

  /* Iterate until we've seen the EOSE via the listener. */
  {
    gint64 deadline = g_get_monotonic_time() + 5 * 1000 * 1000;
    gboolean saw_eose = FALSE;
    while (!saw_eose && g_get_monotonic_time() < deadline) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(2 * 1000);
      /* Drain the queue looking for an EOSE line. */
      for (;;) {
        gchar *ev = g_async_queue_try_pop(cctx.client_events);
        if (ev == NULL) break;
        if (g_str_has_prefix(ev, "EOSE:"))
          saw_eose = TRUE;
        g_free(ev);
      }
    }
    g_assert_true(saw_eose);
  }

  /* Publish an EVENT; server replies with OK. */
  const gchar *ev_frame =
    "[\"EVENT\","
    "{\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"kind\":1,\"created_at\":1710000001,\"content\":\"x\","
    "\"tags\":[],\"sig\":\"deadbeef\"}]";
  g_assert_true(nd_relay_transport_send_frame(t, ev_frame, &err));
  g_assert_no_error(err);

  g_assert_true(wait_for(have_ok, &cctx, 5000));
  g_assert_cmpuint(cctx.ok_calls, ==, 1);
  g_assert_cmpstr(cctx.last_ok_event_id, ==,
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  g_assert_true(cctx.last_ok_accepted);

  nd_relay_transport_disconnect(t);
  /* Give libsoup a moment to close the socket cleanly. */
  for (int i = 0; i < 50; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(2 * 1000);
  }

  nd_relay_transport_unref(t);
  g_free(cctx.last_ok_event_id);
  g_free(cctx.last_ok_reason);
  g_free(cctx.last_auth_challenge);
  g_async_queue_unref(cctx.client_events);

  soup_server_disconnect(server);
  g_object_unref(server);
  if (sctx.conn != NULL) g_object_unref(sctx.conn);
  g_async_queue_unref(sctx.server_events);
}

/* ---- Scenario 2: NIP-42 AUTH handshake ---- */

static void
test_nip42_auth_signs_and_sends(void)
{
  ServerCtx sctx = { .mode = MODE_AUTH_FIRST };
  sctx.server_events = g_async_queue_new_full(g_free);

  ClientCtx cctx = {0};
  cctx.client_events = g_async_queue_new_full(g_free);

  g_autofree gchar *url = NULL;
  SoupServer *server = start_server(&sctx, &url);

  NdRelayTransport *t = nd_relay_transport_new_websocket(url);
  nd_relay_transport_set_listener(t, client_listener, &cctx);
  nd_relay_transport_set_state_callback(t, client_state_cb, &cctx);
  nd_relay_transport_set_auth_callback(t, client_auth_cb, &cctx);

  nd_relay_transport_connect_async(t);

  /* Wait until the client's auth callback fired (server pushed AUTH,
   * client dispatched into our stub, transport queued the reply). */
  g_assert_true(wait_for(have_client_auth, &cctx, 5000));
  g_assert_cmpuint(cctx.auth_calls, ==, 1);
  g_assert_cmpstr(cctx.last_auth_challenge, ==, "challenge-abc-123");

  /* Now spin the loop until the server observes the returning AUTH. */
  {
    gint64 deadline = g_get_monotonic_time() + 5 * 1000 * 1000;
    while (!sctx.saw_client_auth && g_get_monotonic_time() < deadline) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(2 * 1000);
    }
  }
  g_assert_true(sctx.saw_client_auth);

  nd_relay_transport_disconnect(t);
  for (int i = 0; i < 50; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(2 * 1000);
  }

  nd_relay_transport_unref(t);
  g_free(cctx.last_ok_event_id);
  g_free(cctx.last_ok_reason);
  g_free(cctx.last_auth_challenge);
  g_async_queue_unref(cctx.client_events);

  soup_server_disconnect(server);
  g_object_unref(server);
  if (sctx.conn != NULL) g_object_unref(sctx.conn);
  g_async_queue_unref(sctx.server_events);
}

/* ---- Scenario 3: NdRelaySync subscribes over a real WebSocket ---- */

typedef struct {
  NdCalendarStore *cal;
} StoreProbe;

static gboolean
have_wire_event(gpointer data)
{
  StoreProbe *p = data;
  NdCalendarEvent *ev = nd_calendar_store_get(p->cal, "wire-uid", NULL);
  gboolean ok = ev != NULL;
  if (ev) nd_calendar_event_free(ev);
  return ok;
}

static void
test_relay_sync_subscribes_over_websocket(void)
{
  ServerCtx sctx = { .mode = MODE_SYNC };
  sctx.server_events = g_async_queue_new_full(g_free);
  g_autofree gchar *url = NULL;
  SoupServer *server = start_server(&sctx, &url);

  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *db_path = nd_test_db_path(dir);
  GError *err = NULL;
  NdStoreDb *db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);
  NdCalendarStore *cal = nd_calendar_store_new(db);
  NdContactStore *contact = nd_contact_store_new(db);

  NdRelaySync *sync = nd_relay_sync_new(db, cal, contact,
                                        nostr_publish_transport_factory_websocket,
                                        NULL);
  const gchar *relays[] = { url, NULL };
  nd_relay_sync_configure(sync,
    "1111111111111111111111111111111111111111111111111111111111111111",
    (GStrv)relays, ND_UPSTREAM_MODE_DIRECT_ONLY, NULL);
  nd_relay_sync_start(sync);

  StoreProbe probe = { cal };
  g_assert_true(wait_for(have_wire_event, &probe, 5000));

  /* The server saw exactly our REQ (with the author filter). */
  gboolean saw_req = FALSE;
  gchar *e;
  while ((e = g_async_queue_try_pop(sctx.server_events)) != NULL) {
    if (g_str_has_prefix(e, "recv:[\"REQ\",\"nd-sync\",") &&
        strstr(e, "\"authors\":[\"1111") != NULL)
      saw_req = TRUE;
    g_free(e);
  }
  g_assert_true(saw_req);

  nd_relay_sync_free(sync);
  nd_calendar_store_free(cal);
  nd_contact_store_free(contact);
  nd_store_db_unref(db);
  nd_test_rm_rf(dir);
  if (sctx.conn) {
    g_signal_handler_disconnect(sctx.conn, sctx.sig_msg);
    g_signal_handler_disconnect(sctx.conn, sctx.sig_closed);
    g_object_unref(sctx.conn);
  }
  soup_server_disconnect(server);
  g_object_unref(server);
  g_async_queue_unref(sctx.server_events);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-dav/transport/req-event-ok-roundtrip",
                  test_req_event_ok_roundtrip);
  g_test_add_func("/nostr-dav/transport/relay-sync-subscribes-over-websocket",
                  test_relay_sync_subscribes_over_websocket);
  g_test_add_func("/nostr-dav/transport/nip42-auth-signs-and-sends",
                  test_nip42_auth_signs_and_sends);
  return g_test_run();
}
