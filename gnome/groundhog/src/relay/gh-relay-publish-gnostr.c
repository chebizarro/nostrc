#include "gh-relay-publish.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-relay.h>

/* GNostrRelay-backed publish transport.
 *
 * Each URL gets a PRIVATE GNostrRelay (never the URL-shared registry), so
 * closing one publish can never cut another publish's or scope's socket.
 *
 * Threading: nostr-gobject emits "ok" and "state-changed" from g_idle_add()
 * on the global default main context, i.e. on whichever thread iterates it;
 * that is not necessarily the publish's owning context. The signal handlers
 * therefore only copy their arguments and queue them onto the owning context,
 * where the handle's closed flag is checked before the publish is touched.
 * The core relay's EVENT write blocks until the socket write is confirmed, so
 * it runs on a GTask worker thread. */

typedef struct {
  gint refs;
  gint closed;             /* atomic; set on the owning context by close */
  GhRelayPublish *publish; /* borrowed; only touched on the owning context
                            * while !closed (the publish closes us first) */
  GMainContext *context;   /* owning context */
  gchar *url;
  NostrEvent *event;       /* moved to the publish job once connected */
  GNostrRelay *relay;
  GCancellable *cancellable;
  gboolean established;    /* owning context only */
} PublishHandle;

typedef enum { DELIVER_OK, DELIVER_LOST } DeliveryKind;

typedef struct {
  PublishHandle *handle;
  DeliveryKind kind;
  gchar *event_id;
  gboolean accepted;
  gchar *message;
} Delivery;

typedef struct {
  GNostrRelay *relay;
  NostrEvent *event;
} PublishJob;

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
  if (handle->event)
    nostr_event_free(handle->event);
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
  if (delivery->kind == DELIVER_OK)
    gh_relay_publish_ok(handle->publish, handle->url, delivery->event_id,
                        delivery->accepted, delivery->message);
  else if (handle->established)
    gh_relay_publish_failed(handle->publish, handle->url,
                            "relay connection lost before OK");
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

static void
publish_job_free(gpointer data)
{
  PublishJob *job = data;
  g_clear_object(&job->relay);
  if (job->event)
    nostr_event_free(job->event);
  g_free(job);
}

static void
publish_thread(GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  (void)source;
  PublishJob *job = task_data;
  NostrRelay *core = gnostr_relay_get_core_relay(job->relay);
  if (core && !g_cancellable_is_cancelled(cancellable))
    nostr_relay_publish(core, job->event); /* the OK arrives via "ok" */
  g_task_return_boolean(task, TRUE);
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
      PublishJob *job = g_new0(PublishJob, 1);
      job->relay = g_object_ref(handle->relay);
      job->event = g_steal_pointer(&handle->event);
      GTask *task = g_task_new(NULL, handle->cancellable, NULL, NULL);
      g_task_set_task_data(task, job, publish_job_free);
      g_task_run_in_thread(task, publish_thread);
      g_object_unref(task);
    }
  }
  handle_unref(handle); /* the pending connect's reference */
}

static gpointer
open_publish(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
             gpointer transport_data, GError **error)
{
  (void)transport_data;
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_signed(event, event_json, NULL) !=
                  NOSTR_EVENT_VALIDATION_OK) {
    if (event)
      nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "event parse failed");
    return NULL;
  }
  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);
  NostrRelay *core = relay ? gnostr_relay_get_core_relay(relay) : NULL;
  if (!core) {
    g_clear_object(&relay);
    nostr_event_free(event);
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
  handle->event = event;
  handle->relay = relay;
  handle->cancellable = g_cancellable_new();
  g_signal_connect_data(relay, "ok", G_CALLBACK(on_ok), handle_ref(handle),
                        handle_closure_notify, 0);
  g_signal_connect_data(relay, "state-changed", G_CALLBACK(on_state),
                        handle_ref(handle), handle_closure_notify, 0);
  gnostr_relay_connect_async(relay, handle->cancellable, on_connected,
                             handle_ref(handle));
  return handle;
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
