/**
 * @file gnostr-sync-service.c
 * @brief Background sync service implementation
 *
 * Adaptive scheduling strategy:
 * - Base interval: 60 seconds after a change is detected
 * - Back off: interval doubles (up to 600s) on consecutive in-sync results
 * - Reset: interval drops to base on any detected change
 *
 * Relay configuration is injected via GnostrSyncRelayProvider callback
 * passed to gnostr_sync_service_new(). The caller is responsible for
 * calling gnostr_sync_service_sync_now() when relay config changes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-sync-service.h"
#include "neg-client.h"
#include <nostr-gobject-1.0/nostr_event_bus.h>
#include <string.h>

/* Adaptive interval bounds (seconds) */
#define SYNC_INTERVAL_BASE_SEC    60
#define SYNC_INTERVAL_MAX_SEC    600
#define SYNC_BACKOFF_FACTOR        2

/* Replaceable event kinds to keep in sync via negentropy.
 *   0     = profile metadata (NIP-01)
 *   3     = contact/follow list (NIP-02)
 *   10000 = mute list (NIP-51)
 *   10001 = pin list (NIP-51)
 *   10002 = relay list (NIP-65) */
static const int SYNC_KINDS[] = { 0, 3, 10000, 10001, 10002 };
static const size_t SYNC_KIND_COUNT = 5;

struct _GNostrSyncService
{
  GObject parent_instance;

  /* Timer */
  guint timer_id;
  guint current_interval_sec;

  /* Cancellable for pending sync */
  GCancellable *cancellable;

  /* State */
  GnostrSyncState state;
  gint64 last_sync_time;       /* g_get_monotonic_time() of last completion */
  guint consecutive_in_sync;   /* consecutive syncs with no changes */
  guint total_syncs;
  gboolean running;            /* TRUE if periodic timer is active */

  /* Injected relay provider (nostrc-lx25) */
  GnostrSyncRelayProvider relay_provider;
  gpointer relay_provider_data;
};

G_DEFINE_TYPE(GNostrSyncService, gnostr_sync_service, G_TYPE_OBJECT)

/* Forward declarations */
static gboolean on_sync_timer(gpointer user_data);
static void on_sync_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void schedule_next_sync(GNostrSyncService *self);
static void do_sync(GNostrSyncService *self);

/* ============================================================================
 * GObject lifecycle
 * ============================================================================ */

static void
gnostr_sync_service_dispose(GObject *object)
{
  GNostrSyncService *self = GNOSTR_SYNC_SERVICE(object);

  /* Stop timer */
  if (self->timer_id > 0) {
    g_source_remove(self->timer_id);
    self->timer_id = 0;
  }

  /* Cancel pending sync */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  G_OBJECT_CLASS(gnostr_sync_service_parent_class)->dispose(object);
}

static void
gnostr_sync_service_class_init(GNostrSyncServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_sync_service_dispose;
}

static void
gnostr_sync_service_init(GNostrSyncService *self)
{
  self->timer_id = 0;
  self->current_interval_sec = SYNC_INTERVAL_BASE_SEC;
  self->cancellable = NULL;
  self->state = GNOSTR_SYNC_IDLE;
  self->last_sync_time = 0;
  self->consecutive_in_sync = 0;
  self->total_syncs = 0;
  self->running = FALSE;
  self->relay_provider = NULL;
  self->relay_provider_data = NULL;
}

/* ============================================================================
 * Singleton
 * ============================================================================ */

static GNostrSyncService *default_instance = NULL;

GNostrSyncService *
gnostr_sync_service_new(GnostrSyncRelayProvider relay_provider,
                         gpointer user_data)
{
  g_return_val_if_fail(relay_provider != NULL, NULL);

  if (default_instance) {
    g_warning("[SYNC] Sync service already created; updating relay provider");
    default_instance->relay_provider = relay_provider;
    default_instance->relay_provider_data = user_data;
    return default_instance;
  }

  GNostrSyncService *instance = g_object_new(GNOSTR_TYPE_SYNC_SERVICE, NULL);
  instance->relay_provider = relay_provider;
  instance->relay_provider_data = user_data;
  default_instance = instance;
  return instance;
}

GNostrSyncService *
gnostr_sync_service_get_default(void)
{
  return default_instance;
}

void
gnostr_sync_service_shutdown(void)
{
  if (default_instance) {
    gnostr_sync_service_stop(default_instance);
    g_object_unref(default_instance);
    default_instance = NULL;
  }
}

/* ============================================================================
 * Adaptive scheduling
 * ============================================================================ */

static void
emit_bus_event(const char *topic, const char *json)
{
  GNostrEventBus *bus = gnostr_event_bus_get_default();
  if (bus)
    gnostr_event_bus_emit(bus, topic, (gpointer)json);
}

static void
adjust_interval(GNostrSyncService *self, gboolean in_sync)
{
  if (in_sync) {
    self->consecutive_in_sync++;
    /* Back off: double interval up to max */
    guint new_interval = self->current_interval_sec * SYNC_BACKOFF_FACTOR;
    if (new_interval > SYNC_INTERVAL_MAX_SEC)
      new_interval = SYNC_INTERVAL_MAX_SEC;
    self->current_interval_sec = new_interval;
  } else {
    self->consecutive_in_sync = 0;
    /* Reset to base interval when changes detected */
    self->current_interval_sec = SYNC_INTERVAL_BASE_SEC;
  }

  g_debug("[SYNC] Next interval: %u sec (consecutive_in_sync=%u)",
          self->current_interval_sec, self->consecutive_in_sync);

  /* Notify listeners of schedule change */
  g_autofree gchar *json = g_strdup_printf("{\"interval_sec\":%u,\"consecutive_in_sync\":%u}",
                                            self->current_interval_sec,
                                            self->consecutive_in_sync);
  emit_bus_event(GNOSTR_SYNC_TOPIC_SCHEDULE, json);
}

static void
schedule_next_sync(GNostrSyncService *self)
{
  /* Remove existing timer */
  if (self->timer_id > 0) {
    g_source_remove(self->timer_id);
    self->timer_id = 0;
  }

  if (!self->running)
    return;

  self->timer_id = g_timeout_add_seconds(self->current_interval_sec,
                                          on_sync_timer, self);
}

/* ============================================================================
 * Sync execution
 * ============================================================================ */

static gchar *
get_first_relay_url(GNostrSyncService *self)
{
  if (!self->relay_provider) {
    g_debug("[SYNC] No relay provider configured");
    return NULL;
  }

  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  self->relay_provider(relays, self->relay_provider_data);

  gchar *url = NULL;
  if (relays->len > 0)
    url = g_strdup(g_ptr_array_index(relays, 0));

  g_ptr_array_unref(relays);
  return url;
}

static void
do_sync(GNostrSyncService *self)
{
  if (self->state == GNOSTR_SYNC_RUNNING) {
    g_debug("[SYNC] Sync already in progress, skipping");
    return;
  }

  g_autofree gchar *relay_url = get_first_relay_url(self);
  if (!relay_url) {
    g_debug("[SYNC] No relays configured, skipping sync");
    return;
  }

  self->state = GNOSTR_SYNC_RUNNING;

  /* Cancel any previous pending operation */
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();

  g_debug("[SYNC] Starting sync with %s (interval=%us)",
          relay_url, self->current_interval_sec);

  emit_bus_event(GNOSTR_SYNC_TOPIC_STARTED, relay_url);

  gnostr_neg_sync_kinds_async(relay_url, SYNC_KINDS, SYNC_KIND_COUNT,
                               self->cancellable,
                               on_sync_done, g_object_ref(self));
}

static void
on_sync_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GNostrSyncService *self = GNOSTR_SYNC_SERVICE(user_data);

  g_autoptr(GError) error = NULL;
  GnostrNegSyncStats stats = {0};
  gboolean ok = gnostr_neg_sync_kinds_finish(res, &stats, &error);

  if (ok) {
    self->state = GNOSTR_SYNC_IDLE;
    self->last_sync_time = g_get_monotonic_time();
    self->total_syncs++;

    g_debug("[SYNC] Complete: local=%u rounds=%u fetched=%u in_sync=%d",
            stats.local_count, stats.rounds, stats.events_fetched,
            stats.in_sync);

    /* Build stats JSON for events */
    g_autofree gchar *stats_json = g_strdup_printf(
      "{\"local_count\":%u,\"rounds\":%u,\"events_fetched\":%u,\"in_sync\":%s}",
      stats.local_count, stats.rounds, stats.events_fetched,
      stats.in_sync ? "true" : "false");

    /* Emit generic sync completion */
    emit_bus_event(GNOSTR_SYNC_TOPIC_COMPLETED, stats_json);

    /* Emit negentropy-specific completion with kind details */
    {
      GString *kinds_json = g_string_new("{\"kinds\":[");
      for (size_t i = 0; i < SYNC_KIND_COUNT; i++) {
        if (i > 0) g_string_append_c(kinds_json, ',');
        g_string_append_printf(kinds_json, "%d", SYNC_KINDS[i]);
      }
      g_string_append_printf(kinds_json, "],\"in_sync\":%s,\"rounds\":%u}",
                             stats.in_sync ? "true" : "false", stats.rounds);
      emit_bus_event(GNOSTR_NEG_TOPIC_SYNC_COMPLETE, kinds_json->str);
      g_string_free(kinds_json, TRUE);
    }

    /* Emit kind-specific events when changes detected.
     * UI components subscribe to these to trigger data refresh. */
    if (!stats.in_sync) {
      for (size_t i = 0; i < SYNC_KIND_COUNT; i++) {
        g_autofree gchar *topic = g_strdup_printf("%s%d",
                                                   GNOSTR_NEG_TOPIC_KIND_PREFIX,
                                                   SYNC_KINDS[i]);
        emit_bus_event(topic, stats_json);
      }
    }

    /* Adjust interval based on result */
    adjust_interval(self, stats.in_sync);
  } else {
    self->state = GNOSTR_SYNC_ERROR;

    const char *msg = error ? error->message : "unknown error";
    g_debug("[SYNC] Failed: %s", msg);

    emit_bus_event(GNOSTR_SYNC_TOPIC_ERROR, msg);

    /* Keep current interval on error (don't punish transient failures) */
  }

  /* Schedule next sync */
  schedule_next_sync(self);

  g_object_unref(self);
}

static gboolean
on_sync_timer(gpointer user_data)
{
  GNostrSyncService *self = GNOSTR_SYNC_SERVICE(user_data);
  self->timer_id = 0;  /* one-shot: will be rescheduled in on_sync_done */
  do_sync(self);
  return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void
gnostr_sync_service_start(GNostrSyncService *self)
{
  g_return_if_fail(GNOSTR_IS_SYNC_SERVICE(self));

  if (self->running)
    return;

  self->running = TRUE;
  self->current_interval_sec = SYNC_INTERVAL_BASE_SEC;
  self->consecutive_in_sync = 0;

  g_debug("[SYNC] Service started (base interval=%us)", SYNC_INTERVAL_BASE_SEC);

  /* Immediate first sync */
  do_sync(self);
}

void
gnostr_sync_service_stop(GNostrSyncService *self)
{
  g_return_if_fail(GNOSTR_IS_SYNC_SERVICE(self));

  if (!self->running)
    return;

  self->running = FALSE;

  /* Stop timer */
  if (self->timer_id > 0) {
    g_source_remove(self->timer_id);
    self->timer_id = 0;
  }

  /* Cancel pending sync */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  self->state = GNOSTR_SYNC_IDLE;

  g_debug("[SYNC] Service stopped");
}

void
gnostr_sync_service_sync_now(GNostrSyncService *self)
{
  g_return_if_fail(GNOSTR_IS_SYNC_SERVICE(self));

  /* Reset interval to base for responsive behavior */
  self->current_interval_sec = SYNC_INTERVAL_BASE_SEC;
  self->consecutive_in_sync = 0;

  do_sync(self);
}

GnostrSyncState
gnostr_sync_service_get_state(GNostrSyncService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SYNC_SERVICE(self), GNOSTR_SYNC_IDLE);
  return self->state;
}

gint64
gnostr_sync_service_get_last_sync_time(GNostrSyncService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SYNC_SERVICE(self), 0);
  return self->last_sync_time;
}

guint
gnostr_sync_service_get_consecutive_in_sync(GNostrSyncService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SYNC_SERVICE(self), 0);
  return self->consecutive_in_sync;
}

gboolean
gnostr_sync_service_is_running(GNostrSyncService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SYNC_SERVICE(self), FALSE);
  return self->running;
}
