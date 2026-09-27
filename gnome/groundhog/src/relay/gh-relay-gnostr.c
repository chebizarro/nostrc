#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_subscription.h>

typedef struct {
  gint refs;
  GhRelayScope *scope; /* borrowed while signals are connected */
  gchar *url;
  GNostrRelay *relay;
  GNostrSubscription *subscription;
  NostrFilters *filters;
  GCancellable *cancellable;
  gboolean closed;
} GhGnostrHandle;

static void
handle_unref(GhGnostrHandle *handle)
{
  if (!g_atomic_int_dec_and_test(&handle->refs))
    return;
  g_clear_object(&handle->subscription);
  g_clear_object(&handle->relay);
  g_clear_object(&handle->cancellable);
  if (handle->filters)
    nostr_filters_free(handle->filters);
  g_free(handle->url);
  g_free(handle);
}

static void
on_event(GNostrSubscription *subscription, const gchar *json, gpointer data)
{
  (void)subscription;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_event(handle->scope, handle->url, json);
}

static void
on_eose(GNostrSubscription *subscription, gpointer data)
{
  (void)subscription;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_eose(handle->scope, handle->url);
}

static void
on_closed(GNostrSubscription *subscription, const gchar *reason, gpointer data)
{
  (void)subscription;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_notice(handle->scope, handle->url,
                           GH_RELAY_NOTICE_CLOSED, NULL, FALSE, reason);
}

static void
on_ok(GNostrRelay *relay, const gchar *event_id, gboolean accepted,
      const gchar *message, gpointer data)
{
  (void)relay;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_notice(handle->scope, handle->url,
                           GH_RELAY_NOTICE_OK, event_id, accepted, message);
}

static void
on_auth(GNostrRelay *relay, const gchar *challenge, gpointer data)
{
  (void)relay;
  (void)challenge;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_notice(handle->scope, handle->url,
                           GH_RELAY_NOTICE_AUTH, NULL, FALSE,
                           "relay authentication requested");
}

static void
on_state(GNostrRelay *relay, GNostrRelayState old_state,
         GNostrRelayState new_state, gpointer data)
{
  (void)relay;
  GhGnostrHandle *handle = data;
  if (!handle->closed && old_state == GNOSTR_RELAY_STATE_CONNECTED &&
      new_state != GNOSTR_RELAY_STATE_CONNECTED)
    gh_relay_scope_notice(handle->scope, handle->url,
                           GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  GhGnostrHandle *handle = data;
  g_autoptr(GError) error = NULL;
  gboolean connected = gnostr_relay_connect_finish(GNOSTR_RELAY(source), result,
                                                     &error);
  if (!handle->closed) {
    if (!connected) {
      gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                             NULL, FALSE,
                             error ? error->message : "relay connection failed");
    } else {
      handle->subscription = gnostr_subscription_new(handle->relay,
                                                       handle->filters);
      if (!handle->subscription) {
        gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                               NULL, FALSE, "subscription creation failed");
      } else {
        handle->filters = NULL; /* subscription takes ownership on success */
        g_signal_connect(handle->subscription, "event", G_CALLBACK(on_event), handle);
        g_signal_connect(handle->subscription, "eose", G_CALLBACK(on_eose), handle);
        g_signal_connect(handle->subscription, "closed", G_CALLBACK(on_closed), handle);
        if (!gnostr_subscription_fire(handle->subscription, &error))
          gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                                 NULL, FALSE,
                                 error ? error->message : "REQ failed");
      }
    }
  }
  gh_relay_scope_unref(handle->scope); /* async operation's lifetime pin */
  handle_unref(handle);
}

static gpointer
open_relay(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
           gpointer user_data, GError **error)
{
  (void)user_data;
  GhGnostrHandle *handle = g_new0(GhGnostrHandle, 1);
  handle->refs = 2; /* scope handle and pending connect callback */
  handle->scope = scope;
  handle->url = g_strdup(url);
  handle->relay = gnostr_relay_new(url);
  handle->cancellable = g_cancellable_new();
  handle->filters = nostr_filters_new();
  if (!handle->relay || !handle->filters) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "relay allocation failed");
    handle_unref(handle);
    handle_unref(handle);
    return NULL;
  }
  for (size_t i = 0; i < filters->count; i++) {
    NostrFilter *copy = nostr_filter_copy(&filters->filters[i]);
    if (!copy || !nostr_filters_add(handle->filters, copy)) {
      if (copy)
        nostr_filter_free(copy);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "filter copy failed");
      handle_unref(handle);
      handle_unref(handle);
      return NULL;
    }
    nostr_filter_free(copy); /* contents moved into the vector */
  }
  g_signal_connect(handle->relay, "ok", G_CALLBACK(on_ok), handle);
  g_signal_connect(handle->relay, "auth-challenge", G_CALLBACK(on_auth), handle);
  g_signal_connect(handle->relay, "state-changed", G_CALLBACK(on_state), handle);
  gh_relay_scope_ref(scope);
  gnostr_relay_connect_async(handle->relay, handle->cancellable, on_connected,
                              handle);
  return handle;
}

static void
close_relay(gpointer data, gpointer user_data)
{
  (void)user_data;
  GhGnostrHandle *handle = data;
  handle->closed = TRUE;
  g_cancellable_cancel(handle->cancellable);
  if (handle->subscription) {
    g_signal_handlers_disconnect_by_data(handle->subscription, handle);
    gnostr_subscription_close(handle->subscription);
  }
  g_signal_handlers_disconnect_by_data(handle->relay, handle);
  gnostr_relay_disconnect(handle->relay);
  handle_unref(handle);
}

const GhRelayTransport gh_relay_gnostr_transport = {
  .open = open_relay,
  .close = close_relay,
};
