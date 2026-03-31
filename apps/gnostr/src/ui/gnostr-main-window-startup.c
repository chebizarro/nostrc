#define G_LOG_DOMAIN "gnostr-main-window-startup"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-timeline-view.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-startup-live-eose.h"

#include "../model/gn-nostr-event-model.h"
#include "../util/blossom_settings.h"
#include "../util/follow_list.h"
#include "../util/nip42_auth.h"
#include "../util/nip51_settings.h"
#include "../util/nwc.h"
#include "../sync/gnostr-sync-bridge.h"
#include "gnostr-notifications-view.h"
#include "../ipc/gnostr-signer-service.h"

#include <nostr-gobject-1.0/gn-timeline-query.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "../notifications/badge_manager.h"
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_profile_service.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>

static gboolean
profile_provider_log_stats_cb_local(gpointer data)
{
  (void)data;
  gnostr_profile_provider_log_stats();
  return G_SOURCE_CONTINUE;
}

static gboolean
memory_stats_cb_local(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return G_SOURCE_CONTINUE;

  guint seen_texts_size = self->seen_texts ? g_hash_table_size(self->seen_texts) : 0;
  guint profile_queue = self->profile_fetch_queue ? self->profile_fetch_queue->len : 0;
  guint model_items = self->event_model ? g_list_model_get_n_items(G_LIST_MODEL(self->event_model)) : 0;
  guint liked_events_size = self->liked_events ? g_hash_table_size(self->liked_events) : 0;
  guint profile_batches = self->profile_batches ? self->profile_batches->len : 0;
  guint gift_wrap_queue = self->gift_wrap_queue ? self->gift_wrap_queue->len : 0;

  extern guint gnostr_avatar_cache_size(void);
  extern guint nostr_gtk_media_image_cache_size(void);
  guint avatar_tex = gnostr_avatar_cache_size();
  guint media_cache = nostr_gtk_media_image_cache_size();

  g_message("[MEM] seen_texts=%u/%u profile_queue=%u model_items=%u liked_events=%u/%u profile_batches=%u gift_wrap_queue=%u avatar_textures=%u media_cache=%u",
            seen_texts_size, 10000, profile_queue, model_items,
            liked_events_size, 5000, profile_batches, gift_wrap_queue,
            avatar_tex, media_cache);
  return G_SOURCE_CONTINUE;
}

static void
maybe_start_startup_gift_wrap(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;
  if (self->startup_gift_wrap_started)
    return;
  if (!self->startup_live_all_eose_seen)
    return;

  self->startup_gift_wrap_started = TRUE;
  g_debug("[STARTUP] stage3: starting gift-wrap after all initial live relay EOSEs");
  gnostr_main_window_start_gift_wrap_subscription_internal(self);
}

void
gnostr_main_window_initial_refresh_timeout_cb_internal(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_debug("[STARTUP] initial_refresh_timeout_cb: starting async refresh");

  if (self->event_model) {
    GNostrTimelineQuery *query = gnostr_timeline_query_new_global();
    gn_nostr_event_model_set_timeline_query(self->event_model, query);
    gnostr_timeline_query_free(query);
    gn_nostr_event_model_refresh_async(self->event_model);
  }

  /* Stage 2 startup work after the initial refresh has been queued so the
   * timeline can paint first, while gift-wrap itself still waits for live EOSE. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                  (GSourceFunc)gnostr_main_window_run_startup_stage2_internal,
                  g_object_ref(self),
                  g_object_unref);

  g_debug("[STARTUP] initial_refresh_timeout_cb: EXIT");
}

void
gnostr_main_window_note_startup_live_eose_internal(GnostrMainWindow *self,
                                                   const gchar      *relay_url)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || relay_url == NULL || *relay_url == '\0')
    return;

  if (!self->startup_live_eose_relays) {
    self->startup_live_eose_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }

  guint flags = gnostr_main_window_track_startup_live_eose_internal(self->startup_live_eose_relays,
                                                                    self->startup_live_expected_relays,
                                                                    relay_url);
  if ((flags & GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST) != 0u && !self->startup_live_first_eose_seen) {
    self->startup_live_first_eose_seen = TRUE;
    gnostr_avatar_cache_set_startup_mode(FALSE);
    g_debug("[STARTUP] first live relay EOSE observed: %s", relay_url);
  }

  if ((flags & GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL) != 0u && !self->startup_live_all_eose_seen) {
    self->startup_live_all_eose_seen = TRUE;
    guint seen_count = g_hash_table_size(self->startup_live_eose_relays);
    g_debug("[STARTUP] all initial live relay EOSEs observed (%u/%u)",
            seen_count,
            self->startup_live_expected_relays);
  }

  maybe_start_startup_gift_wrap(self);
}

void
gnostr_main_window_run_startup_stage2_internal(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_debug("[STARTUP] stage2: starting profile subscription only");
  gnostr_main_window_start_profile_subscription_internal(self);
  maybe_start_startup_gift_wrap(self);
  g_debug("[STARTUP] stage2: profile subscription started; waiting for all initial live relay EOSEs for gift-wrap");
}

void
gnostr_main_window_run_startup_bootstrap_internal(GnostrMainWindow *self,
                                                  GCallback need_profile_cb,
                                                  GCallback new_items_pending_cb,
                                                  GCallback scroll_cb,
                                                  GCallback tab_filter_cb)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  self->event_model = gn_nostr_event_model_new();

  GnNostrQueryParams params = {
    .kinds = (gint[]){1},
    .n_kinds = 1,
    .authors = NULL,
    .n_authors = 0,
    .since = 0,
    .until = 0,
    .limit = 500
  };
  gn_nostr_event_model_set_query(self->event_model, &params);
  g_debug("[STARTUP] set_query done");
  gnostr_avatar_cache_set_startup_mode(TRUE);

  g_signal_connect(self->event_model, "need-profile", need_profile_cb, self);
  g_signal_connect(self->event_model, "new-items-pending", new_items_pending_cb, self);

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && G_TYPE_CHECK_INSTANCE_TYPE(timeline, NOSTR_GTK_TYPE_TIMELINE_VIEW)) {
    GtkSelectionModel *selection = GTK_SELECTION_MODEL(
      gtk_single_selection_new(G_LIST_MODEL(self->event_model))
    );
    nostr_gtk_timeline_view_set_model(NOSTR_GTK_TIMELINE_VIEW(timeline), selection);
    g_object_unref(selection);

    gn_nostr_event_model_set_drain_enabled(self->event_model, TRUE);

    GtkWidget *scroller = nostr_gtk_timeline_view_get_scrolled_window(NOSTR_GTK_TIMELINE_VIEW(timeline));
    if (scroller && GTK_IS_SCROLLED_WINDOW(scroller)) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
      if (vadj) {
        g_signal_connect(vadj, "value-changed", scroll_cb, self);
      }
    }

    g_signal_connect(timeline, "tab-filter-changed", tab_filter_cb, self);
    gnostr_main_window_set_page(self, GNOSTR_MAIN_WINDOW_PAGE_SESSION);
    g_debug("[STARTUP] page switched to SESSION (cached items visible)");
  }

  self->ingest_queue = g_async_queue_new_full(g_free);
  __atomic_store_n(&self->ingest_running, TRUE, __ATOMIC_SEQ_CST);
  self->ingest_thread = g_thread_new("ndb-ingest", gnostr_main_window_ingest_thread_func_internal, self);

  gnostr_profile_provider_init(0);
  gnostr_profile_provider_set_follow_list_provider(gnostr_follow_list_get_pubkeys_cached);
  gnostr_profile_service_set_relay_provider(gnostr_load_relays_into);
  gnostr_nwc_service_load_from_settings(gnostr_nwc_service_get_default());

  g_timeout_add_seconds(60, profile_provider_log_stats_cb_local, NULL);
  g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 60, memory_stats_cb_local, g_object_ref(self), g_object_unref);

  gnostr_main_window_prepopulate_all_profiles_from_cache_internal(self);

  if (!self->pool)
    self->pool = gnostr_pool_new();

  gnostr_nip42_setup_pool_auth(self->pool);

  g_debug("[STARTUP] starting pool...");
  gnostr_main_window_start_pool_live_internal(self);
  g_debug("[STARTUP] pool started; deferring profile/gift-wrap startup to stage2");
}

