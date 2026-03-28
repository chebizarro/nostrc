#define G_LOG_DOMAIN "gnostr-main-window-gobject"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-login.h"
#include "gnostr-dm-service.h"

#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

/* Keep local property ids aligned with gnostr-main-window.c. */
enum {
  PROP_0,
  PROP_COMPACT,
  N_PROPS_LOCAL
};

typedef struct {
  GThread  *target;
  GMutex    mu;
  GCond     cond;
  gboolean  done;
  gboolean  abandoned;
} IngestJoinCtx;

static gpointer
ingest_join_thread_func(gpointer data)
{
  IngestJoinCtx *ctx = data;
  g_thread_join(ctx->target);

  g_mutex_lock(&ctx->mu);
  ctx->done = TRUE;
  gboolean abandoned = ctx->abandoned;
  g_cond_signal(&ctx->cond);
  g_mutex_unlock(&ctx->mu);

  if (abandoned) {
    g_mutex_clear(&ctx->mu);
    g_cond_clear(&ctx->cond);
    g_free(ctx);
  }
  return NULL;
}

void
gnostr_main_window_get_property_internal(GObject *object,
                                         guint prop_id,
                                         GValue *value,
                                         GParamSpec *pspec)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);

  switch (prop_id) {
    case PROP_COMPACT:
      g_value_set_boolean(value, self->compact);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

void
gnostr_main_window_set_property_internal(GObject *object,
                                         guint prop_id,
                                         const GValue *value,
                                         GParamSpec *pspec)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);

  switch (prop_id) {
    case PROP_COMPACT:
      gnostr_main_window_set_compact(self, g_value_get_boolean(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

void
gnostr_main_window_dispose_internal(GObject *object)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  g_debug("main-window: dispose");

  if (self->ingest_thread) {
    __atomic_store_n(&self->ingest_running, FALSE, __ATOMIC_SEQ_CST);

    if (self->ingest_queue)
      g_async_queue_push(self->ingest_queue, g_strdup(""));

    IngestJoinCtx *jctx = g_new0(IngestJoinCtx, 1);
    g_mutex_init(&jctx->mu);
    g_cond_init(&jctx->cond);
    jctx->target    = self->ingest_thread;
    jctx->done      = FALSE;
    jctx->abandoned = FALSE;

    GThread *joiner = g_thread_new("ingest-join",
                                   ingest_join_thread_func, jctx);

    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    g_mutex_lock(&jctx->mu);
    while (!jctx->done) {
      if (!g_cond_wait_until(&jctx->cond, &jctx->mu, deadline)) {
        g_warning("main-window: ingest thread did not exit within 2 s; abandoning join to avoid blocking shutdown");
        break;
      }
    }
    gboolean joined = jctx->done;
    if (!joined)
      jctx->abandoned = TRUE;
    g_mutex_unlock(&jctx->mu);

    if (joined) {
      g_thread_join(joiner);
      g_mutex_clear(&jctx->mu);
      g_cond_clear(&jctx->cond);
      g_free(jctx);
    } else {
      g_thread_unref(joiner);
    }

    self->ingest_thread = NULL;
  }
  g_clear_pointer(&self->ingest_queue, g_async_queue_unref);

  if (self->profile_watch_id) {
    gnostr_profile_provider_unwatch(self->profile_watch_id);
    self->profile_watch_id = 0;
  }

  if (self->profile_fetch_source_id) {
    g_source_remove(self->profile_fetch_source_id);
    self->profile_fetch_source_id = 0;
  }
  if (self->backfill_source_id) {
    g_source_remove(self->backfill_source_id);
    self->backfill_source_id = 0;
  }
  if (self->health_check_source_id) {
    g_source_remove(self->health_check_source_id);
    self->health_check_source_id = 0;
  }

  g_clear_object(&self->profile_fetch_cancellable);
  g_clear_object(&self->bg_prefetch_cancellable);
  g_clear_object(&self->pool_cancellable);
  if (self->live_urls) {
    gnostr_main_window_free_urls_owned_internal(self->live_urls, self->live_url_count);
    self->live_urls = NULL;
    self->live_url_count = 0;
  }
  if (self->profile_batches) {
    for (guint i = 0; i < self->profile_batches->len; i++) {
      GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
      if (b) g_ptr_array_free(b, TRUE);
    }
    g_ptr_array_free(self->profile_batches, TRUE);
    self->profile_batches = NULL;
  }
  if (self->profile_batch_urls) {
    gnostr_main_window_free_urls_owned_internal(self->profile_batch_urls, self->profile_batch_url_count);
    self->profile_batch_urls = NULL;
    self->profile_batch_url_count = 0;
  }
  g_clear_pointer(&self->profile_batch_filters, nostr_filters_free);
  g_clear_object(&self->profile_pool);
  if (self->pool) {
    if (self->pool_events_handler) {
      g_signal_handler_disconnect(self->pool, self->pool_events_handler);
      self->pool_events_handler = 0;
    }
    g_signal_handlers_disconnect_by_data(self->pool, self);
  }
  g_clear_object(&self->pool);
  g_clear_pointer(&self->seen_texts, g_hash_table_unref);
  g_clear_object(&self->event_model);
  g_clear_pointer(&self->liked_events, g_hash_table_unref);

  gnostr_main_window_stop_gift_wrap_subscription_internal(self);
  if (self->gift_wrap_queue) {
    g_ptr_array_free(self->gift_wrap_queue, TRUE);
    self->gift_wrap_queue = NULL;
  }

  if (self->dm_service_message_handler_id && self->dm_service) {
    g_signal_handler_disconnect(self->dm_service, self->dm_service_message_handler_id);
    self->dm_service_message_handler_id = 0;
  }

  GtkWidget *dm_inbox = self->session_view ? gnostr_session_view_get_dm_inbox(self->session_view) : NULL;
  if (dm_inbox && self->dm_inbox_open_handler_id) {
    g_signal_handler_disconnect(dm_inbox, self->dm_inbox_open_handler_id);
    self->dm_inbox_open_handler_id = 0;
  }
  if (dm_inbox && self->dm_inbox_compose_handler_id) {
    g_signal_handler_disconnect(dm_inbox, self->dm_inbox_compose_handler_id);
    self->dm_inbox_compose_handler_id = 0;
  }

  GtkWidget *dm_conv = self->session_view ? gnostr_session_view_get_dm_conversation(self->session_view) : NULL;
  if (dm_conv && self->dm_conv_back_handler_id) {
    g_signal_handler_disconnect(dm_conv, self->dm_conv_back_handler_id);
    self->dm_conv_back_handler_id = 0;
  }
  if (dm_conv && self->dm_conv_send_handler_id) {
    g_signal_handler_disconnect(dm_conv, self->dm_conv_send_handler_id);
    self->dm_conv_send_handler_id = 0;
  }
  if (dm_conv && self->dm_conv_send_file_handler_id) {
    g_signal_handler_disconnect(dm_conv, self->dm_conv_send_file_handler_id);
    self->dm_conv_send_file_handler_id = 0;
  }
  if (dm_conv && self->dm_conv_open_profile_handler_id) {
    g_signal_handler_disconnect(dm_conv, self->dm_conv_open_profile_handler_id);
    self->dm_conv_open_profile_handler_id = 0;
  }

  if (self->dm_service) {
    gnostr_dm_service_stop(self->dm_service);
    g_clear_object(&self->dm_service);
  }

  self->key_controller = NULL;

  gnostr_profile_provider_shutdown();

  if (self->relay_change_handler_id) {
    gnostr_relay_change_disconnect(self->relay_change_handler_id);
    self->relay_change_handler_id = 0;
  }
}

void
gnostr_main_window_bind_template_internal(GtkWidgetClass *widget_class)
{
  g_type_ensure(GNOSTR_TYPE_SESSION_VIEW);
  g_type_ensure(GNOSTR_TYPE_LOGIN);

  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, main_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, session_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, login_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, error_page);
}
