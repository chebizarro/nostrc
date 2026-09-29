#include "gh-relay-soup.h"
#include "gh-net-tls.h"
#include "gh-relay-gnostr-write.h"

#include <libsoup/soup.h>
#include <nostr-envelope.h>
#include <nostr-event.h>
#include <stdlib.h>
#include <string.h>

/* Relay events are small; Groundhog's own wraps are at most 128 KiB. */
#define MAX_FRAME_BYTES (1024 * 1024)
#define KEEPALIVE_S 20
#define DIAL_TIMEOUT_S 30
#define RETRY_MAX_S 30

typedef struct {
  gint refs;
  gboolean closed;             /* close was called: no callback after it */
  gboolean publish_side;
  GhRelayScope *scope;         /* borrowed; the scope closes us first */
  GhRelayPublish *publish;     /* borrowed; the publish closes us first */
  GMainContext *context;       /* owning context */
  gchar *url;
  GProxyResolver *resolver;
  SoupSession *session;
  SoupWebsocketConnection *ws; /* open connection, if any */
  GCancellable *cancellable;   /* the dial in flight */
  GSource *dial_deadline;      /* cancels a dial that takes too long */
  gboolean dial_timed_out;
  GhRelaySoupStatusFunc status;
  gpointer status_data;
  /* scope */
  NostrFilters *filters;
  gchar *sub_id;               /* the live REQ's id, NULL when none */
  guint sub_serial;
  GSource *retry;
  guint retry_seconds;
  /* publish */
  gchar *event_frame;          /* ["EVENT",<event>] */
} SoupHandle;

static SoupHandle *
handle_ref(SoupHandle *handle)
{
  g_atomic_int_inc(&handle->refs);
  return handle;
}

static void
drop_ws(SoupHandle *handle)
{
  if (!handle->ws)
    return;
  g_signal_handlers_disconnect_by_data(handle->ws, handle);
  if (soup_websocket_connection_get_state(handle->ws) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_close(handle->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  g_clear_object(&handle->ws);
}

static void
clear_dial_deadline(SoupHandle *handle)
{
  if (!handle->dial_deadline)
    return;
  g_source_destroy(handle->dial_deadline);
  g_clear_pointer(&handle->dial_deadline, g_source_unref);
}

static void
clear_retry(SoupHandle *handle)
{
  if (!handle->retry)
    return;
  g_source_destroy(handle->retry);
  g_clear_pointer(&handle->retry, g_source_unref);
}

static void
handle_unref(gpointer data)
{
  SoupHandle *handle = data;
  if (!g_atomic_int_dec_and_test(&handle->refs))
    return;
  clear_retry(handle);
  clear_dial_deadline(handle);
  drop_ws(handle);
  g_clear_object(&handle->cancellable);
  if (handle->session)
    soup_session_abort(handle->session);
  g_clear_object(&handle->session);
  g_clear_object(&handle->resolver);
  if (handle->filters)
    nostr_filters_free(handle->filters);
  g_free(handle->sub_id);
  g_free(handle->event_frame);
  g_free(handle->url);
  g_main_context_unref(handle->context);
  g_free(handle);
}

static SoupHandle *
handle_new(const gchar *url, GProxyResolver *resolver, GhRelaySoupStatusFunc status,
           gpointer status_data)
{
  SoupHandle *handle = g_new0(SoupHandle, 1);
  handle->refs = 1; /* the scope's or publish's, dropped by close */
  handle->context = g_main_context_ref_thread_default();
  handle->url = g_strdup(url);
  handle->status = status;
  handle->status_data = status_data;
  g_autoptr(GProxyResolver) direct = resolver ? NULL : g_simple_proxy_resolver_new(NULL, NULL);
  handle->resolver = g_object_ref(resolver ? resolver : direct);
  /* One session per handle: no connection or cookie is shared with another
   * scope or publish, and no TLS session either, since ws_message() turns
   * resumption off (glib-networking's session cache is process-wide, not per
   * SoupSession: gh-net-tls.h). No cookie jar, cache or HSTS is added; no
   * User-Agent or Accept-Language is sent. No I/O timeout: an open WebSocket
   * may be quiet for long; only the dial has a deadline. */
  handle->session = soup_session_new_with_options(
    "proxy-resolver", handle->resolver, "user-agent", NULL, "accept-language-auto", FALSE,
    "timeout", 0, "idle-timeout", 0, NULL);
  soup_session_remove_feature_by_type(handle->session, SOUP_TYPE_CONTENT_SNIFFER);
  handle->retry_seconds = 1;
  return handle;
}

static void
report_status(SoupHandle *handle, gboolean connected)
{
  if (!handle->closed && handle->status)
    handle->status(handle->status_data, connected);
}

static void
send_text(SoupHandle *handle, const gchar *text)
{
  if (handle->ws && soup_websocket_connection_get_state(handle->ws) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text(handle->ws, text);
}

static gboolean
ws_open(SoupHandle *handle)
{
  return handle->ws &&
         soup_websocket_connection_get_state(handle->ws) == SOUP_WEBSOCKET_STATE_OPEN;
}

/* ---- scope side ------------------------------------------------------------------ */

static void
send_req(SoupHandle *handle)
{
  g_free(handle->sub_id);
  handle->sub_id = g_strdup_printf("gh%08x%u", g_random_int(), ++handle->sub_serial);
  /* ["REQ",<id>,<filter>...] */
  NostrReqEnvelope req = {
    .base = { NOSTR_ENVELOPE_REQ },
    .subscription_id = handle->sub_id,
    .filters = handle->filters,
  };
  char *frame = nostr_envelope_serialize_compact(&req.base);
  if (!frame) {
    g_clear_pointer(&handle->sub_id, g_free);
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR, NULL, FALSE,
                          "REQ serialization failed");
    return;
  }
  send_text(handle, frame);
  free(frame);
}

static void dial(SoupHandle *handle);

static gboolean
on_retry(gpointer data)
{
  SoupHandle *handle = data;
  g_clear_pointer(&handle->retry, g_source_unref);
  if (!handle->closed && !handle->ws && !handle->cancellable)
    dial(handle);
  return G_SOURCE_REMOVE;
}

static void
schedule_retry(SoupHandle *handle)
{
  if (handle->closed || handle->retry)
    return;
  guint base = handle->retry_seconds;
  handle->retry_seconds = MIN(base * 2, RETRY_MAX_S);
  /* Jittered ×U(0.5, 1.5) so reconnects do not line up (charter S5). */
  guint delay_ms = (guint)(base * 1000 * g_random_double_range(0.5, 1.5));
  handle->retry = g_timeout_source_new(MAX(delay_ms, 1));
  g_source_set_callback(handle->retry, on_retry, handle_ref(handle), handle_unref);
  g_source_attach(handle->retry, handle->context);
}

static void
scope_message(SoupHandle *handle, NostrEnvelope *envelope)
{
  switch (nostr_envelope_get_type(envelope)) {
  case NOSTR_ENVELOPE_EVENT: {
    NostrEventEnvelope *event = (NostrEventEnvelope *)envelope;
    if (!event->event || g_strcmp0(event->subscription_id, handle->sub_id) != 0)
      return;
    char *json = nostr_event_serialize_compact(event->event);
    if (json)
      gh_relay_scope_event(handle->scope, handle->url, json);
    free(json);
    return;
  }
  case NOSTR_ENVELOPE_EOSE: {
    const gchar *sub_id = nostr_eose_envelope_get_message((NostrEOSEEnvelope *)envelope);
    if (handle->sub_id && g_strcmp0(sub_id, handle->sub_id) == 0)
      gh_relay_scope_eose(handle->scope, handle->url);
    return;
  }
  case NOSTR_ENVELOPE_CLOSED: {
    NostrClosedEnvelope *closed = (NostrClosedEnvelope *)envelope;
    if (!handle->sub_id || g_strcmp0(closed->subscription_id, handle->sub_id) != 0)
      return;
    g_clear_pointer(&handle->sub_id, g_free); /* the relay ended this REQ */
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                          closed->reason);
    return;
  }
  case NOSTR_ENVELOPE_OK: {
    NostrOKEnvelope *ok = (NostrOKEnvelope *)envelope;
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_OK, ok->event_id, ok->ok,
                          ok->reason);
    return;
  }
  case NOSTR_ENVELOPE_AUTH: {
    const gchar *challenge = nostr_auth_envelope_get_challenge((NostrAuthEnvelope *)envelope);
    if (challenge)
      gh_relay_scope_auth_challenge(handle->scope, handle->url, challenge);
    return;
  }
  default:
    return; /* NOTICE and anything else: not logged (PD-10) */
  }
}

/* ---- publish side ---------------------------------------------------------------- */

static void
publish_message(SoupHandle *handle, NostrEnvelope *envelope)
{
  switch (nostr_envelope_get_type(envelope)) {
  case NOSTR_ENVELOPE_OK: {
    NostrOKEnvelope *ok = (NostrOKEnvelope *)envelope;
    if (ok->event_id)
      gh_relay_publish_ok(handle->publish, handle->url, ok->event_id, ok->ok, ok->reason);
    return;
  }
  case NOSTR_ENVELOPE_AUTH: {
    const gchar *challenge = nostr_auth_envelope_get_challenge((NostrAuthEnvelope *)envelope);
    if (challenge)
      gh_relay_publish_auth_challenge(handle->publish, handle->url, challenge);
    return;
  }
  default:
    return;
  }
}

/* ---- connection ------------------------------------------------------------------ */

static void
on_ws_message(SoupWebsocketConnection *ws, SoupWebsocketDataType type, GBytes *message,
              gpointer data)
{
  (void)ws;
  SoupHandle *handle = data;
  if (handle->closed || type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  gsize length = 0;
  const gchar *bytes = g_bytes_get_data(message, &length);
  g_autofree gchar *text = g_strndup(bytes, length);
  NostrEnvelope *envelope = nostr_envelope_parse(text);
  if (!envelope)
    return;
  handle_ref(handle); /* a delivery may close the handle */
  if (handle->publish_side)
    publish_message(handle, envelope);
  else
    scope_message(handle, envelope);
  nostr_envelope_free(envelope);
  handle_unref(handle);
}

static void
on_ws_closed(SoupWebsocketConnection *ws, gpointer data)
{
  (void)ws;
  SoupHandle *handle = data;
  if (handle->closed)
    return;
  handle_ref(handle);
  g_signal_handlers_disconnect_by_data(handle->ws, handle);
  g_clear_object(&handle->ws);
  g_clear_pointer(&handle->sub_id, g_free);
  if (handle->publish_side) {
    /* No OK will come: report it now, not at the publish deadline. */
    gh_relay_publish_failed(handle->publish, handle->url, "relay connection lost before OK");
  } else {
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_DISCONNECTED, NULL,
                          FALSE, NULL);
    schedule_retry(handle);
  }
  handle_unref(handle);
}

static void
on_ws_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  SoupHandle *handle = data;
  g_autoptr(GError) error = NULL;
  SoupWebsocketConnection *ws =
    soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
  g_clear_object(&handle->cancellable);
  clear_dial_deadline(handle);
  if (handle->closed) {
    if (ws) {
      if (soup_websocket_connection_get_state(ws) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
      g_object_unref(ws);
    }
    handle_unref(handle);
    return;
  }
  if (!ws) {
    report_status(handle, FALSE);
    const gchar *detail = handle->dial_timed_out ? "relay connection timed out"
                          : error                ? error->message
                                                 : "relay connection failed";
    if (handle->publish_side) {
      gh_relay_publish_failed(handle->publish, handle->url, detail);
    } else {
      gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR, NULL, FALSE,
                            detail);
      schedule_retry(handle);
    }
    handle_unref(handle);
    return;
  }
  handle->ws = ws;
  soup_websocket_connection_set_max_incoming_payload_size(ws, MAX_FRAME_BYTES);
  soup_websocket_connection_set_keepalive_interval(ws, KEEPALIVE_S);
  g_signal_connect(ws, "message", G_CALLBACK(on_ws_message), handle);
  g_signal_connect(ws, "closed", G_CALLBACK(on_ws_closed), handle);
  report_status(handle, TRUE);
  if (!handle->closed) {
    if (handle->publish_side) {
      send_text(handle, handle->event_frame); /* the OK arrives as a message */
    } else {
      handle->retry_seconds = 1;
      send_req(handle);
    }
  }
  handle_unref(handle); /* the dial's reference */
}

/* libsoup speaks WebSocket over http(s) URIs; the host, port and path are
 * the relay's own. The message resumes no TLS session and leaves none to
 * resume (PD-6, gh-net-tls.h). NULL for a URL libsoup cannot use. */
static SoupMessage *
ws_message(const gchar *url)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (!scheme || !g_uri_get_host(uri) ||
      (g_ascii_strcasecmp(scheme, "ws") != 0 && g_ascii_strcasecmp(scheme, "wss") != 0))
    return NULL;
  gboolean secure = g_ascii_strcasecmp(scheme, "wss") == 0;
  g_autoptr(GUri) http = soup_uri_copy(uri, SOUP_URI_SCHEME, secure ? "https" : "http",
                                       SOUP_URI_NONE);
  SoupMessage *message = http ? soup_message_new_from_uri(SOUP_METHOD_GET, http) : NULL;
  if (message)
    gh_net_tls_no_resumption(message);
  return message;
}

static gboolean
on_dial_deadline(gpointer data)
{
  SoupHandle *handle = data;
  g_clear_pointer(&handle->dial_deadline, g_source_unref);
  if (handle->cancellable) {
    handle->dial_timed_out = TRUE;
    g_cancellable_cancel(handle->cancellable); /* completes as a failed dial */
  }
  return G_SOURCE_REMOVE;
}

static void
dial(SoupHandle *handle)
{
  g_autoptr(SoupMessage) message = ws_message(handle->url);
  g_return_if_fail(message != NULL); /* checked when the handle was opened */
  handle->cancellable = g_cancellable_new();
  handle->dial_timed_out = FALSE;
  handle->dial_deadline = g_timeout_source_new_seconds(DIAL_TIMEOUT_S);
  g_source_set_callback(handle->dial_deadline, on_dial_deadline, handle_ref(handle),
                        handle_unref);
  g_source_attach(handle->dial_deadline, handle->context);
  soup_session_websocket_connect_async(handle->session, message, NULL, NULL,
                                       G_PRIORITY_DEFAULT, handle->cancellable,
                                       on_ws_connected, handle_ref(handle));
}

static gboolean
url_usable(const gchar *url, GError **error)
{
  g_autoptr(SoupMessage) message = ws_message(url);
  if (!message)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "relay URL not usable");
  return message != NULL;
}

static void
handle_close(SoupHandle *handle)
{
  handle->closed = TRUE;
  if (handle->cancellable)
    g_cancellable_cancel(handle->cancellable);
  clear_dial_deadline(handle);
  clear_retry(handle);
  drop_ws(handle);
  handle_unref(handle);
}

static gboolean
send_auth_frame(SoupHandle *handle, const gchar *signed_event_json, GError **error)
{
  g_autofree gchar *frame = gh_relay_gnostr_event_frame("AUTH", signed_event_json);
  if (handle->closed || !frame || !ws_open(handle)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "AUTH frame not sendable");
    return FALSE;
  }
  send_text(handle, frame);
  return TRUE;
}

/* ---- public ---------------------------------------------------------------------- */

gpointer
gh_relay_soup_scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
                         GProxyResolver *resolver, GhRelaySoupStatusFunc status,
                         gpointer status_data, GError **error)
{
  g_return_val_if_fail(scope != NULL && url != NULL && filters != NULL, NULL);
  if (!url_usable(url, error))
    return NULL;
  NostrFilters *copy = nostr_filters_new();
  for (size_t i = 0; copy && i < filters->count; i++) {
    NostrFilter *filter = nostr_filter_copy(&filters->filters[i]);
    if (!filter || !nostr_filters_add(copy, filter)) {
      if (filter)
        nostr_filter_free(filter);
      nostr_filters_free(copy);
      copy = NULL;
      break;
    }
    nostr_filter_free(filter); /* contents moved into the vector */
  }
  if (!copy) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "filter copy failed");
    return NULL;
  }
  SoupHandle *handle = handle_new(url, resolver, status, status_data);
  handle->scope = scope;
  handle->filters = copy;
  dial(handle);
  return handle;
}

void
gh_relay_soup_scope_close(gpointer handle)
{
  handle_close(handle);
}

gboolean
gh_relay_soup_scope_send_auth(gpointer data, const gchar *signed_event_json, GError **error)
{
  return send_auth_frame(data, signed_event_json, error);
}

/* The relay CLOSED the REQ; issue a fresh one on the same connection. */
void
gh_relay_soup_scope_resubscribe(gpointer data)
{
  SoupHandle *handle = data;
  if (!handle->closed && ws_open(handle))
    send_req(handle);
}

gpointer
gh_relay_soup_publish_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
                           GProxyResolver *resolver, GhRelaySoupStatusFunc status,
                           gpointer status_data, GError **error)
{
  g_return_val_if_fail(publish != NULL && url != NULL, NULL);
  if (!url_usable(url, error))
    return NULL;
  gchar *frame = gh_relay_gnostr_event_frame("EVENT", event_json);
  if (!frame) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "event parse failed");
    return NULL;
  }
  SoupHandle *handle = handle_new(url, resolver, status, status_data);
  handle->publish_side = TRUE;
  handle->publish = publish;
  handle->event_frame = frame;
  dial(handle);
  return handle;
}

void
gh_relay_soup_publish_close(gpointer handle)
{
  handle_close(handle);
}

gboolean
gh_relay_soup_publish_send_auth(gpointer data, const gchar *signed_event_json, GError **error)
{
  return send_auth_frame(data, signed_event_json, error);
}

gboolean
gh_relay_soup_publish_resend(gpointer data, GError **error)
{
  SoupHandle *handle = data;
  if (handle->closed || !ws_open(handle)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "relay connection not established");
    return FALSE;
  }
  send_text(handle, handle->event_frame);
  return TRUE;
}

/* ---- vtables: transport_data is the GProxyResolver (NULL: direct) --------------- */

static gpointer
vt_scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  return gh_relay_soup_scope_open(scope, url, filters, data, NULL, NULL, error);
}

static void
vt_scope_close(gpointer handle, gpointer data)
{
  (void)data;
  gh_relay_soup_scope_close(handle);
}

static gboolean
vt_scope_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)data;
  return gh_relay_soup_scope_send_auth(handle, json, error);
}

static void
vt_scope_resubscribe(gpointer handle, gpointer data)
{
  (void)data;
  gh_relay_soup_scope_resubscribe(handle);
}

static gpointer
vt_publish_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
                gpointer data, GError **error)
{
  return gh_relay_soup_publish_open(publish, url, event_json, data, NULL, NULL, error);
}

static void
vt_publish_close(gpointer handle, gpointer data)
{
  (void)data;
  gh_relay_soup_publish_close(handle);
}

static gboolean
vt_publish_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)data;
  return gh_relay_soup_publish_send_auth(handle, json, error);
}

static gboolean
vt_publish_resend(gpointer handle, gpointer data, GError **error)
{
  (void)data;
  return gh_relay_soup_publish_resend(handle, error);
}

const GhRelayTransport gh_relay_soup_transport = { vt_scope_open, vt_scope_close };
const GhRelayAuthTransport gh_relay_soup_auth_transport = { vt_scope_send_auth,
                                                            vt_scope_resubscribe };
const GhRelayPublishTransport gh_relay_soup_publish_transport = { vt_publish_open,
                                                                  vt_publish_close };
const GhRelayPublishAuthTransport gh_relay_soup_publish_auth_transport = { vt_publish_send_auth,
                                                                           vt_publish_resend };
