#include "nsp-testutil.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nsp-text.h"

char *nsp_test_pubkey(const char *sk_hex) {
  char *pk = nostr_key_get_public(sk_hex);
  char *r = g_strdup(pk);
  free(pk);
  return r;
}

char *nsp_test_event(const char *sk_hex, int kind, gint64 created_at, const char *content,
                     const char *const *tags) {
  NostrEvent *ev = nostr_event_new();
  g_autofree char *pk = nsp_test_pubkey(sk_hex);
  nostr_event_set_pubkey(ev, pk);
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, content);
  NostrTags *t = nostr_tags_new(0);
  for (const char *const *p = tags; p && p[0] && p[1]; p += 2)
    nostr_tags_append(t, nostr_tag_new(p[0], p[1], NULL));
  nostr_event_set_tags(ev, t);
  g_assert_cmpint(nostr_event_sign(ev, sk_hex), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  char *r = g_strdup(json);
  free(json);
  nostr_event_free(ev);
  return r;
}

char *nsp_test_event_id(const char *event_json) {
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, event_json, -1, NULL));
  return g_strdup(json_object_get_string_member(json_node_get_object(json_parser_get_root(p)), "id"));
}

static gboolean set_true(gpointer p) {
  *(gboolean *)p = TRUE;
  return G_SOURCE_REMOVE;
}

gboolean nsp_test_wait(gboolean *flag, guint timeout_ms) {
  gboolean expired = FALSE;
  guint id = g_timeout_add(timeout_ms, set_true, &expired);
  while (!*flag && !expired) g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(id);
  return *flag;
}

/* ------------------------------------------------------------------------ */

struct NspMockRelay {
  NspMockMode mode;
  gboolean nip50;
  char *dir, *sock;
  SoupServer *server;
  GSocketService *service;
  GPtrArray *events; /* JsonNode* objects */
  GPtrArray *conns;
  guint reqs, search_reqs;
};

static gboolean str_in(JsonObject *f, const char *member, const char *v) {
  JsonArray *a = json_object_get_array_member(f, member);
  for (guint i = 0; i < json_array_get_length(a); i++)
    if (g_strcmp0(json_array_get_string_element(a, i), v) == 0) return TRUE;
  return FALSE;
}

static gboolean tag_value_in(JsonObject *ev, const char *key, JsonArray *vals) {
  JsonArray *tags = json_object_get_array_member(ev, "tags");
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) < 2 || g_strcmp0(json_array_get_string_element(t, 0), key)) continue;
    for (guint j = 0; j < json_array_get_length(vals); j++)
      if (!g_strcmp0(json_array_get_string_element(t, 1), json_array_get_string_element(vals, j)))
        return TRUE;
  }
  return FALSE;
}

static gboolean matches(JsonObject *f, JsonObject *ev) {
  if (json_object_has_member(f, "ids") && !str_in(f, "ids", json_object_get_string_member(ev, "id")))
    return FALSE;
  if (json_object_has_member(f, "authors") &&
      !str_in(f, "authors", json_object_get_string_member(ev, "pubkey")))
    return FALSE;
  if (json_object_has_member(f, "kinds")) {
    JsonArray *k = json_object_get_array_member(f, "kinds");
    gboolean ok = FALSE;
    for (guint i = 0; i < json_array_get_length(k); i++)
      ok |= json_array_get_int_element(k, i) == json_object_get_int_member(ev, "kind");
    if (!ok) return FALSE;
  }
  if (json_object_has_member(f, "#d") && !tag_value_in(ev, "d", json_object_get_array_member(f, "#d")))
    return FALSE;
  if (json_object_has_member(f, "search")) {
    g_autofree char *hay = nsp_text_fold(json_object_get_string_member(ev, "content"));
    g_autofree char *q = nsp_text_fold(json_object_get_string_member(f, "search"));
    g_auto(GStrv) words = g_strsplit(q, " ", -1);
    if (!nsp_text_match_all(hay, (const char *const *)words)) return FALSE;
  }
  return TRUE;
}

static void on_ws_message(SoupWebsocketConnection *ws, gint type, GBytes *msg, gpointer user_data) {
  NspMockRelay *m = user_data;
  g_autoptr(JsonParser) p = json_parser_new();
  gsize len = 0;
  const char *data = g_bytes_get_data(msg, &len);
  if (!json_parser_load_from_data(p, data, (gssize)len, NULL)) return;
  JsonArray *a = json_node_get_array(json_parser_get_root(p));
  if (g_strcmp0(json_array_get_string_element(a, 0), "REQ") != 0) return;
  m->reqs++;
  const char *sub = json_array_get_string_element(a, 1);
  gboolean search = FALSE;
  for (guint i = 2; i < json_array_get_length(a); i++)
    search |= json_object_has_member(json_array_get_object_element(a, i), "search");
  if (search) m->search_reqs++;
  if (m->mode == NSP_MOCK_SILENT) return;
  if (search && !m->nip50) {
    g_autofree char *c = g_strdup_printf("[\"CLOSED\",\"%s\",\"unsupported: search\"]", sub);
    soup_websocket_connection_send_text(ws, c);
    return;
  }
  for (guint i = 2; i < json_array_get_length(a); i++) {
    JsonObject *f = json_array_get_object_element(a, i);
    gint64 limit = json_object_get_int_member_with_default(f, "limit", 500), sent = 0;
    /* newest first, like a relay */
    for (gint j = (gint)m->events->len - 1; j >= 0 && sent < limit; j--) {
      JsonNode *evn = m->events->pdata[j];
      if (!matches(f, json_node_get_object(evn))) continue;
      g_autofree char *evs = json_to_string(evn, FALSE);
      g_autofree char *out = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub, evs);
      soup_websocket_connection_send_text(ws, out);
      sent++;
    }
  }
  if (m->mode == NSP_MOCK_ANSWER) {
    g_autofree char *e = g_strdup_printf("[\"EOSE\",\"%s\"]", sub);
    soup_websocket_connection_send_text(ws, e);
  }
}

static void on_ws(SoupServer *s, SoupServerMessage *msg, const char *path,
                  SoupWebsocketConnection *ws, gpointer user_data) {
  NspMockRelay *m = user_data;
  g_ptr_array_add(m->conns, g_object_ref(ws));
  g_signal_connect(ws, "message", G_CALLBACK(on_ws_message), m);
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn, GObject *src,
                            gpointer user_data) {
  NspMockRelay *m = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GSocketAddress) local = g_socket_connection_get_local_address(conn, NULL);
  if (!soup_server_accept_iostream(m->server, G_IO_STREAM(conn), local, local, &err))
    g_warning("mock relay: %s", err->message);
  return TRUE;
}

NspMockRelay *nsp_mock_relay_new(NspMockMode mode, gboolean nip50) {
  NspMockRelay *m = g_new0(NspMockRelay, 1);
  m->mode = mode;
  m->nip50 = nip50;
  m->dir = g_dir_make_tmp("nsp-relay-XXXXXX", NULL);
  m->sock = g_build_filename(m->dir, "relay.sock", NULL);
  m->events = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
  m->conns = g_ptr_array_new_with_free_func(g_object_unref);
  m->server = soup_server_new(NULL, NULL);
  soup_server_add_websocket_handler(m->server, NULL, NULL, NULL, on_ws, m, NULL);
  /* Accept on our own GSocketService and hand each stream to the
   * SoupServer (soup_server_listen_socket cannot import AF_UNIX fds on
   * every platform). */
  g_autoptr(GError) err = NULL;
  g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(m->sock);
  m->service = g_socket_service_new();
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(m->service), addr, G_SOCKET_TYPE_STREAM,
                                     G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err))
    g_error("listen: %s", err->message);
  g_signal_connect(m->service, "incoming", G_CALLBACK(on_incoming), m);
  g_socket_service_start(m->service);
  return m;
}

void nsp_mock_relay_free(NspMockRelay *m) {
  if (!m) return;
  /* Stop, don't close: the cancelled accept still has a GSource on the
   * listening fd; the socket is closed once that accept completes. */
  g_socket_service_stop(m->service);
  g_object_unref(m->service);
  soup_server_disconnect(m->server);
  g_object_unref(m->server);
  g_ptr_array_unref(m->conns);
  g_ptr_array_unref(m->events);
  g_unlink(m->sock);
  g_rmdir(m->dir);
  g_free(m->sock);
  g_free(m->dir);
  g_free(m);
}

const char *nsp_mock_relay_socket(NspMockRelay *m) { return m->sock; }

void nsp_mock_relay_add(NspMockRelay *m, const char *event_json) {
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, event_json, -1, NULL));
  g_ptr_array_add(m->events, json_node_copy(json_parser_get_root(p)));
}

guint nsp_mock_relay_reqs(NspMockRelay *m) { return m->reqs; }
guint nsp_mock_relay_search_reqs(NspMockRelay *m) { return m->search_reqs; }
