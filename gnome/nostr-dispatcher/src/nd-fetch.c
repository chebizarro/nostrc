/* nd-fetch.c — see nd-fetch.h. */
#include "nd-fetch.h"
#include "nd-error.h"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"
#include "error.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "select.h"

#define LOCAL_TIMEOUT_MS_DEFAULT 1500
#define HINTS_BUDGET_MS_DEFAULT 4000
#define HANDSHAKE_TIMEOUT_MS 2500
#define SUB_ID "nostr-dispatcher"

char *nd_fetch_default_socket_path(void) {
  return g_build_filename(g_get_user_runtime_dir(), "nostr", "relay.sock", NULL);
}

char *nd_fetch_filter_json(const NdTarget *t) {
  if (!t) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  switch (t->entity) {
  case ND_ENTITY_EVENT:
    json_builder_set_member_name(b, "ids");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, t->id_hex);
    json_builder_end_array(b);
    break;
  case ND_ENTITY_ADDRESS:
  case ND_ENTITY_PROFILE:
    json_builder_set_member_name(b, "kinds");
    json_builder_begin_array(b);
    json_builder_add_int_value(b, t->entity == ND_ENTITY_PROFILE ? 0 : t->kind);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "authors");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, t->pubkey_hex);
    json_builder_end_array(b);
    if (t->entity == ND_ENTITY_ADDRESS) {
      json_builder_set_member_name(b, "#d");
      json_builder_begin_array(b);
      json_builder_add_string_value(b, t->identifier ? t->identifier : "");
      json_builder_end_array(b);
    }
    break;
  default:
    return NULL;
  }
  json_builder_set_member_name(b, "limit");
  json_builder_add_int_value(b, 1);
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return json_to_string(root, FALSE);
}

/* ------------------------------------------------------------------ */
/* Task state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  NdTarget *t;
  NdFetchOptions opts;
  char *socket_path;
  /* local phase */
  SoupSession *session;
  SoupWebsocketConnection *ws;
  guint timeout_id;
  gulong msg_handler, closed_handler;
  gboolean local_done;
} FetchData;

static void fetch_data_free(gpointer p) {
  FetchData *d = p;
  if (!d) return;
  nd_target_free(d->t);
  g_free(d->socket_path);
  g_clear_object(&d->ws);
  g_clear_object(&d->session);
  g_free(d);
}

static void start_hints(GTask *task);

/* ------------------------------------------------------------------ */
/* Phase 1: session relay over AF_UNIX (libsoup)                       */
/* ------------------------------------------------------------------ */

static void local_finish(GTask *task, NdEvent *ev) {
  FetchData *d = g_task_get_task_data(task);
  if (d->local_done) {
    nd_event_free(ev);
    return;
  }
  d->local_done = TRUE;
  if (d->timeout_id) g_source_remove(d->timeout_id);
  d->timeout_id = 0;
  if (d->ws) {
    if (d->msg_handler) g_signal_handler_disconnect(d->ws, d->msg_handler);
    if (d->closed_handler) g_signal_handler_disconnect(d->ws, d->closed_handler);
    d->msg_handler = d->closed_handler = 0;
    if (soup_websocket_connection_get_state(d->ws) == SOUP_WEBSOCKET_STATE_OPEN) {
      soup_websocket_connection_send_text(d->ws, "[\"CLOSE\",\"" SUB_ID "\"]");
      soup_websocket_connection_close(d->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    }
  }
  if (ev) {
    g_debug("nostr-dispatcher: event found on the session relay");
    g_task_return_pointer(task, ev, (GDestroyNotify)nd_event_free);
    g_object_unref(task);
    return;
  }
  start_hints(task); /* consumes the task ref */
}

static gboolean local_timeout_cb(gpointer user_data) {
  GTask *task = user_data;
  FetchData *d = g_task_get_task_data(task);
  d->timeout_id = 0;
  g_debug("nostr-dispatcher: session relay lookup timed out");
  local_finish(task, NULL);
  return G_SOURCE_REMOVE;
}

static void local_closed_cb(SoupWebsocketConnection *ws, gpointer user_data) {
  local_finish(G_TASK(user_data), NULL);
}

static void local_message_cb(SoupWebsocketConnection *ws, gint type,
                             GBytes *message, gpointer user_data) {
  GTask *task = user_data;
  FetchData *d = g_task_get_task_data(task);
  if (type != SOUP_WEBSOCKET_DATA_TEXT || d->local_done) return;

  gsize len = 0;
  const char *data = g_bytes_get_data(message, &len);
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, data, (gssize)len, NULL)) return;
  JsonNode *root = json_parser_get_root(p);
  if (!JSON_NODE_HOLDS_ARRAY(root)) return;
  JsonArray *a = json_node_get_array(root);
  if (json_array_get_length(a) < 2) return;
  const char *verb = json_array_get_string_element(a, 0);
  if (!verb) return;

  if (strcmp(verb, "EVENT") == 0 && json_array_get_length(a) >= 3 &&
      g_strcmp0(json_array_get_string_element(a, 1), SUB_ID) == 0) {
    JsonNode *evn = json_array_get_element(a, 2);
    g_autofree char *evjson = json_to_string(evn, FALSE);
    NdEvent *ev = nd_event_parse(evjson, -1, NULL);
    if (ev && nd_event_matches_target(ev, d->t)) {
      local_finish(task, ev);
      return;
    }
    nd_event_free(ev);
    g_debug("nostr-dispatcher: session relay returned a non-matching or invalid event");
  } else if ((strcmp(verb, "EOSE") == 0 || strcmp(verb, "CLOSED") == 0) &&
             g_strcmp0(json_array_get_string_element(a, 1), SUB_ID) == 0) {
    local_finish(task, NULL);
  }
}

static void local_connected_cb(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  FetchData *d = g_task_get_task_data(task);
  g_autoptr(GError) err = NULL;
  SoupWebsocketConnection *ws =
      soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
  if (d->local_done) { /* timed out while connecting */
    g_clear_object(&ws);
    g_object_unref(task); /* the connect callback's own ref */
    return;
  }
  if (!ws) {
    g_debug("nostr-dispatcher: session relay unavailable: %s", err->message);
    local_finish(task, NULL); /* consumes the phase ref */
    g_object_unref(task);     /* the connect callback's own ref */
    return;
  }
  d->ws = ws;
  soup_websocket_connection_set_max_incoming_payload_size(ws, 2 * ND_EVENT_MAX_JSON);
  d->msg_handler = g_signal_connect(ws, "message", G_CALLBACK(local_message_cb), task);
  d->closed_handler = g_signal_connect(ws, "closed", G_CALLBACK(local_closed_cb), task);
  g_autofree char *filter = nd_fetch_filter_json(d->t);
  g_autofree char *req = g_strdup_printf("[\"REQ\",\"" SUB_ID "\",%s]", filter);
  soup_websocket_connection_send_text(ws, req);
  g_object_unref(task); /* drop the connect ref; the phase ref is still held */
}

static void start_local(GTask *task) {
  FetchData *d = g_task_get_task_data(task);
  if (!d->opts.use_local || !g_file_test(d->socket_path, G_FILE_TEST_EXISTS)) {
    d->local_done = TRUE;
    start_hints(task);
    return;
  }
  g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(d->socket_path);
  d->session = soup_session_new_with_options("remote-connectable", addr,
                                             "timeout", 2, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", "ws://localhost/");
  guint ms = d->opts.local_timeout_ms ? d->opts.local_timeout_ms : LOCAL_TIMEOUT_MS_DEFAULT;
  d->timeout_id = g_timeout_add(ms, local_timeout_cb, task);
  soup_session_websocket_connect_async(d->session, msg, NULL, NULL, G_PRIORITY_DEFAULT,
                                       g_task_get_cancellable(task),
                                       local_connected_cb, g_object_ref(task));
}

/* ------------------------------------------------------------------ */
/* Phase 2: relay hints (libnostr relay client, worker thread)         */
/* ------------------------------------------------------------------ */

static void on_state(NostrRelay *relay, NostrRelayConnectionState old_state,
                     NostrRelayConnectionState new_state, void *user_data) {
  (void)relay; (void)old_state;
  if (new_state == NOSTR_RELAY_STATE_CONNECTED ||
      new_state == NOSTR_RELAY_STATE_DISCONNECTED) {
    static int one = 1;
    go_channel_try_send((GoChannel *)user_data, &one);
  }
}

static NostrFilters *build_filters(const NdTarget *t) {
  NostrFilter *f = nostr_filter_new();
  if (t->entity == ND_ENTITY_EVENT) {
    nostr_filter_add_id(f, t->id_hex);
  } else {
    int kind = t->entity == ND_ENTITY_PROFILE ? 0 : t->kind;
    nostr_filter_set_kinds(f, &kind, 1);
    nostr_filter_add_author(f, t->pubkey_hex);
    if (t->entity == ND_ENTITY_ADDRESS)
      nostr_filter_tags_append(f, "d", t->identifier ? t->identifier : "", NULL);
  }
  nostr_filter_set_limit(f, 1);
  NostrFilters *fs = nostr_filters_new();
  nostr_filters_add(fs, f); /* moves contents; free the zeroed shell */
  nostr_filter_free(f);
  return fs;
}

static NdEvent *fetch_from_relay(const char *url, const NdTarget *t, gint64 deadline,
                                 GCancellable *cancellable) {
  Error *err = NULL;
  NostrRelay *relay = nostr_relay_new(NULL, url, &err);
  if (!relay) {
    if (err) free_error(err);
    return NULL;
  }
  nostr_relay_set_auto_reconnect(relay, false);
  NdEvent *found = NULL;
  NostrSubscription *sub = NULL;

  if (!nostr_relay_connect(relay, &err)) {
    if (err) free_error(err);
    goto out;
  }
  if (!nostr_relay_is_established(relay)) {
    GoChannel *ready = go_channel_create(1);
    nostr_relay_set_state_callback(relay, on_state, ready);
    if (!nostr_relay_is_established(relay)) {
      gint64 left = (deadline - g_get_monotonic_time()) / 1000;
      GoSelectCase c[1] = {{.op = GO_SELECT_RECEIVE, .chan = ready, .recv_buf = NULL}};
      if (left > 0) go_select_timeout(c, 1, (uint64_t)MIN(left, HANDSHAKE_TIMEOUT_MS));
    }
    nostr_relay_set_state_callback(relay, NULL, NULL);
    go_channel_free(ready);
  }
  if (!nostr_relay_is_established(relay)) goto out;

  /* prepare_subscription registers the sub for dispatch; it requires a
   * non-NULL context (ignored), so pass the relay's own. */
  sub = nostr_relay_prepare_subscription(relay, nostr_relay_get_context(relay),
                                         build_filters(t));
  if (!sub || !nostr_subscription_fire(sub, &err)) {
    if (err) free_error(err);
    goto out;
  }

  GoChannel *events = nostr_subscription_get_events_channel(sub);
  GoChannel *eose = nostr_subscription_get_eose_channel(sub);
  while (!found && !g_cancellable_is_cancelled(cancellable)) {
    gint64 left = (deadline - g_get_monotonic_time()) / 1000;
    if (left <= 0) break;
    NostrEvent *e = NULL;
    void *eose_val = NULL;
    GoSelectCase c[2] = {
        {.op = GO_SELECT_RECEIVE, .chan = events, .recv_buf = (void **)&e},
        {.op = GO_SELECT_RECEIVE, .chan = eose, .recv_buf = &eose_val},
    };
    GoSelectResult r = go_select_timeout(c, 2, (uint64_t)left);
    if (r.selected_case == 0 && e) {
      char *json = nostr_event_serialize_compact(e);
      nostr_event_free(e); /* ownership transferred by the channel */
      NdEvent *ev = json ? nd_event_parse(json, -1, NULL) : NULL;
      free(json);
      if (ev && nd_event_matches_target(ev, t))
        found = ev;
      else
        nd_event_free(ev);
    } else {
      break; /* EOSE, closed channel, or timeout */
    }
  }

out:
  if (sub) {
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
  }
  nostr_relay_disconnect(relay);
  nostr_relay_unref(relay);
  return found;
}

static void hints_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
  FetchData *d = data;
  guint budget = d->opts.hints_budget_ms ? d->opts.hints_budget_ms : HINTS_BUDGET_MS_DEFAULT;
  gint64 deadline = g_get_monotonic_time() + (gint64)budget * 1000;
  for (char **r = d->t->relays; r && *r; r++) {
    if (g_cancellable_is_cancelled(c) || g_get_monotonic_time() >= deadline) break;
    NdEvent *ev = fetch_from_relay(*r, d->t, deadline, c);
    if (ev) {
      g_task_return_pointer(task, ev, (GDestroyNotify)nd_event_free);
      return;
    }
  }
  g_task_return_new_error(task, ND_ERROR, ND_ERROR_NOT_FOUND,
                          "event not found on the session relay or relay hints");
}

static void start_hints(GTask *task) {
  FetchData *d = g_task_get_task_data(task);
  if (!d->opts.use_hints || !d->t->relays || !d->t->relays[0]) {
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_NOT_FOUND,
                            d->opts.use_hints ? "event not found locally and the link "
                                                "carries no usable relay hints"
                                              : "event not found locally (relay-hint "
                                                "fetching disabled)");
    g_object_unref(task);
    return;
  }
  g_task_run_in_thread(task, hints_thread);
  g_object_unref(task);
}

/* ------------------------------------------------------------------ */

void nd_fetch_event_async(const NdTarget *t, const NdFetchOptions *opts,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, nd_fetch_event_async);
  FetchData *d = g_new0(FetchData, 1);
  d->t = nd_target_copy(t);
  if (opts) d->opts = *opts;
  d->socket_path = opts && opts->socket_path ? g_strdup(opts->socket_path)
                                             : nd_fetch_default_socket_path();
  d->opts.socket_path = NULL;
  g_task_set_task_data(task, d, fetch_data_free);
  start_local(task); /* consumes the task ref */
}

NdEvent *nd_fetch_event_finish(GAsyncResult *res, GError **error) {
  return g_task_propagate_pointer(G_TASK(res), error);
}
