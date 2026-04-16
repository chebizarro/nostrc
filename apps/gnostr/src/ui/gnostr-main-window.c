#define G_LOG_DOMAIN "gnostr-main-window"

#include "gnostr-main-window.h"
#include "gnostr-main-window-private.h"
#include "gnostr-tray-icon.h"
#include "gnostr-session-view.h"
#include <nostr-gtk-1.0/gnostr-composer.h>
#include <nostr-gtk-1.0/gnostr-timeline-view.h>
#include <nostr-gtk-1.0/gn-timeline-tabs.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>
#include <nostr-gtk-1.0/gnostr-thread-view.h>
#include "gnostr-article-reader.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_profile_service.h>
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "gnostr-dm-service.h"
#include "gnostr-notifications-view.h"
#include "page-discover.h"
#include "gnostr-search-results-view.h"
#include "gnostr-classifieds-view.h"
#include "gnostr-repo-browser.h"
#include "../util/gnostr-plugin-manager.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include "../model/gn-nostr-event-model.h"
#include <nostr-gobject-1.0/gn-timeline-query.h>
#include "../model/gn-nostr-event-item.h"
#include <nostr-gobject-1.0/gn-nostr-profile.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <time.h>
/* JSON helpers - GObject wrappers and core interface */
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
/* NIP-19 bech32 encoding (GObject wrapper) */
#include <nostr-gobject-1.0/nostr_nip19.h>
/* nostr_pool.h already included below; simple pool removed */
#include "nostr-event.h"
#include "nostr-filter.h"
/* Canonical JSON helpers (for nostr_event_from_json, etc.) */
#include "nostr-json.h"
/* GObject wrappers for profile-related code */
#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
/* NostrdB storage */
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include "libnostr_errors.h"
/* Nostr event kinds */
#include "nostr-kinds.h"
/* Relays helpers */
#include <nostr-gobject-1.0/gnostr-relays.h>
/* NIP-51 mute list */
#include <nostr-gobject-1.0/gnostr-mute-list.h>
/* NIP-02 contact list */
#include "../util/nip02_contacts.h"
/* Follow list (for profile provider pre-warm callback) */
#include "../util/follow_list.h"
/* NIP-77 negentropy sync */
#include <nostr-gobject-1.0/gnostr-sync-service.h>
/* NIP-32 labeling */
#include "../util/nip32_labels.h"
/* NIP-51 settings sync */
#include "../util/nip51_settings.h"
/* Blossom server settings (kind 10063) */
#include "../util/blossom_settings.h"
#include "../util/media_upload.h"
#include "../util/blossom.h"
/* NIP-42 relay authentication */
#include "../util/nip42_auth.h"
/* NIP-47 Nostr Wallet Connect */
#include "../util/nwc.h"
#include "gnostr-nwc-connect.h"
/* Metrics dashboard */
#include "nostr/metrics.h"
#include "nostr/metrics_schema.h"
/* NIP-37 Draft events */
#include "../util/gnostr-drafts.h"
#include "gnostr-login.h"
#include "gnostr-report-dialog.h"
/* NIP-72 Communities */
#include "../util/nip72_communities.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
/* NIP-19 helpers (now via GNostrNip19, included above) */
/* NIP-46 client (remote signer pairing) */
#include "nostr/nip46/nip46_client.h"
/* Gnostr utilities */
#include "../util/utils.h"
/* Sync bridge for negentropy EventBus integration */
#include "../sync/gnostr-sync-bridge.h"

/* Implement as-if SimplePool is fully functional; guarded to avoid breaking builds until wired. */
#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "channel.h"
#include "error.h"
#include "context.h"
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

typedef enum {
  PROP_0,
  PROP_COMPACT,
  N_PROPS
} GnostrMainWindowProperty;

static GParamSpec *props[N_PROPS];

/* Shared GnostrMainWindow instance layout now lives in gnostr-main-window-private.h */

/* Forward declarations for local helpers used before their definitions */
static char *hex_encode_lower(const uint8_t *buf, size_t len);
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);
static void gnostr_load_settings(GnostrMainWindow *self);
static unsigned int getenv_uint_default(const char *name, unsigned int defval);
static gboolean deferred_heavy_init_cb(gpointer data);
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_relays_clicked(GtkButton *btn, gpointer user_data);
static void on_reconnect_requested(GnostrSessionView *view, gpointer user_data);
static void on_settings_clicked(GtkButton *btn, gpointer user_data);
static void on_show_mute_list_activated(GSimpleAction *action, GVariant *param, gpointer user_data);
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
static void on_sidebar_toggle_clicked(GtkToggleButton *button, gpointer user_data);
static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void user_meta_free(gpointer p);
static void gnostr_main_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void gnostr_main_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void on_bg_prefetch_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data);
static gboolean enqueue_author_on_main(gpointer data);

typedef struct {
  GnostrMainWindow *self; /* strong ref */
  char *pubkey_hex;       /* owned */
} EnqueueAuthorCtx;

static gboolean enqueue_author_on_main(gpointer data) {
  EnqueueAuthorCtx *c = (EnqueueAuthorCtx*)data;
  if (c && GNOSTR_IS_MAIN_WINDOW(c->self) && c->pubkey_hex)
    gnostr_main_window_enqueue_profile_author_internal(c->self, c->pubkey_hex);
  if (c) {
    if (c->self) g_object_unref(c->self);
    g_free(c->pubkey_hex);
    g_free(c);
  }
  return G_SOURCE_REMOVE;
}

/* ---- Bulk profile apply support ---- */
#define PROFILES_PER_TICK 20  /* Max profiles to apply per main loop iteration */

typedef struct IdleApplyProfilesCtx {
  GnostrMainWindow *self; /* strong ref */
  GPtrArray *items;       /* ProfileApplyCtx* */
  guint pos;              /* current position for batched iteration */
} IdleApplyProfilesCtx;

static void profile_apply_item_free(gpointer p) {
  ProfileApplyCtx *it = (ProfileApplyCtx*)p;
  if (!it) return;
  g_free(it->pubkey_hex);
  g_free(it->content_json);
  g_free(it);
}

static void idle_apply_profiles_ctx_free(IdleApplyProfilesCtx *c) {
  if (!c) return;
  if (c->items) g_ptr_array_free(c->items, TRUE);
  if (c->self) g_object_unref(c->self);
  g_free(c);
}

static gboolean apply_profiles_idle(gpointer user_data) {
  IdleApplyProfilesCtx *c = (IdleApplyProfilesCtx*)user_data;
  GnostrMainWindow *self = c ? c->self : NULL;
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self) || !c || !c->items) {
    idle_apply_profiles_ctx_free(c);
    return G_SOURCE_REMOVE;
  }

  /* Process up to PROFILES_PER_TICK items per main loop iteration
   * to avoid blocking the GTK main loop when hundreds of profiles
   * arrive at once (e.g., from NDB prepopulation). */
  guint applied = 0;
  guint end = MIN(c->pos + PROFILES_PER_TICK, c->items->len);
  for (guint i = c->pos; i < end; i++) {
    ProfileApplyCtx *it = (ProfileApplyCtx*)g_ptr_array_index(c->items, i);
    if (!it || !it->pubkey_hex || !it->content_json) continue;
    gnostr_main_window_update_meta_from_profile_json_internal(self,
                                                             it->pubkey_hex,
                                                             it->content_json,
                                                             it->created_at);
    if (self->profile_fetch_requested)
      g_hash_table_remove(self->profile_fetch_requested, it->pubkey_hex);
    applied++;
  }
  c->pos = end;

  /* nostrc-sk8o: Refresh thread view ONCE per batch (not per-profile) */
  if (applied > 0) {
    gnostr_main_window_refresh_thread_view_profiles_if_visible_internal(self);
  }

  if (c->pos < c->items->len) {
    return G_SOURCE_CONTINUE;  /* More profiles to process next tick */
  }

  idle_apply_profiles_ctx_free(c);
  return G_SOURCE_REMOVE;
}

void gnostr_main_window_schedule_apply_profiles_internal(GnostrMainWindow *self, GPtrArray *items /* ProfileApplyCtx* */) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !items) { if (items) g_ptr_array_free(items, TRUE); return; }
  IdleApplyProfilesCtx *c = g_new0(IdleApplyProfilesCtx, 1);
  c->self = g_object_ref(self);
  c->items = items; /* transfer */
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, apply_profiles_idle, c, NULL);
}

static gboolean profile_apply_on_main(gpointer data) {
  ProfileApplyCtx *c = (ProfileApplyCtx*)data;
  if (c && c->pubkey_hex && c->content_json) {
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next) {
      if (GNOSTR_IS_MAIN_WINDOW(l->data)) {
        GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(l->data);
        gnostr_main_window_update_meta_from_profile_json_internal(win,
                                                                 c->pubkey_hex,
                                                                 c->content_json,
                                                                 c->created_at);
        if (win->profile_fetch_requested)
          g_hash_table_remove(win->profile_fetch_requested, c->pubkey_hex);
        /* nostrc-sk8o: Refresh thread view after single profile update */
        gnostr_main_window_refresh_thread_view_profiles_if_visible_internal(win);
        break;
      }
    }
    g_list_free(tops);
  }
  if (c) {
    g_free(c->pubkey_hex);
    g_free(c->content_json);
    g_free(c);
  }
  return G_SOURCE_REMOVE;
}

/* Define the window instance struct early so functions can access fields */

/* Old LRU functions removed - now using profile provider */

/* ---- Memory stats logging and cache pruning ---- */

/* Cache size limits to prevent unbounded memory growth.
 * Note: Avatar texture cache has its own LRU limit in gnostr-avatar-cache module. */
#define SEEN_TEXTS_MAX 10000
#define LIKED_EVENTS_MAX 5000
/* Maximum number of queued JSON strings awaiting NDB ingestion.
 * When the nostrdb pipeline blocks (e.g. MDB_MAP_FULL from stale read txns
 * pinning pages), the unbounded GAsyncQueue absorbs all backpressure via
 * g_strdup'd JSON strings (~1-10 KB each).  At 50+ events/sec this grows
 * to multi-GB in hours.  Cap it: newer events evict nothing, we just drop. */
#define INGEST_QUEUE_MAX 4096

/* Proper GSourceFunc wrapper for gnostr_profile_provider_log_stats (ABI-safe) */
static gboolean profile_provider_log_stats_cb(gpointer data) {
  (void)data;
  gnostr_profile_provider_log_stats();
  return G_SOURCE_CONTINUE;
}

static gboolean memory_stats_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_CONTINUE;

  guint seen_texts_size = self->seen_texts ? g_hash_table_size(self->seen_texts) : 0;
  guint profile_queue = self->profile_fetch_queue ? self->profile_fetch_queue->len : 0;
  guint model_items = self->event_model ? g_list_model_get_n_items(G_LIST_MODEL(self->event_model)) : 0;
  guint liked_events_size = self->liked_events ? g_hash_table_size(self->liked_events) : 0;
  guint profile_batches = self->profile_batches ? self->profile_batches->len : 0;
  guint gift_wrap_queue = self->gift_wrap_queue ? self->gift_wrap_queue->len : 0;

  /* Get external cache stats */
  extern guint gnostr_avatar_cache_size(void);
  extern guint nostr_gtk_media_image_cache_size(void);
  guint avatar_tex = gnostr_avatar_cache_size(); /* in-memory GdkTexture cache */
  guint media_cache = nostr_gtk_media_image_cache_size();

  /* Get profile provider stats */
  GnostrProfileProviderStats pstats = {0};
  gnostr_profile_provider_get_stats(&pstats);

  g_message("[MEMORY] model=%u seen=%u avatar_tex=%u media_cache=%u profile_q=%u liked=%u batches=%u giftwrap=%u profile_cache=%u/%u",
          model_items, seen_texts_size, avatar_tex, media_cache,
          profile_queue, liked_events_size, profile_batches, gift_wrap_queue,
          pstats.cache_size, pstats.cache_cap);

  /* Prune caches if they exceed limits to prevent unbounded memory growth */
  gboolean pruned = FALSE;

  /* Avatar texture cache is managed by gnostr-avatar-cache module with its own LRU eviction */

  /* Prune seen_texts - clear if too large */
  if (seen_texts_size > SEEN_TEXTS_MAX) {
    g_debug("[MEMORY] Pruning seen_texts: %u -> 0", seen_texts_size);
    g_hash_table_remove_all(self->seen_texts);
    pruned = TRUE;
  }

  /* Prune liked_events - clear if too large */
  guint liked_size = self->liked_events ? g_hash_table_size(self->liked_events) : 0;
  if (liked_size > LIKED_EVENTS_MAX) {
    g_debug("[MEMORY] Pruning liked_events: %u -> 0", liked_size);
    g_hash_table_remove_all(self->liked_events);
    pruned = TRUE;
  }

  if (pruned) {
    g_debug("[MEMORY] Cache pruning complete");
  }
  
  return G_SOURCE_CONTINUE;
}

/* ---- Avatar HTTP downloader (libsoup) ---- */
#ifdef HAVE_SOUP3
typedef struct {
  GnostrMainWindow *self; /* strong ref */
  char *url;               /* owned */
} AvatarHttpCtx;
static void avatar_http_ctx_free(AvatarHttpCtx *c){ if(!c) return; if (c->self) g_object_unref(c->self); g_free(c->url); g_free(c); }
#endif /* HAVE_SOUP3 */

/* ---- Panel management helpers for OverlaySplitView ---- */
void gnostr_main_window_show_profile_panel_internal(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_show_profile_panel(self->session_view);
}

void gnostr_main_window_show_thread_panel_internal(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_show_thread_panel(self->session_view);

  /* nostrc-oz5: Refresh thread view profiles when showing the panel.
   * Profiles may have arrived from relays while the panel was hidden
   * or while viewing the profile pane. */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    nostr_gtk_thread_view_update_profiles(NOSTR_GTK_THREAD_VIEW(thread_view));
  }
}

void gnostr_main_window_show_article_panel_internal(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_show_article_panel(self->session_view);
}

void gnostr_main_window_hide_panel_internal(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_hide_side_panel(self->session_view);
}

gboolean gnostr_main_window_is_panel_visible_internal(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return FALSE;

  if (self->session_view)
    return gnostr_session_view_is_side_panel_visible(self->session_view);

  return FALSE;
}

/* ---- Relay Manager Dialog ---- */
/* Relay manager and NIP-66 discovery extracted to gnostr-main-window-relay-manager.c */

static void on_relays_clicked(GtkButton *btn, gpointer user_data) {
  gnostr_main_window_on_relays_clicked_internal(btn, user_data);
}


static void on_reconnect_requested(GnostrSessionView *view, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  gnostr_main_window_show_toast_internal(self, "Reconnecting to relays...");
  gnostr_main_window_start_pool_live_internal(self);
}

/* ---- Settings Dialog ---- */
/* Settings dialog and all panels extracted to gnostr-main-window-settings.c */

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
  gnostr_main_window_on_settings_clicked_internal(btn, user_data);
}

/* nostrc-61s.6: Handle window close-request for background mode */
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
  (void)user_data;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return FALSE; /* Let default close happen */

  if (self->background_mode_enabled) {
    /* Hide window instead of closing when background mode is enabled */
    g_debug("[UI] Background mode: hiding window instead of closing");
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE; /* Stop the close request, window stays alive */
  }

  /* Background mode disabled: allow normal close (quit) */
  return FALSE;
}

/* Sidebar row activation is now handled by GnostrSessionView */

/* Sidebar toggle is now handled by GnostrSessionView */

/* ESC key handler to close profile sidebar */
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return GDK_EVENT_PROPAGATE;

  /* nostrc-8mb8.2: Ctrl+Shift+D → protocol diagnostic dump */
  if (keyval == GDK_KEY_D &&
      (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
    gnostr_debug_dump_protocol_state(self);
    return GDK_EVENT_STOP;
  }

  if (keyval == GDK_KEY_Escape) {
    /* Close panel if it's open */
    if (gnostr_main_window_is_panel_visible_internal(self)) {
      if (!gnostr_session_view_is_showing_profile(self->session_view)) {
        g_debug("[UI] ESC pressed: closing thread view");
        GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
        if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
          nostr_gtk_thread_view_clear(NOSTR_GTK_THREAD_VIEW(thread_view));
        }
      } else {
        g_debug("[UI] ESC pressed: closing profile sidebar");
      }
      gnostr_main_window_hide_panel_internal(self);
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

static void gnostr_main_window_init(GnostrMainWindow *self) {
  gnostr_main_window_init_widget_state_internal(self);

  gnostr_main_window_connect_session_view_signals_internal(self,
                                                           G_CALLBACK(on_settings_clicked),
                                                           G_CALLBACK(on_relays_clicked),
                                                           G_CALLBACK(on_reconnect_requested),
                                                           G_CALLBACK(gnostr_main_window_on_avatar_login_clicked_internal),
                                                           G_CALLBACK(gnostr_main_window_on_avatar_logout_clicked_internal),
                                                           G_CALLBACK(gnostr_main_window_on_view_profile_requested_internal),
                                                           G_CALLBACK(gnostr_main_window_on_account_switch_requested_internal),
                                                           G_CALLBACK(gnostr_main_window_on_new_notes_clicked_internal),
                                                           G_CALLBACK(gnostr_main_window_on_compose_requested_internal),
                                                           G_CALLBACK(gnostr_main_window_on_session_search_committed_internal));

  /* Connect to signer service state-changed signal for dynamic UI updates */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  g_signal_connect(signer, "state-changed",
                   G_CALLBACK(gnostr_main_window_on_signer_state_changed_internal), self);

  gnostr_main_window_connect_child_view_signals_internal(self,
                                                         G_CALLBACK(gnostr_main_window_on_profile_pane_close_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_profile_pane_mute_user_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_profile_pane_follow_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_profile_pane_message_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_thread_view_close_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_thread_view_need_profile_internal),
                                                         G_CALLBACK(gnostr_main_window_on_thread_view_open_profile_internal),
                                                         G_CALLBACK(gnostr_main_window_on_article_reader_close_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_article_reader_open_profile_internal),
                                                         G_CALLBACK(gnostr_main_window_on_article_reader_open_url_internal),
                                                         G_CALLBACK(gnostr_main_window_on_article_reader_share_internal),
                                                         G_CALLBACK(gnostr_main_window_on_article_reader_zap_internal),
                                                         G_CALLBACK(gnostr_main_window_on_repo_selected_internal),
                                                         G_CALLBACK(gnostr_main_window_on_clone_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_repo_refresh_requested_internal),
                                                         G_CALLBACK(gnostr_main_window_on_repo_browser_need_profile_internal),
                                                         G_CALLBACK(gnostr_main_window_on_repo_browser_open_profile_internal));

  gnostr_main_window_init_runtime_state_internal(self);

  /* Initialize tuning knobs from env with sensible defaults */
  self->batch_max = getenv_uint_default("GNOSTR_BATCH_MAX", 5);
  self->post_interval_ms = getenv_uint_default("GNOSTR_POST_INTERVAL_MS", 150);
  self->eose_quiet_ms = getenv_uint_default("GNOSTR_EOSE_QUIET_MS", 150);
  self->per_relay_hard_ms = getenv_uint_default("GNOSTR_PER_RELAY_HARD_MS", 5000);
  self->default_limit = getenv_uint_default("GNOSTR_DEFAULT_LIMIT", 30);
  self->use_since = FALSE;
  self->since_seconds = getenv_uint_default("GNOSTR_SINCE_SECONDS", 3600);
  self->backfill_interval_sec = getenv_uint_default("GNOSTR_BACKFILL_SEC", 0);

  /* Load persisted settings (overrides env defaults) */
  gnostr_load_settings(self);
  self->backfill_source_id = 0;

  /* Init demand-driven profile fetch state */
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  self->profile_fetch_source_id = 0;
  self->profile_fetch_debounce_ms = 50; /* Reduced from 150ms for faster response */
  self->profile_fetch_cancellable = g_cancellable_new();
  self->profile_fetch_active = 0;
  self->profile_fetch_max_concurrent = 5; /* Increased from 3 - goroutines are lightweight */
  self->startup_profile_throttle_until_us = g_get_monotonic_time() + (15 * G_USEC_PER_SEC);
  self->startup_profile_batch_size = 20;
  self->startup_profile_max_concurrent = 1;
  self->startup_profile_inter_batch_delay_ms = 250;

  /* Init debounced NostrDB profile sweep */
  self->ndb_sweep_source_id = 0;
  self->ndb_sweep_debounce_ms = 1000; /* 1 second - prevents transaction contention */

  gnostr_main_window_init_dm_internal(self);

  gnostr_main_window_connect_window_signals_internal(self,
                                                     G_CALLBACK(on_window_close_request),
                                                     G_CALLBACK(on_key_pressed));
  /* Register window actions for menu (menu is now in session view) */
  gnostr_main_window_install_actions_internal(self);
  gnostr_main_window_connect_page_signals_internal(self,
                                                   G_CALLBACK(gnostr_main_window_on_discover_open_profile_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_copy_npub_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_open_communities_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_open_article_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_zap_article_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_search_hashtag_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_follow_requested_internal),
                                                   G_CALLBACK(gnostr_main_window_on_discover_unfollow_requested_internal),
                                                   G_CALLBACK(gnostr_main_window_on_search_open_note_internal),
                                                   G_CALLBACK(gnostr_main_window_on_search_open_profile_internal),
                                                   G_CALLBACK(gnostr_main_window_on_search_search_hashtag_internal),
                                                    G_CALLBACK(gnostr_main_window_on_notification_open_note_internal),
                                                    G_CALLBACK(gnostr_main_window_on_notification_open_profile_internal),
                                                    G_CALLBACK(gnostr_main_window_on_notification_open_conversation_internal),
                                                   G_CALLBACK(gnostr_main_window_on_classifieds_open_profile_internal),
                                                   G_CALLBACK(gnostr_main_window_on_classifieds_contact_seller_internal),
                                                   G_CALLBACK(gnostr_main_window_on_classifieds_listing_clicked_internal));

  /* nostrc-75o3.1: Defer ALL heavy initialization to a G_PRIORITY_LOW idle callback.
   * This lets GTK paint the skeleton LOADING page before event model setup,
   * relay connections, NDB queries, profile subscriptions, and gift wrap startup.
   * g_object_ref ensures `self` survives until the idle fires; g_object_unref is
   * the GDestroyNotify that balances it when the source is removed. */
  g_idle_add_full(G_PRIORITY_LOW, deferred_heavy_init_cb,
                  g_object_ref(self), g_object_unref);
}

/* nostrc-75o3.1: Heavy initialization deferred to after first frame paint.
 * Runs at G_PRIORITY_LOW so GTK has already rendered the LOADING page. */
static gboolean
deferred_heavy_init_cb(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;

  g_debug("[STARTUP] deferred_heavy_init_cb: ENTER");

  gnostr_main_window_run_startup_bootstrap_internal(self,
                                                    G_CALLBACK(gnostr_main_window_on_event_model_need_profile_internal),
                                                    G_CALLBACK(gnostr_main_window_on_event_model_new_items_pending_internal),
                                                    G_CALLBACK(gnostr_main_window_on_timeline_scroll_value_changed_internal),
                                                    G_CALLBACK(gnostr_main_window_on_timeline_tab_filter_changed_internal));

  /* nostrc-e03f.4: Initial tab bar setup is done synchronously in
   * gnostr_main_window_init_widget_state_internal so that user input can't
   * race with our tab installation. Nothing more to do here. */

  /* Seed initial items so Timeline page isn't empty.
   * The follow-on profile/gift-wrap startup now runs in stage2 from the
   * initial refresh callback so the first timeline paint happens earlier. */
  g_idle_add_once((GSourceOnceFunc)gnostr_main_window_initial_refresh_timeout_cb_internal, self);
  g_debug("[STARTUP] deferred_heavy_init_cb: EXIT (stage2 scheduled)");

  /* Background profile prefetch disabled (model emits need-profile when required). */

  /* Optional: insert a synthetic timeline event when GNOSTR_SYNTH is set (for wiring validation) */
  {
    const char *synth = g_getenv("GNOSTR_SYNTH");
    if (synth && *synth && g_strcmp0(synth, "0") != 0) {
      g_debug("[INIT] GNOSTR_SYNTH set");
      /* Disabled unless UiEventRow and schedule_apply_events are available */
      #if 0
      GPtrArray *rows = g_ptr_array_new_with_free_func(ui_event_row_free);
      UiEventRow *r = g_new0(UiEventRow, 1);
      /* Stable 64-hex id derived from timestamp string */
      gint64 now_sec = g_get_real_time() / 1000000;
      g_autofree char *seed = g_strdup_printf("gnostr-synth-%" G_GINT64_FORMAT, now_sec);
      r->id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
      r->pubkey = g_strdup("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
      r->created_at = now_sec;
      r->content = g_strdup("Hello Timeline (synth)");
      g_ptr_array_add(rows, r);
      schedule_apply_events(self, rows);
      #endif
    }
  }

  /* LEGITIMATE TIMEOUT - Periodic backfill if configured via GNOSTR_BACKFILL_SEC.
   * nostrc-b0h: Audited - user-configurable periodic operation is appropriate. */
  if (self->backfill_interval_sec > 0) {
    self->backfill_source_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
        self->backfill_interval_sec,
        periodic_backfill_cb,
        g_object_ref(self),
        g_object_unref);
  }

  gnostr_main_window_restore_session_services_internal(self);

  g_debug("[STARTUP] deferred_heavy_init_cb: heavy init complete");
  return G_SOURCE_REMOVE;
}

/* ---- Composer signal handlers (nostr-gtk decoupled signals) ---- */

/* Toast signal: show toast in main window */
static void on_composer_toast_requested(NostrGtkComposer *composer,
                                        const char *message,
                                        gpointer user_data) {
  (void)composer;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !message) return;
  gnostr_main_window_show_toast(GTK_WIDGET(self), message);
}

/* Upload signal: perform Blossom upload and inject result back */
static void on_composer_upload_blossom_done(GnostrBlossomBlob *blob, GError *error, gpointer user_data) {
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) {
    if (blob) gnostr_blossom_blob_free(blob);
    return;
  }

  if (error) {
    nostr_gtk_composer_upload_failed(composer, error->message);
    return;
  }

  if (!blob || !blob->url) {
    nostr_gtk_composer_upload_failed(composer, "Server returned no URL");
    return;
  }

  nostr_gtk_composer_upload_complete(composer, blob->url, blob->sha256,
                                  blob->mime_type, blob->size);
  gnostr_blossom_blob_free(blob);
}

static void on_composer_upload_requested(NostrGtkComposer *composer,
                                         const char *file_path,
                                         gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !file_path) return;

  g_message("main-window: handling upload request for %s", file_path);
  gnostr_media_upload_async(file_path, NULL,
                             on_composer_upload_blossom_done, composer,
                             NULL);
}

/* Draft save signal: read composer state and persist */
static void on_draft_save_done(GnostrDrafts *drafts, gboolean success,
                               const char *error_message, gpointer user_data) {
  (void)drafts;
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;
  const char *d_tag = nostr_gtk_composer_get_current_draft_d_tag(composer);
  nostr_gtk_composer_draft_save_complete(composer, success, error_message, d_tag);
}

static void on_composer_save_draft_requested(NostrGtkComposer *composer,
                                              gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;

  g_autofree char *text = nostr_gtk_composer_get_text(composer);
  if (!text || !*text) return;

  GnostrDraft *draft = gnostr_draft_new();
  draft->content = g_strdup(text);
  draft->target_kind = 1;

  const char *current_d = nostr_gtk_composer_get_current_draft_d_tag(composer);
  if (current_d) draft->d_tag = g_strdup(current_d);

  const char *subject = nostr_gtk_composer_get_subject(composer);
  if (subject) draft->subject = g_strdup(subject);

  const char *reply_id = nostr_gtk_composer_get_reply_to_id(composer);
  if (reply_id) draft->reply_to_id = g_strdup(reply_id);

  const char *root_id = nostr_gtk_composer_get_root_id(composer);
  if (root_id) draft->root_id = g_strdup(root_id);

  const char *reply_pk = nostr_gtk_composer_get_reply_to_pubkey(composer);
  if (reply_pk) draft->reply_to_pubkey = g_strdup(reply_pk);

  const char *quote_id = nostr_gtk_composer_get_quote_id(composer);
  if (quote_id) draft->quote_id = g_strdup(quote_id);

  const char *quote_pk = nostr_gtk_composer_get_quote_pubkey(composer);
  if (quote_pk) draft->quote_pubkey = g_strdup(quote_pk);

  const char *quote_uri = nostr_gtk_composer_get_quote_nostr_uri(composer);
  if (quote_uri) draft->quote_nostr_uri = g_strdup(quote_uri);

  draft->is_sensitive = nostr_gtk_composer_is_sensitive(composer);

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_save_async(drafts_mgr, draft, on_draft_save_done, composer);

  gnostr_draft_free(draft);
}

/* Draft list signal: load drafts and populate composer list */
static void on_composer_load_drafts_requested(NostrGtkComposer *composer,
                                               gpointer user_data) {
  (void)user_data;

  nostr_gtk_composer_clear_draft_rows(composer);

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);
  if (!drafts || drafts->len == 0) {
    if (drafts) g_ptr_array_free(drafts, TRUE);
    return;
  }
  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *d = (GnostrDraft *)g_ptr_array_index(drafts, i);
    const char *content = d->content ? d->content : "";
    char *preview = g_strndup(content, 50);
    for (char *p = preview; *p; p++) {
      if (*p == '\n' || *p == '\r') *p = ' ';
    }
    if (strlen(content) > 50) {
      char *tmp = g_strdup_printf("%s...", preview);
      g_free(preview);
      preview = tmp;
    }
    nostr_gtk_composer_add_draft_row(composer, d->d_tag, preview, d->updated_at);
    g_free(preview);
  }

  g_ptr_array_free(drafts, TRUE);
}

/* Draft load signal: load specific draft by d-tag */
static void on_composer_draft_load_requested(NostrGtkComposer *composer,
                                              const char *d_tag,
                                              gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !d_tag) return;

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);
  if (!drafts) return;

  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *d = (GnostrDraft *)g_ptr_array_index(drafts, i);
    if (d->d_tag && strcmp(d->d_tag, d_tag) == 0) {
      NostrGtkComposerDraftInfo info = {
        .d_tag = d->d_tag,
        .content = d->content,
        .subject = d->subject,
        .reply_to_id = d->reply_to_id,
        .root_id = d->root_id,
        .reply_to_pubkey = d->reply_to_pubkey,
        .quote_id = d->quote_id,
        .quote_pubkey = d->quote_pubkey,
        .quote_nostr_uri = d->quote_nostr_uri,
        .is_sensitive = d->is_sensitive,
        .target_kind = d->target_kind,
        .updated_at = d->updated_at,
      };
      nostr_gtk_composer_load_draft(composer, &info);
      break;
    }
  }

  g_ptr_array_free(drafts, TRUE);
}

/* Draft delete signal: delete by d-tag */
static void on_draft_delete_done(GnostrDrafts *drafts, gboolean success,
                                  const char *error_message, gpointer user_data) {
  (void)drafts;
  (void)error_message;
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;
  nostr_gtk_composer_draft_delete_complete(composer, NULL, success);
}

static void on_composer_draft_delete_requested(NostrGtkComposer *composer,
                                                const char *d_tag,
                                                gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !d_tag) return;

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_delete_async(drafts_mgr, d_tag, on_draft_delete_done, composer);
}

gboolean gnostr_main_window_get_compact(GnostrMainWindow *self) {
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(self), FALSE);
  return self->compact;
}

void gnostr_main_window_set_compact(GnostrMainWindow *self, gboolean compact) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  compact = !!compact;
  if (self->compact == compact) return;

  self->compact = compact;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

GtkWidget *gnostr_main_window_get_repo_browser(GnostrMainWindow *self) {
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(self), NULL);
  if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
    return NULL;
  return gnostr_session_view_get_repo_browser(self->session_view);
}

GnostrSessionView *gnostr_main_window_get_session_view(GnostrMainWindow *self) {
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(self), NULL);
  return self->session_view;
}

static void gnostr_main_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  gnostr_main_window_get_property_internal(object, prop_id, value, pspec);
}

static void gnostr_main_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  gnostr_main_window_set_property_internal(object, prop_id, value, pspec);
}

GnostrMainWindow *gnostr_main_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}

/* ---- GObject type boilerplate and template binding ---- */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, ADW_TYPE_APPLICATION_WINDOW)

static void gnostr_main_window_dispose(GObject *object) {
  gnostr_main_window_dispose_internal(object);
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->dispose(object);
}

static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_main_window_dispose;
  object_class->get_property = gnostr_main_window_get_property;
  object_class->set_property = gnostr_main_window_set_property;

  props[PROP_COMPACT] = g_param_spec_boolean("compact", "Compact layout",
                                             "Whether the window uses the compact responsive layout",
                                             FALSE,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gnostr_main_window_bind_template_internal(widget_class);
}

/* ---- Minimal stub implementations to satisfy build and support cached profiles path ---- */
static void user_meta_free(gpointer p) { g_free(p); }

static unsigned int getenv_uint_default(const char *name, unsigned int defval) {
  const char *v = g_getenv(name);
  if (!v || !*v) return defval;
  gchar *end = NULL;
  unsigned long val = g_ascii_strtoull(v, &end, 10);
  if (end == v) return defval;
  if (val > G_MAXUINT) return defval;
  return (unsigned int)val;
}

static void gnostr_load_settings(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* nostrc-61s.6: Load background mode setting */
  g_autoptr(GSettings) client_settings = g_settings_new("org.gnostr.Client");
  if (client_settings) {
    self->background_mode_enabled = g_settings_get_boolean(client_settings, "background-mode");
    g_debug("[SETTINGS] background_mode_enabled=%d", self->background_mode_enabled);

    /* If background mode is enabled, hold the application to prevent quit on last window close */
    if (self->background_mode_enabled) {
      GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
      if (app) {
        g_application_hold(G_APPLICATION(app));
        g_debug("[SETTINGS] Application held for background mode");
      }
    }
  }
}


static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit) {
  int kind1 = 1;
  gnostr_main_window_build_urls_and_filters_for_kinds_internal(self, &kind1, 1, out_urls, out_count, out_filters, limit);
}

void gnostr_main_window_build_urls_and_filters_for_kinds_internal(GnostrMainWindow *self,
                                            const int *kinds,
                                            size_t n_kinds,
                                            const char ***out_urls,
                                            size_t *out_count,
                                            NostrFilters **out_filters,
                                            int limit) {
  if (out_urls) *out_urls = NULL;
  if (out_count) *out_count = 0;
  if (out_filters) *out_filters = NULL;

  /* Load read-capable relays from config (NIP-65: read-only or read+write) */
  GPtrArray *arr = gnostr_get_read_relay_urls();

  const char **urls = NULL;
  size_t n = arr->len;
  if (n > 0) {
    urls = g_new0(const char*, n);
    for (guint i = 0; i < arr->len; i++) urls[i] = (const char*)g_ptr_array_index(arr, i);
  }
  if (out_urls) *out_urls = urls;
  if (out_count) *out_count = n;

  /* Build filters */
  if (out_filters) {
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();

    if (kinds && n_kinds > 0) {
      nostr_filter_set_kinds(f, (int*)kinds, (int)n_kinds);
    } else {
      int fallback_kind = 1;
      nostr_filter_set_kinds(f, &fallback_kind, 1);
    }

    if (limit > 0) nostr_filter_set_limit(f, limit);

    /* Optional since window: only use when explicitly enabled */
    if (self && self->use_since && self->since_seconds > 0) {
      time_t now = time(NULL);
      int64_t since = (int64_t)(now - (time_t)self->since_seconds);
      if (since > 0) nostr_filter_set_since_i64(f, since);
    }

    nostr_filters_add(fs, f);
    *out_filters = fs;
  }

  /* Transfer ownership: callers must free urls+strings via free_urls_owned(). */
  if (arr) g_ptr_array_free(arr, FALSE); /* do not free contained strings */
}

void gnostr_main_window_free_urls_owned_internal(const char **urls, size_t count) {
  if (!urls) return;
  for (size_t i = 0; i < count; i++) {
    g_free((gpointer)urls[i]);
  }
  g_free((gpointer)urls);
}

static gboolean periodic_backfill_cb(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

/* nostrc-mzab: Background NDB ingestion thread.
 * Drains the ingest_queue and calls storage_ndb_ingest_event_json()
 * off the main thread, so ndb_process_event (which can block when the
 * ndb ingestion pipeline is full) never stalls the GTK main loop. */
gpointer
gnostr_main_window_ingest_thread_func_internal(gpointer data)
{
  GnostrMainWindow *self = (GnostrMainWindow *)data;

  while (__atomic_load_n(&self->ingest_running, __ATOMIC_SEQ_CST)) {
    gchar *json = g_async_queue_timeout_pop(self->ingest_queue, 100000); /* 100ms */
    if (!json)
      continue;

    /* nostrc-u6hlt: Skip empty sentinel pushed by dispose to wake us. */
    if (json[0] == '\0') {
      g_free(json);
      continue;
    }

    int rc = storage_ndb_ingest_event_json(json, NULL);
    if (rc == 0) {
      __atomic_fetch_add(&self->ingest_events_processed, 1, __ATOMIC_RELAXED);
    } else {
      g_debug("[INGEST-BG] Failed: rc=%d json_len=%zu", rc, strlen(json));
    }
    g_free(json);
  }

  /* nostrc-u6hlt: Drain remaining events before exit, but cap at 64
   * items and 500 ms to avoid blocking shutdown if the queue has a
   * large backlog or storage_ndb_ingest is slow.  Remaining events
   * will be picked up on next launch via normal relay sync. */
  {
    gchar *json;
    int drained = 0;
    gint64 drain_deadline = g_get_monotonic_time() + 500000; /* 500 ms */
    while (drained < 64 &&
           g_get_monotonic_time() < drain_deadline &&
           (json = g_async_queue_try_pop(self->ingest_queue)) != NULL) {
      if (json[0] != '\0')  /* skip sentinel */
        storage_ndb_ingest_event_json(json, NULL);
      g_free(json);
      drained++;
    }
    /* Discard any remaining items without processing. */
    while ((json = g_async_queue_try_pop(self->ingest_queue)) != NULL)
      g_free(json);
  }

  return NULL;
}

/* Background prefetch event handler: only enqueue authors for profile fetch */
static void on_bg_prefetch_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data) {
  (void)sub;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_json) return;

  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
    if (evt) nostr_event_free(evt);
    return;
  }
  if (nostr_event_get_kind(evt) == 1) {
    const char *pk = nostr_event_get_pubkey(evt);
    if (pk && strlen(pk) == 64) {
      gnostr_main_window_enqueue_profile_author_internal(self, pk);
    }
  }
  nostr_event_free(evt);
}

/* Lowercase hex encode */
static char *hex_encode_lower(const uint8_t *buf, size_t len) {
  if (!buf || len == 0) return g_strdup("");
  static const char *hex = "0123456789abcdef";
  char *s = g_malloc0(len * 2 + 1);
  for (size_t i = 0; i < len; i++) { s[i*2] = hex[(buf[i] >> 4) & 0xF]; s[i*2+1] = hex[buf[i] & 0xF]; }
  s[len*2] = '\0';
  return s;
}
/* Decode 64-char hex pubkey into 32 bytes */
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex || !out) return FALSE;
  size_t L = strlen(hex);
  if (L != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    char c1 = hex[i*2];
    char c2 = hex[i*2+1];
    int v1, v2;
    if      (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') v1 = 10 + (c1 - 'a');
    else if (c1 >= 'A' && c1 <= 'F') v1 = 10 + (c1 - 'A');
    else return FALSE;
    if      (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') v2 = 10 + (c2 - 'a');
    else if (c2 >= 'A' && c2 <= 'F') v2 = 10 + (c2 - 'A');
    else return FALSE;
    out[i] = (uint8_t)((v1 << 4) | v2);
  }
  return TRUE;
}
