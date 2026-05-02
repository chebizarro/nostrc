#define G_LOG_DOMAIN "gnostr-main-window-pool"

#include "gnostr-main-window-private.h"

#include "gnostr-dm-service.h"
#include "gnostr-live-state-kinds.h"
#include "gnostr-session-view.h"
#include "gnostr-tray-icon.h"

#include "../util/follow_list.h"
#include "../util/gnostr-thread-prefetch.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>

#include "nostr-relay.h"

#include <stdlib.h>
#include <string.h>

#define INGEST_QUEUE_MAX 4096

static gboolean on_relay_config_changed_restart(gpointer user_data);
static void on_pool_relays_connected(GObject *source, GAsyncResult *result, gpointer user_data);
static void on_multi_sub_event(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, const gchar *event_json, gpointer user_data);
static void on_multi_sub_eose(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, gpointer user_data);
static int extract_kind_from_json(const char *json);
static void trigger_live_state_refresh(GnostrMainWindow *self, const gchar *relay_url, const gchar *event_json);
static void refresh_relay_status(GnostrMainWindow *self);
static void ensure_live_multi_sub(GnostrMainWindow *self);
static void on_pool_relay_state_changed(GNostrPool *pool, GNostrRelay *relay, GNostrRelayState state, gpointer user_data);
static void reset_startup_live_eose_tracking(GnostrMainWindow *self);

typedef struct {
    GnostrMainWindow *self;
} PoolConnectCtx;

static guint
count_connected_relays(GnostrMainWindow *self,
                       guint            *total_out)
{
    if (total_out)
        *total_out = 0;

    if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool)
        return 0;

    GListStore *relay_store = gnostr_pool_get_relays(self->pool);
    guint n_relays = g_list_model_get_n_items(G_LIST_MODEL(relay_store));
    guint connected_count = 0;

    for (guint i = 0; i < n_relays; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(relay_store), i);
        if (relay && gnostr_relay_get_state(relay) == GNOSTR_RELAY_STATE_CONNECTED)
            connected_count++;
    }

    if (total_out)
        *total_out = n_relays;

    return connected_count;
}

static void
refresh_relay_status(GnostrMainWindow *self)
{
    guint total_count = 0;
    guint connected_count = count_connected_relays(self, &total_count);

    gnostr_app_update_relay_status((int)connected_count, (int)total_count);

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
        gnostr_session_view_set_relay_status(self->session_view, connected_count, total_count);
}

static void
ensure_pool_signal_handlers(GnostrMainWindow *self)
{
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool)
        return;

    if (self->pool_events_handler == 0) {
        self->pool_events_handler = g_signal_connect(self->pool,
                                                     "relay-state-changed",
                                                     G_CALLBACK(on_pool_relay_state_changed),
                                                     self);
    }
}

static void
ensure_live_multi_sub(GnostrMainWindow *self)
{
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool || !self->live_filters || self->live_multi_sub)
        return;

    guint total_count = 0;
    guint connected_count = count_connected_relays(self, &total_count);
    if (connected_count == 0) {
        g_debug("[RELAY] No connected relays yet; waiting for relay-state changes to start live multi-sub");
        return;
    }

    GError *sub_error = NULL;
    GNostrPoolMultiSub *multi_sub = gnostr_pool_subscribe_multi(self->pool,
                                                                self->live_filters,
                                                                on_multi_sub_event,
                                                                on_multi_sub_eose,
                                                                self,
                                                                NULL,
                                                                &sub_error);
    if (!multi_sub) {
        g_warning("[RELAY] Failed to start live multi-sub after relay-state change: %s",
                  sub_error ? sub_error->message : "(unknown)");
        g_clear_error(&sub_error);
        return;
    }

    self->live_multi_sub = multi_sub;
    guint relay_count = gnostr_pool_multi_sub_get_relay_count(multi_sub);
    self->startup_live_expected_relays = relay_count;
    if (relay_count == 0)
        self->startup_live_all_eose_seen = TRUE;

    g_message("[RELAY] Live multi-sub active on %u/%u connected relay(s)",
              relay_count, total_count);
}

static void
on_pool_relay_state_changed(GNostrPool       *pool,
                            GNostrRelay      *relay,
                            GNostrRelayState  state,
                            gpointer          user_data)
{
    (void)pool;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !relay)
        return;

    g_debug("[RELAY] State changed for %s -> %d",
            gnostr_relay_get_url(relay) ? gnostr_relay_get_url(relay) : "(unknown)",
            state);

    refresh_relay_status(self);

    if (state == GNOSTR_RELAY_STATE_CONNECTED)
        ensure_live_multi_sub(self);
}

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
        if (self->pool_cancellable) {
            g_cancellable_cancel(self->pool_cancellable);
            g_clear_object(&self->pool_cancellable);
        }
        if (self->live_multi_sub) {
            gnostr_pool_multi_sub_close(self->live_multi_sub);
            self->live_multi_sub = NULL;
        }
        g_clear_pointer(&self->live_filters, nostr_filters_free);
        if (self->live_urls) {
            gnostr_main_window_free_urls_owned_internal(self->live_urls, self->live_url_count);
            self->live_urls = NULL;
            self->live_url_count = 0;
        }
        if (self->pool) {
            gnostr_pool_disconnect_all(self->pool);
            while (gnostr_pool_get_relay_count(self->pool) > 0) {
                g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(gnostr_pool_get_relays(self->pool)), 0);
                const char *url = relay ? gnostr_relay_get_url(relay) : NULL;
                if (!url || !*url || !gnostr_pool_remove_relay(self->pool, url))
                    break;
            }
        }
        self->reconnection_in_progress = FALSE;
        reset_startup_live_eose_tracking(self);
        refresh_relay_status(self);
        g_ptr_array_unref(read_relays);
        return;
    }

    if (self->pool) {
        GListStore *relay_store = gnostr_pool_get_relays(self->pool);
        while (g_list_model_get_n_items(G_LIST_MODEL(relay_store)) > 0) {
            g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(relay_store), 0);
            const char *url = relay ? gnostr_relay_get_url(relay) : NULL;
            if (!url || !*url || !gnostr_pool_remove_relay(self->pool, url))
                break;
        }

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

static void
reset_startup_live_eose_tracking(GnostrMainWindow *self)
{
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return;

    g_clear_pointer(&self->startup_live_eose_relays, g_hash_table_unref);
    self->startup_live_eose_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->startup_live_expected_relays = 0;
    self->startup_live_first_eose_seen = FALSE;
    self->startup_live_all_eose_seen = FALSE;
    self->startup_gift_wrap_started = FALSE;
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
    ensure_pool_signal_handlers(self);

    if (self->pool_cancellable)
        g_cancellable_cancel(self->pool_cancellable);
    g_clear_object(&self->pool_cancellable);
    self->pool_cancellable = g_cancellable_new();

    const char **urls = NULL;
    size_t url_count = 0;
    NostrFilters *filters = NULL;
    GPtrArray *read_relays = gnostr_get_read_relay_urls();

    if (read_relays && read_relays->len > 0) {
        url_count = read_relays->len;
        urls = g_new0(const char *, url_count);
        for (guint i = 0; i < read_relays->len; i++)
            urls[i] = g_strdup(g_ptr_array_index(read_relays, i));
    }
    if (read_relays)
        g_ptr_array_unref(read_relays);

    filters = gnostr_live_state_build_subscription_filters(self->user_pubkey_hex);

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

    g_clear_pointer(&self->live_filters, nostr_filters_free);
    self->live_filters = filters;

    g_warning("[RELAY] Initializing %zu relays in pool", self->live_url_count);
    gnostr_pool_sync_relays(self->pool, (const gchar **)self->live_urls, self->live_url_count);
    g_warning("[RELAY] ✓ All relays initialized");

    if (self->live_multi_sub) {
        gnostr_pool_multi_sub_close(self->live_multi_sub);
        self->live_multi_sub = NULL;
    }

    reset_startup_live_eose_tracking(self);

    g_warning("[RELAY] Connecting %zu relays...", self->live_url_count);
    PoolConnectCtx *ctx = g_new0(PoolConnectCtx, 1);
    ctx->self = g_object_ref(self);
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
    g_free(ctx);

    if (!GNOSTR_IS_MAIN_WINDOW(self)) {
        g_object_unref(self);
        return;
    }

    g_clear_object(&self->pool_cancellable);

    GError *err = NULL;
    gboolean connected = gnostr_pool_connect_all_finish(self->pool, result, &err);
    if (!connected) {
        g_warning("[RELAY] No relays connected yet: %s - waiting for relay-state-driven recovery",
                  err ? err->message : "(unknown)");
        g_clear_error(&err);
        self->reconnection_in_progress = FALSE;
        refresh_relay_status(self);
        g_object_unref(self);
        return;
    }
    g_clear_error(&err);

    g_warning("[RELAY] Relay pool connected; ensuring live multi-sub");
    ensure_live_multi_sub(self);
    self->reconnection_in_progress = FALSE;
    refresh_relay_status(self);

    g_object_unref(self);
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
trigger_live_state_refresh(GnostrMainWindow *self,
                           const gchar      *relay_url,
                           const gchar      *event_json)
{
    const char *relay_urls[2] = { NULL, NULL };
    relay_urls[0] = (relay_url && *relay_url) ? relay_url : NULL;

    switch (gnostr_live_state_refresh_kind_from_event_json(event_json, self->user_pubkey_hex)) {
      case GNOSTR_LIVE_STATE_REFRESH_FOLLOW_LIST:
        gnostr_follow_list_fetch_from_relays_async(self->user_pubkey_hex,
                                                   relay_urls[0] ? relay_urls : NULL,
                                                   NULL, NULL, NULL);
        g_debug("[RELAY] Triggered live follow-list refresh from %s",
                relay_url ? relay_url : "(unknown)");
        break;
      case GNOSTR_LIVE_STATE_REFRESH_MUTE_LIST: {
        GNostrMuteList *mute = gnostr_mute_list_get_default();
        if (mute) {
          gnostr_mute_list_fetch_async(mute, self->user_pubkey_hex,
                                       relay_urls[0] ? relay_urls : NULL,
                                       NULL, NULL);
          g_debug("[RELAY] Triggered live mute-list refresh from %s",
                  relay_url ? relay_url : "(unknown)");
        }
        break;
      }
      case GNOSTR_LIVE_STATE_REFRESH_RELAY_LIST:
        gnostr_nip65_fetch_relays_from_urls_async(self->user_pubkey_hex,
                                                  relay_urls[0] ? relay_urls : NULL,
                                                  NULL, NULL, NULL);
        g_debug("[RELAY] Triggered live relay-list refresh from %s",
                relay_url ? relay_url : "(unknown)");
        break;
      case GNOSTR_LIVE_STATE_REFRESH_NONE:
      default:
        break;
    }
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

    if (!(kind == 0 || kind == 1 || kind == 3 || kind == 5 || kind == 6 || kind == 7 ||
          kind == 16 || kind == 1111 || kind == 10000 || kind == 10002 ||
          kind == 30617 || kind == 1617 ||
          kind == 1621 || kind == 1622)) {
        return;
    }

    gchar *copy = g_strdup(event_json);
    if (!gnostr_main_window_ingest_queue_push_internal(self, copy)) {
        g_free(copy);
    } else {
        g_debug("[RELAY] Event from %s (kind %d)", relay_url, kind);

        /* nostrc-4bk: For thread-capable events, eagerly prefetch root and
         * ancestor events so the thread panel can render instantly. */
        if ((kind == 1 || kind == 1111) && self->thread_prefetch)
            gnostr_thread_prefetch_observe_event(self->thread_prefetch,
                                                 event_json, relay_url);
    }

    trigger_live_state_refresh(self, relay_url, event_json);
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
  gnostr_main_window_note_startup_live_eose_internal(self, relay_url);
}
