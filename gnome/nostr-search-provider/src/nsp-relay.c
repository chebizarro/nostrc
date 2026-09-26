/* nsp-relay.c — see nsp-relay.h. */
#include "nsp-relay.h"

#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>
#include <string.h>

#include "nd-event.h"

void nsp_relay_reply_free(NspRelayReply *r) {
  if (!r) return;
  g_free(r->closed_reason);
  if (r->events) g_ptr_array_unref(r->events);
  g_free(r);
}

const char *nsp_relay_status_name(NspRelayStatus s) {
  switch (s) {
  case NSP_RELAY_EOSE: return "eose";
  case NSP_RELAY_CLOSED: return "closed";
  case NSP_RELAY_TIMEOUT: return "timeout";
  case NSP_RELAY_UNAVAILABLE: return "unavailable";
  case NSP_RELAY_CANCELLED: return "cancelled";
  }
  return "?";
}

char *nsp_relay_default_socket_path(void) {
  return g_build_filename(g_get_user_runtime_dir(), "nostr", "relay.sock", NULL);
}

typedef struct {
  char *socket_path;
  char *req;
  char sub_id[32];
  guint max_events;
  GCancellable *cancellable;
  gulong cancel_id;
  SoupSession *session;
  SoupWebsocketConnection *ws;
  gulong msg_handler, closed_handler;
  guint timeout_id, idle_id;
  gboolean done;
  GHashTable *seen;
  NspRelayReply *reply;
} QueryData;

static void query_data_free(gpointer p) {
  QueryData *d = p;
  g_free(d->socket_path);
  g_free(d->req);
  g_clear_object(&d->cancellable);
  g_clear_object(&d->ws);
  g_clear_object(&d->session);
  g_clear_pointer(&d->seen, g_hash_table_unref);
  nsp_relay_reply_free(d->reply);
  g_free(d);
}

/* Completes the task exactly once. Consumes the "operation" ref. */
static void finish(GTask *task, NspRelayStatus status, const char *reason) {
  QueryData *d = g_task_get_task_data(task);
  if (d->done) return;
  d->done = TRUE;
  if (d->timeout_id) g_source_remove(d->timeout_id);
  if (d->idle_id) g_source_remove(d->idle_id);
  d->timeout_id = d->idle_id = 0;
  if (d->cancel_id) {
    /* Never reached from inside the cancelled handler (that defers to an
     * idle), so disconnecting here cannot deadlock. */
    g_cancellable_disconnect(d->cancellable, d->cancel_id);
    d->cancel_id = 0;
  }
  if (d->ws) {
    if (d->msg_handler) g_signal_handler_disconnect(d->ws, d->msg_handler);
    if (d->closed_handler) g_signal_handler_disconnect(d->ws, d->closed_handler);
    d->msg_handler = d->closed_handler = 0;
    if (soup_websocket_connection_get_state(d->ws) == SOUP_WEBSOCKET_STATE_OPEN) {
      g_autofree char *close = g_strdup_printf("[\"CLOSE\",\"%s\"]", d->sub_id);
      soup_websocket_connection_send_text(d->ws, close);
      soup_websocket_connection_close(d->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    }
  }
  NspRelayReply *r = g_steal_pointer(&d->reply);
  r->status = status;
  r->closed_reason = g_strdup(reason);
  g_task_return_pointer(task, r, (GDestroyNotify)nsp_relay_reply_free);
  g_object_unref(task);
}

static gboolean timeout_cb(gpointer user_data) {
  GTask *task = user_data;
  QueryData *d = g_task_get_task_data(task);
  d->timeout_id = 0;
  finish(task, NSP_RELAY_TIMEOUT, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean cancelled_idle(gpointer user_data) {
  GTask *task = user_data;
  QueryData *d = g_task_get_task_data(task);
  d->idle_id = 0;
  finish(task, NSP_RELAY_CANCELLED, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean unavailable_idle(gpointer user_data) {
  GTask *task = user_data;
  QueryData *d = g_task_get_task_data(task);
  d->idle_id = 0;
  finish(task, NSP_RELAY_UNAVAILABLE, NULL);
  return G_SOURCE_REMOVE;
}

static void on_cancelled(GCancellable *c, gpointer user_data) {
  GTask *task = user_data;
  QueryData *d = g_task_get_task_data(task);
  if (!d->done && !d->idle_id) d->idle_id = g_idle_add(cancelled_idle, task);
}

static void on_closed(SoupWebsocketConnection *ws, gpointer user_data) {
  finish(G_TASK(user_data), NSP_RELAY_UNAVAILABLE, "connection closed");
}

static const char *str_at(JsonArray *a, guint i) {
  if (json_array_get_length(a) <= i) return NULL;
  JsonNode *n = json_array_get_element(a, i);
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
  return json_node_get_string(n);
}

static void on_message(SoupWebsocketConnection *ws, gint type, GBytes *message,
                       gpointer user_data) {
  GTask *task = user_data;
  QueryData *d = g_task_get_task_data(task);
  if (type != SOUP_WEBSOCKET_DATA_TEXT || d->done) return;

  gsize len = 0;
  const char *data = g_bytes_get_data(message, &len);
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, data, (gssize)len, NULL)) return;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) return;
  JsonArray *a = json_node_get_array(root);
  const char *verb = str_at(a, 0), *sub = str_at(a, 1);
  if (!verb || g_strcmp0(sub, d->sub_id) != 0) return;

  if (strcmp(verb, "EVENT") == 0 && json_array_get_length(a) >= 3) {
    if (d->reply->events->len >= d->max_events) return;
    g_autofree char *evjson = json_to_string(json_array_get_element(a, 2), FALSE);
    NdEvent *ev = nd_event_parse(evjson, -1, NULL);
    if (!ev || !ev->validated || !ev->id_hex) {
      d->reply->rejected++;
      nd_event_free(ev);
      return;
    }
    if (!g_hash_table_add(d->seen, g_strdup(ev->id_hex))) {
      nd_event_free(ev);
      return;
    }
    g_ptr_array_add(d->reply->events, ev);
  } else if (strcmp(verb, "EOSE") == 0) {
    finish(task, NSP_RELAY_EOSE, NULL);
  } else if (strcmp(verb, "CLOSED") == 0) {
    finish(task, NSP_RELAY_CLOSED, str_at(a, 2));
  }
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data; /* the connect callback's own ref */
  QueryData *d = g_task_get_task_data(task);
  g_autoptr(GError) err = NULL;
  SoupWebsocketConnection *ws =
      soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
  if (d->done) {
    g_clear_object(&ws);
    g_object_unref(task);
    return;
  }
  if (!ws) {
    g_debug("nostr-search-provider: session relay unavailable: %s", err->message);
    finish(task, NSP_RELAY_UNAVAILABLE, NULL);
    g_object_unref(task);
    return;
  }
  d->ws = ws;
  soup_websocket_connection_set_max_incoming_payload_size(ws, 2 * ND_EVENT_MAX_JSON);
  d->msg_handler = g_signal_connect(ws, "message", G_CALLBACK(on_message), task);
  d->closed_handler = g_signal_connect(ws, "closed", G_CALLBACK(on_closed), task);
  soup_websocket_connection_send_text(ws, d->req);
  g_object_unref(task);
}

static char *build_req(const char *sub_id, JsonNode *filters) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "REQ");
  json_builder_add_string_value(b, sub_id);
  JsonArray *fa = json_node_get_array(filters);
  for (guint i = 0; i < json_array_get_length(fa); i++)
    json_builder_add_value(b, json_node_copy(json_array_get_element(fa, i)));
  json_builder_end_array(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return json_to_string(root, FALSE);
}

void nsp_relay_query_async(const char *socket_path, JsonNode *filters, gint64 deadline_us,
                           guint max_events, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data) {
  static guint counter = 0;
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_source_tag(task, nsp_relay_query_async);
  QueryData *d = g_new0(QueryData, 1);
  d->socket_path = socket_path ? g_strdup(socket_path) : nsp_relay_default_socket_path();
  g_snprintf(d->sub_id, sizeof d->sub_id, "nsp-%u", ++counter);
  d->max_events = max_events ? max_events : 500;
  d->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  d->reply = g_new0(NspRelayReply, 1);
  d->reply->events = g_ptr_array_new_with_free_func((GDestroyNotify)nd_event_free);
  if (cancellable) d->cancellable = g_object_ref(cancellable);
  g_task_set_task_data(task, d, query_data_free);

  if (!filters || !JSON_NODE_HOLDS_ARRAY(filters) ||
      !g_file_test(d->socket_path, G_FILE_TEST_EXISTS)) {
    d->idle_id = g_idle_add(unavailable_idle, task);
    return;
  }
  gint64 left_ms = (deadline_us - g_get_monotonic_time()) / 1000;
  d->timeout_id = g_timeout_add((guint)CLAMP(left_ms, 0, G_MAXINT), timeout_cb, task);
  if (cancellable) {
    d->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), task, NULL);
    if (g_cancellable_is_cancelled(cancellable)) return; /* idle already queued */
  }
  d->req = build_req(d->sub_id, filters);
  g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(d->socket_path);
  d->session = soup_session_new_with_options("remote-connectable", addr, "timeout", 2, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", "ws://localhost/");
  soup_session_websocket_connect_async(d->session, msg, NULL, NULL, G_PRIORITY_DEFAULT,
                                       d->cancellable, on_connected, g_object_ref(task));
}

NspRelayReply *nsp_relay_query_finish(GAsyncResult *res) {
  return g_task_propagate_pointer(G_TASK(res), NULL);
}
