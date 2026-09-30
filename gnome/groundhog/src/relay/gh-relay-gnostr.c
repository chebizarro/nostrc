#include "gh-relay-scope.h"
#include "gh-relay-gnostr-write.h"

#include <gio/gio.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
#include <nostr-relay.h>

typedef struct {
  gint refs;
  GhRelayScope *scope; /* borrowed while signals are connected */
  gchar *url;
  GNostrRelay *relay;
  GNostrSubscription *subscription;
  NostrFilters *filters;
  GCancellable *cancellable;
  guint retry_source;
  guint retry_delay_seconds;
  gboolean connect_pending;
  gboolean closed;
} GhGnostrHandle;

/* gnostr_relay_new() returns a process-wide wrapper shared by URL, so
 * disconnecting one scope's endpoint would also cut every other scope (or the
 * next account's scope) on that URL. Each endpoint owns its own socket. */
static GNostrRelay *
private_relay(const gchar *url)
{
  return g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);
}

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

/* A relay's CLOSED, or the subscription's own backlog ceiling (nostrc-5rfp),
 * reported with the scope's overflow prefix whatever GNostrSubscription's
 * wording. */
static void
on_closed(GNostrSubscription *subscription, const gchar *reason, gpointer data)
{
  GhGnostrHandle *handle = data;
  if (handle->closed)
    return;
  if (gnostr_subscription_get_overflowed(subscription))
    reason = GH_RELAY_CLOSED_OVERFLOW_PREFIX " too many events waiting to be read";
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

/* GNostrRelay's own auto-auth (gnostr_relay_set_auth_handler) is never
 * installed: it signs synchronously and sends the 22242 event in an EVENT
 * envelope. The scope decides whether to authenticate; see send_auth. */
static void
on_auth(GNostrRelay *relay, const gchar *challenge, gpointer data)
{
  (void)relay;
  GhGnostrHandle *handle = data;
  if (!handle->closed)
    gh_relay_scope_auth_challenge(handle->scope, handle->url, challenge);
}

static void
reset_subscription(GhGnostrHandle *handle)
{
  if (!handle->subscription)
    return;
  g_signal_handlers_disconnect_by_data(handle->subscription, handle);
  gnostr_subscription_close(handle->subscription);
  g_clear_object(&handle->subscription);
}

/* The filters of one GNostrSubscription, which gnostr_subscription_new()
 * takes ownership of on success; NULL if copying failed. (GNostrSubscription
 * drops them unfreed when it finalizes, nostrc-jwj0: the sanitizer job's
 * tests/lsan.supp names this function, so it stays a frame of its own.) */
static G_GNUC_NO_INLINE NostrFilters *
subscription_filters(const NostrFilters *filters)
{
  NostrFilters *copy = nostr_filters_new();
  if (!copy)
    return NULL;
  for (size_t i = 0; i < filters->count; i++) {
    NostrFilter *filter = nostr_filter_copy(&filters->filters[i]);
    if (!filter || !nostr_filters_add(copy, filter)) {
      if (filter)
        nostr_filter_free(filter);
      nostr_filters_free(copy);
      return NULL;
    }
    nostr_filter_free(filter); /* contents moved into the vector */
  }
  return copy;
}

static void
ensure_subscription(GhGnostrHandle *handle)
{
  if (handle->closed || handle->subscription ||
      !nostr_relay_is_established(gnostr_relay_get_core_relay(handle->relay)))
    return;

  NostrFilters *copy = subscription_filters(handle->filters);
  if (!copy) {
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                           NULL, FALSE, "filter copy failed");
    return;
  }

  handle->subscription = gnostr_subscription_new(handle->relay, copy);
  if (!handle->subscription) {
    nostr_filters_free(copy); /* constructor only takes ownership on success */
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                           NULL, FALSE, "subscription creation failed");
    return;
  }
  /* The scope keeps only what "event" hands it; no event may be dropped
   * (nostrc-dha5). Lossless is the default; say so. Its backlog ceiling
   * (the default) ends a runaway REQ with an overflow CLOSED instead of
   * growing without bound (nostrc-5rfp; see gh-relay-scope.h). */
  gnostr_subscription_set_lossless(handle->subscription, TRUE);
  g_signal_connect(handle->subscription, "event", G_CALLBACK(on_event), handle);
  g_signal_connect(handle->subscription, "eose", G_CALLBACK(on_eose), handle);
  g_signal_connect(handle->subscription, "closed", G_CALLBACK(on_closed), handle);
  g_autoptr(GError) error = NULL;
  if (!gnostr_subscription_fire(handle->subscription, &error)) {
    reset_subscription(handle);
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                           NULL, FALSE, error ? error->message : "REQ failed");
  }
}

static void on_connected(GObject *source, GAsyncResult *result, gpointer data);
static void on_state(GNostrRelay *relay, GNostrRelayState old_state,
                     GNostrRelayState new_state, gpointer data);

static void
attach_relay_signals(GhGnostrHandle *handle)
{
  g_signal_connect(handle->relay, "ok", G_CALLBACK(on_ok), handle);
  g_signal_connect(handle->relay, "auth-challenge", G_CALLBACK(on_auth), handle);
  g_signal_connect(handle->relay, "state-changed", G_CALLBACK(on_state), handle);
}

static gboolean
retry_connect(gpointer data)
{
  GhGnostrHandle *handle = data;
  handle->retry_source = 0;
  if (!handle->closed &&
      nostr_relay_is_established(gnostr_relay_get_core_relay(handle->relay))) {
    handle->retry_delay_seconds = 1;
    ensure_subscription(handle);
    return G_SOURCE_REMOVE;
  }
  if (!handle->closed) {
    /* A failed first dial can leave a dead core connection attached. A new
     * wrapper is required; calling connect_async on that connection only
     * waits on the same failed handshake again. */
    reset_subscription(handle);
    g_signal_handlers_disconnect_by_data(handle->relay, handle);
    gnostr_relay_disconnect(handle->relay);
    g_clear_object(&handle->relay);
    handle->relay = private_relay(handle->url);
    if (handle->relay) {
      attach_relay_signals(handle);
      g_atomic_int_inc(&handle->refs);
      gh_relay_scope_ref(handle->scope);
      handle->connect_pending = TRUE;
      gnostr_relay_connect_async(handle->relay, handle->cancellable,
                                  on_connected, handle);
    }
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_retry(GhGnostrHandle *handle)
{
  if (handle->closed || handle->retry_source)
    return;
  guint delay = handle->retry_delay_seconds ? handle->retry_delay_seconds : 1;
  handle->retry_delay_seconds = MIN(delay * 2, 30);
  g_atomic_int_inc(&handle->refs); /* held by the timeout source */
  handle->retry_source = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, delay,
    retry_connect, handle, (GDestroyNotify)handle_unref);
}

static void
on_state(GNostrRelay *relay, GNostrRelayState old_state,
         GNostrRelayState new_state, gpointer data)
{
  (void)relay;
  GhGnostrHandle *handle = data;
  g_atomic_int_inc(&handle->refs); /* a status callback may cancel the scope */
  if (handle->closed) {
    handle_unref(handle);
    return;
  }
  if (old_state == GNOSTR_RELAY_STATE_CONNECTED &&
      new_state != GNOSTR_RELAY_STATE_CONNECTED) {
    reset_subscription(handle);
    gh_relay_scope_notice(handle->scope, handle->url,
                           GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
    if (!handle->connect_pending)
      schedule_retry(handle);
  }
  if (new_state == GNOSTR_RELAY_STATE_CONNECTED) {
    ensure_subscription(handle);
    if (handle->subscription) {
      if (handle->retry_source) {
        g_source_remove(handle->retry_source);
        handle->retry_source = 0;
      }
      handle->retry_delay_seconds = 1;
    }
  }
  handle_unref(handle);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  GhGnostrHandle *handle = data;
  g_autoptr(GError) error = NULL;
  gboolean connected = gnostr_relay_connect_finish(GNOSTR_RELAY(source), result,
                                                     &error);
  handle->connect_pending = FALSE;
  if (!handle->closed) {
    if (connected ||
        nostr_relay_is_established(gnostr_relay_get_core_relay(handle->relay))) {
      handle->retry_delay_seconds = 1;
      ensure_subscription(handle);
    } else {
      gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                             NULL, FALSE,
                             error ? error->message : "relay connection failed");
      schedule_retry(handle);
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
  handle->relay = private_relay(url);
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
  attach_relay_signals(handle);
  gh_relay_scope_ref(scope);
  handle->connect_pending = TRUE;
  gnostr_relay_connect_async(handle->relay, handle->cancellable, on_connected,
                              handle);
  return handle;
}

static void
on_auth_written(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GhGnostrHandle *handle = data;
  g_autoptr(GError) error = NULL;
  if (!gh_relay_gnostr_write_finish(result, &error) && !handle->closed) {
    g_autofree gchar *detail = g_strdup_printf("AUTH write failed: %s",
                                               error->message);
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR,
                           NULL, FALSE, detail);
  }
  handle_unref(handle);
}

static gboolean
send_auth(gpointer data, const gchar *signed_event_json, gpointer user_data,
          GError **error)
{
  (void)user_data;
  GhGnostrHandle *handle = data;
  g_autofree gchar *frame = gh_relay_gnostr_event_frame("AUTH", signed_event_json);
  if (handle->closed || !frame) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "AUTH frame not sendable");
    return FALSE;
  }
  g_atomic_int_inc(&handle->refs); /* held by the write */
  gh_relay_gnostr_write_async(handle->relay, frame, handle->cancellable,
                              on_auth_written, handle);
  return TRUE;
}

/* The relay CLOSED the REQ; issue a fresh one on the same connection. */
static void
resubscribe(gpointer data, gpointer user_data)
{
  (void)user_data;
  GhGnostrHandle *handle = data;
  reset_subscription(handle);
  ensure_subscription(handle);
}

static void
close_relay(gpointer data, gpointer user_data)
{
  (void)user_data;
  GhGnostrHandle *handle = data;
  handle->closed = TRUE;
  if (handle->retry_source) {
    g_source_remove(handle->retry_source);
    handle->retry_source = 0;
  }
  g_cancellable_cancel(handle->cancellable);
  reset_subscription(handle);
  g_signal_handlers_disconnect_by_data(handle->relay, handle);
  gnostr_relay_disconnect(handle->relay);
  handle_unref(handle);
}

const GhRelayTransport gh_relay_gnostr_transport = {
  .open = open_relay,
  .close = close_relay,
};

const GhRelayAuthTransport gh_relay_gnostr_auth_transport = {
  .send_auth = send_auth,
  .resubscribe = resubscribe,
};
