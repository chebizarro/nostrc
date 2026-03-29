#define G_LOG_DOMAIN "gnostr-main-window-pool"

#include "gnostr-main-window-private.h"

#include "gnostr-dm-service.h"
#include "gnostr-session-view.h"
#include "gnostr-tray-icon.h"

#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>

#include "nostr-relay.h"

#include <stdlib.h>
#include <string.h>

#define INGEST_QUEUE_MAX 4096

static gboolean on_relay_config_changed_restart(gpointer user_data);
static gboolean retry_pool_live(gpointer user_data);
static gboolean check_relay_health(gpointer user_data);
static void on_pool_relays_connected(GObject *source, GAsyncResult *result, gpointer user_data);
static void on_multi_sub_event(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, const gchar *event_json, gpointer user_data);
static void on_multi_sub_eose(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, gpointer user_data);
static int extract_kind_from_json(const char *json);

typedef struct {
    GnostrMainWindow *self;
    NostrFilters *filters;
} PoolConnectCtx;

void
gnostr_main_window_on_relay_config_changed_internal(gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return;

    g_debug("[LIVE_RELAY] Relay configuration changed, syncing pool...");

    GPtrArray *read_relays = gnostr_get_read_relay_urls();
    if (read_relays->len == 0) {
        g_warning("[LIVE_RELAY] No read relays configured");
        g_ptr_array_unref(read_relays);
        return;
    }

    if (self->pool) {
        const char **urls = g_new0(const char *, read_relays->len);
        for (guint i = 0; i < read_relays->len; i++) {
            urls[i] = g_ptr_array_index(read_relays, i);
        }
        gnostr_pool_sync_relays(self->pool, (const gchar **)urls, read_relays->len);
        g_free(urls);
    }

    if (self->live_urls) {
        gnostr_main_window_free_urls_owned_internal(self->live_urls, self->live_url_count);
        self->live_urls = NULL;
        self->live_url_count = 0;
    }

    self->live_urls = g_new0(const char *, read_relays->len);
    self->live_url_count = read_relays->len;
    for (guint i = 0; i < read_relays->len; i++) {
        self->live_urls[i] = g_strdup(g_ptr_array_index(read_relays, i));
    }
    g_ptr_array_unref(read_relays);

    if (self->pool_cancellable) {
        g_debug("[LIVE_RELAY] Restarting live subscription with updated relays");
        g_cancellable_cancel(self->pool_cancellable);
        g_clear_object(&self->pool_cancellable);
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_relay_config_changed_restart,
                        g_object_ref(self), g_object_unref);
    }

    if (self->dm_service) {
        g_debug("[LIVE_RELAY] Restarting DM service with updated DM relays");
        gnostr_dm_service_stop(self->dm_service);
        gnostr_dm_service_start_with_dm_relays(self->dm_service);
    }

    g_debug("[LIVE_RELAY] Relay sync complete");
}

void
gnostr_main_window_start_pool_live_internal(GnostrMainWindow *self)
{
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return;

    if (self->reconnection_in_progress) {
        g_debug("[RELAY] Reconnection already in progress, skipping");
        return;
    }
    self->reconnection_in_progress = TRUE;

    if (!self->pool)
        self->pool = gnostr_pool_new();

    if (self->pool_cancellable)
        g_cancellable_cancel(self->pool_cancellable);
    g_clear_object(&self->pool_cancellable);
    self->pool_cancellable = g_cancellable_new();

    const char **urls = NULL;
    size_t url_count = 0;
    NostrFilters *filters = NULL;
    const int live_kinds[] = {0, 1, 5, 6, 7, 16, 1111};

    gnostr_main_window_build_urls_and_filters_for_kinds_internal(self,
                                                                  live_kinds,
                                                                  G_N_ELEMENTS(live_kinds),
                                                                  &urls,
                                                                  &url_count,
                                                                  &filters,
                                                                  0);

    if (!urls || url_count == 0 || !filters) {
        g_warning("[RELAY] No relay URLs configured, skipping live subscription");
        if (filters)
            nostr_filters_free(filters);
        if (urls)
            gnostr_main_window_free_urls_owned_internal(urls, url_count);
        self->reconnection_in_progress = FALSE;
        return;
    }

    if (self->live_urls) {
        gnostr_main_window_free_urls_owned_internal(self->live_urls, self->live_url_count);
        self->live_urls = NULL;
        self->live_url_count = 0;
    }
    self->live_urls = urls;
    self->live_url_count = url_count;

    g_warning("[RELAY] Initializing %zu relays in pool", self->live_url_count);
    gnostr_pool_sync_relays(self->pool, (const gchar **)self->live_urls, self->live_url_count);
    g_warning("[RELAY] ✓ All relays initialized");

    if (self->live_multi_sub) {
        gnostr_pool_multi_sub_close(self->live_multi_sub);
        self->live_multi_sub = NULL;
    }

    g_warning("[RELAY] Connecting %zu relays...", self->live_url_count);
    PoolConnectCtx *ctx = g_new0(PoolConnectCtx, 1);
    ctx->self = g_object_ref(self);
    ctx->filters = filters;
    gnostr_pool_connect_all_async(self->pool, self->pool_cancellable,
                                  on_pool_relays_connected, ctx);
}

void
gnostr_main_window_start_profile_subscription_internal(GnostrMainWindow *self)
{
    (void)self;
}

static gboolean
on_relay_config_changed_restart(gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return G_SOURCE_REMOVE;

    if (!self->reconnection_in_progress && !self->pool_cancellable) {
        gnostr_main_window_start_pool_live_internal(self);
    }

    return G_SOURCE_REMOVE;
}

static void
on_pool_relays_connected(GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
    (void)source;
    PoolConnectCtx *ctx = user_data;
    GnostrMainWindow *self = ctx->self;
    NostrFilters *filters = ctx->filters;
    g_free(ctx);

    if (!GNOSTR_IS_MAIN_WINDOW(self)) {
        nostr_filters_free(filters);
        g_object_unref(self);
        return;
    }

    GError *err = NULL;
    gboolean connected = gnostr_pool_connect_all_finish(self->pool, result, &err);
    if (!connected) {
        g_warning("[RELAY] No relays connected: %s - retrying in 5 seconds",
                  err ? err->message : "(unknown)");
        g_clear_error(&err);
        nostr_filters_free(filters);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 5000, retry_pool_live,
                           g_object_ref(self), g_object_unref);
        self->reconnection_in_progress = FALSE;
        return;
    }
    g_clear_error(&err);

    g_warning("[RELAY] Starting multi-relay live subscription (relays connected: first, others joining asynchronously)");

    GError *sub_error = NULL;
    GNostrPoolMultiSub *multi_sub = gnostr_pool_subscribe_multi(
        self->pool,
        filters,
        on_multi_sub_event,
        on_multi_sub_eose,
        self,
        NULL,
        &sub_error);
    nostr_filters_free(filters);

    if (!multi_sub) {
        g_warning("[RELAY] pool_subscribe_multi FAILED: %s - retrying in 5 seconds",
                  sub_error ? sub_error->message : "(unknown)");
        g_clear_error(&sub_error);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 5000, retry_pool_live,
                           g_object_ref(self), g_object_unref);
        self->reconnection_in_progress = FALSE;
        return;
    }

    self->live_multi_sub = multi_sub;
    guint relay_count = gnostr_pool_multi_sub_get_relay_count(multi_sub);
    g_warning("[RELAY] Multi-relay live subscription started successfully (%u relays)", relay_count);
    self->reconnection_in_progress = FALSE;

    if (self->health_check_source_id == 0) {
        self->health_check_source_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                                                  30,
                                                                  check_relay_health,
                                                                  g_object_ref(self),
                                                                  g_object_unref);
    }

    g_object_unref(self);
}

static gboolean
check_relay_health(gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool) {
        g_warning("relay_health: invalid window or pool, stopping health checks");
        if (GNOSTR_IS_MAIN_WINDOW(self)) {
            self->health_check_source_id = 0;
        }
        return G_SOURCE_REMOVE;
    }

    if (self->reconnection_in_progress)
        return G_SOURCE_CONTINUE;

    GListStore *relay_store = gnostr_pool_get_relays(self->pool);
    guint n_relays = g_list_model_get_n_items(G_LIST_MODEL(relay_store));
    if (n_relays == 0)
        return G_SOURCE_CONTINUE;

    guint disconnected_count = 0;
    guint connected_count = 0;
    for (guint i = 0; i < n_relays; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(relay_store), i);
        if (!relay)
            continue;
        if (gnostr_pool_get_relay(self->pool, gnostr_relay_get_url(relay)) != NULL)
            connected_count++;
        else
            disconnected_count++;
    }

    gnostr_app_update_relay_status((int)connected_count, (int)n_relays);

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
        gnostr_session_view_set_relay_status(self->session_view, connected_count, n_relays);
    }

    if (disconnected_count > 0 && connected_count == 0) {
        g_warning("relay_health: all %u relay(s) disconnected - reconnecting",
                  disconnected_count);
        gnostr_main_window_start_pool_live_internal(self);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
retry_pool_live(gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) {
        g_object_unref(self);
        return G_SOURCE_REMOVE;
    }

    gnostr_main_window_start_pool_live_internal(self);
    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

gboolean
gnostr_main_window_ingest_queue_push_internal(GnostrMainWindow *self, gchar *json)
{
    __atomic_fetch_add(&self->ingest_events_received, 1, __ATOMIC_RELAXED);

    gint depth = g_async_queue_length(self->ingest_queue);
    if (depth >= INGEST_QUEUE_MAX) {
        __atomic_fetch_add(&self->ingest_events_dropped, 1, __ATOMIC_RELAXED);

        gint64 now = g_get_monotonic_time();
        if (now - self->last_backpressure_warn_us > 5000000) {
            guint64 received = __atomic_load_n(&self->ingest_events_received, __ATOMIC_RELAXED);
            guint64 dropped = __atomic_load_n(&self->ingest_events_dropped, __ATOMIC_RELAXED);
            guint64 processed = __atomic_load_n(&self->ingest_events_processed, __ATOMIC_RELAXED);
            double drop_rate = received > 0 ? (100.0 * dropped / received) : 0.0;

            g_warning("[INGEST] Queue full (%d items, cap %d) — dropping event "
                      "(NDB pipeline backpressure). Stats: received=%" G_GUINT64_FORMAT
                      ", dropped=%" G_GUINT64_FORMAT " (%.1f%%), processed=%" G_GUINT64_FORMAT,
                      depth, INGEST_QUEUE_MAX, received, dropped, drop_rate, processed);
            self->last_backpressure_warn_us = now;
        }
        return FALSE;
    }

    g_async_queue_push(self->ingest_queue, json);
    return TRUE;
}

static int
extract_kind_from_json(const char *json)
{
    const char *p = strstr(json, "\"kind\"");
    if (!p)
        return -1;
    p += 6;
    while (*p == ' ' || *p == ':' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    return (int)strtol(p, NULL, 10);
}

static void
on_multi_sub_event(GNostrPoolMultiSub *multi_sub,
                   const gchar *relay_url,
                   const gchar *event_json,
                   gpointer user_data)
{
    (void)multi_sub;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_json)
        return;

    int kind = extract_kind_from_json(event_json);
    if (kind < 0)
        return;

    if (!(kind == 0 || kind == 1 || kind == 5 || kind == 6 || kind == 7 ||
          kind == 16 || kind == 1111 || kind == 30617 || kind == 1617 ||
          kind == 1621 || kind == 1622)) {
        return;
    }

    gchar *copy = g_strdup(event_json);
    if (!gnostr_main_window_ingest_queue_push_internal(self, copy)) {
        g_free(copy);
    } else {
        g_debug("[RELAY] Event from %s (kind %d)", relay_url, kind);
    }
}

static void
on_multi_sub_eose(GNostrPoolMultiSub *multi_sub,
                  const gchar *relay_url,
                  gpointer user_data)
{
    (void)multi_sub;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return;
  g_debug("[RELAY] EOSE from %s", relay_url);
  gnostr_main_window_note_startup_live_eose_internal(self);
}
