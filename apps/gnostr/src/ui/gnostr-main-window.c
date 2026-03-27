#define G_LOG_DOMAIN "gnostr-main-window"

#include "gnostr-main-window.h"
#include "gnostr-main-window-private.h"
#include "gnostr-tray-icon.h"
#include "gnostr-session-view.h"
#include <nostr-gtk-1.0/gnostr-composer.h>
#include "gnostr-timeline-view.h"
#include <nostr-gtk-1.0/gn-timeline-tabs.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>
#include <nostr-gtk-1.0/gnostr-thread-view.h>
#include "gnostr-article-reader.h"
#include "gnostr-article-composer.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_profile_service.h>
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "gnostr-dm-row.h"
#include "gnostr-dm-service.h"
#include "gnostr-notifications-view.h"
#include "gnostr-notification-row.h"
#include "page-discover.h"
#include "gnostr-search-results-view.h"
#include "gnostr-classifieds-view.h"
#include "gnostr-repo-browser.h"
#include "gnostr-plugin-manager-panel.h"
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
/* NIP-11 relay information */
#include "../util/relay_info.h"
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
/* NIP-66 relay discovery */
#include "../util/nip66_relay_discovery.h"
/* Metrics dashboard */
#include "nostr/metrics.h"
#include "nostr/metrics_schema.h"
#include "nostr/metrics_collector.h"
/* NIP-37 Draft events */
#include "../util/gnostr-drafts.h"
#include "gnostr-login.h"
#include "gnostr-report-dialog.h"
/* NIP-72 Communities */
#include "gnostr-community-card.h"
#include "gnostr-community-view.h"
#include "../util/nip72_communities.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
/* NIP-19 helpers (now via GNostrNip19, included above) */
/* NIP-10 threading */
#include "nip10.h"
/* NIP-46 client (remote signer pairing) */
#include "nostr/nip46/nip46_client.h"
/* Nostr utilities */
#include "nostr-utils.h"
/* Gnostr utilities (shared query pool) */
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
static char *client_settings_get_current_npub(void);
static char *hex_encode_lower(const uint8_t *buf, size_t len);
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);
/* Forward declarations needed early due to reordering */
static void gnostr_load_settings(GnostrMainWindow *self);
static unsigned int getenv_uint_default(const char *name, unsigned int defval);
static gpointer ingest_thread_func(gpointer data);
static gboolean ingest_queue_push(GnostrMainWindow *self, gchar *json);
static void start_pool_live(GnostrMainWindow *self);
static void start_profile_subscription(GnostrMainWindow *self);
static void start_bg_profile_prefetch(GnostrMainWindow *self);
/* nostrc-75o3.1: Deferred heavy init that runs after first frame paint */
static gboolean deferred_heavy_init_cb(gpointer data);
static void on_relay_config_changed(gpointer user_data);
static gboolean on_relay_config_changed_restart(gpointer user_data);
/* Gift wrap (NIP-59) subscription for private DMs */
static void start_gift_wrap_subscription(GnostrMainWindow *self);
static void stop_gift_wrap_subscription(GnostrMainWindow *self);
static void on_gift_wrap_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static char *get_current_user_pubkey_hex(void);
static void on_event_model_need_profile(GnNostrEventModel *model, const char *pubkey_hex, gpointer user_data);
static void on_timeline_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
static void on_event_model_new_items_pending(GnNostrEventModel *model, guint count, gpointer user_data);
static void on_timeline_tab_filter_changed(NostrGtkTimelineView *view, guint type, const char *filter_value, gpointer user_data);
static void on_new_notes_clicked(GtkButton *btn, gpointer user_data);
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_compose_requested(GnostrSessionView *session_view, gpointer user_data);
static void on_relays_clicked(GtkButton *btn, gpointer user_data);
static void on_reconnect_requested(GnostrSessionView *view, gpointer user_data);
static void on_settings_clicked(GtkButton *btn, gpointer user_data);
static void on_show_mute_list_activated(GSimpleAction *action, GVariant *param, gpointer user_data);
static void on_show_about_activated(GSimpleAction *action, GVariant *param, gpointer user_data);
static void on_show_preferences_activated(GSimpleAction *action, GVariant *param, gpointer user_data);
static void on_avatar_login_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_logout_clicked(GtkButton *btn, gpointer user_data);
static void on_view_profile_requested(GnostrSessionView *sv, gpointer user_data);
static void on_signer_state_changed(GnostrSignerService *signer, guint old_state, guint new_state, gpointer user_data);
static void on_note_card_open_profile(NostrGtkNoteCardRow *row, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_close_requested(NostrGtkProfilePane *pane, gpointer user_data);
static void on_profile_pane_mute_user_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_follow_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_message_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
static void navigate_to_dm_conversation(GnostrMainWindow *self, const char *peer_pubkey);
/* Forward declarations for discover page signal handlers */
static void on_discover_open_profile(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data);
static void on_discover_open_communities(GnostrPageDiscover *page, gpointer user_data);
static void on_discover_open_article(GnostrPageDiscover *page, const char *event_id, gint kind, gpointer user_data);
static void on_discover_zap_article(GnostrPageDiscover *page, const char *event_id, const char *pubkey_hex, const char *lud16, gpointer user_data);
static void on_discover_search_hashtag(GnostrPageDiscover *page, const char *hashtag, gpointer user_data);
static void on_stack_visible_child_changed(GObject *stack, GParamSpec *pspec, gpointer user_data);
/* Forward declarations for search results view signal handlers (nostrc-29) */
static void on_search_open_note(GnostrSearchResultsView *view, const char *event_id_hex, gpointer user_data);
static void on_search_open_profile(GnostrSearchResultsView *view, const char *pubkey_hex, gpointer user_data);
/* Forward declarations for notification view signal handlers */
static void on_notification_open_note(GnostrNotificationsView *view, const char *note_id, gpointer user_data);
static void on_notification_open_profile(GnostrNotificationsView *view, const char *pubkey_hex, gpointer user_data);
/* Forward declarations for marketplace/classifieds signal handlers */
static void on_classifieds_open_profile(GnostrClassifiedsView *view, const char *pubkey_hex, gpointer user_data);
static void on_classifieds_contact_seller(GnostrClassifiedsView *view, const char *pubkey_hex, const char *lud16, gpointer user_data);
static void on_classifieds_listing_clicked(GnostrClassifiedsView *view, const char *event_id, const char *naddr, gpointer user_data);
/* Forward declarations for DM conversation signal handlers */
static void on_dm_inbox_open_conversation(GnostrDmInboxView *inbox, const char *peer_pubkey, gpointer user_data);
static void on_dm_inbox_compose(GnostrDmInboxView *inbox, gpointer user_data);
static void on_dm_conversation_go_back(GnostrDmConversationView *view, gpointer user_data);
static void on_dm_conversation_send_message(GnostrDmConversationView *view, const char *content, gpointer user_data);
static void on_dm_conversation_send_file(GnostrDmConversationView *view, const char *file_path, gpointer user_data);
static void on_dm_conversation_open_profile(GnostrDmConversationView *view, const char *pubkey_hex, gpointer user_data);
static void on_dm_service_message_received(GnostrDmService *service, const char *peer_pubkey, GnostrDmMessage *msg, gpointer user_data);
/* Forward declaration for ESC key handler to close profile sidebar */
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
/* Forward declaration for close-request handler (nostrc-61s.6: background mode) */
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
/* Forward declarations for responsive navigation (nostrc-3u7j) */
static void on_sidebar_toggle_clicked(GtkToggleButton *button, gpointer user_data);
static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
/* Forward declarations for repost/quote/like signal handlers */
static void on_note_card_repost_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_quote_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_like_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data);
/* nostrc-c0mp: Forward declarations for compose dialog with context */
typedef enum {
  COMPOSE_CONTEXT_NONE,
  COMPOSE_CONTEXT_REPLY,
  COMPOSE_CONTEXT_QUOTE,
  COMPOSE_CONTEXT_COMMENT
} ComposeContextType;

typedef struct {
  ComposeContextType type;
  /* For reply: */
  char *reply_to_id;
  char *root_id;
  char *reply_to_pubkey;
  char *display_name;
  /* For quote: */
  char *quote_id;
  char *quote_pubkey;
  char *nostr_uri;
  /* For comment: */
  char *comment_root_id;
  int comment_root_kind;
  char *comment_root_pubkey;
} ComposeContext;

static void compose_context_free(ComposeContext *ctx);
static void open_compose_dialog_with_context(GnostrMainWindow *self, ComposeContext *context);

static void user_meta_free(gpointer p);
static void show_toast(GnostrMainWindow *self, const char *msg);
static void gnostr_main_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void gnostr_main_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
/* Pre-populate profile from local DB */
static void prepopulate_profile_from_cache(GnostrMainWindow *self);
/* Initial backfill timeout trampoline */
static void initial_refresh_timeout_cb(gpointer data);
/* Apply profiles for current items by querying local NostrdB */
static void apply_profiles_for_current_items_from_ndb(GnostrMainWindow *self);
/* Debounced scheduler for NostrDB profile sweep */
static void schedule_ndb_profile_sweep(GnostrMainWindow *self);
/* Pre-populate text notes (kind-1) from local DB into timeline */
static void prepopulate_text_notes_from_cache(GnostrMainWindow *self, guint limit);
static void build_urls_and_filters_for_kinds(GnostrMainWindow *self,
                                            const int *kinds,
                                            size_t n_kinds,
                                            const char ***out_urls,
                                            size_t *out_count,
                                            NostrFilters **out_filters,
                                            int limit);
static void free_urls_owned(const char **urls, size_t count);

/* Avatar HTTP downloader (libsoup) helpers declared later, after struct definition */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json);
static void refresh_thread_view_profiles_if_visible(GnostrMainWindow *self);
static void on_pool_sub_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data);
static void on_pool_sub_eose(GNostrSubscription *sub, gpointer user_data);
static void on_multi_sub_event(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, const gchar *event_json, gpointer user_data);
static void on_multi_sub_eose(GNostrPoolMultiSub *multi_sub, const gchar *relay_url, gpointer user_data);
static void on_bg_prefetch_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data);
static gboolean periodic_model_refresh(gpointer user_data);

/* Demand-driven profile fetch helpers */
static void enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex);
static gboolean profile_fetch_fire_idle(gpointer data);
static void on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean profile_dispatch_next(gpointer data);

/* Forward decl: relay URL builder used by profile fetch */
static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit);

/* Forward decl: hex decode utility */
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);

/* Forward decl: main-loop trampoline to enqueue a single author */
static gboolean enqueue_author_on_main(gpointer data);

typedef struct {
  GnostrMainWindow *self; /* strong ref */
  char *pubkey_hex;       /* owned */
} EnqueueAuthorCtx;

static gboolean enqueue_author_on_main(gpointer data) {
  EnqueueAuthorCtx *c = (EnqueueAuthorCtx*)data;
  if (c && GNOSTR_IS_MAIN_WINDOW(c->self) && c->pubkey_hex)
    enqueue_profile_author(c->self, c->pubkey_hex);
  if (c) {
    if (c->self) g_object_unref(c->self);
    g_free(c->pubkey_hex);
    g_free(c);
  }
  return G_SOURCE_REMOVE;
}

typedef struct {
  char *pubkey_hex;   /* owned */
  char *content_json; /* owned */
} ProfileApplyCtx;

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
    update_meta_from_profile_json(self, it->pubkey_hex, it->content_json);
    applied++;
  }
  c->pos = end;

  /* nostrc-sk8o: Refresh thread view ONCE per batch (not per-profile) */
  if (applied > 0) {
    refresh_thread_view_profiles_if_visible(self);
  }

  if (c->pos < c->items->len) {
    return G_SOURCE_CONTINUE;  /* More profiles to process next tick */
  }

  idle_apply_profiles_ctx_free(c);
  return G_SOURCE_REMOVE;
}

static void schedule_apply_profiles(GnostrMainWindow *self, GPtrArray *items /* ProfileApplyCtx* */) {
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
        update_meta_from_profile_json(win, c->pubkey_hex, c->content_json);
        /* nostrc-sk8o: Refresh thread view after single profile update */
        refresh_thread_view_profiles_if_visible(win);
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

/* ---- Demand-driven profile fetch (debounced) ---- */
static void enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || strlen(pubkey_hex) != 64) return;
  
  /* CRITICAL FIX: Don't re-fetch profiles we already have in provider cache!
   * This prevents fetching the same 1411 authors repeatedly on every scroll. */
  GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
  if (meta) {
    /* Already have this profile in provider - skip */
    gnostr_profile_meta_free(meta);
    return;
  }

  /* Note: Removed storage_ndb_is_profile_stale() check here as it causes
   * main-thread NDB queries during scroll pagination, freezing the UI.
   * Profile deduplication is handled by the provider cache check above. */
  
  if (!self->profile_fetch_queue)
    self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  /* Dedup linear scan (queue is expected to stay small) */
  for (guint i = 0; i < self->profile_fetch_queue->len; i++) {
    const char *s = (const char*)g_ptr_array_index(self->profile_fetch_queue, i);
    if (g_strcmp0(s, pubkey_hex) == 0) goto schedule_only; /* already queued */
  }
  g_ptr_array_add(self->profile_fetch_queue, g_strdup(pubkey_hex));
schedule_only:
  /* LEGITIMATE TIMEOUT - Debounce profile fetch triggering.
   * Batches rapid profile requests (e.g., scrolling through timeline) into
   * single network requests. Default 150ms delay is configurable.
   * nostrc-b0h: Audited - debounce for batching is appropriate. */
  if (self->profile_fetch_source_id) {
    /* already scheduled; let it fire */
  } else {
    guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
    GnostrMainWindow *ref = g_object_ref(self);
    self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, delay, profile_fetch_fire_idle, ref, g_object_unref);
  }
}

/* ---- Toast helpers and UI signal handlers ---- */
static void show_toast(GnostrMainWindow *self, const char *msg) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !msg) return;

  if (self->toast_overlay) {
    AdwToast *toast = adw_toast_new(msg);
    adw_toast_set_timeout(toast, 2);
    adw_toast_overlay_add_toast(self->toast_overlay, toast);
  }
}

void gnostr_main_window_show_toast_internal(GnostrMainWindow *self, const char *message) {
  show_toast(self, message);
}

/* ---- Panel management helpers for OverlaySplitView ---- */
static void show_profile_panel(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_show_profile_panel(self->session_view);
}

static void show_thread_panel(GnostrMainWindow *self) {
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

static void show_article_panel(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_show_article_panel(self->session_view);
}

static void hide_panel(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->session_view)
    gnostr_session_view_hide_side_panel(self->session_view);
}

static gboolean is_panel_visible(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return FALSE;

  if (self->session_view)
    return gnostr_session_view_is_side_panel_visible(self->session_view);

  return FALSE;
}

static void settings_on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GtkWindow *win = GTK_WINDOW(user_data);
  if (GTK_IS_WINDOW(win)) gtk_window_close(win);
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

  show_toast(self, "Reconnecting to relays...");
  start_pool_live(self);
}

/* ---- Settings Dialog ---- */
/* Settings dialog and all panels extracted to gnostr-main-window-settings.c */

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
  gnostr_main_window_on_settings_clicked_internal(btn, user_data);
}

static void on_show_about_activated(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action;
  (void)param;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-about-dialog.ui");
  if (!builder) {
    show_toast(self, "About dialog UI missing");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "about_window"));
  if (!win) {
    g_object_unref(builder);
    show_toast(self, "About window missing");
    return;
  }

  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);

  g_object_set_data_full(G_OBJECT(win), "builder", builder, g_object_unref);

  gtk_window_present(win);
}

static void on_show_preferences_activated(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action;
  (void)param;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  on_settings_clicked(NULL, self);
}


/* Forward declaration for updating login UI state */
static void update_login_ui_state(GnostrMainWindow *self);

/* nostrc-51a.10: Callback for badge manager notification events.
 * Populates the NotificationsView when new events arrive. */
static void
on_notification_event(GnostrBadgeManager *manager,
                      GnostrNotificationType type,
                      const char *sender_pubkey,
                      const char *sender_name,
                      const char *content,
                      const char *event_id,
                      guint64 amount_sats,
                      gpointer user_data)
{
  (void)manager;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->session_view) return;

  /* Get the notifications view from the session view */
  GtkWidget *notif_widget = gnostr_session_view_get_notifications_view(self->session_view);
  if (!notif_widget || !GNOSTR_IS_NOTIFICATIONS_VIEW(notif_widget)) return;

  GnostrNotificationsView *notif_view = GNOSTR_NOTIFICATIONS_VIEW(notif_widget);

  /* Create notification object */
  GnostrNotification *notif = g_new0(GnostrNotification, 1);
  notif->id = event_id ? g_strdup(event_id) : g_strdup_printf("notif-%" G_GINT64_FORMAT, g_get_real_time());
  notif->type = type;
  notif->actor_pubkey = sender_pubkey ? g_strdup(sender_pubkey) : NULL;
  notif->actor_name = sender_name ? g_strdup(sender_name) : NULL;
  notif->content_preview = content ? g_strdup(content) : NULL;
  notif->target_note_id = event_id ? g_strdup(event_id) : NULL;
  notif->created_at = g_get_real_time() / G_USEC_PER_SEC;
  notif->is_read = FALSE;
  notif->zap_amount_msats = amount_sats * 1000;  /* Convert sats to msats */

  /* Add to the notifications view */
  gnostr_notifications_view_add_notification(notif_view, notif);

  /* The view takes ownership, but we need to free our copy */
  gnostr_notification_free(notif);

  g_debug("[NOTIFICATIONS] Added notification: type=%d from %.16s...",
          type, sender_pubkey ? sender_pubkey : "(unknown)");
}

/* Callback for user profile fetch at login */
static void on_user_profile_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    g_object_unref(self);
    return;
  }

  GError *error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(
    GNOSTR_POOL(source), res, &error);

  if (error) {
    g_warning("[AUTH] Profile fetch error: %s", error->message);
    g_clear_error(&error);
  }

  if (jsons && jsons->len > 0) {
    /* Parse the first profile event using GNostrEvent */
    const char *evt_json = g_ptr_array_index(jsons, 0);
    if (evt_json) {
      g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *content = gnostr_event_get_content(evt);
        if (content && *content) {
          /* Ingest into nostrdb in background for future use */
          {
            GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(b, g_strdup(evt_json));
            storage_ndb_ingest_events_async(b);
          }

          /* Update profile provider cache (this parses the JSON for us) */
          if (self->user_pubkey_hex) {
            gnostr_profile_provider_update(self->user_pubkey_hex, content);

            /* Now get the parsed profile and update the UI */
            GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
            if (meta) {
              const char *final_name = (meta->display_name && *meta->display_name)
                                       ? meta->display_name : meta->name;
              if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
                gnostr_session_view_set_user_profile(self->session_view,
                                                      self->user_pubkey_hex,
                                                      final_name,
                                                      meta->picture);
              }
              gnostr_profile_meta_free(meta);
            }
          }
        }
      }
    }
    g_ptr_array_unref(jsons);
  }

  g_object_unref(self);
}

/* nostrc-profile-fix: Callback for NIP-65 load, then fetch user profile.
 * This ensures we have the user's relay list before fetching their profile. */
static void on_nip65_loaded_for_profile(GPtrArray *nip65_relays, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->user_pubkey_hex) {
    g_object_unref(self);
    return;
  }

  /* First try cached profile (may have been ingested during NIP-65 fetch) */
  GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
  if (meta) {
    const char *final_name = (meta->display_name && *meta->display_name)
                             ? meta->display_name : meta->name;
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_set_user_profile(self->session_view,
                                            self->user_pubkey_hex,
                                            final_name,
                                            meta->picture);
    }
    gnostr_profile_meta_free(meta);
    g_object_unref(self);
    return;
  }

  /* Build relay list for profile fetch: use NIP-65 read relays + fallbacks */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  /* Add user's read relays from their 10002 (if available) */
  if (nip65_relays && nip65_relays->len > 0) {
    GPtrArray *read_relays = gnostr_nip65_get_read_relays(nip65_relays);
    if (read_relays) {
      for (guint i = 0; i < read_relays->len; i++) {
        g_ptr_array_add(relay_urls, g_strdup(g_ptr_array_index(read_relays, i)));
      }
      g_ptr_array_unref(read_relays);
    }
  }

  /* Also add configured relays as backup */
  gnostr_get_read_relay_urls_into(relay_urls);

  /* Always include profile-indexing relays */
  static const char *profile_relays[] = {
    "wss://purplepag.es", "wss://relay.nostr.band", "wss://relay.damus.io", NULL
  };
  for (int i = 0; profile_relays[i]; i++) {
    gboolean found = FALSE;
    for (guint j = 0; j < relay_urls->len; j++) {
      if (g_strcmp0(g_ptr_array_index(relay_urls, j), profile_relays[i]) == 0) {
        found = TRUE;
        break;
      }
    }
    if (!found) g_ptr_array_add(relay_urls, g_strdup(profile_relays[i]));
  }

  if (relay_urls->len > 0) {
    const gchar **urls = g_new0(const gchar*, relay_urls->len);
    for (guint i = 0; i < relay_urls->len; i++) {
      urls[i] = g_ptr_array_index(relay_urls, i);
    }

    /* Create a GNostrPool, sync relays, build kind-0 filter */
    g_autoptr(GNostrPool) profile_pool = gnostr_pool_new();
    gnostr_pool_sync_relays(profile_pool, urls, relay_urls->len);

    NostrFilter *f = nostr_filter_new();
    int kind0 = 0;
    nostr_filter_set_kinds(f, &kind0, 1);
    const char *authors[1] = { self->user_pubkey_hex };
    nostr_filter_set_authors(f, (const char *const *)authors, 1);
    nostr_filter_set_limit(f, 1);
    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, f);
    nostr_filter_free(f);

    /* nostrc-dblf: gnostr_pool_query_async takes ownership of filters internally
     * (attaches to GTask with destroy notify). Do NOT also attach to pool or
     * we get double-free when both pool and task are disposed. */

    g_debug("[AUTH] Fetching profile from %u relays (after NIP-65 load)", relay_urls->len);
    gnostr_pool_query_async(profile_pool, filters, NULL,
                            on_user_profile_fetched,
                            self); /* self already has a ref from caller */

    g_free(urls);
  } else {
    g_object_unref(self);
  }
  g_ptr_array_unref(relay_urls);
}

/* Reactive profile watch callback: fired when profile provider updates our user's profile.
 * This is dispatched on the main thread via g_idle_add by the profile provider. */
static void on_user_profile_watch(const char *pubkey_hex,
                                  const GnostrProfileMeta *meta,
                                  gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !meta) return;

  const char *final_name = (meta->display_name && *meta->display_name)
                           ? meta->display_name : meta->name;
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_user_profile(self->session_view,
                                          pubkey_hex,
                                          final_name,
                                          meta->picture);
  }

  update_login_ui_state(self);
}

/* Signal handler for when user successfully signs in via login dialog */
static void on_login_signed_in(GnostrLogin *login, const char *npub, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close the login dialog window immediately */
  GtkWidget *login_win = gtk_widget_get_ancestor(GTK_WIDGET(login), GTK_TYPE_WINDOW);
  if (login_win && GTK_IS_WINDOW(login_win) && login_win != GTK_WIDGET(self)) {
    gtk_window_close(GTK_WINDOW(login_win));
  }

  g_debug("[AUTH] User signed in: %s", npub ? npub : "(null)");

  /* Take ownership of the NIP-46 session from the login dialog */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
  }
  self->nip46_session = gnostr_login_take_nip46_session(login);

  /* Configure the unified signer service with the NIP-46 session (or NULL for NIP-55L) */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  gnostr_signer_service_set_nip46_session(signer, self->nip46_session);
  self->nip46_session = NULL; /* Signer service now owns the session */

  if (gnostr_signer_service_get_method(signer) == GNOSTR_SIGNER_METHOD_NIP46) {
    g_debug("[AUTH] Using NIP-46 remote signer");
  } else if (gnostr_signer_service_get_method(signer) == GNOSTR_SIGNER_METHOD_NIP55L) {
    g_debug("[AUTH] Using NIP-55L local signer");
  }

  /* nostrc-daj1: Update user_pubkey_hex from npub (or raw hex fallback) */
  if (npub && g_str_has_prefix(npub, "npub1")) {
    g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
    if (n19) {
      const char *hex = gnostr_nip19_get_pubkey(n19);
      if (hex) {
        g_free(self->user_pubkey_hex);
        self->user_pubkey_hex = g_strdup(hex);
        gnostr_signer_service_set_pubkey(signer, self->user_pubkey_hex);
      } else {
        g_warning("[AUTH] gnostr_nip19_get_pubkey returned NULL for npub: %.12s...", npub);
      }
    } else {
      g_warning("[AUTH] Failed to decode npub: %.12s...", npub);
    }
  } else if (npub && strlen(npub) == 64) {
    /* Fallback: login dialog may pass hex pubkey directly (e.g. NIP-55L) */
    g_free(self->user_pubkey_hex);
    self->user_pubkey_hex = g_strdup(npub);
    gnostr_signer_service_set_pubkey(signer, self->user_pubkey_hex);
    g_debug("[AUTH] Using raw hex pubkey from login: %.16s...", npub);
  } else if (npub) {
    g_warning("[AUTH] Unrecognized pubkey format from login (len=%zu): %.16s...",
              strlen(npub), npub);
  }

  /* Update UI to show signed-in state */
  update_login_ui_state(self);

  /* Add npub to known-accounts for multi-account support */
  if (npub && *npub) {
    g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
    if (settings) {
      char **accounts = g_settings_get_strv(settings, "known-accounts");
      gboolean found = FALSE;

      /* Check if npub is already in the list */
      if (accounts) {
        for (int i = 0; accounts[i]; i++) {
          if (g_strcmp0(accounts[i], npub) == 0) {
            found = TRUE;
            break;
          }
        }
      }

      /* Add npub if not found */
      if (!found) {
        guint len = accounts ? g_strv_length(accounts) : 0;
        char **new_accounts = g_new0(char*, len + 2);
        for (guint i = 0; i < len; i++) {
          new_accounts[i] = g_strdup(accounts[i]);
        }
        new_accounts[len] = g_strdup(npub);
        g_settings_set_strv(settings, "known-accounts", (const char * const *)new_accounts);
        g_strfreev(new_accounts);
        g_debug("[AUTH] Added npub to known-accounts list");
      }

      g_strfreev(accounts);
    }

    /* Refresh account list in session view */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_refresh_account_list(GNOSTR_SESSION_VIEW(self->session_view));
    }
  }

  /* nostrc-myp3: Tell profile pane the current user's pubkey so it can show
   * "Edit Profile" button when viewing own profile */
  if (self->user_pubkey_hex && self->session_view) {
    GtkWidget *pp = gnostr_session_view_get_profile_pane(self->session_view);
    if (pp && NOSTR_GTK_IS_PROFILE_PANE(pp)) {
      nostr_gtk_profile_pane_set_own_pubkey(NOSTR_GTK_PROFILE_PANE(pp), self->user_pubkey_hex);
    }
  }

  /* nostrc-51a.10: Start notification subscriptions */
  if (self->user_pubkey_hex) {
    GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
    gnostr_badge_manager_set_user_pubkey(badge_mgr, self->user_pubkey_hex);
    gnostr_badge_manager_set_event_callback(badge_mgr, on_notification_event, self, NULL);
    gnostr_badge_manager_start_subscriptions(badge_mgr);
    g_debug("[AUTH] Started notification subscriptions for user %.16s...", self->user_pubkey_hex);

    /* nostrc-27: Load historical notifications from NDB */
    GtkWidget *nw = gnostr_session_view_get_notifications_view(self->session_view);
    if (nw && GNOSTR_IS_NOTIFICATIONS_VIEW(nw)) {
      gnostr_notifications_view_set_loading(GNOSTR_NOTIFICATIONS_VIEW(nw), TRUE);
      gnostr_badge_manager_load_history(badge_mgr, GNOSTR_NOTIFICATIONS_VIEW(nw));
    }
  }

  /* Start gift wrap subscription for encrypted DMs */
  start_gift_wrap_subscription(self);

  /* Async initialization - dispatch immediately, pool handles load management.
   * These are all async functions that return immediately. No reason to delay. */
  if (self->user_pubkey_hex) {
    /* nostrc-wmuq: Register reactive profile watch for the user's pubkey.
     * When ANY code path updates the profile (timeline fetch, dedicated fetch,
     * subscription), the account menu will update automatically. */
    if (self->profile_watch_id) {
      gnostr_profile_provider_unwatch(self->profile_watch_id);
    }
    self->profile_watch_id = gnostr_profile_provider_watch(
      self->user_pubkey_hex, on_user_profile_watch, self);

    /* nostrc-profile-fix: NIP-65 load now chains to profile fetch.
     * This ensures we have user's relay list before fetching their profile. */
    gnostr_nip65_load_on_login_async(self->user_pubkey_hex,
                                      on_nip65_loaded_for_profile,
                                      g_object_ref(self));

    /* Blossom: fetch media server preferences for uploads */
    gnostr_blossom_settings_load_from_relays_async(self->user_pubkey_hex, NULL, NULL);

    /* NIP-51: sync mutes, follows, bookmarks from relays */
    gnostr_nip51_settings_auto_sync_on_login(self->user_pubkey_hex);

    /* Update sync bridge with user pubkey for follow list refresh */
    gnostr_sync_bridge_set_user_pubkey(self->user_pubkey_hex);

    /* hq-yrqwk: Pre-warm profile provider cache from NDB.
     * Loads user's own profile + follow list profiles into LRU so
     * the timeline renders with instant profile data. */
    gnostr_profile_provider_prewarm_async(self->user_pubkey_hex);

    /* hq-cnkj3: Start negentropy sync service if enabled */
    {
      g_autoptr(GSettings) client = g_settings_new("org.gnostr.Client");
      if (g_settings_get_boolean(client, "negentropy-auto-sync")) {
        GNostrSyncService *svc = gnostr_sync_service_get_default();
        if (svc) gnostr_sync_service_start(svc);
      }
    }
  }

  show_toast(self, "Signed in successfully");
}

/* Opens the login dialog */
static void open_login_dialog(GnostrMainWindow *self) {
  /* Create a window to host the login widget.
   * Use undecorated window since GnostrLogin has its own AdwHeaderBar. */
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);
  gtk_window_set_default_size(win, 400, 500);
  gtk_window_set_resizable(win, FALSE);
  gtk_window_set_decorated(win, FALSE);  /* GnostrLogin has AdwHeaderBar */

  /* Create and embed the login widget */
  GnostrLogin *login = gnostr_login_new();
  gtk_window_set_child(win, GTK_WIDGET(login));

  g_signal_connect(login, "signed-in", G_CALLBACK(on_login_signed_in), self);

  /* Close window when login is complete */
  g_signal_connect_swapped(login, "signed-in", G_CALLBACK(gtk_window_close), win);

  /* Also close window if user cancels */
  g_signal_connect_swapped(login, "cancelled", G_CALLBACK(gtk_window_close), win);

  gtk_window_present(win);
}

static void on_avatar_login_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Open login dialog */
  open_login_dialog(self);
}

/* Handler for signer service state changes - updates UI dynamically */
static void on_signer_state_changed(GnostrSignerService *signer,
                                     guint old_state,
                                     guint new_state,
                                     gpointer user_data)
{
  (void)signer;
  (void)old_state;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[MAIN] Signer state changed: %u -> %u", old_state, new_state);

  /* Update UI based on new state */
  gboolean is_connected = (new_state == GNOSTR_SIGNER_STATE_CONNECTED);

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    /* Only set authenticated to TRUE if we also have a valid npub */
    if (is_connected) {
      g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
      if (settings) {
        g_autofree char *npub = g_settings_get_string(settings, "current-npub");
        gnostr_session_view_set_authenticated(self->session_view, npub && *npub);
      }
    } else {
      gnostr_session_view_set_authenticated(self->session_view, FALSE);
    }
  }
}

static void on_avatar_logout_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Stop gift wrap subscription when user signs out */
  stop_gift_wrap_subscription(self);

  /* nostrc-51a.10: Stop notification subscriptions */
  GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
  gnostr_badge_manager_stop_subscriptions(badge_mgr);
  gnostr_badge_manager_set_event_callback(badge_mgr, NULL, NULL, NULL);

  /* Clear the current npub from settings */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (settings) {
    g_settings_set_string(settings, "current-npub", "");
  }

  /* Clear user pubkey */
  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;

  /* Clear NIP-46 session if any */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  /* Clear the signer service */
  gnostr_signer_service_clear(gnostr_signer_service_get_default());

  /* Update UI */
  update_login_ui_state(self);

  /* Clear gift wrap queue */
  if (self->gift_wrap_queue) {
    g_ptr_array_set_size(self->gift_wrap_queue, 0);
  }

  show_toast(self, "Signed out");
}

/* nostrc-myp3: Navigate to the current user's own profile.
 * Goes directly to the profile pane — bypasses gnostr_main_window_open_profile
 * to avoid silent failures from type checks on the widget parameter. */
static void on_view_profile_requested(GnostrSessionView *sv, gpointer user_data) {
  (void)sv;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Resolve user pubkey: prefer the instance variable, fall back to GSettings */
  const char *hex = self->user_pubkey_hex;
  g_autofree char *settings_hex = NULL;
  if (!hex || !*hex) {
    settings_hex = get_current_user_pubkey_hex();
    hex = settings_hex;
  }
  if (!hex || !*hex) {
    show_toast(self, _("Not signed in"));
    return;
  }

  /* Resolve profile pane widget directly */
  GtkWidget *pane_w = self->session_view
    ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (!pane_w || !NOSTR_GTK_IS_PROFILE_PANE(pane_w)) {
    show_toast(self, _("Profile unavailable"));
    return;
  }
  NostrGtkProfilePane *pane = NOSTR_GTK_PROFILE_PANE(pane_w);

  /* Toggle: if already showing our own profile in the sidebar, close it */
  gboolean sidebar_up = is_panel_visible(self)
    && gnostr_session_view_is_showing_profile(self->session_view);
  const char *cur = nostr_gtk_profile_pane_get_current_pubkey(pane);
  if (sidebar_up && cur && strcmp(cur, hex) == 0) {
    hide_panel(self);
    return;
  }

  /* Open sidebar and load (or reload) the profile */
  show_profile_panel(self);
  nostr_gtk_profile_pane_set_pubkey(pane, hex);
}

/* Handler for account switch request from session view */
static void on_account_switch_requested(GnostrSessionView *view, const char *npub, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !npub || !*npub) return;

  g_debug("[AUTH] Account switch requested to: %s", npub);

  /* For now, switching accounts requires re-authentication since we don't
   * store per-account credentials. We set the target npub and open login dialog. */

  /* Stop current subscriptions */
  stop_gift_wrap_subscription(self);
  GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
  gnostr_badge_manager_stop_subscriptions(badge_mgr);
  gnostr_badge_manager_set_event_callback(badge_mgr, NULL, NULL, NULL);

  /* Clear current session but keep the npub in settings as hint for login */
  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  gnostr_signer_service_clear(gnostr_signer_service_get_default());

  /* Set target npub - login dialog can use this as a hint */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (settings) {
    g_settings_set_string(settings, "current-npub", npub);
  }

  /* Update UI first */
  update_login_ui_state(self);

  /* Open login dialog for re-authentication */
  open_login_dialog(self);

  show_toast(self, "Please sign in to switch accounts");
}

/* Update the avatar popover UI based on sign-in state */
static void update_login_ui_state(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (!settings) return;

  char *npub = g_settings_get_string(settings, "current-npub");

  /* User is signed in only if both npub exists AND signer is ready */
  gboolean has_npub = (npub && *npub);
  gboolean signer_ready = gnostr_signer_service_is_ready(gnostr_signer_service_get_default());
  gboolean signed_in = has_npub && signer_ready;

  /* SessionView owns auth gating for Notifications/Messages and internal navigation.
   * MainWindow only computes sign-in state and informs SessionView. */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_authenticated(self->session_view, signed_in);
  }

  g_free(npub);
}

/* Profile pane signal handlers — shared path for clicking any user's profile
 * (from timeline note cards, DM conversations, etc.) */
static void on_note_card_open_profile(NostrGtkNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  GtkWidget *profile_pane = self->session_view
    ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (!profile_pane || !NOSTR_GTK_IS_PROFILE_PANE(profile_pane)) return;

  /* Toggle: if sidebar is already showing this exact profile, close it */
  gboolean sidebar_visible = is_panel_visible(self)
    && gnostr_session_view_is_showing_profile(self->session_view);
  const char *current = nostr_gtk_profile_pane_get_current_pubkey(NOSTR_GTK_PROFILE_PANE(profile_pane));
  if (sidebar_visible && current && strcmp(current, pubkey_hex) == 0) {
    hide_panel(self);
    return;
  }

  show_profile_panel(self);
  nostr_gtk_profile_pane_set_pubkey(NOSTR_GTK_PROFILE_PANE(profile_pane), pubkey_hex);
}

static void on_profile_pane_close_requested(NostrGtkProfilePane *pane, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  hide_panel(self);
}

/* nostrc-ch2v: Handle mute user from profile pane */
static void on_profile_pane_mute_user_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_warning("[MUTE] Invalid pubkey hex from profile pane");
    return;
  }

  g_debug("[MUTE] Mute user from profile pane for pubkey=%.16s...", pubkey_hex);

  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  if (self->event_model) {
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));
  }

  show_toast(self, "User muted");
}

/* nostrc-s0e0: Follow/unfollow context for async save */
typedef struct {
  GnostrMainWindow *self;
  NostrGtkProfilePane *pane;
  char *pubkey_hex;
  gboolean was_following;
} FollowContext;

static void on_follow_save_complete(GnostrContactList *cl, gboolean success,
                                     const char *error_msg, gpointer user_data) {
  FollowContext *ctx = (FollowContext *)user_data;
  if (!ctx) return;

  if (success) {
    show_toast(ctx->self, ctx->was_following ? "Unfollowed" : "Followed");
  } else {
    /* Revert optimistic update */
    if (ctx->was_following)
      gnostr_contact_list_add(cl, ctx->pubkey_hex, NULL);
    else
      gnostr_contact_list_remove(cl, ctx->pubkey_hex);

    if (NOSTR_GTK_IS_PROFILE_PANE(ctx->pane))
      nostr_gtk_profile_pane_set_following(ctx->pane, ctx->was_following);

    show_toast(ctx->self, error_msg ? error_msg : "Follow action failed");
  }

  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

/* nostrc-s0e0: Handle follow/unfollow from profile pane */
static void on_profile_pane_follow_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return;

  /* Ensure signer is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Please log in to follow users");
    return;
  }

  /* Load contact list if needed */
  GnostrContactList *cl = gnostr_contact_list_get_default();
  if (!gnostr_contact_list_get_user_pubkey(cl)) {
    const char *my_pk = gnostr_signer_service_get_pubkey(signer);
    if (my_pk)
      gnostr_contact_list_load_from_ndb(cl, my_pk);
  }

  /* Toggle follow state */
  gboolean was_following = gnostr_contact_list_is_following(cl, pubkey_hex);
  if (was_following)
    gnostr_contact_list_remove(cl, pubkey_hex);
  else
    gnostr_contact_list_add(cl, pubkey_hex, NULL);

  /* Optimistic UI update */
  nostr_gtk_profile_pane_set_following(pane, !was_following);

  /* Save to relays */
  FollowContext *ctx = g_new0(FollowContext, 1);
  ctx->self = self;
  ctx->pane = pane;
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->was_following = was_following;

  gnostr_contact_list_save_async(cl, on_follow_save_complete, ctx);
}

/* nostrc-qvba: Handle message from profile pane */
static void on_profile_pane_message_requested(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return;

  /* Navigate to DM conversation with this user */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_show_page(self->session_view, "messages");
    navigate_to_dm_conversation(self, pubkey_hex);
  }
}

/* Discover page signal handlers (nostrc-dr3) */
static void on_discover_open_profile(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data) {
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  on_note_card_open_profile(NULL, pubkey_hex, self);
}

static void on_discover_copy_npub(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data) {
  (void)page;
  (void)user_data;
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return;

  /* Encode as npub using GNostrNip19 */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(pubkey_hex, NULL);
  if (n19) {
    const char *npub = gnostr_nip19_get_bech32(n19);
    if (npub) {
      GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
      gdk_clipboard_set_text(clipboard, npub);
    }
  }
}

/* Search results view signal handlers (nostrc-29) */
static void on_search_open_note(GnostrSearchResultsView *view, const char *event_id_hex, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id_hex) return;
  gnostr_main_window_view_thread(GTK_WIDGET(self), event_id_hex);
}

static void on_search_open_profile(GnostrSearchResultsView *view, const char *pubkey_hex, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  on_note_card_open_profile(NULL, pubkey_hex, self);
}

/* Notification view signal handlers */
static void on_notification_open_note(GnostrNotificationsView *view, const char *note_id, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !note_id) return;
  gnostr_main_window_view_thread(GTK_WIDGET(self), note_id);
  /* Dismiss notification popover so thread panel is visible */
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_POPOVER);
  if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

static void on_notification_open_profile(GnostrNotificationsView *view, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_POPOVER);
  if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

/* Marketplace/Classifieds signal handlers (NIP-15/NIP-99) */
static void on_classifieds_open_profile(GnostrClassifiedsView *view, const char *pubkey_hex, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  on_note_card_open_profile(NULL, pubkey_hex, self);
}

static void on_classifieds_contact_seller(GnostrClassifiedsView *view, const char *pubkey_hex,
                                           const char *lud16, gpointer user_data) {
  (void)view;
  (void)lud16;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  /* Open DM conversation with seller by creating a conversation entry */
  GtkWidget *dm_inbox = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                          ? gnostr_session_view_get_dm_inbox(self->session_view)
                          : NULL;

  if (dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(dm_inbox)) {
    /* Create a minimal conversation for the seller */
    GnostrDmConversation conv = {0};
    conv.peer_pubkey = g_strdup(pubkey_hex);
    conv.display_name = g_strdup("Seller");
    conv.last_timestamp = g_get_real_time() / 1000000;
    gnostr_dm_inbox_view_upsert_conversation(GNOSTR_DM_INBOX_VIEW(dm_inbox), &conv);
    g_free(conv.peer_pubkey);
    g_free(conv.display_name);

    /* Switch to messages tab */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_show_page(self->session_view, "messages");
    }
  }
}

static void on_classifieds_listing_clicked(GnostrClassifiedsView *view, const char *event_id,
                                            const char *naddr, gpointer user_data) {
  (void)view;
  (void)naddr;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id) return;

  /* Show listing details in thread view */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    nostr_gtk_thread_view_set_focus_event(NOSTR_GTK_THREAD_VIEW(thread_view), event_id, NULL);
    show_thread_panel(self);
  }
}

/* ============== DM Conversation Signal Handlers ============== */

/* Helper: navigate to conversation view for a peer */
static void
navigate_to_dm_conversation(GnostrMainWindow *self, const char *peer_pubkey)
{
    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    GtkWidget *dm_conv = gnostr_session_view_get_dm_conversation(self->session_view);
    if (!dm_stack || !dm_conv || !GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv))
        return;

    GnostrDmConversationView *conv_view = GNOSTR_DM_CONVERSATION_VIEW(dm_conv);

    /* Fetch profile info for the peer */
    const char *display_name = NULL;
    const char *avatar_url = NULL;
    GnostrProfileMeta *meta = gnostr_profile_provider_get(peer_pubkey);
    if (meta) {
        display_name = meta->display_name;
        avatar_url = meta->picture;
    }

    /* Set peer on conversation view */
    gnostr_dm_conversation_view_set_peer(conv_view, peer_pubkey,
                                          display_name, avatar_url);

    /* Set user pubkey if available */
    if (self->user_pubkey_hex) {
        gnostr_dm_conversation_view_set_user_pubkey(conv_view, self->user_pubkey_hex);
    }

    /* Load message history */
    gnostr_dm_conversation_view_set_loading(conv_view, TRUE);

    GPtrArray *messages = gnostr_dm_service_get_messages(self->dm_service, peer_pubkey);
    if (messages && messages->len > 0) {
        gnostr_dm_conversation_view_set_messages(conv_view, messages);
        gnostr_dm_conversation_view_set_loading(conv_view, FALSE);
        gnostr_dm_conversation_view_scroll_to_bottom(conv_view);
    } else {
        /* Try async loading (Phase 4 will expand this) */
        gnostr_dm_conversation_view_clear(conv_view);
        gnostr_dm_conversation_view_set_loading(conv_view, FALSE);
    }

    if (meta) gnostr_profile_meta_free(meta);

    /* Mark conversation as read */
    gnostr_dm_service_mark_read(self->dm_service, peer_pubkey);

    /* Switch to conversation page */
    gtk_stack_set_visible_child_name(dm_stack, "conversation");
}

static void on_dm_inbox_open_conversation(GnostrDmInboxView *inbox,
                                           const char *peer_pubkey,
                                           gpointer user_data)
{
    (void)inbox;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !peer_pubkey) return;

    g_message("[DM] Opening conversation with %.8s", peer_pubkey);
    navigate_to_dm_conversation(self, peer_pubkey);
}

static void on_dm_inbox_compose(GnostrDmInboxView *inbox, gpointer user_data)
{
    (void)inbox;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

    /* For now, show a toast — compose dialog is a future feature */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
        gnostr_session_view_show_toast(self->session_view,
                                        "Compose DM: Enter an npub or pubkey to start a conversation");
    }
}

static void on_dm_conversation_go_back(GnostrDmConversationView *view,
                                        gpointer user_data)
{
    (void)view;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    if (dm_stack) {
        gtk_stack_set_visible_child_name(dm_stack, "inbox");
    }
}

/* Callback for send DM completion */
static void
on_dm_send_complete(GnostrDmSendResult *result, gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) {
        gnostr_dm_send_result_free(result);
        return;
    }

    if (!result->success) {
        g_warning("[DM] Send failed: %s", result->error_message ? result->error_message : "unknown");
        if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
            gnostr_session_view_show_toast(self->session_view, "Failed to send message");
        }
    } else {
        g_message("[DM] Message sent to %u relays", result->relays_published);
    }

    gnostr_dm_send_result_free(result);
}

static void on_dm_conversation_send_message(GnostrDmConversationView *view,
                                             const char *content,
                                             gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !content || !*content) return;

    const char *peer_pubkey = gnostr_dm_conversation_view_get_peer_pubkey(view);
    if (!peer_pubkey) return;

    g_message("[DM] Sending message to %.8s", peer_pubkey);

    /* Optimistically add message bubble */
    GnostrDmMessage msg = {
        .event_id = NULL,  /* pending */
        .content = (char *)content,
        .created_at = (gint64)(g_get_real_time() / 1000000),
        .is_outgoing = TRUE,
    };
    gnostr_dm_conversation_view_add_message(view, &msg);
    gnostr_dm_conversation_view_scroll_to_bottom(view);

    /* Send via NIP-17 gift wrap */
    gnostr_dm_service_send_dm_async(self->dm_service,
                                     peer_pubkey,
                                     content,
                                     NULL, /* cancellable */
                                     on_dm_send_complete,
                                     self);
}

static void on_dm_conversation_send_file(GnostrDmConversationView *view,
                                          const char *file_path,
                                          gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !file_path || !*file_path) return;

    const char *peer_pubkey = gnostr_dm_conversation_view_get_peer_pubkey(view);
    if (!peer_pubkey) return;

    g_message("[DM] Sending file to %.8s: %s", peer_pubkey, file_path);

    /* Optimistic "Sending file..." bubble */
    char *basename = g_path_get_basename(file_path);
    g_autofree char *preview = g_strdup_printf("Sending %s...", basename);
    GnostrDmMessage msg = {
        .event_id = NULL,
        .content = preview,
        .created_at = (gint64)(g_get_real_time() / 1000000),
        .is_outgoing = TRUE,
    };
    gnostr_dm_conversation_view_add_message(view, &msg);
    gnostr_dm_conversation_view_scroll_to_bottom(view);
    g_free(basename);

    /* Send via encrypt + upload + gift wrap */
    gnostr_dm_service_send_file_async(self->dm_service,
                                       peer_pubkey,
                                       file_path,
                                       NULL,
                                       on_dm_send_complete,
                                       self);
}

static void on_dm_conversation_open_profile(GnostrDmConversationView *view,
                                             const char *pubkey_hex,
                                             gpointer user_data)
{
    (void)view;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
    on_note_card_open_profile(NULL, pubkey_hex, self);
}

static void on_dm_service_message_received(GnostrDmService *service,
                                            const char *peer_pubkey,
                                            GnostrDmMessage *msg,
                                            gpointer user_data)
{
    (void)service;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !peer_pubkey || !msg) return;

    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    GtkWidget *dm_conv = gnostr_session_view_get_dm_conversation(self->session_view);
    if (!dm_stack || !dm_conv || !GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv))
        return;

    GnostrDmConversationView *conv_view = GNOSTR_DM_CONVERSATION_VIEW(dm_conv);

    /* Only add to conversation view if it's showing this peer */
    const char *current_peer = gnostr_dm_conversation_view_get_peer_pubkey(conv_view);
    const char *visible_child = gtk_stack_get_visible_child_name(dm_stack);
    if (current_peer && visible_child &&
        strcmp(current_peer, peer_pubkey) == 0 &&
        strcmp(visible_child, "conversation") == 0) {
        gnostr_dm_conversation_view_add_message(conv_view, msg);
        gnostr_dm_conversation_view_scroll_to_bottom(conv_view);
    }
}

/**
 * on_discover_open_communities - Handle communities button click from Discover page
 *
 * This shows a toast message for now. In a full implementation, this would
 * open a communities list view showing NIP-72 moderated communities.
 */
static void on_discover_open_communities(GnostrPageDiscover *page, gpointer user_data) {
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Show a toast indicating the feature is being accessed */
  gnostr_main_window_show_toast(GTK_WIDGET(self), "Communities (NIP-72) - Coming soon!");

  /* See nostrc-72lv: Implement community list view (NIP-72) */
  g_debug("[COMMUNITIES] Open communities list requested");
}

/**
 * on_discover_open_article - Handle article click from Discover page Articles mode
 *
 * Opens an article in the dedicated article reader panel (nostrc-zwn4).
 *
 * @param event_id: The event ID of the article
 * @param kind: The Nostr event kind (30023 for NIP-23 long-form, 30818 for NIP-54 wiki)
 */
static void on_discover_open_article(GnostrPageDiscover *page, const char *event_id, gint kind, gpointer user_data) {
  (void)page;
  (void)kind;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_id) return;

  GtkWidget *reader = self->session_view
      ? gnostr_session_view_get_article_reader(self->session_view) : NULL;
  if (reader && GNOSTR_IS_ARTICLE_READER(reader)) {
    gnostr_article_reader_load_event(GNOSTR_ARTICLE_READER(reader), event_id);
    show_article_panel(self);
  }

  g_debug("[ARTICLES] Open article in reader: kind=%d, id=%s", kind, event_id);
}

/**
 * on_discover_zap_article - Handle zap request for an article author from Discover page
 *
 * Opens the zap dialog for the article author.
 */
static void on_discover_zap_article(GnostrPageDiscover *page, const char *event_id,
                                     const char *pubkey_hex, const char *lud16, gpointer user_data) {
  (void)page;
  (void)event_id;  /* Could be used for zap receipts */
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  if (!lud16 || !*lud16) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Author has no Lightning address set");
    return;
  }

  /* See nostrc-z4pd: Implement zap dialog */
  gnostr_main_window_show_toast(GTK_WIDGET(self), "Zap dialog coming soon!");
  g_debug("[ARTICLES] Zap article author requested: pubkey=%s, lud16=%s", pubkey_hex, lud16);
}

/**
 * on_discover_search_hashtag - Navigate to a hashtag feed from Discover page
 *
 * Switches to the timeline and opens a hashtag tab for the selected tag.
 */
static void on_discover_search_hashtag(GnostrPageDiscover *page, const char *hashtag, gpointer user_data) {
  (void)page;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !hashtag || !*hashtag) return;

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && NOSTR_GTK_IS_TIMELINE_VIEW(timeline)) {
    nostr_gtk_timeline_view_add_hashtag_tab(NOSTR_GTK_TIMELINE_VIEW(timeline), hashtag);
    /* Switch to timeline page */
    if (self->session_view) {
      gnostr_session_view_show_page(self->session_view, "timeline");
    }
    g_debug("[DISCOVER] Navigated to hashtag #%s from trending", hashtag);
  }
}

static void on_stack_visible_child_changed(GObject *stack, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Get visible child - works with both GtkStack and AdwViewStack */
  GtkWidget *visible_child = NULL;
  if (ADW_IS_VIEW_STACK(stack)) {
    visible_child = adw_view_stack_get_visible_child(ADW_VIEW_STACK(stack));
  } else if (GTK_IS_STACK(stack)) {
    visible_child = gtk_stack_get_visible_child(GTK_STACK(stack));
  }

  /* When Discover page becomes visible, load profiles */
  GtkWidget *discover_page = self->session_view ? gnostr_session_view_get_discover_page(self->session_view) : NULL;
  if (visible_child == discover_page) {
    if (discover_page && GNOSTR_IS_PAGE_DISCOVER(discover_page)) {
      gnostr_page_discover_load_profiles(GNOSTR_PAGE_DISCOVER(discover_page));
    }
  }

  /* When Marketplace page becomes visible, fetch listings */
  GtkWidget *classifieds_view = self->session_view ? gnostr_session_view_get_classifieds_view(self->session_view) : NULL;
  if (visible_child == classifieds_view) {
    if (classifieds_view && GNOSTR_IS_CLASSIFIEDS_VIEW(classifieds_view)) {
      gnostr_classifieds_view_fetch_listings(GNOSTR_CLASSIFIEDS_VIEW(classifieds_view));
    }
  }
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

  if (keyval == GDK_KEY_Escape) {
    /* Close panel if it's open */
    if (is_panel_visible(self)) {
      if (!gnostr_session_view_is_showing_profile(self->session_view)) {
        g_debug("[UI] ESC pressed: closing thread view");
        GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
        if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
          nostr_gtk_thread_view_clear(NOSTR_GTK_THREAD_VIEW(thread_view));
        }
      } else {
        g_debug("[UI] ESC pressed: closing profile sidebar");
      }
      hide_panel(self);
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

/* Public wrapper for opening profile pane (called from timeline view) */
void gnostr_main_window_open_profile(GtkWidget *window, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  if (!hex) return;
  on_note_card_open_profile(NULL, hex, self);
}

/* Public wrapper for setting reply context (called from timeline view) */
void gnostr_main_window_request_reply(GtkWidget *window, const char *id_hex, const char *root_id, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[REPLY] Request reply to id=%s root=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            root_id ? root_id : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  /* Try to look up the author's display name for a nicer indicator */
  char *display_name = NULL;
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len, NULL) == 0 && meta_json) {
          /* Parse JSON to get display name */
          char *dn = NULL;
          dn = gnostr_json_get_string(meta_json, "display_name", NULL);
          if (!dn || !*dn) {
            g_free(dn);
            dn = gnostr_json_get_string(meta_json, "name", NULL);
          }
          if (dn && *dn) {
            display_name = dn;
          } else {
            g_free(dn);
          }
          /* Note: meta_json is owned by store, do not free */
        }
      }
      storage_ndb_end_query(txn);
    }
  }

  g_debug("[REPLY] Reply context: id=%s root=%s pubkey=%s display=%s",
          id_hex, root_id ? root_id : "(none)", pubkey_hex, display_name ? display_name : "@user");

  /* nostrc-c0mp: Create context and open compose dialog */
  ComposeContext *ctx = g_new0(ComposeContext, 1);
  ctx->type = COMPOSE_CONTEXT_REPLY;
  ctx->reply_to_id = g_strdup(id_hex);
  ctx->root_id = g_strdup(root_id ? root_id : id_hex); /* If no root, this is the root */
  ctx->reply_to_pubkey = g_strdup(pubkey_hex);
  ctx->display_name = display_name; /* Takes ownership */
  open_compose_dialog_with_context(self, ctx);
}

/* Public wrapper for requesting a quote post (kind 1 with q-tag) */
void gnostr_main_window_request_quote(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[QUOTE] Request quote of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for quote");
    return;
  }

  /* Convert event ID hex to note1 bech32 URI using GNostrNip19 */
  g_autoptr(GNostrNip19) n19_note = gnostr_nip19_encode_note(id_hex, NULL);
  if (!n19_note) {
    show_toast(self, "Failed to encode note ID");
    return;
  }

  /* Build nostr: URI */
  char *nostr_uri = g_strdup_printf("nostr:%s", gnostr_nip19_get_bech32(n19_note));

  /* Try to look up the author's display name */
  char *display_name = NULL;
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len, NULL) == 0 && meta_json) {
          char *dn = NULL;
          dn = gnostr_json_get_string(meta_json, "display_name", NULL);
          if (!dn || !*dn) {
            g_free(dn);
            dn = gnostr_json_get_string(meta_json, "name", NULL);
          }
          if (dn && *dn) {
            display_name = dn;
          } else {
            g_free(dn);
          }
        }
      }
      storage_ndb_end_query(txn);
    }
  }

  g_debug("[QUOTE] Quote context: id=%s pubkey=%s uri=%s display=%s",
          id_hex, pubkey_hex, nostr_uri, display_name ? display_name : "@user");

  /* nostrc-c0mp: Create context and open compose dialog */
  ComposeContext *ctx = g_new0(ComposeContext, 1);
  ctx->type = COMPOSE_CONTEXT_QUOTE;
  ctx->quote_id = g_strdup(id_hex);
  ctx->quote_pubkey = g_strdup(pubkey_hex);
  ctx->nostr_uri = nostr_uri; /* Takes ownership */
  ctx->display_name = display_name; /* Takes ownership */
  open_compose_dialog_with_context(self, ctx);
}

/* NIP-22: Public wrapper for requesting a comment (kind 1111) on any event */
void gnostr_main_window_request_comment(GtkWidget *window, const char *id_hex, int kind, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[COMMENT] Request comment on id=%s kind=%d pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            kind,
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for comment");
    return;
  }

  /* Try to look up the author's display name */
  char *display_name = NULL;
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len, NULL) == 0 && meta_json) {
          char *dn = NULL;
          dn = gnostr_json_get_string(meta_json, "display_name", NULL);
          if (!dn || !*dn) {
            g_free(dn);
            dn = gnostr_json_get_string(meta_json, "name", NULL);
          }
          if (dn && *dn) {
            display_name = dn;
          } else {
            g_free(dn);
          }
        }
      }
      storage_ndb_end_query(txn);
    }
  }

  g_debug("[COMMENT] Comment context: id=%s kind=%d pubkey=%s display=%s",
          id_hex, kind, pubkey_hex, display_name ? display_name : "@user");

  /* nostrc-c0mp: Create context and open compose dialog */
  ComposeContext *ctx = g_new0(ComposeContext, 1);
  ctx->type = COMPOSE_CONTEXT_COMMENT;
  ctx->comment_root_id = g_strdup(id_hex);
  ctx->comment_root_kind = kind;
  ctx->comment_root_pubkey = g_strdup(pubkey_hex);
  ctx->display_name = display_name; /* Takes ownership */
  open_compose_dialog_with_context(self, ctx);
}

static void on_note_card_repost_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  gnostr_main_window_request_repost(GTK_WIDGET(self), id_hex, pubkey_hex);
}

/* Signal handler for quote-requested from note card */
static void on_note_card_quote_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  gnostr_main_window_request_quote(GTK_WIDGET(self), id_hex, pubkey_hex);
}

/* Signal handler for like-requested from note card (NIP-25) */
static void on_note_card_like_requested(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gnostr_main_window_request_like(GTK_WIDGET(self), id_hex, pubkey_hex, event_kind, reaction_content, row);
}

/* Signal handler for comment-requested from note card (NIP-22) */
static void on_note_card_comment_requested(NostrGtkNoteCardRow *row, const char *id_hex, int kind, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gnostr_main_window_request_comment(GTK_WIDGET(self), id_hex, kind, pubkey_hex);
}

/* Forward declaration for thread view close handler */
static void on_thread_view_close_requested(NostrGtkThreadView *view, gpointer user_data);
static void on_thread_view_open_profile(NostrGtkThreadView *view, const char *pubkey_hex, gpointer user_data);

/* Public wrapper for viewing a thread (called from timeline view) */
void gnostr_main_window_view_thread(GtkWidget *window, const char *root_event_id) {
  gnostr_main_window_view_thread_with_json(window, root_event_id, NULL);
}

/* nostrc-a2zd: View thread with optional event JSON to avoid nostrdb race condition */
void gnostr_main_window_view_thread_with_json(GtkWidget *window, const char *root_event_id, const char *event_json) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!root_event_id || strlen(root_event_id) != 64) {
    g_warning("[THREAD] Invalid root event ID for thread view");
    return;
  }

  g_debug("[THREAD] View thread requested for root=%s (json=%s)",
          root_event_id, event_json ? "provided" : "NULL");

  /* Show thread view panel */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (!thread_view || !NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    g_warning("[THREAD] Thread view widget not available");
    show_toast(self, "Thread view not available");
    return;
  }

  /* Set the thread root and load the thread.
   * If event_json is provided, the event is pre-populated to avoid nostrdb lookup. */
  nostr_gtk_thread_view_set_thread_root_with_json(NOSTR_GTK_THREAD_VIEW(thread_view), root_event_id, event_json);

  /* Show the thread panel */
  show_thread_panel(self);
}

/* Handler for thread view close button */
static void on_thread_view_close_requested(NostrGtkThreadView *view, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Hide thread panel */
  hide_panel(self);

  /* Clear thread view to free resources */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    nostr_gtk_thread_view_clear(NOSTR_GTK_THREAD_VIEW(thread_view));
  }
}

/* ---- Article reader signal handlers (nostrc-zwn4) ---- */

static void on_article_reader_close_requested(GnostrArticleReader *reader, gpointer user_data) {
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  hide_panel(self);
}

static void on_article_reader_open_profile(GnostrArticleReader *reader, const char *pubkey_hex, gpointer user_data) {
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  hide_panel(self);
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

static void on_article_reader_open_url(GnostrArticleReader *reader, const char *url, gpointer user_data) {
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !url) return;
  GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
  gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
  g_object_unref(launcher);
}

static void on_article_reader_share(GnostrArticleReader *reader, const char *naddr_uri, gpointer user_data) {
  (void)reader;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !naddr_uri) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, naddr_uri);
  gnostr_main_window_show_toast(GTK_WIDGET(self), "Article link copied to clipboard");
}

static void on_article_reader_zap(GnostrArticleReader *reader,
                                   const char *event_id, const char *pubkey_hex,
                                   const char *lud16, gpointer user_data) {
  (void)reader;
  (void)event_id;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  if (!lud16 || !*lud16) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Author has no Lightning address set");
    return;
  }

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Zap dialog coming soon!");
  g_debug("[ARTICLE-READER] Zap requested: pubkey=%s", pubkey_hex);
}

/* Handler for open profile from thread view */
static void on_thread_view_open_profile(NostrGtkThreadView *view, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close thread view first */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    on_thread_view_close_requested(NOSTR_GTK_THREAD_VIEW(thread_view), self);
  }

  /* Open profile pane */
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

/* Handler for need-profile signal from thread view - fetches profile from relays */
static void on_thread_view_need_profile(NostrGtkThreadView *view, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  if (strlen(pubkey_hex) != 64) return;

  enqueue_profile_author(self, pubkey_hex);
}

/* ============================================================================
 * Repository Browser signal handlers
 * ============================================================================ */

/* Handler for repo-selected signal from repo browser */
static void on_repo_selected(GnostrRepoBrowser *browser, const char *repo_id, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)browser;

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !repo_id) return;

  g_debug("[REPO] Repository selected: %s", repo_id);

  /* For now, just log the selection. In the future:
   * - Open repo details view
   * - Show patches, issues, activity
   * - Enable clone/pull operations with libgit2 */
  if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
    g_autofree char *msg = g_strdup_printf("Selected repository: %.16s...", repo_id);
    adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new(msg));
  }
}

/* Handler for clone-requested signal from repo browser */
static void on_clone_requested(GnostrRepoBrowser *browser, const char *clone_url, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)browser;

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !clone_url) return;

  g_debug("[REPO] Clone requested: %s", clone_url);

  /* Dispatch to nip34-git plugin's git client */
  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  GVariant *param = g_variant_new_string(clone_url);

  if (gnostr_plugin_manager_dispatch_action(manager, "nip34-git",
                                             "open-git-client", param)) {
    g_debug("[REPO] Dispatched to nip34-git plugin");
  } else {
    /* Fallback: copy URL to clipboard if plugin not available */
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(clipboard, clone_url);

    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay,
                                   adw_toast_new("Clone URL copied to clipboard"));
    }
  }
}

/* Handler for need-profile signal from repo browser - fetches maintainer profiles */
static void on_repo_browser_need_profile(GnostrRepoBrowser *browser, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)browser;

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  if (strlen(pubkey_hex) != 64) return;

  g_debug("[REPO] Profile fetch requested for maintainer: %.16s...", pubkey_hex);
  enqueue_profile_author(self, pubkey_hex);
}

/* Handler for open-profile signal from repo browser - opens profile viewer panel */
static void on_repo_browser_open_profile(GnostrRepoBrowser *browser, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)browser;

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  if (strlen(pubkey_hex) != 64) return;

  g_debug("[REPO] Open profile requested for maintainer: %.16s...", pubkey_hex);
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

/* Handler for refresh-requested signal from repo browser */
static void on_repo_refresh_requested(GnostrRepoBrowser *browser, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)browser;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[REPO] Refresh requested - dispatching to NIP-34 plugin");

  /* Dispatch refresh action to NIP-34 plugin to fetch from relays */
  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  if (gnostr_plugin_manager_dispatch_action(manager, "nip34-git",
                                             "nip34-refresh", NULL)) {
    g_debug("[REPO] Dispatched nip34-refresh action to plugin");

    /* Show feedback */
    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new("Fetching repositories from relays..."));
    }
  } else {
    g_warning("[REPO] Failed to dispatch refresh - NIP-34 plugin not available");

    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new("NIP-34 plugin not available"));
    }
  }
}

/* Public: Mute a user (adds to mute list and refreshes timeline) */
void gnostr_main_window_mute_user(GtkWidget *window, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_warning("[MUTE] Invalid pubkey hex for mute user");
    return;
  }

  g_debug("[MUTE] Mute user requested for pubkey=%.16s...", pubkey_hex);

  /* Add to mute list */
  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  /* Refresh the timeline to filter out the muted user */
  if (self->event_model) {
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));
  }

  show_toast(self, "User muted");
}

/* Public: Mute a thread (adds root event to mute list and refreshes timeline) */
void gnostr_main_window_mute_thread(GtkWidget *window, const char *event_id_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!event_id_hex || strlen(event_id_hex) != 64) {
    g_warning("[MUTE] Invalid event ID hex for mute thread");
    return;
  }

  g_debug("[MUTE] Mute thread requested for event=%.16s...", event_id_hex);

  /* Add to mute list (events) */
  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_event(mute_list, event_id_hex, FALSE);

  /* Refresh the timeline to filter out the muted thread */
  if (self->event_model) {
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));
  }

  show_toast(self, "Thread muted");
}

/* Public: Show a toast message in the main window */
void gnostr_main_window_show_toast(GtkWidget *window, const char *message) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  show_toast(self, message);
}

/* Public wrappers so other UI components (e.g., timeline view) can request prefetch */
void gnostr_main_window_enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex) {
  enqueue_profile_author(self, pubkey_hex);
}

void gnostr_main_window_enqueue_profile_authors(GnostrMainWindow *self, const char **pubkey_hexes, size_t count) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hexes || count == 0) return;
  for (size_t i = 0; i < count; i++) {
    const char *pk = pubkey_hexes[i];
    if (pk && strlen(pk) == 64) enqueue_profile_author(self, pk);
  }
}

static gboolean profile_fetch_fire_idle(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;
  /* Removed noisy debug */
  self->profile_fetch_source_id = 0;
  
  /* Don't fetch profiles if pool isn't initialized with relays yet */
  if (!self->pool) {
    g_debug("[PROFILE] Pool not initialized, skipping fetch");
    /* Clear the queue since we can't process it */
    if (self->profile_fetch_queue) {
      g_ptr_array_free(self->profile_fetch_queue, TRUE);
      self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
    }
    return G_SOURCE_REMOVE;
  }
  
  if (!self->profile_fetch_queue || self->profile_fetch_queue->len == 0) {
    return G_SOURCE_REMOVE;
  }
  /* Snapshot and clear queue */
  GPtrArray *authors = self->profile_fetch_queue;
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  
  /* OPTIMIZATION: Check DB first and apply cached profiles immediately */
  void *txn = NULL;
  guint cached_applied = 0;
  if (storage_ndb_begin_query(&txn, NULL) == 0) {
    for (guint i = 0; i < authors->len; i++) {
      const char *pkhex = (const char*)g_ptr_array_index(authors, i);
      if (!pkhex || strlen(pkhex) != 64) continue;
      
      uint8_t pk32[32];
      if (!hex_to_bytes32(pkhex, pk32)) continue;
      
      char *pjson = NULL;
      int plen = 0;
      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen, NULL) == 0 && pjson && plen > 0) {
        /* Found in DB - apply immediately for fast UI update */
        gchar *content_str = gnostr_json_get_string(pjson, "content", NULL);
        if (content_str) {
          update_meta_from_profile_json(self, pkhex, content_str);
          cached_applied++;
          g_free(content_str);
        }
        free(pjson);
      }
    }
    storage_ndb_end_query(txn);
    /* nostrc-sk8o: Refresh thread view ONCE after batch of cached profiles */
    if (cached_applied > 0) {
      refresh_thread_view_profiles_if_visible(self);
    }
  }

  (void)cached_applied; /* Used for DB cache optimization */

  /* hq-xxnm5: Filter out profiles that were recently fetched from relays.
   * Only re-fetch profiles that are stale (older than STORAGE_NDB_PROFILE_STALE_SECS).
   * This dramatically reduces redundant relay traffic on every scroll. */
  {
    GPtrArray *stale_authors = g_ptr_array_new_with_free_func(g_free);
    guint skipped = 0;
    for (guint i = 0; i < authors->len; i++) {
      const char *pkhex = (const char*)g_ptr_array_index(authors, i);
      if (!pkhex) continue;
      if (storage_ndb_is_profile_stale(pkhex, 0)) {
        g_ptr_array_add(stale_authors, g_strdup(pkhex));
      } else {
        skipped++;
      }
    }
    if (skipped > 0) {
      g_debug("[PROFILE] hq-xxnm5: Skipped %u recently-fetched profiles, %u stale to fetch",
              skipped, stale_authors->len);
    }
    g_ptr_array_free(authors, TRUE);
    authors = stale_authors;
    if (authors->len == 0) {
      g_ptr_array_free(authors, TRUE);
      return G_SOURCE_REMOVE;
    }
  }

  /* Build relay URLs */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *dummy = NULL;
  build_urls_and_filters(self, &urls, &url_count, &dummy, 0 /* unused for profiles */);
  if (!urls || url_count == 0) {
    g_warning("[PROFILE] No relays configured, using %u cached profiles only", cached_applied);
    g_ptr_array_free(authors, TRUE);
    if (dummy) nostr_filters_free(dummy);
    if (urls) free_urls_owned(urls, url_count);
    return G_SOURCE_REMOVE;
  }
  
  /* Build batch list; dispatch sequentially (EOSE-gated) */
  const guint total = authors->len;
  const guint batch_sz = 100;
  const guint n_batches = (total + batch_sz - 1) / batch_sz;
  (void)n_batches; /* For batch organization only */
  
  /* Check for existing batch state */
  if (self->profile_batches) {
    if (self->profile_fetch_active > 0) {
      /* Active fetches in progress - append new authors to existing batch sequence
       * instead of re-queuing them. This allows continuous profile loading during scrolling. */
      g_debug("[PROFILE] Fetch in progress (active=%u), appending %u authors to batch sequence",
              self->profile_fetch_active, authors->len);

      /* Create new batches from the new authors and append to existing sequence */
      for (guint off = 0; off < authors->len; off += batch_sz) {
        guint n = (off + batch_sz <= authors->len) ? batch_sz : (authors->len - off);
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        for (guint j = 0; j < n; j++) {
          char *s = (char*)g_ptr_array_index(authors, off + j);
          g_ptr_array_index(authors, off + j) = NULL; /* transfer ownership */
          g_ptr_array_add(b, s);
        }
        g_ptr_array_add(self->profile_batches, b);
      }
      g_debug("[PROFILE] Batch sequence now has %u batches total", self->profile_batches->len);

      g_ptr_array_free(authors, TRUE);
      if (dummy) nostr_filters_free(dummy);
      if (urls) free_urls_owned(urls, url_count);
      return G_SOURCE_REMOVE;
    } else {
      /* No active fetches but profile_batches still exists - truly stale state */
      g_warning("[PROFILE] ⚠️ STALE BATCH DETECTED - profile_batches is non-NULL but no fetch running!");
      g_warning("[PROFILE] This indicates a previous fetch never completed. Clearing stale state.");

      /* Clean up stale batch state */
      for (guint i = 0; i < self->profile_batches->len; i++) {
        GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
        if (b) g_ptr_array_free(b, TRUE);
      }
      g_ptr_array_free(self->profile_batches, TRUE);
      self->profile_batches = NULL;

      if (self->profile_batch_urls) {
        free_urls_owned(self->profile_batch_urls, self->profile_batch_url_count);
        self->profile_batch_urls = NULL;
        self->profile_batch_url_count = 0;
      }
      self->profile_batch_pos = 0;
      /* Fall through to create new batch sequence */
    }
  }
  /* Do not set a free-func: we'll free each batch when its callback completes,
     and clean up any remaining (if canceled) at sequence end. */
  self->profile_batches = g_ptr_array_new();
  self->profile_batch_pos = 0;
  /* Capture relay URLs for the whole sequence (free array pointer + strings at end only) */
  self->profile_batch_urls = urls; /* take ownership of array pointer + strings */
  self->profile_batch_url_count = url_count;
  /* Partition authors into batches; transfer ownership into batch arrays */
  for (guint off = 0; off < total; off += batch_sz) {
    guint n = (off + batch_sz <= total) ? batch_sz : (total - off);
    GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
    for (guint j = 0; j < n; j++) {
      char *s = (char*)g_ptr_array_index(authors, off + j);
      /* transfer: clear original slot to avoid double free */
      g_ptr_array_index(authors, off + j) = NULL;
      g_ptr_array_add(b, s);
    }
    g_ptr_array_add(self->profile_batches, b);
  }
  /* Original authors container can now be freed (most slots NULL) */
  if (authors) g_ptr_array_free(authors, TRUE);
  /* Free filters from build_urls_and_filters */
  if (dummy) nostr_filters_free(dummy);
  /* Kick off the first batch */
  profile_dispatch_next(g_object_ref(self));
  return G_SOURCE_REMOVE;
}

typedef struct ProfileBatchCtx {
  GnostrMainWindow *self;      /* strong ref */
  GPtrArray        *batch;     /* owned; char* */
} ProfileBatchCtx;

static void on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GNostrPool *pool = GNOSTR_POOL(source); (void)pool;
  ProfileBatchCtx *ctx = (ProfileBatchCtx*)user_data;
  
  if (!ctx) {
    g_critical("profile_fetch: callback ctx is NULL!");
    return;
  }
  
  GError *error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);
  if (error) {
    g_warning("profile_fetch: error - %s", error->message);
    g_clear_error(&error);
  }
  /* Update cache/UI from returned events */
  if (jsons) {
    guint deserialized = 0, dispatched = 0;
    GPtrArray *items = g_ptr_array_new_with_free_func(profile_apply_item_free);
    
    /* NOTE: We used to accumulate an LDJSON buffer for batch ingestion here, but that
     * duplicates memory for all events and can spike process RSS by hundreds of MB.
     * Since we ingest one-by-one below, remove the LDJSON accumulation to reduce peak memory. */
    GHashTable *unique_pks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char*)g_ptr_array_index(jsons, i);
      if (evt_json) {
        /* Track unique pubkeys */
        g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
        if (evt) {
          const char *pk = gnostr_event_get_pubkey(evt);
          if (pk) g_hash_table_add(unique_pks, g_strdup(pk));
        }
      }
    }
    guint unique_count = g_hash_table_size(unique_pks);
    g_hash_table_unref(unique_pks);
    g_debug("[PROFILE] Batch received %u events (%u unique authors)", jsons->len, unique_count);
    
    /* nostrc-mzab: Queue events for background ingestion instead of blocking
     * the main thread. Uses the same ingest_queue as the live subscription. */
    guint queued = 0;
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char*)g_ptr_array_index(jsons, i);
      if (!evt_json) continue;

      /* CRITICAL FIX: nostrdb requires "tags" field even if empty.
       * Many relays omit it. Add it if missing. */
      if (!strstr(evt_json, "\"tags\"")) {
        const char *kind_pos = strstr(evt_json, "\"kind\"");
        if (kind_pos) {
          const char *comma_after_kind = strchr(kind_pos, ',');
          if (comma_after_kind) {
            GString *fixed = g_string_new("");
            g_string_append_len(fixed, evt_json, comma_after_kind - evt_json + 1);
            g_string_append(fixed, "\"tags\":[],");
            g_string_append(fixed, comma_after_kind + 1);
            gchar *fixed_str = g_string_free(fixed, FALSE);
            if (ingest_queue_push(ctx->self, fixed_str))
              queued++;
            else
              g_free(fixed_str);
            continue;
          }
        }
      }
      {
        gchar *dup = g_strdup(evt_json);
        if (ingest_queue_push(ctx->self, dup))
          queued++;
        else
          g_free(dup);
      }
    }
    if (queued > 0) {
      g_debug("[PROFILE] Queued %u events for background ingestion", queued);
    }
    /* No LDJSON buffer to free: removed to avoid memory spikes */
    /* Now parse events for UI application */
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char*)g_ptr_array_index(jsons, i);
      if (!evt_json) continue;
      g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *pk_hex = gnostr_event_get_pubkey(evt);
        const char *content = gnostr_event_get_content(evt);
        if (pk_hex && content) {
          /* Collect for bulk dispatch */
          ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
          pctx->pubkey_hex = g_strdup(pk_hex);
          pctx->content_json = g_strdup(content);
          g_ptr_array_add(items, pctx);
          dispatched++;

          /* hq-xxnm5: Record fetch timestamp so we can skip re-fetching
           * this profile until it becomes stale. */
          uint8_t pk32[32];
          if (strlen(pk_hex) == 64 && hex_to_bytes32(pk_hex, pk32)) {
            uint64_t now = (uint64_t)(g_get_real_time() / G_USEC_PER_SEC);
            storage_ndb_write_last_profile_fetch(pk32, now);
          }
        }
        deserialized++;
      } else {
        /* Surface parse problem with a short snippet (first 120 chars) */
        size_t len = strlen(evt_json);
        char snippet[121];
        size_t copy = len < 120 ? len : 120;
        memcpy(snippet, evt_json, copy);
        snippet[copy] = '\0';
        g_warning("profile_fetch: deserialize failed at index %u len=%zu json='%s'%s",
                 i, len, snippet, len > 120 ? "…" : "");
      }
    }
    g_debug("[PROFILE] ✓ Batch complete: %u profiles applied", dispatched);
    if (items->len > 0 && GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
      schedule_apply_profiles(ctx->self, items); /* transfers ownership */
      items = NULL;
      /* Note: We don't trigger a sweep here because nostrdb's async ingestion
       * takes too long. Profiles are applied immediately above via schedule_apply_profiles.
       * Sweeps are triggered periodically by other events (new notes, etc.) */
    }
    if (items) g_ptr_array_free(items, TRUE);
    g_ptr_array_free(jsons, TRUE);
  }
  else {
    g_debug("[PROFILE] Batch returned no results");
  }
  /* Done with this batch's author list */
  if (ctx && ctx->batch) g_ptr_array_free(ctx->batch, TRUE);
  /* Advance to next batch immediately - goroutines handle cleanup internally */
  if (ctx && GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    GnostrMainWindow *self = ctx->self;
    
    /* Decrement active fetch counter */
    if (self->profile_fetch_active > 0) {
      self->profile_fetch_active--;
    }
    
    /* NOTE: With goroutine-based fetching, we don't need artificial delays!
     * Goroutines complete in ~3-5 seconds and clean up properly.
     * The old delay system (5+ seconds per batch) was for the broken GLib thread implementation.
     * Dispatch next batch immediately for faster profile loading. */
    g_debug("[PROFILE] Batch %u/%u complete (active=%u/%u), dispatching next",
            self->profile_batch_pos, 
            self->profile_batches ? self->profile_batches->len : 0,
            self->profile_fetch_active, self->profile_fetch_max_concurrent);
    /* nostrc-pri1: DEFAULT_IDLE so profile batches don't compete with input */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    profile_dispatch_next, g_object_ref(self),
                    g_object_unref);
    /* NOTE: Don't unref self here - ctx->self holds the reference and will be freed below */
  } else {
    g_warning("profile_fetch: cannot dispatch next batch - invalid context");
  }
  /* Free ctx - this will unref ctx->self */
  if (ctx && ctx->self) g_object_unref(ctx->self);
  g_free(ctx);
}


/* Worker thread: query all kind-0 profiles from nostrdb.
 * This avoids blocking the GTK main thread during startup. */
static void
prepopulate_profiles_thread(GTask        *task,
                            gpointer      source_object,
                            gpointer      task_data G_GNUC_UNUSED,
                            GCancellable *cancellable G_GNUC_UNUSED)
{
  void *txn = NULL; char **arr = NULL; int n = 0;
  int brc = storage_ndb_begin_query(&txn, NULL);
  if (brc != 0) {
    g_warning("prepopulate_profiles_thread: begin_query failed rc=%d", brc);
    g_task_return_pointer(task, NULL, NULL);
    return;
  }
  const char *filters = "[{\"kinds\":[0]}]";
  int rc = storage_ndb_query(txn, filters, &arr, &n, NULL);
  g_debug("prepopulate_profiles_thread: query rc=%d count=%d", rc, n);

  GPtrArray *items = NULL;
  if (rc == 0 && arr && n > 0) {
    items = g_ptr_array_new_with_free_func(profile_apply_item_free);
    for (int i = 0; i < n; i++) {
      const char *evt_json = arr[i];
      if (!evt_json) continue;
      g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        if (gnostr_event_get_kind(evt) == 0) {
          const char *pk_hex = gnostr_event_get_pubkey(evt);
          const char *content = gnostr_event_get_content(evt);
          if (pk_hex && content) {
            ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
            pctx->pubkey_hex = g_strdup(pk_hex);
            pctx->content_json = g_strdup(content);
            g_ptr_array_add(items, pctx);
          }
        }
      }
    }
    if (items->len == 0) {
      g_ptr_array_free(items, TRUE);
      items = NULL;
    }
  }
  storage_ndb_free_results(arr, n);
  storage_ndb_end_query(txn);

  g_task_return_pointer(task, items, NULL);
}

/* Main-thread callback: apply profiles loaded in worker thread. */
static void
on_prepopulate_profiles_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(source);
  GPtrArray *items = g_task_propagate_pointer(G_TASK(result), NULL);
  if (items && items->len > 0 && GNOSTR_IS_MAIN_WINDOW(self)) {
    g_debug("prepopulate_all_profiles_from_cache: scheduling %u cached profiles", items->len);
    schedule_apply_profiles(self, items); /* transfers ownership */
  } else if (items) {
    g_ptr_array_free(items, TRUE);
  }
}

static void prepopulate_all_profiles_from_cache(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  GTask *task = g_task_new(self, NULL, on_prepopulate_profiles_done, NULL);
  g_task_run_in_thread(task, prepopulate_profiles_thread);
  g_object_unref(task);
}

/* ---- Cached timeline prepopulation (kind-1 text notes) -------------------- */
static char *format_timestamp_approx(gint64 created_at) {
  if (created_at <= 0) return g_strdup("now");
  time_t now = time(NULL);
  long diff = (long)(now - (time_t)created_at);
  if (diff < 0) diff = 0;
  if (diff < 5) return g_strdup("now");
  const char *unit = "s"; long val = diff;
  if (diff >= 60) { val = diff / 60; unit = "m"; }
  if (diff >= 3600) { val = diff / 3600; unit = "h"; }
  if (diff >= 86400) { val = diff / 86400; unit = "d"; }
  return g_strdup_printf("%ld%s", val, unit);
}

/* Legacy thread_roots system removed - GnNostrEventModel is now the sole data source.
 * Profile prefetch from cached notes is handled by the model's need-profile signal. */

static void prepopulate_text_notes_from_cache(GnostrMainWindow *self, guint limit) {
  (void)limit; /* No longer used - model handles its own data loading */
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  
  /* The GnNostrEventModel now handles loading from nostrdb directly.
   * This function is kept as a stub for compatibility but does nothing. */
  g_debug("prepopulate_text_notes_from_cache: skipped (model handles data loading)");
}

static void gnostr_main_window_init(GnostrMainWindow *self) {
  self->compact = FALSE;
  gtk_widget_init_template(GTK_WIDGET(self));

  if (self->session_view && self->toast_overlay)
    gnostr_session_view_set_toast_overlay(self->session_view, self->toast_overlay);

  if (self->session_view)
    g_object_bind_property(self, "compact", self->session_view, "compact",
                           G_BINDING_SYNC_CREATE);

  if (self->session_view) {
    g_signal_connect(self->session_view, "settings-requested",
                     G_CALLBACK(on_settings_clicked), self);
    g_signal_connect(self->session_view, "relays-requested",
                     G_CALLBACK(on_relays_clicked), self);
    g_signal_connect(self->session_view, "reconnect-requested",
                     G_CALLBACK(on_reconnect_requested), self);
    g_signal_connect(self->session_view, "login-requested",
                     G_CALLBACK(on_avatar_login_clicked), self);
    g_signal_connect(self->session_view, "logout-requested",
                     G_CALLBACK(on_avatar_logout_clicked), self);
    g_signal_connect(self->session_view, "view-profile-requested",
                     G_CALLBACK(on_view_profile_requested), self);
    g_signal_connect(self->session_view, "account-switch-requested",
                     G_CALLBACK(on_account_switch_requested), self);
    g_signal_connect(self->session_view, "new-notes-clicked",
                     G_CALLBACK(on_new_notes_clicked), self);
    g_signal_connect(self->session_view, "compose-requested",
                     G_CALLBACK(on_compose_requested), self);
  }

  /* Connect to signer service state-changed signal for dynamic UI updates */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  g_signal_connect(signer, "state-changed",
                   G_CALLBACK(on_signer_state_changed), self);

  /* Connect profile pane and thread view close signals */
  {
    GtkWidget *profile_pane = self->session_view ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
    if (profile_pane && NOSTR_GTK_IS_PROFILE_PANE(profile_pane)) {
      g_signal_connect(profile_pane, "close-requested",
                       G_CALLBACK(on_profile_pane_close_requested), self);
      /* nostrc-ch2v: Handle mute from profile pane */
      g_signal_connect(profile_pane, "mute-user-requested",
                       G_CALLBACK(on_profile_pane_mute_user_requested), self);
      /* nostrc-qvba: Handle follow and message from profile pane */
      g_signal_connect(profile_pane, "follow-requested",
                       G_CALLBACK(on_profile_pane_follow_requested), self);
      g_signal_connect(profile_pane, "message-requested",
                       G_CALLBACK(on_profile_pane_message_requested), self);
    }
    
    GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
    if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
      g_signal_connect(thread_view, "close-requested",
                       G_CALLBACK(on_thread_view_close_requested), self);
      /* nostrc-oz5: Connect need-profile and open-profile signals to enable
       * profile display in thread panel. Without these connections, profiles
       * requested by thread view were never fetched from relays. */
      g_signal_connect(thread_view, "need-profile",
                       G_CALLBACK(on_thread_view_need_profile), self);
      g_signal_connect(thread_view, "open-profile",
                       G_CALLBACK(on_thread_view_open_profile), self);
    }

    /* Connect article reader signals (nostrc-zwn4) */
    GtkWidget *article_reader = gnostr_session_view_get_article_reader(self->session_view);
    if (article_reader && GNOSTR_IS_ARTICLE_READER(article_reader)) {
      g_signal_connect(article_reader, "close-requested",
                       G_CALLBACK(on_article_reader_close_requested), self);
      g_signal_connect(article_reader, "open-profile",
                       G_CALLBACK(on_article_reader_open_profile), self);
      g_signal_connect(article_reader, "open-url",
                       G_CALLBACK(on_article_reader_open_url), self);
      g_signal_connect(article_reader, "share-article",
                       G_CALLBACK(on_article_reader_share), self);
      g_signal_connect(article_reader, "zap-requested",
                       G_CALLBACK(on_article_reader_zap), self);
    }

    /* Connect repo browser signals */
    GtkWidget *repo_browser = gnostr_session_view_get_repo_browser(self->session_view);
    if (repo_browser && GNOSTR_IS_REPO_BROWSER(repo_browser)) {
      g_signal_connect(repo_browser, "repo-selected",
                       G_CALLBACK(on_repo_selected), self);
      g_signal_connect(repo_browser, "clone-requested",
                       G_CALLBACK(on_clone_requested), self);
      g_signal_connect(repo_browser, "refresh-requested",
                       G_CALLBACK(on_repo_refresh_requested), self);
      g_signal_connect(repo_browser, "need-profile",
                       G_CALLBACK(on_repo_browser_need_profile), self);
      g_signal_connect(repo_browser, "open-profile",
                       G_CALLBACK(on_repo_browser_open_profile), self);
    }
  }

  gnostr_main_window_set_page(self, GNOSTR_MAIN_WINDOW_PAGE_LOADING);

  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize liked events cache (NIP-25 reactions) */
  self->liked_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize reconnection flag */
  self->reconnection_in_progress = FALSE;

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

  /* Init debounced NostrDB profile sweep */
  self->ndb_sweep_source_id = 0;
  self->ndb_sweep_debounce_ms = 1000; /* 1 second - prevents transaction contention */

  /* nostrc-bkor: Init gift wrap state and DM service BEFORE starting the subscription.
   * Previously these were initialized AFTER start_gift_wrap_subscription(), which wiped
   * the user_pubkey_hex and sub_gift_wrap that the subscription had just set. This caused
   * the logged-in user's pubkey to be NULL on app restart, breaking View Profile and
   * other features that depend on user_pubkey_hex. */
  self->sub_gift_wrap = 0;
  self->user_pubkey_hex = NULL;
  self->gift_wrap_queue = NULL; /* Created lazily when first gift wrap arrives */

  /* Init NIP-17 DM service and wire to inbox view (before gift wrap subscription
   * so dm_service is available when start_gift_wrap_subscription sets pubkey on it) */
  self->dm_service = gnostr_dm_service_new();

  GtkWidget *dm_inbox = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                          ? gnostr_session_view_get_dm_inbox(self->session_view)
                          : NULL;
  if (dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(dm_inbox)) {
    gnostr_dm_service_set_inbox_view(self->dm_service, GNOSTR_DM_INBOX_VIEW(dm_inbox));
    g_debug("[DM_SERVICE] Connected DM service to inbox view");

    /* Wire inbox signals for conversation navigation */
    g_signal_connect(dm_inbox, "open-conversation",
                     G_CALLBACK(on_dm_inbox_open_conversation), self);
    g_signal_connect(dm_inbox, "compose-dm",
                     G_CALLBACK(on_dm_inbox_compose), self);
  }

  /* Wire conversation view signals */
  GtkWidget *dm_conv = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                         ? gnostr_session_view_get_dm_conversation(self->session_view)
                         : NULL;
  if (dm_conv && GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv)) {
    g_signal_connect(dm_conv, "go-back",
                     G_CALLBACK(on_dm_conversation_go_back), self);
    g_signal_connect(dm_conv, "send-message",
                     G_CALLBACK(on_dm_conversation_send_message), self);
    g_signal_connect(dm_conv, "send-file",
                     G_CALLBACK(on_dm_conversation_send_file), self);
    g_signal_connect(dm_conv, "open-profile",
                     G_CALLBACK(on_dm_conversation_open_profile), self);
    g_debug("[DM_SERVICE] Connected conversation view signals");
  }

  /* Wire DM service message-received for live updates */
  g_signal_connect(self->dm_service, "message-received",
                   G_CALLBACK(on_dm_service_message_received), self);

  /* nostrc-61s.6: Connect close-request signal for background mode */
  g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request), NULL);

  /* Register for relay configuration changes (live relay switching, nostrc-36y.4) */
  self->relay_change_handler_id = gnostr_relay_change_connect(on_relay_config_changed, self);
  /* Register window actions for menu (menu is now in session view) */
  {
    GSimpleAction *about_action = g_simple_action_new("show-about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(on_show_about_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(about_action));
    g_object_unref(about_action);

    GSimpleAction *prefs_action = g_simple_action_new("show-preferences", NULL);
    g_signal_connect(prefs_action, "activate", G_CALLBACK(on_show_preferences_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(prefs_action));
    g_object_unref(prefs_action);
  }
  /* Connect discover page signals (nostrc-dr3) - accessed via session view */
  {
    GtkWidget *discover_page = self->session_view ? gnostr_session_view_get_discover_page(self->session_view) : NULL;
    if (discover_page && GNOSTR_IS_PAGE_DISCOVER(discover_page)) {
      g_signal_connect(discover_page, "open-profile",
                       G_CALLBACK(on_discover_open_profile), self);
      g_signal_connect(discover_page, "copy-npub-requested",
                       G_CALLBACK(on_discover_copy_npub), self);
      g_signal_connect(discover_page, "open-communities",
                       G_CALLBACK(on_discover_open_communities), self);
      g_signal_connect(discover_page, "open-article",
                       G_CALLBACK(on_discover_open_article), self);
      g_signal_connect(discover_page, "zap-article-requested",
                       G_CALLBACK(on_discover_zap_article), self);
      g_signal_connect(discover_page, "search-hashtag",
                       G_CALLBACK(on_discover_search_hashtag), self);
    }
  }
  /* Connect search results view signals (nostrc-29) */
  {
    GtkWidget *search_view = self->session_view ? gnostr_session_view_get_search_results_view(self->session_view) : NULL;
    if (search_view && GNOSTR_IS_SEARCH_RESULTS_VIEW(search_view)) {
      g_signal_connect(search_view, "open-note",
                       G_CALLBACK(on_search_open_note), self);
      g_signal_connect(search_view, "open-profile",
                       G_CALLBACK(on_search_open_profile), self);
    }
  }
  /* Connect notification view signals */
  {
    GtkWidget *notif_view = self->session_view ? gnostr_session_view_get_notifications_view(self->session_view) : NULL;
    if (notif_view && GNOSTR_IS_NOTIFICATIONS_VIEW(notif_view)) {
      g_signal_connect(notif_view, "open-note",
                       G_CALLBACK(on_notification_open_note), self);
      g_signal_connect(notif_view, "open-profile",
                       G_CALLBACK(on_notification_open_profile), self);
    }
  }
  /* Connect marketplace/classifieds view signals (NIP-15/NIP-99) - accessed via session view */
  {
    GtkWidget *classifieds_view = self->session_view ? gnostr_session_view_get_classifieds_view(self->session_view) : NULL;
    if (classifieds_view && GNOSTR_IS_CLASSIFIEDS_VIEW(classifieds_view)) {
      g_signal_connect(classifieds_view, "open-profile",
                       G_CALLBACK(on_classifieds_open_profile), self);
      g_signal_connect(classifieds_view, "contact-seller",
                       G_CALLBACK(on_classifieds_contact_seller), self);
      g_signal_connect(classifieds_view, "listing-clicked",
                       G_CALLBACK(on_classifieds_listing_clicked), self);
    }
  }
  /* Add key event controller for ESC to close profile sidebar */
  {
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);
  }

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

  g_warning("[STARTUP] deferred_heavy_init_cb: ENTER");

  /* Initialize GListModel-based event model */
  self->event_model = gn_nostr_event_model_new();

  /* Configure initial query for kind-1 notes */
  GnNostrQueryParams params = {
    .kinds = (gint[]){1},
    .n_kinds = 1,
    .authors = NULL,
    .n_authors = 0,
    .since = 0,
    .until = 0,
    .limit = 500  /* Match MODEL_MAX_EVENTS to avoid unnecessary work */
  };
  gn_nostr_event_model_set_query(self->event_model, &params);
  g_warning("[STARTUP] set_query done");

  /* REPOMARK:SCOPE: 4 - Wire GnNostrEventModel "need-profile" signal to enqueue_profile_author() and disable legacy thread_roots/prefetch initialization in gnostr_main_window_init */
  g_signal_connect(self->event_model, "need-profile", G_CALLBACK(on_event_model_need_profile), self);
  /* nostrc-yi2: Calm timeline - connect new items pending signal */
  g_signal_connect(self->event_model, "new-items-pending", G_CALLBACK(on_event_model_new_items_pending), self);

  /* Attach model to timeline view (accessed via session view) */
  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && G_TYPE_CHECK_INSTANCE_TYPE(timeline, NOSTR_GTK_TYPE_TIMELINE_VIEW)) {
    /* Wrap GListModel in a selection model */
    GtkSelectionModel *selection = GTK_SELECTION_MODEL(
      gtk_single_selection_new(G_LIST_MODEL(self->event_model))
    );
    nostr_gtk_timeline_view_set_model(NOSTR_GTK_TIMELINE_VIEW(timeline), selection);
    g_object_unref(selection); /* View takes ownership */

    /* Enable frame-rate insertion buffer drain (~60fps GLib timer) */
    gn_nostr_event_model_set_drain_enabled(self->event_model, TRUE);

    /* Connect scroll edge detection for sliding window pagination */
    GtkWidget *scroller = nostr_gtk_timeline_view_get_scrolled_window(NOSTR_GTK_TIMELINE_VIEW(timeline));
    if (scroller && GTK_IS_SCROLLED_WINDOW(scroller)) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
      if (vadj) {
        g_signal_connect(vadj, "value-changed", G_CALLBACK(on_timeline_scroll_value_changed), self);
      }
    }

    /* nostrc-7vm: Connect tab filter changed signal for hashtag/author feeds */
    g_signal_connect(timeline, "tab-filter-changed", G_CALLBACK(on_timeline_tab_filter_changed), self);

    /* Switch to SESSION page NOW so the user sees cached items immediately.
     * Relay connections, profile fetches, and full refresh happen progressively
     * after this point. Don't defer the page switch to a 150ms timeout. */
    gnostr_main_window_set_page(self, GNOSTR_MAIN_WINDOW_PAGE_SESSION);
    g_warning("[STARTUP] page switched to SESSION (cached items visible)");
  }

  /* nostrc-mzab: Start background NDB ingestion thread */
  self->ingest_queue = g_async_queue_new_full(g_free);
  __atomic_store_n(&self->ingest_running, TRUE, __ATOMIC_SEQ_CST);
  self->ingest_thread = g_thread_new("ndb-ingest", ingest_thread_func, self);
  /* Initialize profile provider */
  gnostr_profile_provider_init(0); /* Use env/default cap */
  gnostr_profile_provider_set_follow_list_provider(gnostr_follow_list_get_pubkeys_cached);
  gnostr_profile_service_set_relay_provider(gnostr_load_relays_into);
  /* NIP-47: Load saved NWC connection from GSettings */
  gnostr_nwc_service_load_from_settings(gnostr_nwc_service_get_default());
  /* LEGITIMATE TIMEOUTS - Periodic stats logging (60s intervals).
   * nostrc-b0h: Audited - diagnostic logging at fixed intervals is appropriate. */
  g_timeout_add_seconds(60, profile_provider_log_stats_cb, NULL);
  g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 60, memory_stats_cb, g_object_ref(self), g_object_unref);

  /* Pre-populate/apply cached profiles here */
  prepopulate_all_profiles_from_cache(self);

  if (!self->pool) self->pool = gnostr_pool_new();

  /* NIP-42: Install relay AUTH handler so challenges are signed automatically (nostrc-kn38) */
  gnostr_nip42_setup_pool_auth(self->pool);

  /* CRITICAL: Initialize pool and relays BEFORE timeline prepopulation!
   * Timeline prepopulation triggers profile fetches, which need relays in the pool.
   * If we prepopulate first, profile fetches will skip all relays (not in pool yet). */
  g_warning("[STARTUP] starting pool...");
  start_pool_live(self);
  g_warning("[STARTUP] pool started, starting profile sub...");
  /* Also start profile subscription if identity is configured */
  start_profile_subscription(self);

  /* nostrc-bkor: Start gift wrap subscription AFTER state init and DM service setup.
   * This sets user_pubkey_hex from saved settings on app restart. */
  start_gift_wrap_subscription(self);
  g_warning("[STARTUP] gift wrap sub done, scheduling 150ms timeout");

  /* Seed initial items so Timeline page isn't empty.
   * Timeout-audit: Use g_idle_add instead of 150ms timeout — the refresh
   * runs on the next main loop iteration after pool setup completes. */
  g_idle_add_once((GSourceOnceFunc)initial_refresh_timeout_cb, self);
  g_warning("[STARTUP] deferred_heavy_init_cb: EXIT (timeout scheduled)");

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

  /* Initialize button sensitivity based on current sign-in state */
  {
    g_autofree char *npub = client_settings_get_current_npub();
    gboolean signed_in = (npub && *npub);

    /* nostrc-1wfi: Restore NIP-46 session from settings if user was signed in */
    if (signed_in) {
      GnostrSignerService *signer = gnostr_signer_service_get_default();
      if (gnostr_signer_service_restore_from_settings(signer)) {
        g_message("[MAIN] Restored NIP-46 session from saved credentials");
        /* Also restore pubkey for the signer service */
        g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
        if (n19) {
          const char *pubkey_hex = gnostr_nip19_get_pubkey(n19);
          if (pubkey_hex) {
            gnostr_signer_service_set_pubkey(signer, pubkey_hex);
            /* nostrc-daj1: Also set user_pubkey_hex if not already set by
             * start_gift_wrap_subscription (belt-and-suspenders) */
            if (!self->user_pubkey_hex) {
              self->user_pubkey_hex = g_strdup(pubkey_hex);
              g_debug("[AUTH] Restored user_pubkey_hex from session restore: %.16s...", pubkey_hex);
            }
          }
        } else if (strlen(npub) == 64) {
          /* nostrc-daj1: Settings has raw hex, not npub */
          gnostr_signer_service_set_pubkey(signer, npub);
          if (!self->user_pubkey_hex) {
            self->user_pubkey_hex = g_strdup(npub);
            g_debug("[AUTH] Restored user_pubkey_hex from raw hex in settings: %.16s...", npub);
          }
        }
      } else {
        g_debug("[MAIN] No NIP-46 credentials to restore, checking NIP-55L fallback");
      }

      /* Verify signer is actually available after restore attempt */
      if (!gnostr_signer_service_is_available(signer)) {
        g_warning("[MAIN] Signer not available after restore - clearing signed-in state");
        signed_in = FALSE;
      }
    }

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_set_authenticated(self->session_view, signed_in);
    }

    /* nostrc-avatar: On session restore, run the same per-user initialization
     * that on_login_signed_in does. Without this, avatar/name never populate,
     * DMs don't work, notifications don't subscribe, etc. */
    if (signed_in && self->user_pubkey_hex) {
      /* Register reactive profile watch -- any future profile update
       * (from timeline subscription, dedicated fetch, etc.) auto-updates
       * the account menu avatar/name. */
      if (self->profile_watch_id) {
        gnostr_profile_provider_unwatch(self->profile_watch_id);
      }
      self->profile_watch_id = gnostr_profile_provider_watch(
        self->user_pubkey_hex, on_user_profile_watch, self);

      /* Try cached profile first for instant avatar */
      GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
      if (meta) {
        const char *final_name = (meta->display_name && *meta->display_name)
                                 ? meta->display_name : meta->name;
        if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
          gnostr_session_view_set_user_profile(self->session_view,
                                                self->user_pubkey_hex,
                                                final_name,
                                                meta->picture);
        }
        gnostr_profile_meta_free(meta);
      }

      /* Async: NIP-65 -> profile fetch from relays (updates via profile watch) */
      gnostr_nip65_load_on_login_async(self->user_pubkey_hex,
                                        on_nip65_loaded_for_profile,
                                        g_object_ref(self));

      /* Pre-warm profile provider cache from NDB */
      gnostr_profile_provider_prewarm_async(self->user_pubkey_hex);

      /* Start gift wrap subscription for encrypted DMs */
      start_gift_wrap_subscription(self);

      /* Start notification subscriptions */
      {
        GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
        gnostr_badge_manager_set_user_pubkey(badge_mgr, self->user_pubkey_hex);
        gnostr_badge_manager_set_event_callback(badge_mgr, on_notification_event, self, NULL);
        gnostr_badge_manager_start_subscriptions(badge_mgr);

        GtkWidget *nw = gnostr_session_view_get_notifications_view(self->session_view);
        if (nw && GNOSTR_IS_NOTIFICATIONS_VIEW(nw)) {
          gnostr_notifications_view_set_loading(GNOSTR_NOTIFICATIONS_VIEW(nw), TRUE);
          gnostr_badge_manager_load_history(badge_mgr, GNOSTR_NOTIFICATIONS_VIEW(nw));
        }
      }

      /* NIP-51: sync mutes, follows, bookmarks from relays */
      gnostr_nip51_settings_auto_sync_on_login(self->user_pubkey_hex);

      /* Update sync bridge with user pubkey */
      gnostr_sync_bridge_set_user_pubkey(self->user_pubkey_hex);

      /* Blossom: fetch media server preferences */
      gnostr_blossom_settings_load_from_relays_async(self->user_pubkey_hex, NULL, NULL);

      /* Tell profile pane the current user's pubkey */
      GtkWidget *pp = self->session_view
        ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
      if (pp && NOSTR_GTK_IS_PROFILE_PANE(pp)) {
        nostr_gtk_profile_pane_set_own_pubkey(NOSTR_GTK_PROFILE_PANE(pp), self->user_pubkey_hex);
      }

      g_debug("[AUTH] Restored session services for user %.16s...", self->user_pubkey_hex);
    }
  }

  g_debug("[STARTUP] deferred_heavy_init_cb: heavy init complete");
  return G_SOURCE_REMOVE;
}

static void on_event_model_need_profile(GnNostrEventModel *model, const char *pubkey_hex, gpointer user_data) {
  (void)model;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  if (strlen(pubkey_hex) != 64) return;
  enqueue_profile_author(self, pubkey_hex);
}

/* Scroll edge detection for sliding window pagination */
static void on_timeline_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->event_model) return;
  if (gn_nostr_event_model_is_async_loading(self->event_model)) return;

  gdouble value = gtk_adjustment_get_value(adj);
  gdouble upper = gtk_adjustment_get_upper(adj);
  gdouble page_size = gtk_adjustment_get_page_size(adj);
  gdouble lower = gtk_adjustment_get_lower(adj);

  /* nostrc-7o7: Estimate visible range based on scroll position
   * Assume ~100px per row (approximate). This doesn't need to be exact,
   * just needs to include items that could plausibly be visible. */
  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->event_model));
  if (n_items > 0 && upper > lower) {
    gdouble row_height_estimate = (upper - lower) / (gdouble)n_items;
    if (row_height_estimate > 0) {
      guint visible_start = (guint)(value / row_height_estimate);
      guint visible_count = (guint)(page_size / row_height_estimate) + 2; /* +2 for partial rows */
      guint visible_end = visible_start + visible_count;
      if (visible_end >= n_items) visible_end = n_items - 1;
      gn_nostr_event_model_set_visible_range(self->event_model, visible_start, visible_end);
    }
  }

  /* nostrc-yi2: Calm timeline - track if user is at top of scroll
   * "At top" means within 50px of the top edge. This allows auto-scroll when
   * the user is viewing the most recent content, but defers updates when
   * they're scrolled down reading older content. */
  gboolean user_at_top = (value <= lower + 50.0);
  gn_nostr_event_model_set_user_at_top(self->event_model, user_at_top);

  guint batch = self->load_older_batch_size > 0 ? self->load_older_batch_size : 30;
  guint max_items = 200; /* Keep at most 200 items in memory */

  /* Trigger load newer when within 20% of the top */
  gdouble top_threshold = lower + (page_size * 0.2);
  if (value <= top_threshold && upper > page_size) {
    gn_nostr_event_model_load_newer_async(self->event_model, batch, max_items);
    return;
  }

  /* Trigger load older when within 20% of the bottom */
  gdouble bottom_threshold = upper - page_size - (page_size * 0.2);
  if (value >= bottom_threshold && upper > page_size) {
    gn_nostr_event_model_load_older_async(self->event_model, batch, max_items);
  }
}


/* nostrc-yi2: Calm timeline - handle pending new items count change */
static void on_event_model_new_items_pending(GnNostrEventModel *model, guint count, gpointer user_data) {
  (void)model;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[NEW_NOTES] Pending count: %u", count);

  /* Forward to session view to update the new notes indicator */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_new_notes_count(self->session_view, count);
  }
}

/* nostrc-7vm: Handle timeline tab filter changes (hashtag/author tabs) */
static void on_timeline_tab_filter_changed(NostrGtkTimelineView *view, guint type, const char *filter_value, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->event_model) return;

  g_debug("[TAB_FILTER] type=%u filter='%s'", type, filter_value ? filter_value : "(null)");

  GNostrTimelineQuery *query = NULL;

  switch ((GnTimelineTabType)type) {
    case GN_TIMELINE_TAB_GLOBAL:
      /* Global timeline - kinds 1 and 6, no filter */
      query = gnostr_timeline_query_new_global();
      break;

    case GN_TIMELINE_TAB_FOLLOWING:
      /* nostrc-f0ll: Filter by followed pubkeys from user's contact list (kind 3) */
      if (self->user_pubkey_hex && *self->user_pubkey_hex) {
        char **followed = storage_ndb_get_followed_pubkeys(self->user_pubkey_hex);
        if (followed) {
          /* Count followed pubkeys */
          gsize n_followed = 0;
          for (char **p = followed; *p; p++) n_followed++;

          if (n_followed > 0) {
            query = gnostr_timeline_query_new_for_authors((const char **)followed, n_followed);
            g_debug("[TAB_FILTER] Following tab: %zu followed pubkeys", n_followed);
          }
          g_strfreev(followed);
        }
      }
      if (!query) {
        /* Fallback: not logged in or no follows yet */
        query = gnostr_timeline_query_new_global();
        g_debug("[TAB_FILTER] Following tab: no contact list, showing global");
      }
      break;

    case GN_TIMELINE_TAB_HASHTAG:
      /* Hashtag filter */
      if (filter_value && *filter_value) {
        query = gnostr_timeline_query_new_for_hashtag(filter_value);
        g_debug("[TAB_FILTER] Created hashtag query for #%s", filter_value);
      } else {
        query = gnostr_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_AUTHOR:
      /* Author filter */
      if (filter_value && *filter_value) {
        query = gnostr_timeline_query_new_for_author(filter_value);
        g_debug("[TAB_FILTER] Created author query for %s", filter_value);
      } else {
        query = gnostr_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_CUSTOM:
      /* Custom filter - fallback to global for now */
      query = gnostr_timeline_query_new_global();
      break;
  }

  if (query) {
    /* Apply the new query to the model */
    gn_nostr_event_model_set_timeline_query(self->event_model, query);
    gn_nostr_event_model_refresh_async(self->event_model);
    gnostr_timeline_query_free(query);
  }
}

/* nostrc-9f4: Idle callback to scroll timeline to top after model changes complete */
static gboolean scroll_to_top_idle(gpointer user_data) {
  GnostrMainWindow *self = user_data;

  /* Defensive: ensure window is still valid */
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self)) {
    return G_SOURCE_REMOVE;
  }

  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && NOSTR_GTK_IS_TIMELINE_VIEW(timeline)) {
    /* Get the internal GtkListView from the timeline and use gtk_list_view_scroll_to
     * which is the GTK4-recommended way to scroll to a specific item.
     * Using adjustment directly doesn't work reliably with ListView because
     * the adjustment bounds may not be updated yet after model changes. */
    GtkWidget *list_view = nostr_gtk_timeline_view_get_list_view(NOSTR_GTK_TIMELINE_VIEW(timeline));
    if (list_view && GTK_IS_LIST_VIEW(list_view)) {
      gtk_list_view_scroll_to(GTK_LIST_VIEW(list_view), 0, GTK_LIST_SCROLL_FOCUS, NULL);
    }
  }

  /* nostrc-3r8k: NOW hide the new notes indicator - after flush + scroll complete.
   * This lets the spinner remain visible while notes are being processed. */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_new_notes_count(self->session_view, 0);
  }

  /* Unref the window we ref'd when scheduling this idle */
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

/* nostrc-yi2: Calm timeline - handle new notes indicator button click */
static void on_new_notes_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* nostrc-3r8k: Don't hide the indicator here - the session view's click
   * handler already swapped the arrow for a spinner. We hide everything
   * in scroll_to_top_idle after flush + scroll complete so the spinner
   * remains visible while notes are being processed. */

  /* Mark user as at top BEFORE flushing so new items insert directly */
  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model)) {
    gn_nostr_event_model_set_user_at_top(self->event_model, TRUE);
  }

  /* Flush pending notes - this now uses batched insertion with single signal */
  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model)) {
    gn_nostr_event_model_flush_pending(self->event_model);
  }

  /* Defer scroll to next main loop iteration to let GTK finish processing
   * the model changes from flush_pending.
   * nostrc-pri1: Use DEFAULT priority (not HIGH) so this doesn't preempt
   * pending user input events.  Still runs before rendering (120). */
  g_idle_add_full(G_PRIORITY_DEFAULT, scroll_to_top_idle, g_object_ref(self), NULL);
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
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;

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

/* Connect all decoupled composer signals to app service implementations */
static void connect_composer_signals(NostrGtkComposer *composer, GnostrMainWindow *self) {
  g_signal_connect(composer, "toast-requested",
                   G_CALLBACK(on_composer_toast_requested), self);
  g_signal_connect(composer, "upload-requested",
                   G_CALLBACK(on_composer_upload_requested), self);
  g_signal_connect(composer, "save-draft-requested",
                   G_CALLBACK(on_composer_save_draft_requested), self);
  g_signal_connect(composer, "load-drafts-requested",
                   G_CALLBACK(on_composer_load_drafts_requested), self);
  g_signal_connect(composer, "draft-load-requested",
                   G_CALLBACK(on_composer_draft_load_requested), self);
  g_signal_connect(composer, "draft-delete-requested",
                   G_CALLBACK(on_composer_draft_delete_requested), self);
}

/* nostrc-yo2m: Handle compose button click from session view */
static void on_compose_requested(GnostrSessionView *session_view, gpointer user_data) {
  (void)session_view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Create the compose dialog */
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("New Note"));
  adw_dialog_set_content_width(dialog, 500);
  adw_dialog_set_content_height(dialog, 400);

  /* Create a toolbar view for the dialog content */
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());

  /* Create header bar with close button */
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  /* Create the composer widget */
  GtkWidget *composer = nostr_gtk_composer_new();

  /* Connect the post signal to our existing handler */
  g_signal_connect(composer, "post-requested",
                   G_CALLBACK(gnostr_main_window_handle_composer_post_requested), self);

  /* Connect decoupled service signals (upload, drafts, toast) */
  connect_composer_signals(NOSTR_GTK_COMPOSER(composer), self);

  /* Store dialog reference on composer so we can close it after post */
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);

  /* Add composer to toolbar view content */
  adw_toolbar_view_set_content(toolbar, composer);

  /* Set the toolbar view as dialog content */
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));

  /* Present the dialog */
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* nostrc-zwn4: Article composer publish handler */
static void on_article_compose_publish(GnostrArticleComposer *composer,
                                        gboolean is_draft, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  const char *title = gnostr_article_composer_get_title(composer);
  g_autofree char *content = gnostr_article_composer_get_content(composer);
  const char *d_tag = gnostr_article_composer_get_d_tag(composer);

  if (!title || !*title) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Title is required");
    return;
  }
  if (!content || !*content) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Content is required");
    return;
  }

  /* TODO: Build and sign kind 30023/30024 event via signer, publish to relays */
  const char *action = is_draft ? "Draft saved" : "Article published";
  g_autofree char *msg = g_strdup_printf("%s: %s", action, title);
  gnostr_main_window_show_toast(GTK_WIDGET(self), msg);

  g_debug("[ARTICLE-COMPOSER] %s: title=%s, d_tag=%s, draft=%d",
          action, title, d_tag ? d_tag : "(none)", is_draft);

  /* Close the dialog */
  AdwDialog *dialog = ADW_DIALOG(g_object_get_data(G_OBJECT(composer), "compose-dialog"));
  if (dialog)
    adw_dialog_close(dialog);
}

/* nostrc-zwn4: Open article compose dialog */
void gnostr_main_window_compose_article(GtkWidget *window) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Write Article"));
  adw_dialog_set_content_width(dialog, 700);
  adw_dialog_set_content_height(dialog, 600);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  GtkWidget *composer = gnostr_article_composer_new();
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);
  g_signal_connect(composer, "publish-requested",
                   G_CALLBACK(on_article_compose_publish), self);
  adw_toolbar_view_set_content(toolbar, composer);
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* nostrc-c0mp: Free compose context helper */
static void compose_context_free(ComposeContext *ctx) {
  if (!ctx) return;
  g_free(ctx->reply_to_id);
  g_free(ctx->root_id);
  g_free(ctx->reply_to_pubkey);
  g_free(ctx->display_name);
  g_free(ctx->quote_id);
  g_free(ctx->quote_pubkey);
  g_free(ctx->nostr_uri);
  g_free(ctx->comment_root_id);
  g_free(ctx->comment_root_pubkey);
  g_free(ctx);
}

/* nostrc-c0mp: Open compose dialog with optional context (reply/quote/comment) */
static void open_compose_dialog_with_context(GnostrMainWindow *self, ComposeContext *context) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    compose_context_free(context);
    return;
  }

  /* Create the compose dialog */
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_content_width(dialog, 500);
  adw_dialog_set_content_height(dialog, 400);

  /* Set appropriate title based on context */
  const char *title = _("New Note");
  if (context) {
    switch (context->type) {
      case COMPOSE_CONTEXT_REPLY:
        title = _("Reply");
        break;
      case COMPOSE_CONTEXT_QUOTE:
        title = _("Quote");
        break;
      case COMPOSE_CONTEXT_COMMENT:
        title = _("Comment");
        break;
      default:
        break;
    }
  }
  adw_dialog_set_title(dialog, title);

  /* Create a toolbar view for the dialog content */
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());

  /* Create header bar with close button */
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  /* Create the composer widget */
  GtkWidget *composer = nostr_gtk_composer_new();

  /* Connect the post signal to our existing handler */
  g_signal_connect(composer, "post-requested",
                   G_CALLBACK(gnostr_main_window_handle_composer_post_requested), self);

  /* Connect decoupled service signals (upload, drafts, toast) */
  connect_composer_signals(NOSTR_GTK_COMPOSER(composer), self);

  /* Store dialog reference on composer so we can close it after post */
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);

  /* Set context on composer */
  if (context) {
    switch (context->type) {
      case COMPOSE_CONTEXT_REPLY:
        nostr_gtk_composer_set_reply_context(NOSTR_GTK_COMPOSER(composer),
                                          context->reply_to_id,
                                          context->root_id,
                                          context->reply_to_pubkey,
                                          context->display_name);
        break;
      case COMPOSE_CONTEXT_QUOTE:
        nostr_gtk_composer_set_quote_context(NOSTR_GTK_COMPOSER(composer),
                                          context->quote_id,
                                          context->quote_pubkey,
                                          context->nostr_uri,
                                          context->display_name);
        break;
      case COMPOSE_CONTEXT_COMMENT:
        nostr_gtk_composer_set_comment_context(NOSTR_GTK_COMPOSER(composer),
                                            context->comment_root_id,
                                            context->comment_root_kind,
                                            context->comment_root_pubkey,
                                            context->display_name);
        break;
      default:
        break;
    }
  }

  /* Add composer to toolbar view content */
  adw_toolbar_view_set_content(toolbar, composer);

  /* Set the toolbar view as dialog content */
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));

  /* Present the dialog */
  adw_dialog_present(dialog, GTK_WIDGET(self));

  /* Cleanup context */
  compose_context_free(context);
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

void gnostr_main_window_set_page(GnostrMainWindow *self, GnostrMainWindowPage page) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  const char *name = NULL;
  switch (page) {
    case GNOSTR_MAIN_WINDOW_PAGE_LOADING: name = "loading"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_SESSION: name = "session"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_LOGIN: name = "login"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_ERROR: name = "error"; break;
    default: break;
  }

  if (name && self->main_stack)
    gtk_stack_set_visible_child_name(self->main_stack, name);
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
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);

  switch ((GnostrMainWindowProperty)prop_id) {
    case PROP_COMPACT:
      g_value_set_boolean(value, self->compact);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gnostr_main_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);

  switch ((GnostrMainWindowProperty)prop_id) {
    case PROP_COMPACT:
      gnostr_main_window_set_compact(self, g_value_get_boolean(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

GnostrMainWindow *gnostr_main_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}

/* ---- GObject type boilerplate and template binding ---- */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, ADW_TYPE_APPLICATION_WINDOW)

/* nostrc-u6hlt: Helper for timed g_thread_join so dispose never blocks
 * the main thread indefinitely.  A short-lived helper thread performs the
 * blocking join and signals a condition variable when done. */
typedef struct {
  GThread  *target;      /* thread to join */
  GMutex    mu;
  GCond     cond;
  gboolean  done;        /* TRUE once g_thread_join has returned */
  gboolean  abandoned;   /* TRUE if caller timed out and will not free ctx */
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

  /* If the caller timed out and abandoned us, we own the context and
   * must free it ourselves to avoid leaking / use-after-free. */
  if (abandoned) {
    g_mutex_clear(&ctx->mu);
    g_cond_clear(&ctx->cond);
    g_free(ctx);
  }
  return NULL;
}

static void gnostr_main_window_dispose(GObject *object) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  g_debug("main-window: dispose");

  /* nostrc-u6hlt: Stop background NDB ingestion thread WITHOUT blocking
   * the main thread indefinitely.
   *
   * Previous code called g_thread_join() directly on the main thread.
   * If the ingest thread was stuck (mutex contention, large queue
   * backlog in the drain loop, blocked I/O in storage_ndb_ingest),
   * the app hung on shutdown with no error output.
   *
   * Fix: (1) signal the thread to stop, (2) push a sentinel into the
   * queue so it wakes immediately, (3) dispatch g_thread_join to a
   * short-lived helper thread and wait on a GCond with a 2 s deadline.
   * The ingest thread's drain loop is also capped so it exits fast. */
  if (self->ingest_thread) {
    __atomic_store_n(&self->ingest_running, FALSE, __ATOMIC_SEQ_CST);

    /* Wake the thread immediately instead of waiting up to 100 ms. */
    if (self->ingest_queue)
      g_async_queue_push(self->ingest_queue, g_strdup(""));

    /* Timed join: helper thread performs the blocking g_thread_join
     * while we wait on a condition variable with a deadline.
     * Heap-allocate the context so the joiner thread can safely
     * reference it even if we time out and return from dispose. */
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
        /* Timeout elapsed -- ingest thread did not exit in time. */
        g_warning("main-window: ingest thread did not exit within 2 s; "
                  "abandoning join to avoid blocking shutdown");
        break;
      }
    }
    gboolean joined = jctx->done;
    if (!joined)
      jctx->abandoned = TRUE;   /* joiner thread will free jctx */
    g_mutex_unlock(&jctx->mu);

    if (joined) {
      /* Joiner thread has finished; collect it and free context. */
      g_thread_join(joiner);
      g_mutex_clear(&jctx->mu);
      g_cond_clear(&jctx->cond);
      g_free(jctx);
    } else {
      /* Joiner is still blocked inside g_thread_join on the ingest
       * thread.  Detach it -- both threads are leaked but the process
       * is exiting and the OS will reclaim resources.  The joiner
       * thread owns jctx and will free it when it eventually returns. */
      g_thread_unref(joiner);
    }

    self->ingest_thread = NULL;
  }
  g_clear_pointer(&self->ingest_queue, g_async_queue_unref);

  /* Unwatch profile provider to prevent callbacks after dispose */
  if (self->profile_watch_id) {
    gnostr_profile_provider_unwatch(self->profile_watch_id);
    self->profile_watch_id = 0;
  }

  /* Remove pending timeout/idle sources to prevent callbacks after dispose */
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
    free_urls_owned(self->live_urls, self->live_url_count);
    self->live_urls = NULL;
    self->live_url_count = 0;
  }
  /* Clean up any outstanding profile batch sequence */
  if (self->profile_batches) {
    for (guint i = 0; i < self->profile_batches->len; i++) {
      GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
      if (b) g_ptr_array_free(b, TRUE);
    }
    g_ptr_array_free(self->profile_batches, TRUE);
    self->profile_batches = NULL;
  }
  if (self->profile_batch_urls) {
    free_urls_owned(self->profile_batch_urls, self->profile_batch_url_count);
    self->profile_batch_urls = NULL;
    self->profile_batch_url_count = 0;
  }
  g_clear_pointer(&self->profile_batch_filters, nostr_filters_free);
  g_clear_object(&self->profile_pool);
  if (self->pool) {
    /* Disconnect signal handlers BEFORE unreffing to prevent use-after-free
     * when pending main loop callbacks try to emit on the freed pool */
    if (self->pool_events_handler) {
      g_signal_handler_disconnect(self->pool, self->pool_events_handler);
      self->pool_events_handler = 0;
    }
    /* Disconnect all remaining handlers from this instance (e.g., bg prefetch) */
    g_signal_handlers_disconnect_by_data(self->pool, self);
  }
  g_clear_object(&self->pool);
  g_clear_pointer(&self->seen_texts, g_hash_table_unref);
  g_clear_object(&self->event_model);
  /* Avatar texture cache cleanup is handled by gnostr-avatar-cache module */
  g_clear_pointer(&self->liked_events, g_hash_table_unref);

  /* Stop gift wrap subscription */
  stop_gift_wrap_subscription(self);
  if (self->gift_wrap_queue) {
    g_ptr_array_free(self->gift_wrap_queue, TRUE);
    self->gift_wrap_queue = NULL;
  }

  /* Stop and cleanup DM service */
  if (self->dm_service) {
    gnostr_dm_service_stop(self->dm_service);
    g_clear_object(&self->dm_service);
  }

  /* Shutdown profile provider */
  gnostr_profile_provider_shutdown();

  /* Disconnect relay change handler (live relay switching) */
  if (self->relay_change_handler_id) {
    gnostr_relay_change_disconnect(self->relay_change_handler_id);
    self->relay_change_handler_id = 0;
  }

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

  /* Ensure custom template child types are registered before parsing template */
  g_type_ensure(GNOSTR_TYPE_SESSION_VIEW);
  g_type_ensure(GNOSTR_TYPE_LOGIN);

  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, main_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, session_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, login_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, error_page);
}

/* ---- Minimal stub implementations to satisfy build and support cached profiles path ---- */
static void initial_refresh_timeout_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  g_warning("[STARTUP] initial_refresh_timeout_cb: starting async refresh");

  /* Set initial global timeline query before refresh so cached events load */
  if (self->event_model) {
    GNostrTimelineQuery *query = gnostr_timeline_query_new_global();
    gn_nostr_event_model_set_timeline_query(self->event_model, query);
    gnostr_timeline_query_free(query);
    
    gn_nostr_event_model_refresh_async(self->event_model);
  }

  g_warning("[STARTUP] initial_refresh_timeout_cb: EXIT");
}

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
  build_urls_and_filters_for_kinds(self, &kind1, 1, out_urls, out_count, out_filters, limit);
}

static void build_urls_and_filters_for_kinds(GnostrMainWindow *self,
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

static void free_urls_owned(const char **urls, size_t count) {
  if (!urls) return;
  for (size_t i = 0; i < count; i++) {
    g_free((gpointer)urls[i]);
  }
  g_free((gpointer)urls);
}

static gboolean profile_dispatch_next(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  /* Removed noisy debug */
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    g_warning("profile_fetch: dispatch_next called with invalid self");
    return G_SOURCE_REMOVE;
  }
  
  /* LEGITIMATE TIMEOUT - Rate limit concurrent profile fetches.
   * When max concurrent is reached, wait 500ms before checking again.
   * This prevents goroutine explosion while still processing the queue.
   * nostrc-b0h: Audited - backpressure mechanism is appropriate. */
  if (self->profile_fetch_active >= self->profile_fetch_max_concurrent) {
    g_debug("profile_fetch: at max concurrent (%u/%u), deferring batch",
            self->profile_fetch_active, self->profile_fetch_max_concurrent);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 500, profile_dispatch_next, g_object_ref(self), g_object_unref);
    return G_SOURCE_REMOVE;
  }
  
  /* NOTE: With goroutine-based fetching, we can allow concurrent batches!
   * Goroutines are lightweight (not OS threads), so multiple batches can run safely.
   * The old serialization flag was needed for the broken GLib thread implementation,
   * but is no longer necessary with goroutines. */
  /* Nothing to do? Clean up sequence if finished */
  if (!self->profile_batches || self->profile_batch_pos >= self->profile_batches->len) {
    if (self->profile_batches) {
      g_debug("profile_fetch: sequence complete (batches=%u)", self->profile_batches->len);
    } else {
      g_debug("profile_fetch: sequence complete (no batches)");
    }
    if (self->profile_batches) {
      /* Free any remaining batches (if cancelled mid-flight) */
      for (guint i = self->profile_batch_pos; i < self->profile_batches->len; i++) {
        GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
        if (b) g_ptr_array_free(b, TRUE);
      }
      g_ptr_array_free(self->profile_batches, TRUE);
      self->profile_batches = NULL;
    }
    /* Free captured URLs array pointer */
    if (self->profile_batch_urls) {
      free_urls_owned(self->profile_batch_urls, self->profile_batch_url_count);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
    
    /* CRITICAL: Check if there are queued authors waiting and trigger a new fetch.
     * LEGITIMATE TIMEOUT - debounce to batch queued authors.
     * nostrc-b0h: Audited - same debounce pattern as initial queue. */
    if (self->profile_fetch_queue && self->profile_fetch_queue->len > 0) {
      g_debug("profile_fetch: SEQUENCE COMPLETE - %u authors queued, scheduling new fetch",
             self->profile_fetch_queue->len);
      if (!self->profile_fetch_source_id) {
        guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
        self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, delay, profile_fetch_fire_idle, g_object_ref(self), g_object_unref);
      } else {
        g_warning("profile_fetch: fetch already scheduled (source_id=%u)", self->profile_fetch_source_id);
      }
    } else {
      g_debug("profile_fetch: SEQUENCE COMPLETE - no authors queued");
    }
    /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
    return G_SOURCE_REMOVE;
  }

  if (!self->pool) self->pool = gnostr_pool_new();
  if (!self->profile_pool) self->profile_pool = gnostr_pool_new();
  if (!self->profile_fetch_cancellable) self->profile_fetch_cancellable = g_cancellable_new();
  if (g_cancellable_is_cancelled(self->profile_fetch_cancellable)) {
    /* Cancelled: clean up any leftover state */
    if (self->profile_batches) {
      for (guint i = self->profile_batch_pos; i < self->profile_batches->len; i++) {
        GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
        if (b) g_ptr_array_free(b, TRUE);
      }
      g_ptr_array_free(self->profile_batches, TRUE);
      self->profile_batches = NULL;
    }
    if (self->profile_batch_urls) {
        free_urls_owned(self->profile_batch_urls, self->profile_batch_url_count);
        self->profile_batch_urls = NULL;
        self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
    /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
    return G_SOURCE_REMOVE;
  }

  /* Take next batch and remove it from the array (transfer ownership) */
  guint batch_idx = self->profile_batch_pos;
  GPtrArray *batch = g_ptr_array_index(self->profile_batches, batch_idx);
  g_ptr_array_index(self->profile_batches, batch_idx) = NULL; /* clear slot to avoid double-free */
  self->profile_batch_pos++;
  if (!batch || batch->len == 0) {
    if (batch) g_ptr_array_free(batch, TRUE);
    /* Continue to next - we're already in a callback, so schedule via idle.
     * Use g_idle_add_full() instead of g_timeout_add_full(..., 0, ...) for
     * efficiency - both run on next main loop iteration but idle is cleaner. */
    /* nostrc-pri1: DEFAULT_IDLE so profile batches don't compete with input */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, profile_dispatch_next, g_object_ref(self), (GDestroyNotify)g_object_unref);
    /* NOTE: Don't unref - GLib handles it via g_idle_add_full's GDestroyNotify */
    return G_SOURCE_REMOVE;
  }

  /* Prepare authors array (borrow strings) */
  size_t n = batch->len;
  const char **authors = g_new0(const char*, n);
  for (guint i = 0; i < n; i++) authors[i] = (const char*)g_ptr_array_index(batch, i);

  ProfileBatchCtx *ctx = g_new0(ProfileBatchCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->batch = batch; /* ownership transferred; freed in callback */

  g_debug("[PROFILE] Dispatching batch %u/%u (%zu authors, active=%u/%u)",
          self->profile_batch_pos, self->profile_batches ? self->profile_batches->len : 0, n,
          self->profile_fetch_active, self->profile_fetch_max_concurrent);

  /* Increment active fetch counter */
  self->profile_fetch_active++;

  /* Sync relays on the profile pool and build kind-0 filter */
  gnostr_pool_sync_relays(self->profile_pool,
                          self->profile_batch_urls,
                          self->profile_batch_url_count);

  NostrFilter *f = nostr_filter_new();
  int kind0 = 0;
  nostr_filter_set_kinds(f, &kind0, 1);
  nostr_filter_set_authors(f, (const char *const *)authors, n);
  /* Free any previous filters */
  if (self->profile_batch_filters) {
    nostr_filters_free(self->profile_batch_filters);
  }
  self->profile_batch_filters = nostr_filters_new();
  nostr_filters_add(self->profile_batch_filters, f);
  nostr_filter_free(f);

  gnostr_pool_query_async(self->profile_pool,
                          self->profile_batch_filters,
                          self->profile_fetch_cancellable,
                          on_profiles_batch_done,
                          ctx);
  /* nostrc-uaf5: GTask takes ownership of filters — NULL out to prevent
   * double-free when profile_dispatch_next is called again. */
  self->profile_batch_filters = NULL;

  g_free((gpointer)authors);
  /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
  return G_SOURCE_REMOVE;
}

static gboolean periodic_backfill_cb(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

/* Live relay switching callback (nostrc-36y.4) */
static void on_relay_config_changed(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[LIVE_RELAY] Relay configuration changed, syncing pool...");

  /* Get updated relay URLs */
  GPtrArray *read_relays = gnostr_get_read_relay_urls();

  if (read_relays->len == 0) {
    g_warning("[LIVE_RELAY] No read relays configured");
    g_ptr_array_unref(read_relays);
    return;
  }

  /* Sync pool with new relay list */
  if (self->pool) {
    const char **urls = g_new0(const char*, read_relays->len);
    for (guint i = 0; i < read_relays->len; i++) {
      urls[i] = g_ptr_array_index(read_relays, i);
    }

    gnostr_pool_sync_relays(self->pool, (const gchar **)urls, read_relays->len);
    g_free(urls);
  }

  /* Update cached live URLs */
  if (self->live_urls) {
    free_urls_owned(self->live_urls, self->live_url_count);
    self->live_urls = NULL;
    self->live_url_count = 0;
  }

  /* Build new live URL list */
  self->live_urls = g_new0(const char*, read_relays->len);
  self->live_url_count = read_relays->len;
  for (guint i = 0; i < read_relays->len; i++) {
    self->live_urls[i] = g_strdup(g_ptr_array_index(read_relays, i));
  }

  g_ptr_array_unref(read_relays);

  /* If we have an active subscription, restart it to use new relays */
  if (self->pool_cancellable) {
    g_debug("[LIVE_RELAY] Restarting live subscription with updated relays");
    g_cancellable_cancel(self->pool_cancellable);
    g_clear_object(&self->pool_cancellable);

    /* Timeout-audit: Use idle instead of 100ms timeout — the restart runs
     * on the next main loop iteration, after cleanup signals propagate. */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_relay_config_changed_restart,
                    g_object_ref(self), g_object_unref);
  }

  /* Restart DM service to pick up new DM relays (nostrc-36y.4) */
  if (self->dm_service) {
    g_debug("[LIVE_RELAY] Restarting DM service with updated DM relays");
    gnostr_dm_service_stop(self->dm_service);
    gnostr_dm_service_start_with_dm_relays(self->dm_service);
  }

  g_debug("[LIVE_RELAY] Relay sync complete");
}

/* Helper to restart live subscription after relay change */
static gboolean on_relay_config_changed_restart(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;

  /* Only restart if we're not already reconnecting */
  if (!self->reconnection_in_progress && !self->pool_cancellable) {
    start_pool_live(self);
  }

  return G_SOURCE_REMOVE;
}

static gboolean retry_pool_live(gpointer user_data);
static gboolean check_relay_health(gpointer user_data);

/* nostrc-p2f6: Context for async relay connect → subscribe pipeline */
typedef struct {
    GnostrMainWindow *self;    /* strong ref */
    NostrFilters     *filters; /* owned */
} PoolConnectCtx;

static void
on_pool_relays_connected(GObject      *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer      user_data)
{
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
        /* retry_pool_live expects a ref; reuse the one we hold.
         * g_timeout_add_full adds a safety ref via destroy notify. */
        g_timeout_add_full(G_PRIORITY_DEFAULT, 5000, retry_pool_live,
                           g_object_ref(self), g_object_unref);
        self->reconnection_in_progress = FALSE;
        return;
    }
    g_clear_error(&err);

    /* nostrc-b0h: REMOVED g_usleep(500000) "wait for other relays."
     * The multi-sub already handles relays connecting after creation —
     * it subscribes to new relays via pool state-changed signals.
     * Blocking the GTask callback thread for 500ms added latency to
     * EVERY startup and reconnection with zero benefit. */
    g_warning("[RELAY] Starting multi-relay live subscription (relays connected: first, others joining asynchronously)");

    GError *sub_error = NULL;
    GNostrPoolMultiSub *multi_sub = gnostr_pool_subscribe_multi(
        self->pool,
        filters,
        on_multi_sub_event,
        on_multi_sub_eose,
        self,
        NULL,  /* no destroy notify needed */
        &sub_error);
    
    /* Multi-sub does NOT take ownership of filters, we must free them */
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
        self->health_check_source_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 30, check_relay_health, g_object_ref(self), g_object_unref);
    }

    g_object_unref(self); /* balance ref from start_pool_live */
}

static void start_pool_live(GnostrMainWindow *self) {
  /* Removed noisy debug */
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Prevent concurrent reconnection attempts */
  if (self->reconnection_in_progress) {
    g_debug("[RELAY] Reconnection already in progress, skipping");
    return;
  }
  self->reconnection_in_progress = TRUE;

  if (!self->pool) self->pool = gnostr_pool_new();

  /* Cancel any existing subscription before starting a new one to prevent FD leak */
  if (self->pool_cancellable)
    g_cancellable_cancel(self->pool_cancellable);
  g_clear_object(&self->pool_cancellable);
  self->pool_cancellable = g_cancellable_new();

  /* Build live URLs and filters: subscribe to all required kinds for persistence-first operation.
   * No limit on subscription since all events go into nostrdb - UI models handle their own windowing. */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;

  /* Build live subscription filter with since=now to get only NEW events.
   * Historical events are loaded from nostrdb cache, live sub is for real-time updates. */
  const int live_kinds[] = {0, 1, 5, 6, 7, 16, 1111};
  build_urls_and_filters_for_kinds(self,
                                  live_kinds,
                                  G_N_ELEMENTS(live_kinds),
                                  &urls,
                                  &url_count,
                                  &filters,
                                  0);  /* No limit - nostrdb handles storage */
  
  /* No since filter - nostrdb handles deduplication automatically.
   * Live subscription receives all matching events, nostrdb stores them,
   * and the timeline model queries nostrdb for display. */
  if (!urls || url_count == 0 || !filters) {
    g_warning("[RELAY] No relay URLs configured, skipping live subscription");
    if (filters) nostr_filters_free(filters);
    if (urls) free_urls_owned(urls, url_count);
    self->reconnection_in_progress = FALSE;
    return;
  }

  if (self->live_urls) {
    free_urls_owned(self->live_urls, self->live_url_count);
    self->live_urls = NULL;
    self->live_url_count = 0;
  }
  self->live_urls = urls;            /* take ownership of urls + strings */
  self->live_url_count = url_count;

  /* CRITICAL: Initialize relays in the pool so profile fetches can find them.
   * Profile fetch code skips relays not in pool (to avoid blocking main thread).
   * We call sync_relays() here BEFORE starting subscriptions to populate the pool.
   * This is acceptable because start_pool_live() runs early at startup, not on main loop yet. */
  g_warning("[RELAY] Initializing %zu relays in pool", self->live_url_count);
  gnostr_pool_sync_relays(self->pool, (const gchar **)self->live_urls, self->live_url_count);
  g_warning("[RELAY] ✓ All relays initialized");
  /* Close previous multi-subscription if any */
  if (self->live_multi_sub) {
    gnostr_pool_multi_sub_close(self->live_multi_sub);
    self->live_multi_sub = NULL;
  }

  /* nostrc-p2f6: Connect all relays BEFORE subscribing.
   * Previously we subscribed immediately after sync_relays(), but relays
   * start disconnected — pool_subscribe requires at least one connected relay. */
  g_warning("[RELAY] Connecting %zu relays...", self->live_url_count);
  PoolConnectCtx *ctx = g_new0(PoolConnectCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->filters = filters; /* transfer ownership to callback */
  gnostr_pool_connect_all_async(self->pool, self->pool_cancellable,
                                on_pool_relays_connected, ctx);
  /* subscribe happens in on_pool_relays_connected callback */
}

static void start_profile_subscription(GnostrMainWindow *self) {
  /* Optional: one-time fetch of current profile if signed in. We'll rely on demand-driven fetch otherwise. */
  (void)self; /* Intentionally minimal at this stage. */
}

static void start_bg_profile_prefetch(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->pool) self->pool = gnostr_pool_new();
  if (!self->bg_prefetch_cancellable) self->bg_prefetch_cancellable = g_cancellable_new();

  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;
  /* Use same filter as live timeline (kind 1), but paginate to sweep authors and queue profiles */
  build_urls_and_filters(self, &urls, &url_count, &filters, (int)self->default_limit);
  if (!urls || url_count == 0 || !filters) {
    if (filters) nostr_filters_free(filters);
    if (urls) free_urls_owned(urls, url_count);
    return;
  }

  /* TODO: paginate_with_interval not yet available on GNostrPool.
   * The old paginator is disabled until GNostrPool gains equivalent API.
   * For now, bg prefetch relies on the live subscription to discover authors. */
  g_debug("start_bg_profile_prefetch: paginate disabled (GNostrPool migration)");
  (void)url_count;
  /* filters owned by us; urls array free */
  nostr_filters_free(filters);
  free_urls_owned(urls, url_count);
}

/* Periodic health check to detect and reconnect dead relay connections */
static gboolean check_relay_health(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool) {
    g_warning("relay_health: invalid window or pool, stopping health checks");
    if (GNOSTR_IS_MAIN_WINDOW(self)) {
      self->health_check_source_id = 0;
    }
    return G_SOURCE_REMOVE;
  }
  
  /* Skip health check if reconnection is already in progress */
  if (self->reconnection_in_progress) {
    return G_SOURCE_CONTINUE;
  }

  /* Get relay list from the pool (GListStore of GNostrRelay) */
  GListStore *relay_store = gnostr_pool_get_relays(self->pool);
  guint n_relays = g_list_model_get_n_items(G_LIST_MODEL(relay_store));
  if (n_relays == 0) {
    return G_SOURCE_CONTINUE;
  }

  /* Check connection status of each relay */
  guint disconnected_count = 0;
  guint connected_count = 0;

  for (guint i = 0; i < n_relays; i++) {
    g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(relay_store), i);
    if (!relay) continue;
    /* Check if relay is present (added to pool = considered "connected" for health check) */
    if (gnostr_pool_get_relay(self->pool, gnostr_relay_get_url(relay)) != NULL) {
      connected_count++;
    } else {
      disconnected_count++;
    }
  }

  /* Update tray icon with current relay status */
  gnostr_app_update_relay_status((int)connected_count, (int)n_relays);

  /* Update relay status indicator in UI */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_relay_status(self->session_view,
                                         connected_count,
                                         n_relays);
  }

  /* If ALL relays are disconnected, trigger reconnection */
  if (disconnected_count > 0 && connected_count == 0) {
    g_warning("relay_health: all %u relay(s) disconnected - reconnecting",
              disconnected_count);
    start_pool_live(self);
  }
  return G_SOURCE_CONTINUE; /* Keep checking every interval */
}

/* Periodic model refresh to pick up new events from nostrdb */
static gboolean periodic_model_refresh(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    return G_SOURCE_REMOVE;
  }
  
  if (self->event_model) {
    gn_nostr_event_model_refresh_async(self->event_model);
  }

  return G_SOURCE_CONTINUE;
}

/* Retry live subscription after failure.
 * Inherits a strong ref from the failure path in on_pool_relays_connected.
 * start_pool_live takes its own ref, so we unref the inherited one. (nostrc-04er) */
static gboolean retry_pool_live(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    g_object_unref(self);
    return G_SOURCE_REMOVE;
  }
  start_pool_live(self);
  g_object_unref(self); /* balance ref inherited from failure path */
  return G_SOURCE_REMOVE;
}

/* Bounded push to ingest_queue.  Returns TRUE if queued, FALSE if dropped.
 * Takes ownership of `json` on success; caller must g_free on failure. */
static gboolean
ingest_queue_push(GnostrMainWindow *self, gchar *json)
{
  __atomic_fetch_add(&self->ingest_events_received, 1, __ATOMIC_RELAXED);
  
  gint depth = g_async_queue_length(self->ingest_queue);
  if (depth >= INGEST_QUEUE_MAX) {
    __atomic_fetch_add(&self->ingest_events_dropped, 1, __ATOMIC_RELAXED);
    
    gint64 now = g_get_monotonic_time();
    if (now - self->last_backpressure_warn_us > 5000000) { /* warn at most every 5s */
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

/* nostrc-mzab: Background NDB ingestion thread.
 * Drains the ingest_queue and calls storage_ndb_ingest_event_json()
 * off the main thread, so ndb_process_event (which can block when the
 * ndb ingestion pipeline is full) never stalls the GTK main loop. */
static gpointer
ingest_thread_func(gpointer data)
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

/* nostrc-mzab: Quick kind extraction from event JSON without full deserialization.
 * Returns -1 if "kind" field not found. Avoids the overhead of allocating
 * and parsing a full NostrEvent just to check the kind number. */
static int
extract_kind_from_json(const char *json)
{
  const char *p = strstr(json, "\"kind\"");
  if (!p) return -1;
  p += 6; /* skip "kind" */
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return -1;
  return (int)strtol(p, NULL, 10);
}

/* Live subscription event handler: filter by kind and queue for background ingestion.
 * nostrc-mzab: No longer calls storage_ndb_ingest_event_json on the main thread.
 * Uses lightweight JSON scanning instead of full NostrEvent deserialization. */
/* Multi-relay subscription event handler */
static void on_multi_sub_event(GNostrPoolMultiSub *multi_sub,
                                const gchar        *relay_url,
                                const gchar        *event_json,
                                gpointer            user_data) {
  (void)multi_sub;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_json) return;

  /* Quick kind extraction (no allocation, no full JSON parse) */
  int kind = extract_kind_from_json(event_json);
  if (kind < 0) return;

  /* Filter: only ingest timeline events and NIP-34 git events */
  if (!(kind == 0 || kind == 1 || kind == 5 || kind == 6 || kind == 7 || kind == 16 || kind == 1111 ||
        kind == 30617 || kind == 1617 || kind == 1621 || kind == 1622)) {
    return;
  }

  /* Queue for background ingestion (never blocks main thread) */
  gchar *copy = g_strdup(event_json);
  if (!ingest_queue_push(self, copy)) {
    g_free(copy);
  } else {
    /* Track relay activity for monitoring - only log at debug level */
    g_debug("[RELAY] Event from %s (kind %d)", relay_url, kind);
  }
}

/* Multi-relay subscription EOSE handler */
static void on_multi_sub_eose(GNostrPoolMultiSub *multi_sub,
                               const gchar        *relay_url,
                               gpointer            user_data) {
  (void)multi_sub;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  g_debug("[RELAY] EOSE from %s", relay_url);
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
      enqueue_profile_author(self, pk);
    }
  }
  nostr_event_free(evt);
}

/* Get the current user's npub from GSettings */
static char *client_settings_get_current_npub(void) {
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (!settings) return NULL;
  char *npub = g_settings_get_string(settings, "current-npub");
  /* Return NULL if empty string */
  if (npub && !*npub) {
    g_free(npub);
    return NULL;
  }
  return npub;
}

/* Get the current user's pubkey as 64-char hex (from npub bech32).
 * Returns newly allocated string or NULL if not signed in. Caller must free.
 * nostrc-daj1: Also handles case where settings stores raw hex (not npub). */
static char *get_current_user_pubkey_hex(void) {
  g_autofree char *npub = client_settings_get_current_npub();
  if (!npub) return NULL;

  /* If settings already contains a 64-char hex key (not bech32), use it directly */
  if (strlen(npub) == 64 && !g_str_has_prefix(npub, "npub1")) {
    g_debug("[AUTH] current-npub setting contains raw hex pubkey, using directly");
    return g_strdup(npub);
  }

  /* Decode bech32 npub using GNostrNip19 */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
  if (!n19) {
    g_warning("[AUTH] Failed to decode current-npub to pubkey: %.16s...", npub);
    return NULL;
  }

  const char *hex = gnostr_nip19_get_pubkey(n19);
  if (!hex) {
    g_warning("[AUTH] gnostr_nip19_get_pubkey returned NULL for: %.16s...", npub);
    return NULL;
  }

  return g_strdup(hex);
}

/* ============== Gift Wrap (NIP-59) Subscription ============== */

/* Callback for gift wrap (kind 1059) events from nostrdb subscription.
 * Gift wraps are encrypted events addressed to the current user via p-tag.
 * Events are processed immediately by the DM service for decryption. */
static void on_gift_wrap_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[GIFTWRAP] Failed to begin query transaction");
    return;
  }

  guint processed = 0;
  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != NOSTR_KIND_GIFT_WRAP) continue;

    /* Get the note ID for logging */
    const unsigned char *id32 = storage_ndb_note_id(note);
    if (!id32) continue;

    char id_hex[65];
    storage_ndb_hex_encode(id32, id_hex);

    /* Get the JSON representation for the DM service */
    char *json = NULL;
    int json_len = 0;
    if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len, NULL) == 0 && json) {
      /* Send to DM service for decryption */
      if (self->dm_service) {
        gnostr_dm_service_process_gift_wrap(self->dm_service, json);
        processed++;
        g_debug("[GIFTWRAP] Sent gift wrap %.8s... to DM service for decryption", id_hex);
      }
      g_free(json);
    }
  }

  storage_ndb_end_query(txn);

  if (processed > 0) {
    g_debug("[GIFTWRAP] Processed %u gift wrap event(s) via DM service", processed);
  }
}

/* Start subscription to kind 1059 (gift wrap) events addressed to current user.
 * The subscription uses a p-tag filter to only receive events where the current
 * user is the recipient. */
static void start_gift_wrap_subscription(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Don't start if already subscribed */
  if (self->sub_gift_wrap > 0) {
    g_debug("[GIFTWRAP] Subscription already active (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->sub_gift_wrap);
    return;
  }

  /* Get current user's pubkey */
  char *pubkey_hex = get_current_user_pubkey_hex();
  if (!pubkey_hex) {
    g_debug("[GIFTWRAP] No user signed in, skipping gift wrap subscription");
    return;
  }

  /* Store user pubkey for later use */
  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = pubkey_hex;

  /* Set user pubkey on DM service for message direction detection */
  if (self->dm_service) {
    gnostr_dm_service_set_user_pubkey(self->dm_service, pubkey_hex);
    g_debug("[DM_SERVICE] Set user pubkey %.8s... on DM service", pubkey_hex);
  }

  /* Build filter JSON for kind 1059 with p-tag matching current user.
   * Filter format: {"kinds":[1059],"#p":["<pubkey>"]}
   * This ensures we only receive gift wraps addressed to us. */
  g_autofree char *filter_json = g_strdup_printf(
    "{\"kinds\":[%d],\"#p\":[\"%s\"]}",
    NOSTR_KIND_GIFT_WRAP,
    pubkey_hex
  );

  /* Subscribe via the NDB dispatcher */
  self->sub_gift_wrap = gn_ndb_subscribe(filter_json, on_gift_wrap_batch, self, NULL);

  if (self->sub_gift_wrap > 0) {
    g_debug("[GIFTWRAP] Started subscription for user %.8s... (subid=%" G_GUINT64_FORMAT ")",
            pubkey_hex, (guint64)self->sub_gift_wrap);
  } else {
    g_warning("[GIFTWRAP] Failed to subscribe to gift wrap events");
  }
}

/* Stop the gift wrap subscription (e.g., on logout or window dispose) */
static void stop_gift_wrap_subscription(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (self->sub_gift_wrap > 0) {
    gn_ndb_unsubscribe(self->sub_gift_wrap);
    g_debug("[GIFTWRAP] Stopped subscription (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->sub_gift_wrap);
    self->sub_gift_wrap = 0;
  }

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;
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

/* nostrc-sk8o: Helper to refresh thread view profiles once after batch updates */
static void refresh_thread_view_profiles_if_visible(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    if (is_panel_visible(self) && !gnostr_session_view_is_showing_profile(self->session_view)) {
      nostr_gtk_thread_view_update_profiles(NOSTR_GTK_THREAD_VIEW(thread_view));
    }
  }
}

/* Parses content_json and stores in profile provider, then updates the event model.
 * nostrc-sk8o: Does NOT update thread view - caller must call
 * refresh_thread_view_profiles_if_visible() once after batch updates to avoid
 * O(N*M) main thread blocking. */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !content_json) return;

  /* Update profile provider cache */
  gnostr_profile_provider_update(pubkey_hex, content_json);

  /* Update GnNostrEventModel items */
  extern void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json);
  if (self->event_model) {
    gn_nostr_event_model_update_profile(G_OBJECT(self->event_model), pubkey_hex, content_json);
  }

  /* nostrc-sk8o: Thread view update removed - now done once after batch in caller */

  /* nostrc-loed: Refresh login UI if this is the current user's profile */
  if (self->user_pubkey_hex && pubkey_hex &&
      g_ascii_strcasecmp(self->user_pubkey_hex, pubkey_hex) == 0) {
    update_login_ui_state(self);

    /* Update session view account button with user's profile */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
      if (meta) {
        const char *display_name = meta->display_name ? meta->display_name : meta->name;
        gnostr_session_view_set_user_profile(self->session_view,
                                              pubkey_hex,
                                              display_name,
                                              meta->picture);
        gnostr_profile_meta_free(meta);
      }
    }
  }
}
