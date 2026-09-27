/*
 * fake_remote_relay.h — a scriptable NIP-01 relay on 127.0.0.1 for the
 * session relay's federation tests (bead nostrc-7d96). Header-only.
 *
 * Runs a libsoup-3 WebSocket server on its own thread / GMainContext so the
 * test thread can block on the relay-under-test and still observe what the
 * "remote" received. Waits are condition-based with deadlines (no sleeps).
 *
 * Modes:
 *   FAKE_ACCEPT      every EVENT -> OK true
 *   FAKE_REJECT      every EVENT -> OK false "blocked: …"
 *   FAKE_AUTH        sends ["AUTH", challenge] on connect; EVENT before a
 *                    valid NIP-42 AUTH -> OK false "auth-required: …";
 *                    AUTH is verified (kind 22242, challenge + relay tags,
 *                    signature) -> OK true; later EVENTs -> OK true
 *   FAKE_DROP_FIRST  the first connection is closed on its first EVENT
 *                    without an OK; later connections accept
 *   FAKE_SILENT      never answers EVENT
 */
#ifndef RELAYD_FAKE_REMOTE_RELAY_H
#define RELAYD_FAKE_REMOTE_RELAY_H

#include <libsoup/soup.h>
#include <string.h>

#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"

typedef enum { FAKE_ACCEPT, FAKE_REJECT, FAKE_AUTH, FAKE_DROP_FIRST, FAKE_SILENT } FakeMode;

typedef struct {
  FakeMode mode;
  guint port;
  char url[64];
  GThread *thread;
  GMainContext *ctx;
  GMainLoop *loop;
  SoupServer *server;
  GMutex lock;
  GCond cond;
  GPtrArray *events;      /* received event ids (every EVENT frame) */
  GPtrArray *kinds;       /* GINT_TO_POINTER(kind), parallel to events */
  GPtrArray *auth_pubkeys;/* pubkeys of valid AUTHs */
  guint connections;
  guint auth_frames;      /* AUTH frames received (valid or not) */
  guint drops;
  gboolean ready;
  GList *open_conns;      /* FakeConn*, fake thread only */
} FakeRelay;

typedef struct {
  FakeRelay *fr;
  SoupWebsocketConnection *conn;
  char challenge[40];
  gboolean authed;
  guint conn_no;
} FakeConn;

static NostrEvent *fake_parse_obj(const char *frame) {
  const char *a = strchr(frame, '{');
  const char *b = strrchr(frame, '}');
  if (!a || !b || b < a) return NULL;
  char *obj = g_strndup(a, (gsize)(b - a + 1));
  NostrEvent *ev = nostr_event_new();
  int rc = nostr_event_deserialize(ev, obj);
  g_free(obj);
  if (rc != 0) {
    nostr_event_free(ev);
    return NULL;
  }
  return ev;
}

static const char *fake_tag(NostrEvent *ev, const char *key) {
  NostrTags *tags = nostr_event_get_tags(ev);
  size_t n = tags ? nostr_tags_size(tags) : 0;
  for (size_t i = 0; i < n; i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    if (t && nostr_tag_size(t) >= 2 && g_strcmp0(nostr_tag_get_key(t), key) == 0)
      return nostr_tag_get(t, 1);
  }
  return NULL;
}

static void fake_send_ok(FakeConn *c, const char *id, gboolean ok, const char *reason) {
  char *f = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id, ok ? "true" : "false", reason);
  soup_websocket_connection_send_text(c->conn, f);
  g_free(f);
}

static void fake_on_message(SoupWebsocketConnection *conn, gint type, GBytes *msg, gpointer ud) {
  (void)conn;
  FakeConn *c = ud;
  FakeRelay *fr = c->fr;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
  const char *text = g_bytes_get_data(msg, NULL);
  if (!text) return;
  if (g_str_has_prefix(text, "[\"AUTH\"")) {
    NostrEvent *ev = fake_parse_obj(text);
    char *id = ev ? nostr_event_get_id(ev) : NULL;
    gboolean valid = ev && nostr_event_get_kind(ev) == 22242 &&
                     g_strcmp0(fake_tag(ev, "challenge"), c->challenge) == 0 &&
                     g_strcmp0(fake_tag(ev, "relay"), fr->url) == 0 &&
                     nostr_event_check_signature(ev);
    g_mutex_lock(&fr->lock);
    fr->auth_frames++;
    if (valid) g_ptr_array_add(fr->auth_pubkeys, g_strdup(nostr_event_get_pubkey(ev)));
    g_cond_broadcast(&fr->cond);
    g_mutex_unlock(&fr->lock);
    if (valid) c->authed = TRUE;
    fake_send_ok(c, id ? id : "0000", valid, valid ? "" : "invalid: bad AUTH");
    free(id);
    if (ev) nostr_event_free(ev);
    return;
  }
  if (!g_str_has_prefix(text, "[\"EVENT\"")) return;
  NostrEvent *ev = fake_parse_obj(text);
  if (!ev) return;
  char *id = nostr_event_get_id(ev);
  g_mutex_lock(&fr->lock);
  g_ptr_array_add(fr->events, g_strdup(id));
  g_ptr_array_add(fr->kinds, GINT_TO_POINTER(nostr_event_get_kind(ev)));
  g_cond_broadcast(&fr->cond);
  g_mutex_unlock(&fr->lock);
  switch (fr->mode) {
    case FAKE_ACCEPT:
      fake_send_ok(c, id, TRUE, "");
      break;
    case FAKE_REJECT:
      fake_send_ok(c, id, FALSE, "blocked: not on the allow list");
      break;
    case FAKE_AUTH:
      if (c->authed) fake_send_ok(c, id, TRUE, "");
      else fake_send_ok(c, id, FALSE, "auth-required: authenticate first");
      break;
    case FAKE_DROP_FIRST:
      if (c->conn_no == 1) {
        if (soup_websocket_connection_get_state(c->conn) == SOUP_WEBSOCKET_STATE_OPEN) {
          g_mutex_lock(&fr->lock);
          fr->drops++;
          g_mutex_unlock(&fr->lock);
          soup_websocket_connection_close(c->conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "bye");
        }
      } else {
        fake_send_ok(c, id, TRUE, "");
      }
      break;
    case FAKE_SILENT:
      break;
  }
  free(id);
  nostr_event_free(ev);
}

static void fake_on_closed(SoupWebsocketConnection *conn, gpointer ud) {
  FakeConn *c = ud;
  c->fr->open_conns = g_list_remove(c->fr->open_conns, c);
  g_signal_handlers_disconnect_by_data(conn, c);
  g_object_unref(c->conn);
  g_free(c);
}

static void fake_on_ws(SoupServer *server, SoupServerMessage *msg, const char *path,
                       SoupWebsocketConnection *conn, gpointer ud) {
  (void)server; (void)msg; (void)path;
  FakeRelay *fr = ud;
  FakeConn *c = g_new0(FakeConn, 1);
  c->fr = fr;
  c->conn = g_object_ref(conn);
  g_mutex_lock(&fr->lock);
  c->conn_no = ++fr->connections;
  g_cond_broadcast(&fr->cond);
  g_mutex_unlock(&fr->lock);
  g_snprintf(c->challenge, sizeof c->challenge, "challenge-%u-%u", fr->port, c->conn_no);
  g_signal_connect(conn, "message", G_CALLBACK(fake_on_message), c);
  g_signal_connect(conn, "closed", G_CALLBACK(fake_on_closed), c);
  fr->open_conns = g_list_prepend(fr->open_conns, c);
  if (fr->mode == FAKE_AUTH) {
    char *f = g_strdup_printf("[\"AUTH\",\"%s\"]", c->challenge);
    soup_websocket_connection_send_text(conn, f);
    g_free(f);
  }
}

static gpointer fake_thread(gpointer p) {
  FakeRelay *fr = p;
  g_main_context_push_thread_default(fr->ctx);
  fr->server = soup_server_new(NULL, NULL);
  soup_server_add_websocket_handler(fr->server, NULL, NULL, NULL, fake_on_ws, fr, NULL);
  GError *err = NULL;
  gboolean ok = soup_server_listen_local(fr->server, fr->port, SOUP_SERVER_LISTEN_IPV4_ONLY, &err);
  if (ok) {
    GSList *uris = soup_server_get_uris(fr->server);
    fr->port = (guint)g_uri_get_port(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    g_snprintf(fr->url, sizeof fr->url, "ws://127.0.0.1:%u", fr->port);
  } else {
    g_printerr("fake relay: listen: %s\n", err->message);
    g_clear_error(&err);
  }
  g_mutex_lock(&fr->lock);
  fr->ready = TRUE;
  g_cond_broadcast(&fr->cond);
  g_mutex_unlock(&fr->lock);
  if (ok) g_main_loop_run(fr->loop);
  while (fr->open_conns) {
    FakeConn *c = fr->open_conns->data;
    fr->open_conns = g_list_delete_link(fr->open_conns, fr->open_conns);
    g_signal_handlers_disconnect_by_data(c->conn, c);
    if (soup_websocket_connection_get_state(c->conn) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(c->conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    g_object_unref(c->conn);
    g_free(c);
  }
  soup_server_disconnect(fr->server);
  g_clear_object(&fr->server);
  while (g_main_context_iteration(fr->ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(fr->ctx);
  return NULL;
}

/* @port 0 = pick a free one. Returns NULL if the listen failed. */
static FakeRelay *fake_relay_start(FakeMode mode, guint port) {
  FakeRelay *fr = g_new0(FakeRelay, 1);
  fr->mode = mode;
  fr->port = port;
  g_mutex_init(&fr->lock);
  g_cond_init(&fr->cond);
  fr->events = g_ptr_array_new_with_free_func(g_free);
  fr->kinds = g_ptr_array_new();
  fr->auth_pubkeys = g_ptr_array_new_with_free_func(g_free);
  fr->ctx = g_main_context_new();
  fr->loop = g_main_loop_new(fr->ctx, FALSE);
  fr->thread = g_thread_new("fake-relay", fake_thread, fr);
  g_mutex_lock(&fr->lock);
  while (!fr->ready) g_cond_wait(&fr->cond, &fr->lock);
  g_mutex_unlock(&fr->lock);
  if (!fr->url[0]) {
    g_thread_join(fr->thread);
    g_free(fr);
    return NULL;
  }
  return fr;
}

static gboolean fake_quit(gpointer p) {
  g_main_loop_quit(p);
  return G_SOURCE_REMOVE;
}

static void fake_relay_stop(FakeRelay *fr) {
  if (!fr) return;
  GSource *s = g_idle_source_new();
  g_source_set_callback(s, fake_quit, fr->loop, NULL);
  g_source_attach(s, fr->ctx);
  g_source_unref(s);
  g_thread_join(fr->thread);
  g_main_loop_unref(fr->loop);
  g_main_context_unref(fr->ctx);
  g_ptr_array_unref(fr->events);
  g_ptr_array_unref(fr->kinds);
  g_ptr_array_unref(fr->auth_pubkeys);
  g_mutex_clear(&fr->lock);
  g_cond_clear(&fr->cond);
  g_free(fr);
}

static guint fake_count_id_locked(FakeRelay *fr, const char *id) {
  guint n = 0;
  for (guint i = 0; i < fr->events->len; i++)
    if (strcmp(g_ptr_array_index(fr->events, i), id) == 0) n++;
  return n;
}

static guint fake_count_id(FakeRelay *fr, const char *id) {
  g_mutex_lock(&fr->lock);
  guint n = fake_count_id_locked(fr, id);
  g_mutex_unlock(&fr->lock);
  return n;
}

/* Wait until @id has been received at least @n times. */
static gboolean fake_wait_id(FakeRelay *fr, const char *id, guint n, int timeout_s) {
  gint64 end = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
  g_mutex_lock(&fr->lock);
  gboolean ok;
  while (!(ok = fake_count_id_locked(fr, id) >= n))
    if (!g_cond_wait_until(&fr->cond, &fr->lock, end)) break;
  ok = fake_count_id_locked(fr, id) >= n;
  g_mutex_unlock(&fr->lock);
  return ok;
}

static guint fake_auth_count(FakeRelay *fr, const char *pubkey) {
  g_mutex_lock(&fr->lock);
  guint n = 0;
  for (guint i = 0; i < fr->auth_pubkeys->len; i++)
    if (!pubkey || strcmp(g_ptr_array_index(fr->auth_pubkeys, i), pubkey) == 0) n++;
  g_mutex_unlock(&fr->lock);
  return n;
}

#endif /* RELAYD_FAKE_REMOTE_RELAY_H */
