#include "gh-relay-publish.h"
#include "gh-relay-gnostr-write.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-relay.h>

/* GNostrRelay-backed publish transport.
 *
 * Each URL gets a PRIVATE GNostrRelay (never the URL-shared registry), so
 * closing one publish can never cut another publish's or scope's socket.
 *
 * Threading: nostr-gobject emits "ok", "auth-challenge" and "state-changed"
 * from g_idle_add() on the global default main context, i.e. on whichever
 * thread iterates it; that is not necessarily the publish's owning context.
 * The signal handlers therefore only copy their arguments and queue them
 * onto the owning context, where the handle's closed flag is checked before
 * the publish is touched. Frame writes (EVENT, AUTH, the re-sent EVENT)
 * block until the socket write is confirmed, so they run on a GTask worker
 * thread and report a failure back on the owning context.
 *
 * GNostrRelay's own auto-auth (gnostr_relay_set_auth_handler) is never
 * installed: it signs synchronously and sends the 22242 event in an EVENT
 * envelope. The publish drives NIP-42 through send_auth/resend. */

typedef struct {
  gint refs;
  gint closed;             /* atomic; set on the owning context by close */
  GhRelayPublish *publish; /* borrowed; only touched on the owning context
                            * while !closed (the publish closes us first) */
  GMainContext *context;   /* owning context */
  gchar *url;
  gchar *event_frame;      /* ["EVENT",<event>] */
  GNostrRelay *relay;
  GCancellable *cancellable;
  gboolean established;    /* owning context only */
} PublishHandle;

typedef enum { DELIVER_OK, DELIVER_LOST, DELIVER_CHALLENGE } DeliveryKind;

typedef struct {
  PublishHandle *handle;
  DeliveryKind kind;
  gchar *event_id;
  gboolean accepted;
  gchar *message;
} Delivery;

static PublishHandle *
handle_ref(PublishHandle *handle)
{
  g_atomic_int_inc(&handle->refs);
  return handle;
}

static void
handle_unref(gpointer data)
{
  PublishHandle *handle = data;
  if (!g_atomic_int_dec_and_test(&handle->refs))
    return;
  g_clear_object(&handle->relay);
  g_clear_object(&handle->cancellable);
  g_free(handle->event_frame);
  g_main_context_unref(handle->context);
  g_free(handle->url);
  g_free(handle);
}

static void
handle_closure_notify(gpointer data, GClosure *closure)
{
  (void)closure;
  handle_unref(data);
}

static void
delivery_free(gpointer data)
{
  Delivery *delivery = data;
  handle_unref(delivery->handle);
  g_free(delivery->event_id);
  g_free(delivery->message);
  g_free(delivery);
}

static gboolean
deliver(gpointer data)
{
  Delivery *delivery = data;
  PublishHandle *handle = delivery->handle;
  if (g_atomic_int_get(&handle->closed))
    return G_SOURCE_REMOVE;
  switch (delivery->kind) {
  case DELIVER_OK:
    gh_relay_publish_ok(handle->publish, handle->url, delivery->event_id,
                        delivery->accepted, delivery->message);
    break;
  case DELIVER_CHALLENGE:
    gh_relay_publish_auth_challenge(handle->publish, handle->url,
                                    delivery->message);
    break;
  case DELIVER_LOST:
    /* Before or after the handshake, a lost connection means no OK will
     * come: report it now rather than at the failure deadline. */
    gh_relay_publish_failed(handle->publish, handle->url,
                            handle->established
                              ? "relay connection lost before OK"
                              : "relay connection lost before the WebSocket handshake");
    break;
  }
  return G_SOURCE_REMOVE;
}

/* Always queued, never invoked inline, so the publish is never re-entered
 * from inside a GNostrRelay signal emission. */
static void
queue_delivery(PublishHandle *handle, Delivery *delivery)
{
  delivery->handle = handle_ref(handle);
  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, deliver, delivery, delivery_free);
  g_source_attach(source, handle->context);
  g_source_unref(source);
}

static void
on_ok(GNostrRelay *relay, const gchar *event_id, gboolean accepted,
      const gchar *message, gpointer data)
{
  (void)relay;
  PublishHandle *handle = data;
  if (g_atomic_int_get(&handle->closed))
    return;
  Delivery *delivery = g_new0(Delivery, 1);
  delivery->kind = DELIVER_OK;
  delivery->event_id = g_strdup(event_id);
  delivery->accepted = accepted;
  delivery->message = g_strdup(message);
  queue_delivery(handle, delivery);
}

static void
on_auth_challenge(GNostrRelay *relay, const gchar *challenge, gpointer data)
{
  (void)relay;
  PublishHandle *handle = data;
  if (g_atomic_int_get(&handle->closed) || !challenge)
    return;
  Delivery *delivery = g_new0(Delivery, 1);
  delivery->kind = DELIVER_CHALLENGE;
  delivery->message = g_strdup(challenge);
  queue_delivery(handle, delivery);
}

static void
on_state(GNostrRelay *relay, GNostrRelayState old_state,
         GNostrRelayState new_state, gpointer data)
{
  (void)relay;
  (void)old_state;
  PublishHandle *handle = data;
  if (g_atomic_int_get(&handle->closed) ||
      (new_state != GNOSTR_RELAY_STATE_DISCONNECTED &&
       new_state != GNOSTR_RELAY_STATE_ERROR))
    return;
  Delivery *delivery = g_new0(Delivery, 1);
  delivery->kind = DELIVER_LOST;
  queue_delivery(handle, delivery);
}

typedef struct {
  PublishHandle *handle;
  const gchar *what;       /* static: "EVENT" or "AUTH" */
} WriteOp;

/* Runs on the owning context (the GTask was created there). */
static void
on_written(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  WriteOp *op = data;
  PublishHandle *handle = op->handle;
  g_autoptr(GError) error = NULL;
  if (!gh_relay_gnostr_write_finish(result, &error) &&
      !g_atomic_int_get(&handle->closed)) {
    g_autofree gchar *detail = g_strdup_printf("%s write failed: %s", op->what,
                                               error->message);
    gh_relay_publish_failed(handle->publish, handle->url, detail);
  }
  handle_unref(handle);
  g_free(op);
}

static void
write_frame(PublishHandle *handle, const gchar *frame, const gchar *what)
{
  WriteOp *op = g_new0(WriteOp, 1);
  op->handle = handle_ref(handle);
  op->what = what;
  gh_relay_gnostr_write_async(handle->relay, frame, handle->cancellable,
                              on_written, op);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  PublishHandle *handle = data;
  g_autoptr(GError) error = NULL;
  gboolean connected = gnostr_relay_connect_finish(GNOSTR_RELAY(source), result,
                                                   &error);
  if (!g_atomic_int_get(&handle->closed)) {
    NostrRelay *core = gnostr_relay_get_core_relay(handle->relay);
    if (!connected && !(core && nostr_relay_is_established(core))) {
      gh_relay_publish_failed(handle->publish, handle->url,
                              error ? error->message : "relay connection failed");
    } else {
      handle->established = TRUE;
      write_frame(handle, handle->event_frame, "EVENT"); /* OK via "ok" */
    }
  }
  handle_unref(handle); /* the pending connect's reference */
}

static gpointer
open_publish(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
             gpointer transport_data, GError **error)
{
  (void)transport_data;
  gchar *event_frame = gh_relay_gnostr_event_frame("EVENT", event_json);
  if (!event_frame) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "event parse failed");
    return NULL;
  }
  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);
  NostrRelay *core = relay ? gnostr_relay_get_core_relay(relay) : NULL;
  if (!core) {
    g_clear_object(&relay);
    g_free(event_frame);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "relay allocation failed");
    return NULL;
  }
  /* One attempt per publish: a lost connection is CONNECTION_FAILED, never
   * a silent background reconnect-and-resend. */
  nostr_relay_set_auto_reconnect(core, false);

  PublishHandle *handle = g_new0(PublishHandle, 1);
  handle->refs = 1; /* the publish's reference, dropped by close */
  handle->publish = publish;
  handle->context = g_main_context_ref_thread_default();
  handle->url = g_strdup(url);
  handle->event_frame = event_frame;
  handle->relay = relay;
  handle->cancellable = g_cancellable_new();
  g_signal_connect_data(relay, "ok", G_CALLBACK(on_ok), handle_ref(handle),
                        handle_closure_notify, 0);
  g_signal_connect_data(relay, "auth-challenge", G_CALLBACK(on_auth_challenge),
                        handle_ref(handle), handle_closure_notify, 0);
  g_signal_connect_data(relay, "state-changed", G_CALLBACK(on_state),
                        handle_ref(handle), handle_closure_notify, 0);
  gnostr_relay_connect_async(relay, handle->cancellable, on_connected,
                             handle_ref(handle));
  return handle;
}

static gboolean
send_auth(gpointer data, const gchar *signed_event_json, gpointer transport_data,
          GError **error)
{
  (void)transport_data;
  PublishHandle *handle = data;
  g_autofree gchar *frame = gh_relay_gnostr_event_frame("AUTH", signed_event_json);
  if (!frame || !handle->established) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "AUTH frame not sendable");
    return FALSE;
  }
  write_frame(handle, frame, "AUTH");
  return TRUE;
}

static gboolean
resend(gpointer data, gpointer transport_data, GError **error)
{
  (void)transport_data;
  PublishHandle *handle = data;
  if (!handle->established) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "relay connection not established");
    return FALSE;
  }
  write_frame(handle, handle->event_frame, "EVENT");
  return TRUE;
}

static void
close_publish(gpointer data, gpointer transport_data)
{
  (void)transport_data;
  PublishHandle *handle = data;
  g_atomic_int_set(&handle->closed, TRUE);
  g_cancellable_cancel(handle->cancellable);
  g_signal_handlers_disconnect_by_data(handle->relay, handle);
  gnostr_relay_disconnect(handle->relay);
  handle_unref(handle);
}

const GhRelayPublishTransport gh_relay_publish_gnostr_transport = {
  .open = open_publish,
  .close = close_publish,
};

const GhRelayPublishAuthTransport gh_relay_publish_gnostr_auth_transport = {
  .send_auth = send_auth,
  .resend = resend,
};
