#define G_LOG_DOMAIN "gnostr-main-window"

#include "gnostr-main-window.h"
#include "gnostr-tray-icon.h"
#include "gnostr-session-view.h"
#include "gnostr-composer.h"
#include "gnostr-timeline-view.h"
#include "gn-timeline-tabs.h"
#include "gnostr-profile-pane.h"
#include "gnostr-thread-view.h"
#include "gnostr-article-reader.h"
#include "gnostr-article-composer.h"
#include "gnostr-profile-provider.h"
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
#include "note_card_row.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include "../model/gn-nostr-event-model.h"
#include "../model/gn-timeline-query.h"
#include "../model/gn-nostr-event-item.h"
#include "../model/gn-nostr-profile.h"
#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <time.h>
/* JSON helpers - GObject wrappers and core interface */
#include "nostr_json.h"
#include "json.h"
/* NIP-19 bech32 encoding (GObject wrapper) */
#include "nostr_nip19.h"
/* nostr_pool.h already included below; simple pool removed */
#include "nostr-event.h"
#include "nostr-filter.h"
/* Canonical JSON helpers (for nostr_event_from_json, etc.) */
#include "nostr-json.h"
/* GObject wrappers for profile-related code */
#include "nostr_event.h"
#include "nostr_pool.h"
#include "nostr_subscription.h"
/* NostrdB storage */
#include "../storage_ndb.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "libnostr_errors.h"
/* Nostr event kinds */
#include "nostr-kinds.h"
/* Relays helpers */
#include "../util/relays.h"
/* NIP-11 relay information */
#include "../util/relay_info.h"
/* NIP-51 mute list */
#include "../util/mute_list.h"
/* NIP-32 labeling */
#include "../util/nip32_labels.h"
/* NIP-51 settings sync */
#include "../util/nip51_settings.h"
/* Blossom server settings (kind 10063) */
#include "../util/blossom_settings.h"
/* NIP-42 relay authentication */
#include "../util/nip42_auth.h"
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

/* Define the window instance struct early so functions can access fields */
struct _GnostrMainWindow {
  AdwApplicationWindow parent_instance;

  /* New Fractal-style state stack */
  GtkStack *main_stack;
  GnostrSessionView *session_view;
  GtkWidget *login_view;
  AdwStatusPage *error_page;
  AdwToastOverlay *toast_overlay;

  /* Responsive mode */
  gboolean compact;

  /* Session state */
  GHashTable *seen_texts; /* owned; keys are g_strdup(text), values unused */

  /* GListModel-based timeline (primary data source) */
  GnNostrEventModel *event_model; /* owned; reactive model over nostrdb */
  guint model_refresh_pending;    /* debounced refresh source id, 0 if none */

  /* REMOVED: avatar_tex_cache was dead code - never populated.
   * Avatar textures are now managed by the centralized gnostr-avatar-cache module.
   * Use gnostr_avatar_cache_size() for texture cache stats. */

  /* Profile subscription */
  gulong profile_sub_id;        /* signal handler ID for profile events */
  GCancellable *profile_sub_cancellable; /* cancellable for profile sub */

  /* Background profile prefetch (paginate kind-1 authors) */
  gulong bg_prefetch_handler;   /* signal handler ID */
  GCancellable *bg_prefetch_cancellable; /* cancellable for paginator */
  guint bg_prefetch_interval_ms; /* default 250ms between pages */

  /* Demand-driven profile fetch (debounced batch) */
  GPtrArray   *profile_fetch_queue;   /* owned; char* pubkey hex to fetch */
  guint        profile_fetch_source_id; /* GLib source id for debounce */
  guint        profile_fetch_debounce_ms; /* default 150ms */
  GCancellable *profile_fetch_cancellable; /* async cancellable */
  guint        profile_fetch_active;  /* count of active concurrent fetches */
  guint        profile_fetch_max_concurrent; /* max concurrent fetches (default 3) */

  /* Remote signer (NIP-46) session */
  NostrNip46Session *nip46_session; /* owned */

  /* Tuning knobs (UI-editable) */
  guint batch_max;             /* default 5; max items per UI batch post */
  guint post_interval_ms;      /* default 150; max ms before forcing a batch */
  guint eose_quiet_ms;         /* default 150; quiet ms after EOSE to stop */
  guint per_relay_hard_ms;     /* default 5000; hard cap per relay */
  guint default_limit;         /* default 30; timeline default limit */
  gboolean use_since;          /* default FALSE; use since window */
  guint since_seconds;         /* default 3600; when use_since */

  /* Backfill interval */
  guint backfill_interval_sec; /* default 0; disabled when 0 */
  guint backfill_source_id;    /* GLib source id, 0 if none */

  /* GNostrPool live stream */
  GNostrPool      *pool;       /* owned */
  GNostrSubscription *live_sub; /* owned; current live subscription */
  GCancellable    *pool_cancellable; /* owned */
  NostrFilters    *live_filters; /* owned; current live filter set */
  gulong           pool_events_handler; /* signal handler id */
  gboolean         reconnection_in_progress; /* prevent concurrent reconnection attempts */
  guint            health_check_source_id;   /* GLib source id for relay health check */
  const char     **live_urls;          /* owned array pointer + strings */
  size_t           live_url_count;     /* number of current live relays */

  /* Sequential profile batch dispatch state */
  GNostrPool     *profile_pool;          /* owned; GObject pool for profile fetching */
  NostrFilters   *profile_batch_filters; /* owned; kept alive during async query */
  GPtrArray      *profile_batches;       /* owned; elements: GPtrArray* of char* authors */
  guint           profile_batch_pos;     /* next batch index */
  const char    **profile_batch_urls;    /* owned array pointer + strings */
  size_t          profile_batch_url_count;

  /* Debounced local NostrDB profile sweep */
  guint           ndb_sweep_source_id;   /* GLib source id, 0 if none */
  guint           ndb_sweep_debounce_ms; /* default ~150ms */

  /* Sliding window pagination */
  gboolean        loading_older;         /* TRUE while loading older events */
  guint           load_older_batch_size; /* default 30 */

  /* Gift wrap (NIP-59) subscription for DMs */
  uint64_t        sub_gift_wrap;         /* nostrdb subscription ID for kind 1059 */
  char           *user_pubkey_hex;       /* current user's pubkey (64-char hex), NULL if not signed in */
  guint           profile_watch_id;     /* profile provider watch for user's pubkey, 0 if none */
  GPtrArray      *gift_wrap_queue;       /* pending gift wrap events to process */

  /* NIP-17 DM Service for decryption and conversation management */
  GnostrDmService *dm_service;           /* owned; handles gift wrap decryption */

  /* Live relay switching (nostrc-36y.4) */
  gulong           relay_change_handler_id; /* relay config change handler */

  /* Liked events cache (NIP-25 reactions) */
  GHashTable      *liked_events;  /* owned; key=event_id_hex (char*), value=unused (GINT_TO_POINTER(1)) */

  /* nostrc-61s.6: Background operation mode */
  gboolean         background_mode_enabled; /* hide on close instead of quit */
};

/* Forward declarations for local helpers used before their definitions */
static char *client_settings_get_current_npub(void);
static char *hex_encode_lower(const uint8_t *buf, size_t len);
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);
/* Forward declarations needed early due to reordering */
static void gnostr_load_settings(GnostrMainWindow *self);
static unsigned int getenv_uint_default(const char *name, unsigned int defval);
static void start_pool_live(GnostrMainWindow *self);
static void start_profile_subscription(GnostrMainWindow *self);
static void start_bg_profile_prefetch(GnostrMainWindow *self);
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
static void on_timeline_tab_filter_changed(GnostrTimelineView *view, guint type, const char *filter_value, gpointer user_data);
static void on_new_notes_clicked(GtkButton *btn, gpointer user_data);
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data);
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
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_close_requested(GnostrProfilePane *pane, gpointer user_data);
static void on_profile_pane_mute_user_requested(GnostrProfilePane *pane, const char *pubkey_hex, gpointer user_data);
/* Forward declarations for discover page signal handlers */
static void on_discover_open_profile(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data);
static void on_discover_open_communities(GnostrPageDiscover *page, gpointer user_data);
static void on_discover_open_article(GnostrPageDiscover *page, const char *event_id, gint kind, gpointer user_data);
static void on_discover_zap_article(GnostrPageDiscover *page, const char *event_id, const char *pubkey_hex, const char *lud16, gpointer user_data);
static void on_stack_visible_child_changed(GObject *stack, GParamSpec *pspec, gpointer user_data);
/* Forward declaration (nostrc-29) */
void gnostr_main_window_view_thread(GtkWidget *window, const char *root_event_id);
/* Forward declarations for search results view signal handlers (nostrc-29) */
static void on_search_open_note(GnostrSearchResultsView *view, const char *event_id_hex, gpointer user_data);
static void on_search_open_profile(GnostrSearchResultsView *view, const char *pubkey_hex, gpointer user_data);
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
static void on_note_card_repost_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_quote_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_like_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data);
/* Forward declarations for publish context (needed by repost function) */
typedef struct _PublishContext PublishContext;
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);
/* Forward declarations for like context and callback (NIP-25) */
typedef struct _LikeContext LikeContext;
static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);
/* Forward declarations for public repost/quote/like functions (defined after PublishContext) */
void gnostr_main_window_request_repost(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
void gnostr_main_window_request_quote(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, GnostrNoteCardRow *row);

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
typedef struct IdleApplyProfilesCtx {
  GnostrMainWindow *self; /* strong ref */
  GPtrArray *items;       /* ProfileApplyCtx* */
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
  guint applied = 0;
  for (guint i = 0; i < c->items->len; i++) {
    ProfileApplyCtx *it = (ProfileApplyCtx*)g_ptr_array_index(c->items, i);
    if (!it || !it->pubkey_hex || !it->content_json) continue;
    update_meta_from_profile_json(self, it->pubkey_hex, it->content_json);
    applied++;
  }
  /* nostrc-sk8o: Refresh thread view ONCE after all profiles applied (not per-profile) */
  if (applied > 0) {
    refresh_thread_view_profiles_if_visible(self);
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
  extern guint gnostr_media_image_cache_size(void);
  guint avatar_tex = gnostr_avatar_cache_size(); /* in-memory GdkTexture cache */
  guint media_cache = gnostr_media_image_cache_size();

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
    self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, delay, (GSourceFunc)profile_fetch_fire_idle, ref, (GDestroyNotify)g_object_unref);
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
  if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
    gnostr_thread_view_update_profiles(GNOSTR_THREAD_VIEW(thread_view));
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

/* ---- Relay Manager Dialog with NIP-11 support ---- */

/* Forward declarations for relay manager */
typedef struct _RelayManagerCtx RelayManagerCtx;
static void relay_manager_fetch_info(RelayManagerCtx *ctx, const gchar *url);

struct _RelayManagerCtx {
  GtkWindow *window;
  GtkBuilder *builder;
  GtkStringList *relay_model;
  GtkSingleSelection *selection;
  GCancellable *fetch_cancellable;
  gchar *selected_url;  /* Currently selected relay URL */
  gboolean modified;    /* Track if relays list was modified */
  GHashTable *relay_types; /* URL -> GnostrRelayType (as GINT_TO_POINTER) */
  gboolean destroyed;   /* Set to TRUE when window is destroyed */
  gint ref_count;       /* Reference count for safe async cleanup */
  GnostrMainWindow *main_window; /* Reference to main window for pool access */
};

static void relay_manager_ctx_ref(RelayManagerCtx *ctx) {
  if (ctx) g_atomic_int_inc(&ctx->ref_count);
}

static void relay_manager_ctx_unref(RelayManagerCtx *ctx) {
  if (!ctx) return;
  if (!g_atomic_int_dec_and_test(&ctx->ref_count)) return;

  /* Last reference - actually free */
  if (ctx->fetch_cancellable) {
    g_object_unref(ctx->fetch_cancellable);
  }
  if (ctx->relay_types) {
    g_hash_table_destroy(ctx->relay_types);
  }
  g_free(ctx->selected_url);
  g_free(ctx);
}

static void relay_manager_ctx_free(RelayManagerCtx *ctx) {
  if (!ctx) return;
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
  }
  /* Don't unref cancellable here - let unref handle it */
  relay_manager_ctx_unref(ctx);
}

static void relay_manager_update_status(RelayManagerCtx *ctx) {
  GtkLabel *status = GTK_LABEL(gtk_builder_get_object(ctx->builder, "status_label"));
  if (!status) return;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  gchar *text = g_strdup_printf("<small>%u relay%s%s</small>", n, n == 1 ? "" : "s",
                                 ctx->modified ? " (modified)" : "");
  gtk_label_set_markup(status, text);
  g_free(text);
}

/* Helper to clear all children from a container */
static void relay_manager_clear_container(GtkWidget *container) {
  if (!container || !GTK_IS_WIDGET(container)) return;

  /* For GtkFlowBox, use gtk_flow_box_remove to properly handle GtkFlowBoxChild wrappers */
  if (GTK_IS_FLOW_BOX(container)) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_flow_box_remove(GTK_FLOW_BOX(container), child);
    }
  } else if (GTK_IS_BOX(container)) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_box_remove(GTK_BOX(container), child);
    }
  } else {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_widget_unparent(child);
    }
  }
}

/* Helper to create a NIP badge button */
static GtkWidget *relay_manager_create_nip_badge(gint nip_num) {
  gchar *label = g_strdup_printf("NIP-%02d", nip_num);
  GtkWidget *btn = gtk_button_new_with_label(label);
  g_free(label);

  gtk_widget_add_css_class(btn, "pill");
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_set_can_focus(btn, FALSE);

  /* Tooltip with NIP description for common NIPs */
  const gchar *tooltip = NULL;
  switch (nip_num) {
    case 1: tooltip = "Basic protocol flow"; break;
    case 2: tooltip = "Follow List"; break;
    case 4: tooltip = "Encrypted Direct Messages (deprecated)"; break;
    case 5: tooltip = "Event Deletion Request"; break;
    case 9: tooltip = "Event Deletion"; break;
    case 10: tooltip = "Conventions for clients' use of e and p tags"; break;
    case 11: tooltip = "Relay Information Document"; break;
    case 13: tooltip = "Proof of Work"; break;
    case 15: tooltip = "Nostr Marketplace"; break;
    case 17: tooltip = "Private Direct Messages"; break;
    case 20: tooltip = "Expiration"; break;
    case 22: tooltip = "Comment"; break;
    case 25: tooltip = "Reactions"; break;
    case 26: tooltip = "Delegated Event Signing"; break;
    case 28: tooltip = "Public Chat"; break;
    case 29: tooltip = "Relay-based Groups"; break;
    case 40: tooltip = "Relay Authentication"; break;
    case 42: tooltip = "Authentication of clients to relays"; break;
    case 44: tooltip = "Versioned encryption"; break;
    case 45: tooltip = "Counting results"; break;
    case 50: tooltip = "Search Capability"; break;
    case 51: tooltip = "Lists"; break;
    case 56: tooltip = "Reporting"; break;
    case 57: tooltip = "Lightning Zaps"; break;
    case 58: tooltip = "Badges"; break;
    case 59: tooltip = "Gift Wrap"; break;
    case 65: tooltip = "Relay List Metadata"; break;
    case 70: tooltip = "Protected Events"; break;
    case 78: tooltip = "Arbitrary custom app data"; break;
    case 89: tooltip = "Recommended Application Handlers"; break;
    case 90: tooltip = "Data Vending Machine"; break;
    case 94: tooltip = "File Metadata"; break;
    case 96: tooltip = "HTTP File Storage Integration"; break;
    case 98: tooltip = "HTTP Auth"; break;
    case 99: tooltip = "Classified Listings"; break;
  }
  if (tooltip) {
    gchar *full_tooltip = g_strdup_printf("NIP-%02d: %s", nip_num, tooltip);
    gtk_widget_set_tooltip_text(btn, full_tooltip);
    g_free(full_tooltip);
  }

  return btn;
}

/* Helper to create a warning badge */
static GtkWidget *relay_manager_create_warning_badge(const gchar *icon_name, const gchar *label, const gchar *tooltip) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, "warning");

  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(box), icon);

  GtkWidget *lbl = gtk_label_new(label);
  gtk_widget_add_css_class(lbl, "warning");
  gtk_box_append(GTK_BOX(box), lbl);

  if (tooltip) gtk_widget_set_tooltip_text(box, tooltip);

  return box;
}

/* Callback for copy pubkey button */
static void relay_manager_on_pubkey_copy(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const gchar *pubkey = (const gchar*)user_data;
  if (!pubkey) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, pubkey);
}

static void relay_manager_populate_info(RelayManagerCtx *ctx, GnostrRelayInfo *info) {
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (!stack) return;

  /* Basic labels */
  GtkLabel *name = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_name"));
  GtkLabel *desc = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_description"));
  GtkLabel *software = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_software"));
  GtkLabel *contact = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_contact"));
  GtkLabel *limitations = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_limitations"));
  GtkLabel *pubkey = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_pubkey"));

  /* Enhanced UI elements */
  GtkWidget *warnings_box = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_warnings_box"));
  GtkWidget *nips_flowbox = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nips_flowbox"));
  GtkWidget *nips_empty = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nips_empty"));
  GtkWidget *contact_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_contact_link"));
  GtkWidget *pubkey_copy = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_pubkey_copy"));
  GtkWidget *policy_box = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_policy_box"));
  GtkWidget *posting_policy_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_posting_policy_link"));
  GtkWidget *payments_url_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_payments_url_link"));

  /* Populate basic fields */
  if (name) gtk_label_set_text(name, info->name ? info->name : "(not provided)");
  if (desc) gtk_label_set_text(desc, info->description ? info->description : "(not provided)");

  if (software) {
    gchar *sw_text = info->software && info->version
      ? g_strdup_printf("%s v%s", info->software, info->version)
      : (info->software ? g_strdup(info->software) : g_strdup("(not provided)"));
    gtk_label_set_text(software, sw_text);
    g_free(sw_text);
  }

  /* Contact with clickable link */
  if (contact) {
    gtk_label_set_text(contact, info->contact ? info->contact : "(not provided)");
  }
  if (contact_link) {
    if (info->contact && (g_str_has_prefix(info->contact, "mailto:") ||
                          g_str_has_prefix(info->contact, "http://") ||
                          g_str_has_prefix(info->contact, "https://"))) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(contact_link), info->contact);
      gtk_widget_set_visible(contact_link, TRUE);
    } else if (info->contact && strchr(info->contact, '@')) {
      /* Looks like an email without mailto: prefix */
      gchar *mailto = g_strdup_printf("mailto:%s", info->contact);
      gtk_link_button_set_uri(GTK_LINK_BUTTON(contact_link), mailto);
      gtk_widget_set_visible(contact_link, TRUE);
      g_free(mailto);
    } else {
      gtk_widget_set_visible(contact_link, FALSE);
    }
  }

  /* Pubkey with copy button */
  if (pubkey) {
    if (info->pubkey) {
      /* Truncate display but keep full value for copy */
      gchar *truncated = g_strndup(info->pubkey, 16);
      gchar *display = g_strdup_printf("%s...", truncated);
      gtk_label_set_text(pubkey, display);
      gtk_widget_set_tooltip_text(GTK_WIDGET(pubkey), info->pubkey);
      g_free(truncated);
      g_free(display);
    } else {
      gtk_label_set_text(pubkey, "(not provided)");
      gtk_widget_set_tooltip_text(GTK_WIDGET(pubkey), NULL);
    }
  }
  if (pubkey_copy) {
    if (info->pubkey) {
      gtk_widget_set_visible(pubkey_copy, TRUE);
      /* Store pubkey as object data for the callback */
      g_object_set_data_full(G_OBJECT(pubkey_copy), "pubkey", g_strdup(info->pubkey), g_free);
      g_signal_handlers_disconnect_by_func(pubkey_copy, G_CALLBACK(relay_manager_on_pubkey_copy), NULL);
      g_signal_connect(pubkey_copy, "clicked", G_CALLBACK(relay_manager_on_pubkey_copy),
                       g_object_get_data(G_OBJECT(pubkey_copy), "pubkey"));
    } else {
      gtk_widget_set_visible(pubkey_copy, FALSE);
    }
  }

  /* Populate NIP badges */
  if (nips_flowbox && GTK_IS_FLOW_BOX(nips_flowbox)) {
    relay_manager_clear_container(nips_flowbox);

    if (info->supported_nips && info->supported_nips_count > 0) {
      for (gsize i = 0; i < info->supported_nips_count; i++) {
        GtkWidget *badge = relay_manager_create_nip_badge(info->supported_nips[i]);
        if (badge && GTK_IS_WIDGET(badge)) {
          gtk_flow_box_append(GTK_FLOW_BOX(nips_flowbox), badge);
        }
      }
      gtk_widget_set_visible(nips_flowbox, TRUE);
      if (nips_empty) gtk_widget_set_visible(nips_empty, FALSE);
    } else {
      gtk_widget_set_visible(nips_flowbox, FALSE);
      if (nips_empty) gtk_widget_set_visible(nips_empty, TRUE);
    }
  }

  /* Limitations */
  if (limitations) {
    gchar *lim_str = gnostr_relay_info_format_limitations(info);
    gtk_label_set_text(limitations, lim_str);
    g_free(lim_str);
  }

  /* Warning indicators */
  if (warnings_box) {
    relay_manager_clear_container(warnings_box);
    gboolean has_warnings = FALSE;

    if (info->auth_required) {
      GtkWidget *badge = relay_manager_create_warning_badge("dialog-password-symbolic", "Auth Required",
        "This relay requires authentication (NIP-42). You may need to sign in to use it.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    if (info->payment_required) {
      GtkWidget *badge = relay_manager_create_warning_badge("emblem-money-symbolic", "Payment Required",
        "This relay requires payment to use.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    if (info->restricted_writes) {
      GtkWidget *badge = relay_manager_create_warning_badge("action-unavailable-symbolic", "Restricted Writes",
        "This relay has write restrictions. Not all events may be accepted.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    gtk_widget_set_visible(warnings_box, has_warnings);
  }

  /* Policy links */
  gboolean has_policy_links = FALSE;
  if (posting_policy_link) {
    if (info->posting_policy) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(posting_policy_link), info->posting_policy);
      gtk_widget_set_visible(posting_policy_link, TRUE);
      has_policy_links = TRUE;
    } else {
      gtk_widget_set_visible(posting_policy_link, FALSE);
    }
  }
  if (payments_url_link) {
    if (info->payments_url) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(payments_url_link), info->payments_url);
      gtk_widget_set_visible(payments_url_link, TRUE);
      has_policy_links = TRUE;
    } else {
      gtk_widget_set_visible(payments_url_link, FALSE);
    }
  }
  if (policy_box) {
    gtk_widget_set_visible(policy_box, has_policy_links);
  }

  /* NIP-65 Permission display */
  GtkWidget *nip65_icon = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nip65_icon"));
  GtkLabel *nip65_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_nip65_label"));
  if (nip65_icon && nip65_label && ctx->selected_url && ctx->relay_types) {
    gpointer type_ptr = g_hash_table_lookup(ctx->relay_types, ctx->selected_url);
    GnostrRelayType type = type_ptr ? GPOINTER_TO_INT(type_ptr) : GNOSTR_RELAY_READWRITE;
    const gchar *icon_name;
    const gchar *label_text;
    switch (type) {
      case GNOSTR_RELAY_READ:
        icon_name = "go-down-symbolic";
        label_text = "Read Only";
        break;
      case GNOSTR_RELAY_WRITE:
        icon_name = "go-up-symbolic";
        label_text = "Write Only";
        break;
      case GNOSTR_RELAY_READWRITE:
      default:
        icon_name = "network-transmit-receive-symbolic";
        label_text = "Read + Write";
        break;
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(nip65_icon), icon_name);
    gtk_label_set_text(nip65_label, label_text);
  }

  gtk_stack_set_visible_child_name(stack, "info");
}

static void on_relay_info_fetched(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;

  /* Always consume the result to avoid leaks */
  GError *err = NULL;
  GnostrRelayInfo *info = gnostr_relay_info_fetch_finish(result, &err);

  /* CRITICAL: Check if context was destroyed before accessing any GTK widgets.
   * The context is kept alive via ref counting until this callback completes. */
  if (!ctx || ctx->destroyed || !ctx->builder) {
    /* Context was destroyed - clean up and release our reference */
    if (info) gnostr_relay_info_free(info);
    g_clear_error(&err);
    if (ctx) relay_manager_ctx_unref(ctx);
    return;
  }

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (!stack) {
    if (info) gnostr_relay_info_free(info);
    g_clear_error(&err);
    relay_manager_ctx_unref(ctx);
    return;
  }

  if (err) {
    GtkLabel *error_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_error_label"));
    if (error_label) {
      gchar *msg = g_strdup_printf("Failed to fetch relay info:\n%s", err->message);
      gtk_label_set_text(error_label, msg);
      g_free(msg);
    }
    gtk_stack_set_visible_child_name(stack, "error");
    g_clear_error(&err);
    relay_manager_ctx_unref(ctx);
    return;
  }

  if (!info) {
    GtkLabel *error_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_error_label"));
    if (error_label) gtk_label_set_text(error_label, "Failed to parse relay info");
    gtk_stack_set_visible_child_name(stack, "error");
    relay_manager_ctx_unref(ctx);
    return;
  }

  relay_manager_populate_info(ctx, info);
  gnostr_relay_info_free(info);
  relay_manager_ctx_unref(ctx);
}

static void relay_manager_fetch_info(RelayManagerCtx *ctx, const gchar *url) {
  if (!ctx || !url) return;

  /* Cancel any pending fetch - the callback will handle its own unref */
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
    g_object_unref(ctx->fetch_cancellable);
    /* Note: Don't unref here - the cancelled callback will do it */
  }
  ctx->fetch_cancellable = g_cancellable_new();

  /* Copy url first since it might be ctx->selected_url (from retry button) */
  gchar *url_copy = g_strdup(url);
  g_free(ctx->selected_url);
  ctx->selected_url = url_copy;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "loading");

  /* Take a reference for the async callback */
  relay_manager_ctx_ref(ctx);
  gnostr_relay_info_fetch_async(ctx->selected_url, ctx->fetch_cancellable, on_relay_info_fetched, ctx);
}

static void on_relay_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data) {
  (void)position; (void)n_items;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint selected_pos = gtk_single_selection_get_selected(sel);
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_single_selection_get_selected_item(sel));

  if (obj) {
    const gchar *url = gtk_string_object_get_string(obj);
    if (url && *url) {
      relay_manager_fetch_info(ctx, url);
    }
  } else {
    GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
    if (stack) gtk_stack_set_visible_child_name(stack, "empty");
  }
}

static void relay_manager_on_retry_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->selected_url) return;
  relay_manager_fetch_info(ctx, ctx->selected_url);
}

static void relay_manager_on_add_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "relay_entry"));
  if (!entry) return;

  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (!text || !*text) return;

  gchar *normalized = gnostr_normalize_relay_url(text);
  if (!normalized) {
    /* Invalid URL */
    return;
  }

  /* Check for duplicates */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  for (guint i = 0; i < n; i++) {
    GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(ctx->relay_model), i));
    if (obj) {
      const gchar *existing = gtk_string_object_get_string(obj);
      if (existing && g_strcmp0(existing, normalized) == 0) {
        g_object_unref(obj);
        g_free(normalized);
        return; /* Already exists */
      }
      g_object_unref(obj);
    }
  }

  gtk_string_list_append(ctx->relay_model, normalized);
  /* New relays default to read+write */
  g_hash_table_insert(ctx->relay_types, g_strdup(normalized), GINT_TO_POINTER(GNOSTR_RELAY_READWRITE));
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);
  g_free(normalized);
}

static void relay_manager_on_entry_activate(GtkEntry *entry, gpointer user_data) {
  relay_manager_on_add_clicked(NULL, user_data);
}

static void relay_manager_on_remove_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->selection) return;

  guint pos = gtk_single_selection_get_selected(ctx->selection);
  if (pos == GTK_INVALID_LIST_POSITION) return;

  gtk_string_list_remove(ctx->relay_model, pos);
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);

  /* Clear info pane */
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "empty");
}

static void relay_manager_on_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  /* Collect all relays with their types as NIP-65 entries */
  GPtrArray *relays = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  for (guint i = 0; i < n; i++) {
    GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(ctx->relay_model), i));
    if (obj) {
      const gchar *url = gtk_string_object_get_string(obj);
      if (url && *url) {
        GnostrNip65Relay *relay = g_new0(GnostrNip65Relay, 1);
        relay->url = g_strdup(url);
        /* Get type from hash table, default to READWRITE */
        gpointer stored = g_hash_table_lookup(ctx->relay_types, url);
        relay->type = stored ? GPOINTER_TO_INT(stored) : GNOSTR_RELAY_READWRITE;
        g_ptr_array_add(relays, relay);
      }
      g_object_unref(obj);
    }
  }

  gnostr_save_nip65_relays(relays);

  /* Publish NIP-65 relay list event to relays */
  g_debug("[RELAYS] Publishing NIP-65 relay list with %u relays", relays->len);
  gnostr_nip65_publish_async(relays, NULL, NULL);

  g_ptr_array_unref(relays);

  ctx->modified = FALSE;
  relay_manager_update_status(ctx);
  gtk_window_close(ctx->window);
}

static void relay_manager_on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->window) return;
  gtk_window_close(ctx->window);
}

static void relay_manager_on_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  /* Mark as destroyed so pending callbacks know not to access widgets */
  ctx->destroyed = TRUE;
  ctx->window = NULL;

  if (ctx->builder) {
    g_object_unref(ctx->builder);
    ctx->builder = NULL;
  }

  /* Cancel any pending fetch - this will cause the callback to be invoked
   * with a cancellation error, at which point it will see destroyed=TRUE
   * and clean up properly */
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
  }

  /* Free context now - the callback checks destroyed flag before accessing fields */
  relay_manager_ctx_free(ctx);
}

/* ============== NIP-66 Relay Discovery Dialog ============== */

typedef struct {
  GtkWindow *window;
  GtkBuilder *builder;
  GtkListStore *list_store;
  GtkSingleSelection *selection;
  GCancellable *cancellable;
  GPtrArray *discovered_relays;  /* GnostrNip66RelayMeta* array */
  GHashTable *selected_urls;  /* URLs selected for addition */
  GHashTable *seen_urls;  /* URLs already added (for deduplication) */
  RelayManagerCtx *relay_manager_ctx;  /* Parent relay manager context */
  guint filter_timeout_id;  /* Debounce timer for filter updates */
  gboolean filter_pending;  /* Flag indicating filter update is pending */
} RelayDiscoveryCtx;

static void relay_discovery_ctx_free(RelayDiscoveryCtx *ctx) {
  if (!ctx) return;
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }
  if (ctx->cancellable) {
    g_cancellable_cancel(ctx->cancellable);
    g_object_unref(ctx->cancellable);
  }
  if (ctx->discovered_relays) {
    g_ptr_array_unref(ctx->discovered_relays);
  }
  if (ctx->selected_urls) {
    g_hash_table_destroy(ctx->selected_urls);
  }
  if (ctx->seen_urls) {
    g_hash_table_destroy(ctx->seen_urls);
  }
  g_free(ctx);
}

/* Map dropdown selections to filter values */
static const gchar *s_region_values[] = {
  NULL,  /* All Regions */
  "North America",
  "Europe",
  "Asia Pacific",
  "South America",
  "Middle East",
  "Africa",
  "Other"
};

static const gint s_nip_values[] = {
  0,   /* Any NIPs */
  1,   /* NIP-01 */
  11,  /* NIP-11 */
  17,  /* NIP-17 */
  42,  /* NIP-42 */
  50,  /* NIP-50 */
  57,  /* NIP-57 */
  59,  /* NIP-59 */
  65   /* NIP-65 */
};

/* Forward declaration */
static void relay_discovery_apply_filter(RelayDiscoveryCtx *ctx);

static void relay_discovery_update_results_label(RelayDiscoveryCtx *ctx, guint count) {
  GtkLabel *label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "results_label"));
  if (label) {
    gchar *text = g_strdup_printf("<small>Found %u relay%s</small>", count, count == 1 ? "" : "s");
    gtk_label_set_markup(label, text);
    g_free(text);
  }
}

/* Row widget references for discovery list */
typedef struct {
  GtkWidget *check;
  GtkWidget *name_label;
  GtkWidget *url_label;
  GtkWidget *region_label;
  GtkWidget *status_icon;
  GtkWidget *nips_label;
  GtkWidget *uptime_label;
  GtkWidget *latency_label;
} DiscoveryRowWidgets;

static void relay_discovery_setup_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory; (void)user_data;

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(row, 6);
  gtk_widget_set_margin_bottom(row, 6);
  gtk_widget_set_margin_start(row, 8);
  gtk_widget_set_margin_end(row, 8);

  /* Checkbox for selection */
  GtkWidget *check = gtk_check_button_new();
  gtk_box_append(GTK_BOX(row), check);

  /* Status icon */
  GtkWidget *status_icon = gtk_image_new_from_icon_name("network-transmit-receive-symbolic");
  gtk_widget_set_size_request(status_icon, 16, 16);
  gtk_box_append(GTK_BOX(row), status_icon);

  /* Main content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content, TRUE);

  GtkWidget *name_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_box_append(GTK_BOX(content), name_label);

  GtkWidget *url_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(url_label, "dim-label");
  gtk_widget_add_css_class(url_label, "caption");
  gtk_box_append(GTK_BOX(content), url_label);

  GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(info_box, 2);

  GtkWidget *region_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(region_label, "caption");
  gtk_widget_add_css_class(region_label, "dim-label");
  gtk_box_append(GTK_BOX(info_box), region_label);

  GtkWidget *nips_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(nips_label, "caption");
  gtk_widget_add_css_class(nips_label, "dim-label");
  gtk_box_append(GTK_BOX(info_box), nips_label);

  gtk_box_append(GTK_BOX(content), info_box);
  gtk_box_append(GTK_BOX(row), content);

  /* Stats column */
  GtkWidget *stats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign(stats, GTK_ALIGN_END);

  GtkWidget *uptime_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(uptime_label, "caption");
  gtk_box_append(GTK_BOX(stats), uptime_label);

  GtkWidget *latency_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(latency_label, "caption");
  gtk_widget_add_css_class(latency_label, "dim-label");
  gtk_box_append(GTK_BOX(stats), latency_label);

  gtk_box_append(GTK_BOX(row), stats);

  /* Store widget references */
  DiscoveryRowWidgets *widgets = g_new0(DiscoveryRowWidgets, 1);
  widgets->check = check;
  widgets->name_label = name_label;
  widgets->url_label = url_label;
  widgets->region_label = region_label;
  widgets->status_icon = status_icon;
  widgets->nips_label = nips_label;
  widgets->uptime_label = uptime_label;
  widgets->latency_label = latency_label;
  g_object_set_data_full(G_OBJECT(row), "widgets", widgets, g_free);

  gtk_list_item_set_child(list_item, row);
}

static void on_discovery_check_toggled(GtkCheckButton *check, gpointer user_data);

static void relay_discovery_bind_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  GtkWidget *row = gtk_list_item_get_child(list_item);
  GObject *item = gtk_list_item_get_item(list_item);

  if (!row || !item) return;

  DiscoveryRowWidgets *widgets = g_object_get_data(G_OBJECT(row), "widgets");
  if (!widgets) return;

  /* nostrc-dth1: Get relay URL from the GtkStringObject in the filtered list,
   * then look up the meta by URL. Using position was wrong because the list
   * is filtered - positions don't match the original discovered_relays array. */
  const gchar *relay_url = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
  if (!relay_url) return;

  /* Find the meta for this URL */
  GnostrNip66RelayMeta *meta = NULL;
  for (guint i = 0; i < ctx->discovered_relays->len; i++) {
    GnostrNip66RelayMeta *m = g_ptr_array_index(ctx->discovered_relays, i);
    if (m && m->relay_url && g_strcmp0(m->relay_url, relay_url) == 0) {
      meta = m;
      break;
    }
  }
  if (!meta) return;

  /* Bind data */
  if (meta->name && *meta->name) {
    gtk_label_set_text(GTK_LABEL(widgets->name_label), meta->name);
  } else {
    /* Use hostname from URL */
    gchar *hostname = NULL;
    if (meta->relay_url) {
      const gchar *start = meta->relay_url;
      if (g_str_has_prefix(start, "wss://")) start += 6;
      else if (g_str_has_prefix(start, "ws://")) start += 5;
      const gchar *end = start;
      while (*end && *end != '/' && *end != ':') end++;
      hostname = g_strndup(start, end - start);
    }
    gtk_label_set_text(GTK_LABEL(widgets->name_label), hostname ? hostname : "(unknown)");
    g_free(hostname);
  }

  gtk_label_set_text(GTK_LABEL(widgets->url_label), meta->relay_url ? meta->relay_url : "");

  /* Region */
  gchar *region_text = g_strdup_printf("%s%s%s",
    meta->region ? meta->region : "",
    (meta->region && meta->country_code) ? " " : "",
    meta->country_code ? meta->country_code : "");
  gtk_label_set_text(GTK_LABEL(widgets->region_label), region_text);
  g_free(region_text);

  /* NIPs summary */
  if (meta->supported_nips_count > 0) {
    gchar *nips_text = g_strdup_printf("%zu NIPs", meta->supported_nips_count);
    gtk_label_set_text(GTK_LABEL(widgets->nips_label), nips_text);
    g_free(nips_text);
  } else {
    gtk_label_set_text(GTK_LABEL(widgets->nips_label), "");
  }

  /* Status */
  if (meta->is_online) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-transmit-receive-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "error");
    gtk_widget_add_css_class(widgets->status_icon, "success");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Online");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-offline-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "success");
    gtk_widget_add_css_class(widgets->status_icon, "error");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Offline");
  }

  /* Uptime */
  gchar *uptime_str = gnostr_nip66_format_uptime(meta->uptime_percent);
  gtk_label_set_text(GTK_LABEL(widgets->uptime_label), uptime_str);
  if (meta->uptime_percent >= 99.0) {
    gtk_widget_add_css_class(widgets->uptime_label, "success");
  } else if (meta->uptime_percent >= 90.0) {
    gtk_widget_remove_css_class(widgets->uptime_label, "success");
  } else {
    gtk_widget_add_css_class(widgets->uptime_label, "warning");
  }
  g_free(uptime_str);

  /* Latency */
  gchar *latency_str = gnostr_nip66_format_latency(meta->latency_ms);
  gtk_label_set_text(GTK_LABEL(widgets->latency_label), latency_str);
  g_free(latency_str);

  /* Checkbox state */
  gboolean is_selected = ctx->selected_urls &&
    g_hash_table_contains(ctx->selected_urls, meta->relay_url);
  g_signal_handlers_block_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->check), is_selected);
  g_signal_handlers_unblock_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);

  /* Store URL for checkbox callback */
  g_object_set_data_full(G_OBJECT(widgets->check), "relay_url",
    g_strdup(meta->relay_url), g_free);

  /* Connect checkbox signal */
  g_signal_handlers_disconnect_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);
  g_signal_connect(widgets->check, "toggled", G_CALLBACK(on_discovery_check_toggled), ctx);
}

static void on_discovery_check_toggled(GtkCheckButton *check, gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->selected_urls) return;

  const gchar *url = g_object_get_data(G_OBJECT(check), "relay_url");
  if (!url) return;

  gboolean active = gtk_check_button_get_active(check);
  if (active) {
    g_hash_table_add(ctx->selected_urls, g_strdup(url));
  } else {
    g_hash_table_remove(ctx->selected_urls, url);
  }

  /* Update Add Selected button sensitivity */
  GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "btn_add_selected"));
  if (btn) {
    guint count = g_hash_table_size(ctx->selected_urls);
    gtk_widget_set_sensitive(btn, count > 0);
    gchar *label = count > 0 ? g_strdup_printf("_Add %u Selected", count) : g_strdup("_Add Selected");
    gtk_button_set_label(GTK_BUTTON(btn), label);
    g_free(label);
  }
}

/* Debounce callback for filter updates during streaming */
static gboolean relay_discovery_debounced_filter(gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx) return G_SOURCE_REMOVE;

  ctx->filter_timeout_id = 0;
  ctx->filter_pending = FALSE;
  relay_discovery_apply_filter(ctx);
  return G_SOURCE_REMOVE;
}

/* LEGITIMATE TIMEOUT - Debounced filter update (100ms delay).
 * Batches rapid relay discovery events into single UI updates.
 * nostrc-b0h: Audited - debounce for batching is appropriate. */
static void relay_discovery_schedule_filter_update(RelayDiscoveryCtx *ctx) {
  if (!ctx) return;

  ctx->filter_pending = TRUE;

  /* Reset timer if already scheduled (restartable debounce) */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
  }

  ctx->filter_timeout_id = g_timeout_add(100, relay_discovery_debounced_filter, ctx);
}

/* Streaming callback: called for each relay as it's discovered */
static void relay_discovery_on_relay_found(GnostrNip66RelayMeta *meta, gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->builder || !meta || !meta->relay_url) return;

  /* Deduplicate by URL at UI level (belt-and-suspenders with streaming dedup) */
  if (ctx->seen_urls) {
    gchar *url_lower = g_ascii_strdown(meta->relay_url, -1);
    if (g_hash_table_contains(ctx->seen_urls, url_lower)) {
      g_free(url_lower);
      return;  /* Skip duplicate */
    }
    g_hash_table_add(ctx->seen_urls, url_lower);  /* Takes ownership */
  }

  /* Copy the meta since we need to own it */
  GnostrNip66RelayMeta *meta_copy = g_new0(GnostrNip66RelayMeta, 1);
  meta_copy->relay_url = g_strdup(meta->relay_url);
  meta_copy->name = meta->name ? g_strdup(meta->name) : NULL;
  meta_copy->description = meta->description ? g_strdup(meta->description) : NULL;
  meta_copy->region = meta->region ? g_strdup(meta->region) : NULL;
  meta_copy->country_code = meta->country_code ? g_strdup(meta->country_code) : NULL;
  meta_copy->software = meta->software ? g_strdup(meta->software) : NULL;
  meta_copy->version = meta->version ? g_strdup(meta->version) : NULL;
  meta_copy->has_status = meta->has_status;
  meta_copy->is_online = meta->is_online;
  meta_copy->payment_required = meta->payment_required;
  meta_copy->auth_required = meta->auth_required;
  meta_copy->uptime_percent = meta->uptime_percent;
  meta_copy->latency_ms = meta->latency_ms;
  meta_copy->network = meta->network;
  if (meta->supported_nips && meta->supported_nips_count > 0) {
    meta_copy->supported_nips = g_new(gint, meta->supported_nips_count);
    memcpy(meta_copy->supported_nips, meta->supported_nips, meta->supported_nips_count * sizeof(gint));
    meta_copy->supported_nips_count = meta->supported_nips_count;
  }

  /* Add to discovered relays */
  g_ptr_array_add(ctx->discovered_relays, meta_copy);

  /* Switch from loading to results on first relay */
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (stack) {
    const gchar *current = gtk_stack_get_visible_child_name(stack);
    if (g_strcmp0(current, "loading") == 0) {
      gtk_stack_set_visible_child_name(stack, "results");
    }
  }

  /* Schedule debounced filter update instead of updating immediately */
  relay_discovery_schedule_filter_update(ctx);
}

static void relay_discovery_on_complete(GPtrArray *relays, GPtrArray *monitors,
                                         GError *error, gpointer user_data) {
  (void)relays;  /* We already have relays from streaming callbacks */
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (!stack) return;

  if (error) {
    GtkLabel *err_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "error_label"));
    if (err_label) gtk_label_set_text(err_label, error->message);
    gtk_stack_set_visible_child_name(stack, "error");
    g_error_free(error);
    return;
  }

  /* Cancel any pending debounced update */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }

  /* Final update - apply filter immediately */
  relay_discovery_apply_filter(ctx);

  /* If no relays found, show empty state */
  if (ctx->discovered_relays->len == 0) {
    gtk_stack_set_visible_child_name(stack, "empty");
  }

  /* Clean up monitors array */
  if (monitors) g_ptr_array_unref(monitors);
}

static void relay_discovery_apply_filter(RelayDiscoveryCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (!stack) return;

  /* Get filter values */
  GtkDropDown *region_dd = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "filter_region"));
  GtkDropDown *nip_dd = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "filter_nip"));
  GtkCheckButton *online_check = GTK_CHECK_BUTTON(gtk_builder_get_object(ctx->builder, "filter_online"));
  GtkCheckButton *free_check = GTK_CHECK_BUTTON(gtk_builder_get_object(ctx->builder, "filter_free"));

  guint region_idx = region_dd ? gtk_drop_down_get_selected(region_dd) : 0;
  guint nip_idx = nip_dd ? gtk_drop_down_get_selected(nip_dd) : 0;
  gboolean online_only = online_check ? gtk_check_button_get_active(online_check) : TRUE;
  gboolean free_only = free_check ? gtk_check_button_get_active(free_check) : FALSE;

  const gchar *region_filter = (region_idx < G_N_ELEMENTS(s_region_values)) ?
    s_region_values[region_idx] : NULL;
  gint nip_filter = (nip_idx < G_N_ELEMENTS(s_nip_values)) ?
    s_nip_values[nip_idx] : 0;

  /* Build filtered list model */
  GtkStringList *filtered_model = gtk_string_list_new(NULL);
  guint match_count = 0;

  for (guint i = 0; i < ctx->discovered_relays->len; i++) {
    GnostrNip66RelayMeta *meta = g_ptr_array_index(ctx->discovered_relays, i);
    if (!meta || !meta->relay_url) continue;

    gboolean matches = TRUE;

    /* Online filter - only exclude relays explicitly marked offline.
     * Treat unknown status (no l tag) as possibly online. */
    if (online_only && meta->has_status && !meta->is_online) matches = FALSE;

    /* Free filter */
    if (free_only && meta->payment_required) matches = FALSE;

    /* Region filter */
    if (region_filter && *region_filter) {
      if (!meta->region || g_ascii_strcasecmp(meta->region, region_filter) != 0) {
        matches = FALSE;
      }
    }

    /* NIP filter */
    if (nip_filter > 0) {
      if (!gnostr_nip66_relay_supports_nip(meta, nip_filter)) {
        matches = FALSE;
      }
    }

    if (matches) {
      gtk_string_list_append(filtered_model, meta->relay_url);
      match_count++;
    }
  }

  /* Update list view */
  GtkListView *list_view = GTK_LIST_VIEW(gtk_builder_get_object(ctx->builder, "relay_list"));
  if (list_view) {
    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(filtered_model));
    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_single_selection_set_can_unselect(selection, TRUE);

    /* Create factory if needed */
    if (!gtk_list_view_get_factory(list_view)) {
      GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
      g_signal_connect(factory, "setup", G_CALLBACK(relay_discovery_setup_factory_cb), ctx);
      g_signal_connect(factory, "bind", G_CALLBACK(relay_discovery_bind_factory_cb), ctx);
      gtk_list_view_set_factory(list_view, factory);
      g_object_unref(factory);
    }

    gtk_list_view_set_model(list_view, GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);
  }

  /* Update results label and show appropriate page */
  relay_discovery_update_results_label(ctx, match_count);

  if (match_count > 0) {
    gtk_stack_set_visible_child_name(stack, "results");
  } else if (ctx->discovered_relays->len > 0) {
    /* Have relays but none match filter */
    gtk_stack_set_visible_child_name(stack, "empty");
  } else {
    gtk_stack_set_visible_child_name(stack, "empty");
  }
}

static void relay_discovery_start_fetch(RelayDiscoveryCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "loading");

  /* Cancel any pending filter update */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }

  /* Cancel any pending operation */
  if (ctx->cancellable) {
    g_cancellable_cancel(ctx->cancellable);
    g_object_unref(ctx->cancellable);
  }
  ctx->cancellable = g_cancellable_new();

  /* Clear previous results for fresh fetch */
  if (ctx->discovered_relays) {
    g_ptr_array_set_size(ctx->discovered_relays, 0);
  }

  /* Clear seen URLs for deduplication */
  if (ctx->seen_urls) {
    g_hash_table_remove_all(ctx->seen_urls);
  }

  /* Start streaming discovery - relays appear as they're discovered */
  gnostr_nip66_discover_relays_streaming_async(
    relay_discovery_on_relay_found,
    relay_discovery_on_complete,
    ctx,
    ctx->cancellable);
}

static void relay_discovery_on_filter_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)obj; (void)pspec;
  relay_discovery_apply_filter((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_check_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  relay_discovery_apply_filter((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  relay_discovery_start_fetch((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (ctx && ctx->window) gtk_window_close(ctx->window);
}

static void relay_discovery_on_add_selected_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->selected_urls || !ctx->relay_manager_ctx) return;

  GHashTableIter iter;
  gpointer key, value;
  guint added = 0;

  g_hash_table_iter_init(&iter, ctx->selected_urls);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const gchar *url = (const gchar*)key;
    if (!url || !*url) continue;

    /* Check if already in relay manager */
    gboolean exists = FALSE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_manager_ctx->relay_model));
    for (guint i = 0; i < n; i++) {
      GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(
        G_LIST_MODEL(ctx->relay_manager_ctx->relay_model), i));
      if (obj) {
        const gchar *existing = gtk_string_object_get_string(obj);
        if (existing && g_ascii_strcasecmp(existing, url) == 0) {
          exists = TRUE;
        }
        g_object_unref(obj);
        if (exists) break;
      }
    }

    if (!exists) {
      gtk_string_list_append(ctx->relay_manager_ctx->relay_model, url);
      g_hash_table_insert(ctx->relay_manager_ctx->relay_types,
        g_strdup(url), GINT_TO_POINTER(GNOSTR_RELAY_READWRITE));
      added++;
    }
  }

  if (added > 0) {
    ctx->relay_manager_ctx->modified = TRUE;
    relay_manager_update_status(ctx->relay_manager_ctx);
  }

  /* Close discovery dialog */
  if (ctx->window) gtk_window_close(ctx->window);
}

static void relay_discovery_on_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (ctx && ctx->builder) {
    g_object_unref(ctx->builder);
    ctx->builder = NULL;
  }
  relay_discovery_ctx_free(ctx);
}

static void relay_manager_on_discover_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *manager_ctx = (RelayManagerCtx*)user_data;
  if (!manager_ctx) return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-relay-discovery.ui");
  if (!builder) {
    g_warning("Failed to load relay discovery UI");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "relay_discovery_window"));
  if (!win) {
    g_object_unref(builder);
    return;
  }

  gtk_window_set_transient_for(win, manager_ctx->window);
  gtk_window_set_modal(win, TRUE);

  /* Create discovery context */
  RelayDiscoveryCtx *ctx = g_new0(RelayDiscoveryCtx, 1);
  ctx->window = win;
  ctx->builder = builder;
  ctx->relay_manager_ctx = manager_ctx;
  ctx->selected_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->seen_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->discovered_relays = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gnostr_nip66_relay_meta_free);

  /* Wire filter controls */
  GtkDropDown *region_dd = GTK_DROP_DOWN(gtk_builder_get_object(builder, "filter_region"));
  GtkDropDown *nip_dd = GTK_DROP_DOWN(gtk_builder_get_object(builder, "filter_nip"));
  GtkCheckButton *online_check = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "filter_online"));
  GtkCheckButton *free_check = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "filter_free"));

  if (region_dd) g_signal_connect(region_dd, "notify::selected",
    G_CALLBACK(relay_discovery_on_filter_changed), ctx);
  if (nip_dd) g_signal_connect(nip_dd, "notify::selected",
    G_CALLBACK(relay_discovery_on_filter_changed), ctx);
  if (online_check) g_signal_connect(online_check, "toggled",
    G_CALLBACK(relay_discovery_on_check_toggled), ctx);
  if (free_check) g_signal_connect(free_check, "toggled",
    G_CALLBACK(relay_discovery_on_check_toggled), ctx);

  /* Wire buttons */
  GtkWidget *btn_refresh = GTK_WIDGET(gtk_builder_get_object(builder, "btn_refresh"));
  GtkWidget *btn_refresh_empty = GTK_WIDGET(gtk_builder_get_object(builder, "btn_refresh_empty"));
  GtkWidget *btn_retry = GTK_WIDGET(gtk_builder_get_object(builder, "btn_retry"));
  GtkWidget *btn_close = GTK_WIDGET(gtk_builder_get_object(builder, "btn_close"));
  GtkWidget *btn_add_selected = GTK_WIDGET(gtk_builder_get_object(builder, "btn_add_selected"));

  if (btn_refresh) g_signal_connect(btn_refresh, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_refresh_empty) g_signal_connect(btn_refresh_empty, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_retry) g_signal_connect(btn_retry, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_close) g_signal_connect(btn_close, "clicked",
    G_CALLBACK(relay_discovery_on_close_clicked), ctx);
  if (btn_add_selected) g_signal_connect(btn_add_selected, "clicked",
    G_CALLBACK(relay_discovery_on_add_selected_clicked), ctx);

  g_signal_connect(win, "destroy", G_CALLBACK(relay_discovery_on_destroy), ctx);

  /* Start initial fetch */
  relay_discovery_start_fetch(ctx);

  gtk_window_present(win);
}

/* Structure to hold row widget references */
typedef struct {
  GtkWidget *name_label;
  GtkWidget *url_label;
  GtkWidget *status_icon;
  GtkWidget *connection_icon;  /* Live connection status indicator */
  GtkWidget *nips_box;
  GtkWidget *warning_icon;
  GtkWidget *type_dropdown;
  GtkWidget *type_icon;
} RelayRowWidgets;

static void relay_manager_setup_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory; (void)user_data;

  /* Create a more sophisticated row layout */
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Connection status indicator (live connection) */
  GtkWidget *connection_icon = gtk_image_new_from_icon_name("network-offline-symbolic");
  gtk_widget_set_size_request(connection_icon, 16, 16);
  gtk_widget_add_css_class(connection_icon, "dim-label");
  gtk_widget_set_tooltip_text(connection_icon, "Not connected");
  gtk_box_append(GTK_BOX(row), connection_icon);

  /* Status indicator (relay info fetch status) */
  GtkWidget *status_icon = gtk_image_new_from_icon_name("network-offline-symbolic");
  gtk_widget_set_size_request(status_icon, 16, 16);
  gtk_widget_add_css_class(status_icon, "dim-label");
  gtk_box_append(GTK_BOX(row), status_icon);

  /* Main content box (name + url + nips) */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content, TRUE);

  /* Name label (primary) */
  GtkWidget *name_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_box_append(GTK_BOX(content), name_label);

  /* URL label (secondary, smaller) */
  GtkWidget *url_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(url_label, "dim-label");
  gtk_widget_add_css_class(url_label, "caption");
  gtk_box_append(GTK_BOX(content), url_label);

  /* NIP badges row (key NIPs only) */
  GtkWidget *nips_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_top(nips_box, 2);
  gtk_box_append(GTK_BOX(content), nips_box);

  gtk_box_append(GTK_BOX(row), content);

  /* Type indicator icon (shows R/W/RW) */
  GtkWidget *type_icon = gtk_image_new_from_icon_name("network-transmit-receive-symbolic");
  gtk_widget_set_size_request(type_icon, 16, 16);
  gtk_widget_set_tooltip_text(type_icon, "Read + Write");
  gtk_box_append(GTK_BOX(row), type_icon);

  /* Type dropdown (Read/Write/Both) */
  const char *type_options[] = {"R+W", "Read", "Write", NULL};
  GtkWidget *type_dropdown = gtk_drop_down_new_from_strings(type_options);
  gtk_widget_set_size_request(type_dropdown, 80, -1);
  gtk_widget_set_valign(type_dropdown, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(type_dropdown, "Relay permission: Read+Write, Read-only, or Write-only");
  gtk_box_append(GTK_BOX(row), type_dropdown);

  /* Warning icon (for auth/payment required) */
  GtkWidget *warning_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_set_visible(warning_icon, FALSE);
  gtk_widget_add_css_class(warning_icon, "warning");
  gtk_box_append(GTK_BOX(row), warning_icon);

  /* Store widget references for binding */
  RelayRowWidgets *widgets = g_new0(RelayRowWidgets, 1);
  widgets->name_label = name_label;
  widgets->url_label = url_label;
  widgets->status_icon = status_icon;
  widgets->connection_icon = connection_icon;
  widgets->nips_box = nips_box;
  widgets->warning_icon = warning_icon;
  widgets->type_dropdown = type_dropdown;
  widgets->type_icon = type_icon;
  g_object_set_data_full(G_OBJECT(row), "widgets", widgets, g_free);

  gtk_list_item_set_child(list_item, row);
}

/* Helper to extract hostname from URL for display */
static gchar *relay_manager_extract_hostname(const gchar *url) {
  if (!url) return NULL;

  /* Skip protocol prefix */
  const gchar *start = url;
  if (g_str_has_prefix(url, "wss://")) {
    start = url + 6;
  } else if (g_str_has_prefix(url, "ws://")) {
    start = url + 5;
  }

  /* Find end of hostname (before path or port) */
  const gchar *end = start;
  while (*end && *end != '/' && *end != ':') {
    end++;
  }

  return g_strndup(start, end - start);
}

/* Add a small NIP badge to the nips box */
static void relay_manager_add_small_nip_badge(GtkWidget *box, gint nip) {
  gchar *text = g_strdup_printf("%d", nip);
  GtkWidget *badge = gtk_label_new(text);
  g_free(text);

  gtk_widget_add_css_class(badge, "caption");
  gtk_widget_add_css_class(badge, "pill");
  gtk_widget_add_css_class(badge, "accent");

  /* Tooltip for NIP description */
  gchar *tooltip = g_strdup_printf("NIP-%02d", nip);
  gtk_widget_set_tooltip_text(badge, tooltip);
  g_free(tooltip);

  gtk_box_append(GTK_BOX(box), badge);
}

/* Helper to convert dropdown index to GnostrRelayType */
static GnostrRelayType relay_type_from_dropdown(guint index) {
  switch (index) {
    case 0: return GNOSTR_RELAY_READWRITE;
    case 1: return GNOSTR_RELAY_READ;
    case 2: return GNOSTR_RELAY_WRITE;
    default: return GNOSTR_RELAY_READWRITE;
  }
}

/* Helper to convert GnostrRelayType to dropdown index */
static guint relay_type_to_dropdown(GnostrRelayType type) {
  switch (type) {
    case GNOSTR_RELAY_READ: return 1;
    case GNOSTR_RELAY_WRITE: return 2;
    case GNOSTR_RELAY_READWRITE:
    default: return 0;
  }
}

/* Helper to update type icon based on relay type */
static void relay_manager_update_type_icon(GtkWidget *icon, GnostrRelayType type) {
  const gchar *icon_name;
  const gchar *tooltip;

  switch (type) {
    case GNOSTR_RELAY_READ:
      icon_name = "go-down-symbolic";
      tooltip = "Read-only (subscribe from this relay)";
      break;
    case GNOSTR_RELAY_WRITE:
      icon_name = "go-up-symbolic";
      tooltip = "Write-only (publish to this relay)";
      break;
    case GNOSTR_RELAY_READWRITE:
    default:
      icon_name = "network-transmit-receive-symbolic";
      tooltip = "Read + Write (subscribe and publish)";
      break;
  }

  gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
  gtk_widget_set_tooltip_text(icon, tooltip);
}

/* Callback for type dropdown change */
static void on_relay_type_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  const gchar *url = g_object_get_data(G_OBJECT(dropdown), "relay_url");
  if (!url) return;

  guint selected = gtk_drop_down_get_selected(dropdown);
  GnostrRelayType type = relay_type_from_dropdown(selected);

  /* Store in hash table */
  g_hash_table_replace(ctx->relay_types, g_strdup(url), GINT_TO_POINTER(type));
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);

  /* Update the type icon */
  GtkWidget *type_icon = g_object_get_data(G_OBJECT(dropdown), "type_icon");
  if (type_icon) {
    relay_manager_update_type_icon(type_icon, type);
  }
}

static void relay_manager_bind_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  GtkWidget *row = gtk_list_item_get_child(list_item);
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_list_item_get_item(list_item));

  if (!row || !obj) return;

  RelayRowWidgets *widgets = g_object_get_data(G_OBJECT(row), "widgets");
  if (!widgets) return;

  const gchar *url = gtk_string_object_get_string(obj);
  if (!url) return;

  /* Update connection status indicator */
  if (widgets->connection_icon && ctx && ctx->main_window && ctx->main_window->pool) {
    gboolean connected = (gnostr_pool_get_relay(ctx->main_window->pool, url) != NULL);
    if (connected) {
      gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-wired-symbolic");
      gtk_widget_remove_css_class(widgets->connection_icon, "dim-label");
      gtk_widget_remove_css_class(widgets->connection_icon, "error");
      gtk_widget_add_css_class(widgets->connection_icon, "success");
      gtk_widget_set_tooltip_text(widgets->connection_icon, "Connected");
    } else {
      gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-offline-symbolic");
      gtk_widget_remove_css_class(widgets->connection_icon, "success");
      gtk_widget_remove_css_class(widgets->connection_icon, "error");
      gtk_widget_add_css_class(widgets->connection_icon, "dim-label");
      gtk_widget_set_tooltip_text(widgets->connection_icon, "Not connected");
    }
  }

  /* Setup type dropdown for this relay */
  if (widgets->type_dropdown && ctx && ctx->relay_types) {
    /* Disconnect any previous signal handler */
    g_signal_handlers_disconnect_by_func(widgets->type_dropdown, G_CALLBACK(on_relay_type_changed), ctx);

    /* Store URL and icon reference in dropdown */
    g_object_set_data_full(G_OBJECT(widgets->type_dropdown), "relay_url", g_strdup(url), g_free);
    g_object_set_data(G_OBJECT(widgets->type_dropdown), "type_icon", widgets->type_icon);

    /* Get stored type, default to READWRITE */
    gpointer stored = g_hash_table_lookup(ctx->relay_types, url);
    GnostrRelayType type = stored ? GPOINTER_TO_INT(stored) : GNOSTR_RELAY_READWRITE;

    /* Set dropdown selection without triggering signal */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->type_dropdown), relay_type_to_dropdown(type));

    /* Update type icon */
    if (widgets->type_icon) {
      relay_manager_update_type_icon(widgets->type_icon, type);
    }

    /* Connect change signal */
    g_signal_connect(widgets->type_dropdown, "notify::selected", G_CALLBACK(on_relay_type_changed), ctx);
  }

  /* Try to get cached relay info */
  GnostrRelayInfo *info = gnostr_relay_info_cache_get(url);

  /* Set name (from cache or hostname fallback) */
  if (info && info->name && *info->name) {
    gtk_label_set_text(GTK_LABEL(widgets->name_label), info->name);
  } else {
    gchar *hostname = relay_manager_extract_hostname(url);
    gtk_label_set_text(GTK_LABEL(widgets->name_label), hostname ? hostname : url);
    g_free(hostname);
  }

  /* Set URL */
  gtk_label_set_text(GTK_LABEL(widgets->url_label), url);

  /* Update status icon based on cache availability */
  if (info && !info->fetch_failed) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-transmit-receive-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "dim-label");
    gtk_widget_add_css_class(widgets->status_icon, "success");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Relay info available");
  } else if (info && info->fetch_failed) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-error-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "dim-label");
    gtk_widget_add_css_class(widgets->status_icon, "error");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Failed to fetch relay info");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-offline-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "success");
    gtk_widget_remove_css_class(widgets->status_icon, "error");
    gtk_widget_add_css_class(widgets->status_icon, "dim-label");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Relay info not yet fetched");
  }

  /* Clear and populate NIP badges (show key NIPs only) */
  relay_manager_clear_container(widgets->nips_box);
  if (info && info->supported_nips && info->supported_nips_count > 0) {
    /* Show only key NIPs: 1, 11, 17, 42, 50, 59 */
    gint key_nips[] = {1, 11, 17, 42, 50, 59};
    gint shown = 0;
    for (gsize i = 0; i < info->supported_nips_count && shown < 4; i++) {
      gint nip = info->supported_nips[i];
      for (gsize j = 0; j < G_N_ELEMENTS(key_nips); j++) {
        if (nip == key_nips[j]) {
          relay_manager_add_small_nip_badge(widgets->nips_box, nip);
          shown++;
          break;
        }
      }
    }
    /* If we have more NIPs, show count */
    if (info->supported_nips_count > 4) {
      gchar *more = g_strdup_printf("+%zu", info->supported_nips_count - shown);
      GtkWidget *more_label = gtk_label_new(more);
      g_free(more);
      gtk_widget_add_css_class(more_label, "dim-label");
      gtk_widget_add_css_class(more_label, "caption");
      gtk_box_append(GTK_BOX(widgets->nips_box), more_label);
    }
  }

  /* Show warning icon if auth/payment required */
  if (info && (info->auth_required || info->payment_required || info->restricted_writes)) {
    gtk_widget_set_visible(widgets->warning_icon, TRUE);
    GString *tooltip = g_string_new("Warning: ");
    if (info->auth_required) g_string_append(tooltip, "Auth required. ");
    if (info->payment_required) g_string_append(tooltip, "Payment required. ");
    if (info->restricted_writes) g_string_append(tooltip, "Restricted writes.");
    gtk_widget_set_tooltip_text(widgets->warning_icon, tooltip->str);
    g_string_free(tooltip, TRUE);
  } else {
    gtk_widget_set_visible(widgets->warning_icon, FALSE);
  }

  if (info) gnostr_relay_info_free(info);
}

static void on_relays_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-relay-manager.ui");
  if (!builder) {
    show_toast(self, "Relay manager UI missing");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "relay_manager_window"));
  if (!win) {
    g_object_unref(builder);
    show_toast(self, "Relay manager window missing");
    return;
  }

  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);

  /* Create context */
  RelayManagerCtx *ctx = g_new0(RelayManagerCtx, 1);
  ctx->ref_count = 1;  /* Initial reference for the window */
  ctx->window = win;
  ctx->builder = builder;
  ctx->modified = FALSE;
  ctx->relay_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->main_window = self;  /* Store reference for pool access */

  /* Create relay list model */
  ctx->relay_model = gtk_string_list_new(NULL);

  /* Load saved relays with their NIP-65 types */
  GPtrArray *saved = gnostr_load_nip65_relays();
  for (guint i = 0; i < saved->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(saved, i);
    if (relay && relay->url && *relay->url) {
      gtk_string_list_append(ctx->relay_model, relay->url);
      g_hash_table_insert(ctx->relay_types, g_strdup(relay->url), GINT_TO_POINTER(relay->type));
    }
  }
  g_ptr_array_unref(saved);

  /* Setup selection model */
  ctx->selection = gtk_single_selection_new(G_LIST_MODEL(ctx->relay_model));
  gtk_single_selection_set_autoselect(ctx->selection, FALSE);
  gtk_single_selection_set_can_unselect(ctx->selection, TRUE);

  /* Setup list view with factory */
  GtkListView *list_view = GTK_LIST_VIEW(gtk_builder_get_object(builder, "relay_list"));
  if (list_view) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(relay_manager_setup_factory_cb), ctx);
    g_signal_connect(factory, "bind", G_CALLBACK(relay_manager_bind_factory_cb), ctx);
    gtk_list_view_set_factory(list_view, factory);
    gtk_list_view_set_model(list_view, GTK_SELECTION_MODEL(ctx->selection));
    g_object_unref(factory);
  }

  /* Connect selection change signal */
  g_signal_connect(ctx->selection, "selection-changed", G_CALLBACK(on_relay_selection_changed), ctx);

  /* Wire buttons */
  GtkWidget *btn_add = GTK_WIDGET(gtk_builder_get_object(builder, "btn_add"));
  GtkWidget *btn_remove = GTK_WIDGET(gtk_builder_get_object(builder, "btn_remove"));
  GtkWidget *btn_save = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save"));
  GtkWidget *btn_cancel = GTK_WIDGET(gtk_builder_get_object(builder, "btn_cancel"));
  GtkWidget *btn_retry = GTK_WIDGET(gtk_builder_get_object(builder, "btn_retry"));
  GtkWidget *btn_discover = GTK_WIDGET(gtk_builder_get_object(builder, "btn_discover"));
  GtkWidget *relay_entry = GTK_WIDGET(gtk_builder_get_object(builder, "relay_entry"));

  if (btn_add) g_signal_connect(btn_add, "clicked", G_CALLBACK(relay_manager_on_add_clicked), ctx);
  if (btn_remove) g_signal_connect(btn_remove, "clicked", G_CALLBACK(relay_manager_on_remove_clicked), ctx);
  if (btn_save) g_signal_connect(btn_save, "clicked", G_CALLBACK(relay_manager_on_save_clicked), ctx);
  if (btn_cancel) g_signal_connect(btn_cancel, "clicked", G_CALLBACK(relay_manager_on_cancel_clicked), ctx);
  if (btn_retry) g_signal_connect(btn_retry, "clicked", G_CALLBACK(relay_manager_on_retry_clicked), ctx);
  if (btn_discover) g_signal_connect(btn_discover, "clicked", G_CALLBACK(relay_manager_on_discover_clicked), ctx);
  if (relay_entry) g_signal_connect(relay_entry, "activate", G_CALLBACK(relay_manager_on_entry_activate), ctx);

  /* Update status and cleanup on destroy */
  relay_manager_update_status(ctx);
  g_signal_connect(win, "destroy", G_CALLBACK(relay_manager_on_destroy), ctx);

  /* nostrc-50v: Select first relay and populate info pane on open */
  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  if (n_items > 0) {
    gtk_single_selection_set_selected(ctx->selection, 0);
  }

  gtk_window_present(win);
}

static void on_reconnect_requested(GnostrSessionView *view, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  show_toast(self, "Reconnecting to relays...");
  start_pool_live(self);
}

/* Settings dialog context for callbacks */
typedef struct {
  GtkWindow *win;
  GtkBuilder *builder;
  GnostrMainWindow *main_window;
} SettingsDialogCtx;

static void settings_dialog_ctx_free(SettingsDialogCtx *ctx) {
  if (!ctx) return;
  if (ctx->builder) g_object_unref(ctx->builder);
  g_free(ctx);
}

/* Callback for NIP-51 backup button */
static void on_nip51_backup_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  if (!ctx || !ctx->main_window) return;
  show_toast(ctx->main_window, "Backing up settings to relays...");
  gnostr_nip51_settings_backup_async(NULL, NULL);
}

/* Callback for NIP-51 restore button */
static void on_nip51_restore_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  if (!ctx || !ctx->main_window) return;

  const gchar *pubkey = ctx->main_window->user_pubkey_hex;
  if (!pubkey || !*pubkey) {
    show_toast(ctx->main_window, "Sign in to restore settings");
    return;
  }
  show_toast(ctx->main_window, "Restoring settings from relays...");
  gnostr_nip51_settings_load_async(pubkey, NULL, NULL);
}

/* Forward declaration for background mode callback */
static void on_background_mode_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);

/* nostrc-61s.6: Setup general settings panel (background mode) */
static void settings_dialog_setup_general_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GSettings *client_settings = g_settings_new("org.gnostr.Client");
  if (!client_settings) return;

  /* Background mode switch */
  GtkSwitch *w_background_mode = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_background_mode"));
  if (w_background_mode) {
    gtk_switch_set_active(w_background_mode, g_settings_get_boolean(client_settings, "background-mode"));
    g_signal_connect(w_background_mode, "notify::active", G_CALLBACK(on_background_mode_changed), ctx);
  }

  g_object_unref(client_settings);
}

/* Update Display settings panel from GSettings */
static void settings_dialog_setup_display_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GSettings *display_settings = g_settings_new("org.gnostr.Display");
  if (!display_settings) return;

  /* Color scheme dropdown (System=0, Light=1, Dark=2) */
  GtkDropDown *w_color_scheme = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "w_color_scheme"));
  if (w_color_scheme) {
    g_autofree gchar *scheme = g_settings_get_string(display_settings, "color-scheme");
    guint idx = 0;
    if (g_strcmp0(scheme, "light") == 0) idx = 1;
    else if (g_strcmp0(scheme, "dark") == 0) idx = 2;
    gtk_drop_down_set_selected(w_color_scheme, idx);
  }

  /* Font scale slider */
  GtkScale *w_font_scale = GTK_SCALE(gtk_builder_get_object(ctx->builder, "w_font_scale"));
  if (w_font_scale) {
    gdouble scale = g_settings_get_double(display_settings, "font-scale");
    gtk_range_set_value(GTK_RANGE(w_font_scale), scale);
  }

  /* Timeline density dropdown (Compact=0, Normal=1, Comfortable=2) */
  GtkDropDown *w_density = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "w_timeline_density"));
  if (w_density) {
    g_autofree gchar *density = g_settings_get_string(display_settings, "timeline-density");
    guint idx = 1;
    if (g_strcmp0(density, "compact") == 0) idx = 0;
    else if (g_strcmp0(density, "comfortable") == 0) idx = 2;
    gtk_drop_down_set_selected(w_density, idx);
  }

  /* Boolean switches */
  GtkSwitch *w_avatars = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_show_avatars"));
  if (w_avatars) gtk_switch_set_active(w_avatars, g_settings_get_boolean(display_settings, "show-avatars"));

  GtkSwitch *w_media = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_show_media_previews"));
  if (w_media) gtk_switch_set_active(w_media, g_settings_get_boolean(display_settings, "show-media-previews"));

  GtkSwitch *w_anim = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_enable_animations"));
  if (w_anim) gtk_switch_set_active(w_anim, g_settings_get_boolean(display_settings, "enable-animations"));

  g_object_unref(display_settings);
}

/* Update Account settings panel */
static void settings_dialog_setup_account_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder || !ctx->main_window) return;

  /* Check login state from GSettings for consistency with update_login_ui_state() */
  GSettings *acct_settings = g_settings_new("org.gnostr.Client");
  char *acct_npub = acct_settings ? g_settings_get_string(acct_settings, "current-npub") : NULL;
  gboolean is_logged_in = (acct_npub && *acct_npub);
  g_free(acct_npub);
  if (acct_settings) g_object_unref(acct_settings);

  /* Toggle login required / account content visibility */
  GtkWidget *account_login_required = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "account_login_required"));
  GtkWidget *account_content = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "account_content"));
  if (account_login_required) gtk_widget_set_visible(account_login_required, !is_logged_in);
  if (account_content) gtk_widget_set_visible(account_content, is_logged_in);

  /* NIP-51 sync enabled switch */
  GtkSwitch *w_sync = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_nip51_sync_enabled"));
  if (w_sync) gtk_switch_set_active(w_sync, gnostr_nip51_settings_sync_enabled());

  /* Last sync label */
  GtkLabel *lbl_sync = GTK_LABEL(gtk_builder_get_object(ctx->builder, "lbl_nip51_last_sync"));
  if (lbl_sync) {
    gint64 last_sync = gnostr_nip51_settings_last_sync();
    if (last_sync > 0) {
      GDateTime *dt = g_date_time_new_from_unix_local(last_sync);
      if (dt) {
        gchar *formatted = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        gtk_label_set_text(lbl_sync, formatted);
        g_free(formatted);
        g_date_time_unref(dt);
      }
    } else {
      gtk_label_set_text(lbl_sync, "Never");
    }
  }

  /* Connect backup/restore buttons */
  GtkButton *btn_backup = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_nip51_backup"));
  GtkButton *btn_restore = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_nip51_restore"));
  if (btn_backup) g_signal_connect(btn_backup, "clicked", G_CALLBACK(on_nip51_backup_clicked), ctx);
  if (btn_restore) g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_nip51_restore_clicked), ctx);
}

/* Populate the relay list in settings */
static void settings_dialog_setup_relay_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list_relays = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_relays"));
  if (!list_relays) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list_relays))) != NULL) {
    gtk_list_box_remove(list_relays, child);
  }

  /* Load relays */
  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relays);

  for (guint i = 0; i < relays->len; i++) {
    const gchar *url = g_ptr_array_index(relays, i);

    /* Create row with relay URL */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    /* URL label */
    GtkWidget *label = gtk_label_new(url);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Type dropdown (R+W, Read, Write) */
    const gchar *types[] = {"R+W", "Read", "Write", NULL};
    GtkWidget *type_dd = gtk_drop_down_new_from_strings(types);
    gtk_widget_set_valign(type_dd, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), type_dd);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list_relays, row);
  }

  g_ptr_array_unref(relays);
}

/* ---- Index Relay Panel (NIP-50 Search) ---- */

/* Context for index relay row */
typedef struct {
  SettingsDialogCtx *dialog_ctx;
  gsize relay_index;
  char *relay_url;
} IndexRelayRowCtx;

static void index_relay_row_ctx_free(gpointer data) {
  IndexRelayRowCtx *ctx = (IndexRelayRowCtx *)data;
  if (!ctx) return;
  g_free(ctx->relay_url);
  g_free(ctx);
}

/* Forward declare refresh function */
static void settings_dialog_refresh_index_relay_list(SettingsDialogCtx *ctx);

/* Callback for remove button on an index relay row */
static void on_index_relay_remove(GtkButton *btn, gpointer user_data) {
  (void)btn;
  IndexRelayRowCtx *row_ctx = (IndexRelayRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  /* Load current index relays from GSettings */
  GSettings *settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");
  if (!relays) {
    g_object_unref(settings);
    return;
  }

  /* Count relays and build new array without the removed one */
  gsize count = g_strv_length(relays);
  GPtrArray *new_relays = g_ptr_array_new();

  for (gsize i = 0; i < count; i++) {
    if (g_strcmp0(relays[i], row_ctx->relay_url) != 0) {
      g_ptr_array_add(new_relays, relays[i]);
    }
  }
  g_ptr_array_add(new_relays, NULL);

  g_settings_set_strv(settings, "index-relays", (const gchar *const *)new_relays->pdata);

  g_ptr_array_free(new_relays, TRUE);
  g_strfreev(relays);
  g_object_unref(settings);

  settings_dialog_refresh_index_relay_list(row_ctx->dialog_ctx);

  if (row_ctx->dialog_ctx->main_window) {
    show_toast(row_ctx->dialog_ctx->main_window, "Index relay removed");
  }
}

/* Callback for add index relay button */
static void on_index_relay_add(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "entry_index_relay"));
  if (!entry) return;

  GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
  const char *url = gtk_entry_buffer_get_text(buffer);
  if (!url || !*url) {
    if (ctx->main_window) show_toast(ctx->main_window, "Enter a relay URL");
    return;
  }

  /* Validate URL starts with wss:// or ws:// */
  if (!g_str_has_prefix(url, "wss://") && !g_str_has_prefix(url, "ws://")) {
    if (ctx->main_window) show_toast(ctx->main_window, "URL must start with wss:// or ws://");
    return;
  }

  /* Load current index relays from GSettings */
  GSettings *settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");

  /* Check if relay already exists */
  gboolean exists = FALSE;
  if (relays) {
    for (gsize i = 0; relays[i] != NULL; i++) {
      if (g_strcmp0(relays[i], url) == 0) {
        exists = TRUE;
        break;
      }
    }
  }

  if (exists) {
    g_strfreev(relays);
    g_object_unref(settings);
    if (ctx->main_window) show_toast(ctx->main_window, "Relay already in list");
    return;
  }

  /* Add new relay to array */
  gsize count = relays ? g_strv_length(relays) : 0;
  gchar **new_relays = g_new0(gchar *, count + 2);
  for (gsize i = 0; i < count; i++) {
    new_relays[i] = g_strdup(relays[i]);
  }
  new_relays[count] = g_strdup(url);
  new_relays[count + 1] = NULL;

  g_settings_set_strv(settings, "index-relays", (const gchar *const *)new_relays);

  g_strfreev(new_relays);
  g_strfreev(relays);
  g_object_unref(settings);

  gtk_entry_buffer_set_text(buffer, "", 0);
  settings_dialog_refresh_index_relay_list(ctx);
  if (ctx->main_window) show_toast(ctx->main_window, "Index relay added");
}

/* Refresh the index relay list in settings */
static void settings_dialog_refresh_index_relay_list(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_index_relays"));
  if (!list) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
    gtk_list_box_remove(list, child);
  }

  /* Load index relays from GSettings */
  GSettings *settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");
  g_object_unref(settings);

  if (!relays) return;

  for (gsize i = 0; relays[i] != NULL; i++) {
    /* Create row context */
    IndexRelayRowCtx *row_ctx = g_new0(IndexRelayRowCtx, 1);
    row_ctx->dialog_ctx = ctx;
    row_ctx->relay_index = i;
    row_ctx->relay_url = g_strdup(relays[i]);

    /* Create row widget */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* URL label */
    GtkWidget *label = gtk_label_new(relays[i]);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Remove button */
    GtkWidget *btn_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(btn_remove, "flat");
    gtk_widget_add_css_class(btn_remove, "error");
    gtk_widget_set_tooltip_text(btn_remove, "Remove relay");
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_index_relay_remove), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_remove);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list, row);

    /* Free row context when row is destroyed */
    g_object_set_data_full(G_OBJECT(row), "row-ctx", row_ctx, index_relay_row_ctx_free);
  }

  g_strfreev(relays);
}

/* Setup index relay panel */
static void settings_dialog_setup_index_relay_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Connect add button */
  GtkButton *btn_add = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_add_index_relay"));
  if (btn_add) {
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_index_relay_add), ctx);
  }

  /* Allow Enter key in entry to add relay */
  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "entry_index_relay"));
  if (entry) {
    g_signal_connect_swapped(entry, "activate", G_CALLBACK(on_index_relay_add), ctx);
  }

  /* Populate relay list */
  settings_dialog_refresh_index_relay_list(ctx);
}

/* ---- Blossom Server Panel ---- */

/* Context for blossom server row */
typedef struct {
  SettingsDialogCtx *dialog_ctx;
  gsize server_index;
  char *server_url;
} BlossomServerRowCtx;

static void blossom_server_row_ctx_free(gpointer data) {
  BlossomServerRowCtx *ctx = (BlossomServerRowCtx *)data;
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx);
}

/* Refresh the blossom server list */
static void settings_dialog_refresh_blossom_list(SettingsDialogCtx *ctx);

/* Callback for remove button on a blossom server row */
static void on_blossom_server_remove(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  gnostr_blossom_settings_remove_server(row_ctx->server_url);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);

  if (row_ctx->dialog_ctx->main_window) {
    show_toast(row_ctx->dialog_ctx->main_window, "Server removed");
  }
}

/* Callback for move up button on a blossom server row */
static void on_blossom_server_move_up(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx || row_ctx->server_index == 0) return;

  gnostr_blossom_settings_reorder_server(row_ctx->server_index, row_ctx->server_index - 1);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);
}

/* Callback for move down button on a blossom server row */
static void on_blossom_server_move_down(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  gsize count = gnostr_blossom_settings_get_server_count();
  if (row_ctx->server_index >= count - 1) return;

  gnostr_blossom_settings_reorder_server(row_ctx->server_index, row_ctx->server_index + 1);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);
}

/* Callback for add server button */
static void on_blossom_add_server(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "w_blossom_server"));
  if (!entry) return;

  GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
  const char *url = gtk_entry_buffer_get_text(buffer);
  if (!url || !*url) {
    if (ctx->main_window) show_toast(ctx->main_window, "Enter a server URL");
    return;
  }

  /* Validate URL starts with https:// */
  if (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://")) {
    if (ctx->main_window) show_toast(ctx->main_window, "URL must start with https://");
    return;
  }

  if (gnostr_blossom_settings_add_server(url)) {
    gtk_entry_buffer_set_text(buffer, "", 0);
    settings_dialog_refresh_blossom_list(ctx);
    if (ctx->main_window) show_toast(ctx->main_window, "Server added");
  } else {
    if (ctx->main_window) show_toast(ctx->main_window, "Server already exists");
  }
}

/* Callback for publish button (kind 10063) */
static void on_blossom_publish_complete(gboolean success, GError *error, gpointer user_data) {
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->main_window) return;

  if (success) {
    show_toast(ctx->main_window, "Server list published to relays");
  } else {
    char *msg = g_strdup_printf("Publish failed: %s",
                                 error ? error->message : "unknown error");
    show_toast(ctx->main_window, msg);
    g_free(msg);
  }
}

static void on_blossom_publish_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->main_window) return;

  /* Check if logged in */
  if (!ctx->main_window->user_pubkey_hex || !ctx->main_window->user_pubkey_hex[0]) {
    show_toast(ctx->main_window, "Sign in to publish server list");
    return;
  }

  show_toast(ctx->main_window, "Publishing server list...");
  gnostr_blossom_settings_publish_async(on_blossom_publish_complete, ctx);
}

/* Refresh the blossom server list in settings */
static void settings_dialog_refresh_blossom_list(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "blossom_server_list"));
  if (!list) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
    gtk_list_box_remove(list, child);
  }

  /* Load servers */
  gsize count = 0;
  GnostrBlossomServer **servers = gnostr_blossom_settings_get_servers(&count);

  for (gsize i = 0; i < count; i++) {
    GnostrBlossomServer *server = servers[i];

    /* Create row context */
    BlossomServerRowCtx *row_ctx = g_new0(BlossomServerRowCtx, 1);
    row_ctx->dialog_ctx = ctx;
    row_ctx->server_index = i;
    row_ctx->server_url = g_strdup(server->url);

    /* Create row widget */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Priority indicator (first is default) */
    GtkWidget *priority = gtk_label_new(i == 0 ? "1" : NULL);
    if (i == 0) {
      gtk_widget_set_size_request(priority, 20, -1);
      gtk_widget_add_css_class(priority, "accent");
      gtk_widget_set_tooltip_text(priority, "Primary server");
    } else {
      char num[8];
      g_snprintf(num, sizeof(num), "%zu", i + 1);
      gtk_label_set_text(GTK_LABEL(priority), num);
      gtk_widget_set_size_request(priority, 20, -1);
      gtk_widget_add_css_class(priority, "dim-label");
    }
    gtk_box_append(GTK_BOX(box), priority);

    /* URL label */
    GtkWidget *label = gtk_label_new(server->url);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Move up button */
    GtkWidget *btn_up = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(btn_up, "flat");
    gtk_widget_set_sensitive(btn_up, i > 0);
    gtk_widget_set_tooltip_text(btn_up, "Move up (higher priority)");
    g_signal_connect(btn_up, "clicked", G_CALLBACK(on_blossom_server_move_up), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_up);

    /* Move down button */
    GtkWidget *btn_down = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(btn_down, "flat");
    gtk_widget_set_sensitive(btn_down, i < count - 1);
    gtk_widget_set_tooltip_text(btn_down, "Move down (lower priority)");
    g_signal_connect(btn_down, "clicked", G_CALLBACK(on_blossom_server_move_down), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_down);

    /* Remove button */
    GtkWidget *btn_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(btn_remove, "flat");
    gtk_widget_add_css_class(btn_remove, "error");
    gtk_widget_set_tooltip_text(btn_remove, "Remove server");
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_blossom_server_remove), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_remove);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list, row);

    /* Free row context when row is destroyed */
    g_object_set_data_full(G_OBJECT(row), "row-ctx", row_ctx, blossom_server_row_ctx_free);
  }

  gnostr_blossom_servers_free(servers, count);
}

/* Setup blossom server panel */
static void settings_dialog_setup_blossom_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Connect add button */
  GtkButton *btn_add = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_blossom_add"));
  if (btn_add) {
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_blossom_add_server), ctx);
  }

  /* Connect publish button */
  GtkButton *btn_publish = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_blossom_publish"));
  if (btn_publish) {
    g_signal_connect(btn_publish, "clicked", G_CALLBACK(on_blossom_publish_clicked), ctx);
  }

  /* Allow Enter key in entry to add server */
  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "w_blossom_server"));
  if (entry) {
    g_signal_connect_swapped(entry, "activate", G_CALLBACK(on_blossom_add_server), ctx);
  }

  /* Populate server list */
  settings_dialog_refresh_blossom_list(ctx);
}

/* Signal handlers for media settings switches */
static void on_video_autoplay_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Client");
  g_settings_set_boolean(settings, "video-autoplay", active);
  g_object_unref(settings);
}

static void on_video_loop_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Client");
  g_settings_set_boolean(settings, "video-loop", active);
  g_object_unref(settings);
}

/* nostrc-61s.6: Background mode switch handler */
static void on_background_mode_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);

  /* Save to GSettings */
  GSettings *settings = g_settings_new("org.gnostr.Client");
  g_settings_set_boolean(settings, "background-mode", active);
  g_object_unref(settings);

  /* Update the main window's background_mode_enabled flag live */
  if (ctx && ctx->main_window && GNOSTR_IS_MAIN_WINDOW(ctx->main_window)) {
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(ctx->main_window);
    gboolean was_enabled = self->background_mode_enabled;
    self->background_mode_enabled = active;

    /* Manage application hold/release based on change */
    GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
    if (app) {
      if (active && !was_enabled) {
        g_application_hold(G_APPLICATION(app));
        g_debug("[SETTINGS] Background mode enabled - application held");
      } else if (!active && was_enabled) {
        g_application_release(G_APPLICATION(app));
        g_debug("[SETTINGS] Background mode disabled - application released");
      }
    }
  }
}

/* Setup media (playback) settings panel */
static void settings_dialog_setup_media_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GSettings *client_settings = g_settings_new("org.gnostr.Client");
  if (!client_settings) return;

  /* Video autoplay switch */
  GtkSwitch *w_autoplay = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_video_autoplay"));
  if (w_autoplay) {
    gtk_switch_set_active(w_autoplay, g_settings_get_boolean(client_settings, "video-autoplay"));
    g_signal_connect(w_autoplay, "notify::active", G_CALLBACK(on_video_autoplay_changed), NULL);
  }

  /* Video loop switch */
  GtkSwitch *w_loop = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_video_loop"));
  if (w_loop) {
    gtk_switch_set_active(w_loop, g_settings_get_boolean(client_settings, "video-loop"));
    g_signal_connect(w_loop, "notify::active", G_CALLBACK(on_video_loop_changed), NULL);
  }

  g_object_unref(client_settings);
}

/* Signal handlers for notification settings switches */
static void on_notif_enabled_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "enabled", active);
  g_object_unref(settings);
}

static void on_notif_mention_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "notify-mention-enabled", active);
  g_object_unref(settings);
}

static void on_notif_dm_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "notify-dm-enabled", active);
  g_object_unref(settings);
}

static void on_notif_zap_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "notify-zap-enabled", active);
  g_object_unref(settings);
}

static void on_notif_reply_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "notify-reply-enabled", active);
  g_object_unref(settings);
}

static void on_notif_sound_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "sound-enabled", active);
  g_object_unref(settings);
}

static void on_notif_tray_badge_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "tray-badge-enabled", active);
  g_object_unref(settings);
}

static void on_notif_desktop_popup_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec; (void)user_data;
  gboolean active = gtk_switch_get_active(sw);
  GSettings *settings = g_settings_new("org.gnostr.Notifications");
  g_settings_set_boolean(settings, "desktop-popup-enabled", active);
  g_object_unref(settings);
}

/* Setup notifications settings panel */
static void settings_dialog_setup_notifications_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GSettings *notif_settings = g_settings_new("org.gnostr.Notifications");
  if (!notif_settings) return;

  /* Global enable switch */
  GtkSwitch *w_enabled = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_enabled"));
  if (w_enabled) {
    gtk_switch_set_active(w_enabled, g_settings_get_boolean(notif_settings, "enabled"));
    g_signal_connect(w_enabled, "notify::active", G_CALLBACK(on_notif_enabled_changed), NULL);
  }

  /* Per-type toggles */
  GtkSwitch *w_mention = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_mention"));
  if (w_mention) {
    gtk_switch_set_active(w_mention, g_settings_get_boolean(notif_settings, "notify-mention-enabled"));
    g_signal_connect(w_mention, "notify::active", G_CALLBACK(on_notif_mention_changed), NULL);
  }

  GtkSwitch *w_dm = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_dm"));
  if (w_dm) {
    gtk_switch_set_active(w_dm, g_settings_get_boolean(notif_settings, "notify-dm-enabled"));
    g_signal_connect(w_dm, "notify::active", G_CALLBACK(on_notif_dm_changed), NULL);
  }

  GtkSwitch *w_zap = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_zap"));
  if (w_zap) {
    gtk_switch_set_active(w_zap, g_settings_get_boolean(notif_settings, "notify-zap-enabled"));
    g_signal_connect(w_zap, "notify::active", G_CALLBACK(on_notif_zap_changed), NULL);
  }

  GtkSwitch *w_reply = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_reply"));
  if (w_reply) {
    gtk_switch_set_active(w_reply, g_settings_get_boolean(notif_settings, "notify-reply-enabled"));
    g_signal_connect(w_reply, "notify::active", G_CALLBACK(on_notif_reply_changed), NULL);
  }

  /* Presentation switches */
  GtkSwitch *w_sound = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_sound"));
  if (w_sound) {
    gtk_switch_set_active(w_sound, g_settings_get_boolean(notif_settings, "sound-enabled"));
    g_signal_connect(w_sound, "notify::active", G_CALLBACK(on_notif_sound_changed), NULL);
  }

  GtkSwitch *w_tray_badge = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_tray_badge"));
  if (w_tray_badge) {
    gtk_switch_set_active(w_tray_badge, g_settings_get_boolean(notif_settings, "tray-badge-enabled"));
    g_signal_connect(w_tray_badge, "notify::active", G_CALLBACK(on_notif_tray_badge_changed), NULL);
  }

  GtkSwitch *w_desktop_popup = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_desktop_popup"));
  if (w_desktop_popup) {
    gtk_switch_set_active(w_desktop_popup, g_settings_get_boolean(notif_settings, "desktop-popup-enabled"));
    g_signal_connect(w_desktop_popup, "notify::active", G_CALLBACK(on_notif_desktop_popup_changed), NULL);
  }

  g_object_unref(notif_settings);
}

/* Plugin panel signal handlers */
static void on_plugin_settings_signal(GnostrPluginManagerPanel *panel,
                                      const char *plugin_id,
                                      gpointer user_data) {
  (void)user_data;
  gnostr_plugin_manager_panel_show_plugin_settings(panel, plugin_id);
}

static void on_plugin_info_signal(GnostrPluginManagerPanel *panel,
                                  const char *plugin_id,
                                  gpointer user_data) {
  (void)user_data;
  gnostr_plugin_manager_panel_show_plugin_info(panel, plugin_id);
}

/* --- Metrics Panel (6.3) --- */

typedef struct {
  GtkLabel *lbl_connected_relays;
  GtkLabel *lbl_active_subs;
  GtkLabel *lbl_queue_depth;
  GtkLabel *lbl_events_received;
  GtkLabel *lbl_events_dispatched;
  GtkLabel *lbl_events_dropped;
  GtkLabel *lbl_drop_rate;
  GtkLabel *lbl_dispatch_p50;
  GtkLabel *lbl_dispatch_p99;
  GtkLabel *lbl_status_icon;
  /* NDB storage stats (nostrc-o6w) */
  GtkLabel *lbl_ndb_notes;
  GtkLabel *lbl_ndb_profiles;
  GtkLabel *lbl_ndb_storage;
  GtkLabel *lbl_ndb_text;
  GtkLabel *lbl_ndb_reactions;
  GtkLabel *lbl_ndb_zaps;
  GtkLabel *lbl_ndb_ingest;
  GtkWidget *panel;
  guint timer_id;
} MetricsPanelCtx;

static GtkWidget *metrics_add_row(GtkListBox *list, const char *title, const char *initial) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  GtkWidget *lbl_title = gtk_label_new(title);
  gtk_widget_set_hexpand(lbl_title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0);

  GtkWidget *lbl_value = gtk_label_new(initial);
  gtk_widget_add_css_class(lbl_value, "dim-label");

  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_value);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(list, row);
  return lbl_value;
}

static void metrics_panel_refresh(MetricsPanelCtx *mctx) {
  if (!mctx || !mctx->panel) return;
  if (!gtk_widget_get_mapped(mctx->panel)) return;

  NostrMetricsSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  if (!nostr_metrics_collector_latest(&snap)) {
    /* Collector not running or no data — try an immediate collect */
    nostr_metrics_snapshot_collect(&snap);
  }

  char buf[64];

  /* Find specific metrics by name */
  int64_t connected = 0, active_subs = 0, queue_depth = 0;
  uint64_t events_recv = 0, events_disp = 0, events_drop = 0;
  uint64_t recv_delta = 0, disp_delta = 0, drop_delta = 0;
  uint64_t disp_p50 = 0, disp_p99 = 0;

  for (size_t i = 0; i < snap.gauge_count; i++) {
    if (!snap.gauges[i].name) continue;
    if (strcmp(snap.gauges[i].name, METRIC_CONNECTED_RELAYS) == 0)
      connected = snap.gauges[i].value;
    else if (strcmp(snap.gauges[i].name, METRIC_ACTIVE_SUBSCRIPTIONS) == 0)
      active_subs = snap.gauges[i].value;
    else if (strcmp(snap.gauges[i].name, METRIC_QUEUE_DEPTH) == 0)
      queue_depth = snap.gauges[i].value;
  }

  for (size_t i = 0; i < snap.counter_count; i++) {
    if (!snap.counters[i].name) continue;
    if (strcmp(snap.counters[i].name, METRIC_EVENTS_RECEIVED) == 0) {
      events_recv = snap.counters[i].total;
      recv_delta = snap.counters[i].delta_60s;
    } else if (strcmp(snap.counters[i].name, METRIC_EVENTS_DISPATCHED) == 0) {
      events_disp = snap.counters[i].total;
      disp_delta = snap.counters[i].delta_60s;
    } else if (strcmp(snap.counters[i].name, METRIC_EVENTS_DROPPED) == 0) {
      events_drop = snap.counters[i].total;
      drop_delta = snap.counters[i].delta_60s;
    }
  }

  for (size_t i = 0; i < snap.histogram_count; i++) {
    if (!snap.histograms[i].name) continue;
    if (strcmp(snap.histograms[i].name, METRIC_DISPATCH_LATENCY_NS) == 0) {
      disp_p50 = snap.histograms[i].p50_ns;
      disp_p99 = snap.histograms[i].p99_ns;
    }
  }

  /* Update labels */
  snprintf(buf, sizeof(buf), "%lld", (long long)connected);
  gtk_label_set_text(mctx->lbl_connected_relays, buf);

  snprintf(buf, sizeof(buf), "%lld", (long long)active_subs);
  gtk_label_set_text(mctx->lbl_active_subs, buf);

  snprintf(buf, sizeof(buf), "%lld", (long long)queue_depth);
  gtk_label_set_text(mctx->lbl_queue_depth, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_recv,
           (unsigned long long)recv_delta);
  gtk_label_set_text(mctx->lbl_events_received, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_disp,
           (unsigned long long)disp_delta);
  gtk_label_set_text(mctx->lbl_events_dispatched, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_drop,
           (unsigned long long)drop_delta);
  gtk_label_set_text(mctx->lbl_events_dropped, buf);

  /* Drop rate */
  double drop_rate = events_recv > 0
    ? (double)events_drop / (double)events_recv * 100.0 : 0.0;
  snprintf(buf, sizeof(buf), "%.2f%%", drop_rate);
  gtk_label_set_text(mctx->lbl_drop_rate, buf);

  /* Dispatch latency (convert ns to us for readability) */
  snprintf(buf, sizeof(buf), "%.1f \xC2\xB5s", disp_p50 / 1000.0);
  gtk_label_set_text(mctx->lbl_dispatch_p50, buf);

  snprintf(buf, sizeof(buf), "%.1f \xC2\xB5s", disp_p99 / 1000.0);
  gtk_label_set_text(mctx->lbl_dispatch_p99, buf);

  /* Status indicator: green if drop_rate < 1%, yellow < 5%, red >= 5% */
  if (drop_rate >= 5.0)
    gtk_label_set_text(mctx->lbl_status_icon, "Degraded");
  else if (drop_rate >= 1.0)
    gtk_label_set_text(mctx->lbl_status_icon, "Warning");
  else
    gtk_label_set_text(mctx->lbl_status_icon, "Healthy");

  nostr_metrics_snapshot_free(&snap);

  /* NDB storage stats (nostrc-o6w) */
  StorageNdbStat nst;
  if (storage_ndb_get_stat(&nst) == 0) {
    snprintf(buf, sizeof(buf), "%zu", nst.note_count);
    gtk_label_set_text(mctx->lbl_ndb_notes, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.profile_count);
    gtk_label_set_text(mctx->lbl_ndb_profiles, buf);

    if (nst.total_bytes >= 1024 * 1024)
      snprintf(buf, sizeof(buf), "%.1f MB", nst.total_bytes / (1024.0 * 1024.0));
    else if (nst.total_bytes >= 1024)
      snprintf(buf, sizeof(buf), "%.1f KB", nst.total_bytes / 1024.0);
    else
      snprintf(buf, sizeof(buf), "%zu B", nst.total_bytes);
    gtk_label_set_text(mctx->lbl_ndb_storage, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_text);
    gtk_label_set_text(mctx->lbl_ndb_text, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_reaction);
    gtk_label_set_text(mctx->lbl_ndb_reactions, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_zap);
    gtk_label_set_text(mctx->lbl_ndb_zaps, buf);

    uint64_t ic = storage_ndb_get_ingest_count();
    uint64_t ib = storage_ndb_get_ingest_bytes();
    if (ib >= 1024 * 1024)
      snprintf(buf, sizeof(buf), "%llu events / %.1f MB",
               (unsigned long long)ic, ib / (1024.0 * 1024.0));
    else
      snprintf(buf, sizeof(buf), "%llu events / %llu B",
               (unsigned long long)ic, (unsigned long long)ib);
    gtk_label_set_text(mctx->lbl_ndb_ingest, buf);

    /* Also push to metrics gauges for Prometheus export */
    storage_ndb_update_metrics();
  }
}

static gboolean metrics_panel_tick(gpointer user_data) {
  MetricsPanelCtx *mctx = user_data;
  metrics_panel_refresh(mctx);
  return G_SOURCE_CONTINUE;
}

static void metrics_panel_ctx_free(gpointer data) {
  MetricsPanelCtx *mctx = data;
  if (!mctx) return;
  if (mctx->timer_id) g_source_remove(mctx->timer_id);
  g_free(mctx);
}

static void settings_dialog_setup_metrics_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkBox *panel = GTK_BOX(gtk_builder_get_object(ctx->builder, "metrics_panel"));
  if (!panel) return;

  MetricsPanelCtx *mctx = g_new0(MetricsPanelCtx, 1);
  mctx->panel = GTK_WIDGET(panel);

  /* Connection Health section */
  GtkWidget *health_label = gtk_label_new("Connection Health");
  gtk_label_set_xalign(GTK_LABEL(health_label), 0);
  gtk_widget_add_css_class(health_label, "heading");
  gtk_box_append(panel, health_label);

  GtkWidget *health_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(health_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(health_list, "boxed-list");
  gtk_box_append(panel, health_list);

  mctx->lbl_status_icon = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Status", "Healthy"));
  mctx->lbl_connected_relays = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Connected Relays", "0"));
  mctx->lbl_active_subs = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Active Subscriptions", "0"));
  mctx->lbl_queue_depth = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Queue Depth", "0"));

  /* Event Flow section */
  GtkWidget *flow_label = gtk_label_new("Event Flow");
  gtk_label_set_xalign(GTK_LABEL(flow_label), 0);
  gtk_widget_add_css_class(flow_label, "heading");
  gtk_box_append(panel, flow_label);

  GtkWidget *flow_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(flow_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(flow_list, "boxed-list");
  gtk_box_append(panel, flow_list);

  mctx->lbl_events_received = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Received", "0"));
  mctx->lbl_events_dispatched = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Dispatched", "0"));
  mctx->lbl_events_dropped = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Dropped", "0"));
  mctx->lbl_drop_rate = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Drop Rate", "0.00%"));

  /* Latency section */
  GtkWidget *lat_label = gtk_label_new("Dispatch Latency");
  gtk_label_set_xalign(GTK_LABEL(lat_label), 0);
  gtk_widget_add_css_class(lat_label, "heading");
  gtk_box_append(panel, lat_label);

  GtkWidget *lat_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(lat_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(lat_list, "boxed-list");
  gtk_box_append(panel, lat_list);

  mctx->lbl_dispatch_p50 = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(lat_list), "p50", "0.0 \xC2\xB5s"));
  mctx->lbl_dispatch_p99 = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(lat_list), "p99", "0.0 \xC2\xB5s"));

  /* NDB Storage section (nostrc-o6w) */
  GtkWidget *ndb_label = gtk_label_new("Storage");
  gtk_label_set_xalign(GTK_LABEL(ndb_label), 0);
  gtk_widget_add_css_class(ndb_label, "heading");
  gtk_box_append(panel, ndb_label);

  GtkWidget *ndb_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(ndb_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(ndb_list, "boxed-list");
  gtk_box_append(panel, ndb_list);

  mctx->lbl_ndb_notes     = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Notes", "0"));
  mctx->lbl_ndb_profiles  = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Profiles", "0"));
  mctx->lbl_ndb_storage   = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "DB Size", "0 B"));
  mctx->lbl_ndb_text      = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Text Notes", "0"));
  mctx->lbl_ndb_reactions = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Reactions", "0"));
  mctx->lbl_ndb_zaps      = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Zaps", "0"));
  mctx->lbl_ndb_ingest    = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Ingested", "0 events / 0 B"));

  /* Initial refresh */
  metrics_panel_refresh(mctx);

  /* Timer: refresh every 2 seconds while dialog is open */
  mctx->timer_id = g_timeout_add_seconds(2, metrics_panel_tick, mctx);

  /* Clean up when dialog is destroyed */
  g_object_set_data_full(G_OBJECT(ctx->win), "metrics-panel-ctx", mctx, metrics_panel_ctx_free);
}

static void on_settings_dialog_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  settings_dialog_ctx_free(ctx);
}

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-settings-dialog.ui");
  if (!builder) { show_toast(self, "Settings UI missing"); return; }
  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "settings_window"));
  if (!win) { g_object_unref(builder); show_toast(self, "Settings window missing"); return; }
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);

  /* Create context for the dialog */
  SettingsDialogCtx *ctx = g_new0(SettingsDialogCtx, 1);
  ctx->win = win;
  ctx->builder = builder;
  ctx->main_window = self;

  /* Check if user is logged in and update mute list visibility.
   * Use GSettings directly for consistency with update_login_ui_state() */
  GSettings *mute_settings = g_settings_new("org.gnostr.Client");
  char *mute_npub = mute_settings ? g_settings_get_string(mute_settings, "current-npub") : NULL;
  gboolean is_logged_in = (mute_npub && *mute_npub);
  g_free(mute_npub);
  if (mute_settings) g_object_unref(mute_settings);

  GtkWidget *mute_login_required = GTK_WIDGET(gtk_builder_get_object(builder, "mute_login_required"));
  GtkWidget *mute_content = GTK_WIDGET(gtk_builder_get_object(builder, "mute_content"));
  if (mute_login_required) gtk_widget_set_visible(mute_login_required, !is_logged_in);
  if (mute_content) gtk_widget_set_visible(mute_content, is_logged_in);

  /* Load current settings values (Advanced panel - technical options) */
  GtkSpinButton *w_limit = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_limit"));
  GtkSpinButton *w_batch = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_batch"));
  GtkSpinButton *w_interval = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_interval"));
  GtkSpinButton *w_quiet = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_quiet"));
  GtkSwitch *w_use_since = GTK_SWITCH(gtk_builder_get_object(builder, "w_use_since"));
  GtkSpinButton *w_since = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_since"));
  GtkSpinButton *w_backfill = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_backfill"));

  if (w_limit) gtk_spin_button_set_value(w_limit, self->default_limit);
  if (w_batch) gtk_spin_button_set_value(w_batch, self->batch_max);
  if (w_interval) gtk_spin_button_set_value(w_interval, self->post_interval_ms);
  if (w_quiet) gtk_spin_button_set_value(w_quiet, self->eose_quiet_ms);
  if (w_use_since) gtk_switch_set_active(w_use_since, self->use_since);
  if (w_since) gtk_spin_button_set_value(w_since, self->since_seconds);
  if (w_backfill) gtk_spin_button_set_value(w_backfill, self->backfill_interval_sec);

  /* Setup new panels */
  settings_dialog_setup_general_panel(ctx);  /* nostrc-61s.6: background mode */
  settings_dialog_setup_relay_panel(ctx);
  settings_dialog_setup_index_relay_panel(ctx);
  settings_dialog_setup_display_panel(ctx);
  settings_dialog_setup_notifications_panel(ctx);
  settings_dialog_setup_account_panel(ctx);
  settings_dialog_setup_blossom_panel(ctx);
  settings_dialog_setup_media_panel(ctx);
  settings_dialog_setup_metrics_panel(ctx);

  /* Connect plugin manager panel signals */
  GnostrPluginManagerPanel *plugin_panel = GNOSTR_PLUGIN_MANAGER_PANEL(
      gtk_builder_get_object(builder, "plugin_manager_panel"));
  if (plugin_panel) {
    g_signal_connect(plugin_panel, "plugin-settings",
                     G_CALLBACK(on_plugin_settings_signal), NULL);
    g_signal_connect(plugin_panel, "plugin-info",
                     G_CALLBACK(on_plugin_info_signal), NULL);
  }

  /* Context is freed when window is destroyed */
  g_signal_connect(win, "destroy", G_CALLBACK(on_settings_dialog_destroy), ctx);
  gtk_window_present(win);
}

/* Handler for the "About" menu action - opens the About dialog */
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

  /* Builder is unref'd when window closes - store reference */
  g_object_set_data_full(G_OBJECT(win), "builder", builder, g_object_unref);

  gtk_window_present(win);
}

/* Handler for the "Preferences" menu action - opens Settings dialog */
static void on_show_preferences_activated(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action;
  (void)param;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Reuse the settings button click handler logic */
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
      GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *content = gnostr_event_get_content(evt);
        if (content && *content) {
          /* Ingest into nostrdb for future use */
          storage_ndb_ingest_event_json(evt_json, NULL);

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
        g_object_unref(evt);
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
    GNostrPool *profile_pool = gnostr_pool_new();
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

    /* Stash filters on the pool so they stay alive during the async query.
     * The pool outlives the query since the callback receives it as source. */
    g_object_set_data_full(G_OBJECT(profile_pool), "profile-filters",
                           filters, (GDestroyNotify)nostr_filters_free);

    g_debug("[AUTH] Fetching profile from %u relays (after NIP-65 load)", relay_urls->len);
    gnostr_pool_query_async(profile_pool, filters, NULL,
                            on_user_profile_fetched,
                            self); /* self already has a ref from caller */

    g_object_unref(profile_pool); /* the GTask holds a ref during the query */
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
    GSettings *settings = g_settings_new("org.gnostr.Client");
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
      g_object_unref(settings);
    }

    /* Refresh account list in session view */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_refresh_account_list(GNOSTR_SESSION_VIEW(self->session_view));
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
      GSettings *settings = g_settings_new("org.gnostr.Client");
      if (settings) {
        g_autofree char *npub = g_settings_get_string(settings, "current-npub");
        gnostr_session_view_set_authenticated(self->session_view, npub && *npub);
        g_object_unref(settings);
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
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (settings) {
    g_settings_set_string(settings, "current-npub", "");
    g_object_unref(settings);
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

/* nostrc-bkor: Navigate to the current user's own profile */
static void on_view_profile_requested(GnostrSessionView *sv, gpointer user_data) {
  (void)sv;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) return;

  /* nostrc-daj1: Validate user_pubkey_hex is actually hex, not npub */
  if (g_str_has_prefix(self->user_pubkey_hex, "npub1")) {
    g_warning("[PROFILE] user_pubkey_hex contains npub, not hex: %.16s... — converting",
              self->user_pubkey_hex);
    g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(self->user_pubkey_hex, NULL);
    if (n19) {
      const char *hex = gnostr_nip19_get_pubkey(n19);
      if (hex) {
        g_free(self->user_pubkey_hex);
        self->user_pubkey_hex = g_strdup(hex);
      }
    }
  }

  if (strlen(self->user_pubkey_hex) != 64) {
    g_warning("[PROFILE] user_pubkey_hex has invalid length %zu: %.16s...",
              strlen(self->user_pubkey_hex), self->user_pubkey_hex);
    return;
  }

  gnostr_main_window_open_profile(GTK_WIDGET(self), self->user_pubkey_hex);
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
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (settings) {
    g_settings_set_string(settings, "current-npub", npub);
    g_object_unref(settings);
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

  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return;

  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);

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

/* Profile pane signal handlers */
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  /* Get profile pane from session view */
  GtkWidget *profile_pane = self->session_view ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;

  /* Check if profile pane is currently visible */
  gboolean sidebar_visible = is_panel_visible(self) && gnostr_session_view_is_showing_profile(self->session_view);

  /* Check if profile pane is already showing this profile */
  extern const char* gnostr_profile_pane_get_current_pubkey(GnostrProfilePane *pane);
  if (profile_pane && GNOSTR_IS_PROFILE_PANE(profile_pane)) {
    const char *current = gnostr_profile_pane_get_current_pubkey(GNOSTR_PROFILE_PANE(profile_pane));
    if (sidebar_visible && current && strcmp(current, pubkey_hex) == 0) {
      /* Same profile clicked while sidebar is visible - toggle OFF */
      hide_panel(self);
      return;
    }
  }

  /* Different profile or sidebar was closed - show the profile pane */
  show_profile_panel(self);
  
  /* Set the pubkey on the profile pane — this triggers NDB cache lookup
   * and async network fetch internally via fetch_profile_from_cache_or_network().
   * nostrc-4aen: Previously we duplicated the NDB query here which caused
   * banner image loads to be cancelled by the redundant update_from_json call. */
  if (profile_pane && GNOSTR_IS_PROFILE_PANE(profile_pane)) {
    gnostr_profile_pane_set_pubkey(GNOSTR_PROFILE_PANE(profile_pane), pubkey_hex);
  }
}

static void on_profile_pane_close_requested(GnostrProfilePane *pane, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  hide_panel(self);
}

/* nostrc-ch2v: Handle mute user from profile pane */
static void on_profile_pane_mute_user_requested(GnostrProfilePane *pane, const char *pubkey_hex, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_warning("[MUTE] Invalid pubkey hex from profile pane");
    return;
  }

  g_debug("[MUTE] Mute user from profile pane for pubkey=%.16s...", pubkey_hex);

  GnostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  if (self->event_model) {
    gn_nostr_event_model_refresh(GN_NOSTR_EVENT_MODEL(self->event_model));
  }

  show_toast(self, "User muted");
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
  if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
    gnostr_thread_view_set_focus_event(GNOSTR_THREAD_VIEW(thread_view), event_id);
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
    char *preview = g_strdup_printf("Sending %s...", basename);
    GnostrDmMessage msg = {
        .event_id = NULL,
        .content = preview,
        .created_at = (gint64)(g_get_real_time() / 1000000),
        .is_outgoing = TRUE,
    };
    gnostr_dm_conversation_view_add_message(view, &msg);
    gnostr_dm_conversation_view_scroll_to_bottom(view);
    g_free(preview);
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
        if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
          gnostr_thread_view_clear(GNOSTR_THREAD_VIEW(thread_view));
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
  /* nostrc-akyz: defensively normalize npub/nprofile to hex */
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
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len) == 0 && meta_json) {
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
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len) == 0 && meta_json) {
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
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *meta_json = NULL;
        int meta_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len) == 0 && meta_json) {
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

static void on_note_card_repost_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  gnostr_main_window_request_repost(GTK_WIDGET(self), id_hex, pubkey_hex);
}

/* Signal handler for quote-requested from note card */
static void on_note_card_quote_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  gnostr_main_window_request_quote(GTK_WIDGET(self), id_hex, pubkey_hex);
}

/* Signal handler for like-requested from note card (NIP-25) */
static void on_note_card_like_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gnostr_main_window_request_like(GTK_WIDGET(self), id_hex, pubkey_hex, event_kind, reaction_content, row);
}

/* Signal handler for comment-requested from note card (NIP-22) */
static void on_note_card_comment_requested(GnostrNoteCardRow *row, const char *id_hex, int kind, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gnostr_main_window_request_comment(GTK_WIDGET(self), id_hex, kind, pubkey_hex);
}

/* Forward declaration for thread view close handler */
static void on_thread_view_close_requested(GnostrThreadView *view, gpointer user_data);
static void on_thread_view_open_profile(GnostrThreadView *view, const char *pubkey_hex, gpointer user_data);

/* Forward declaration */
void gnostr_main_window_view_thread_with_json(GtkWidget *window, const char *root_event_id, const char *event_json);

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
  if (!thread_view || !GNOSTR_IS_THREAD_VIEW(thread_view)) {
    g_warning("[THREAD] Thread view widget not available");
    show_toast(self, "Thread view not available");
    return;
  }

  /* Set the thread root and load the thread.
   * If event_json is provided, the event is pre-populated to avoid nostrdb lookup. */
  gnostr_thread_view_set_thread_root_with_json(GNOSTR_THREAD_VIEW(thread_view), root_event_id, event_json);

  /* Show the thread panel */
  show_thread_panel(self);
}

/* Handler for thread view close button */
static void on_thread_view_close_requested(GnostrThreadView *view, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Hide thread panel */
  hide_panel(self);

  /* Clear thread view to free resources */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
    gnostr_thread_view_clear(GNOSTR_THREAD_VIEW(thread_view));
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
static void on_thread_view_open_profile(GnostrThreadView *view, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close thread view first */
  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
    on_thread_view_close_requested(GNOSTR_THREAD_VIEW(thread_view), self);
  }

  /* Open profile pane */
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

/* Handler for need-profile signal from thread view - fetches profile from relays */
static void on_thread_view_need_profile(GnostrThreadView *view, const char *pubkey_hex, gpointer user_data) {
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
  GnostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  /* Refresh the timeline to filter out the muted user */
  if (self->event_model) {
    gn_nostr_event_model_refresh(GN_NOSTR_EVENT_MODEL(self->event_model));
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
  GnostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_event(mute_list, event_id_hex, FALSE);

  /* Refresh the timeline to filter out the muted thread */
  if (self->event_model) {
    gn_nostr_event_model_refresh(GN_NOSTR_EVENT_MODEL(self->event_model));
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
  if (storage_ndb_begin_query(&txn) == 0) {
    for (guint i = 0; i < authors->len; i++) {
      const char *pkhex = (const char*)g_ptr_array_index(authors, i);
      if (!pkhex || strlen(pkhex) != 64) continue;
      
      uint8_t pk32[32];
      if (!hex_to_bytes32(pkhex, pk32)) continue;
      
      char *pjson = NULL;
      int plen = 0;
      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen) == 0 && pjson && plen > 0) {
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
  
  /* NOTE: We still fetch ALL profiles from relays to check for updates.
   * Cached profiles give instant UI feedback, relay fetch keeps them fresh. */
  
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
        GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
        if (evt) {
          const char *pk = gnostr_event_get_pubkey(evt);
          if (pk) g_hash_table_add(unique_pks, g_strdup(pk));
          g_object_unref(evt);
        }
      }
    }
    guint unique_count = g_hash_table_size(unique_pks);
    g_hash_table_unref(unique_pks);
    g_debug("[PROFILE] Batch received %u events (%u unique authors)", jsons->len, unique_count);
    
    /* Ingest events ONE AT A TIME - batch ingestion fails if ANY event is invalid */
    guint ingested = 0, failed = 0;
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char*)g_ptr_array_index(jsons, i);
      if (!evt_json) continue;
      
      /* CRITICAL FIX: nostrdb requires "tags" field even if empty.
       * Many relays omit it. Add it if missing. */
      GString *fixed_json = g_string_new("");
      if (!strstr(evt_json, "\"tags\"")) {
        // Find insertion point after "kind" field
        const char *kind_pos = strstr(evt_json, "\"kind\"");
        if (kind_pos) {
          const char *comma_after_kind = strchr(kind_pos, ',');
          if (comma_after_kind) {
            // Copy prefix, insert tags, copy suffix
            g_string_append_len(fixed_json, evt_json, comma_after_kind - evt_json + 1);
            g_string_append(fixed_json, "\"tags\":[],");
            g_string_append(fixed_json, comma_after_kind + 1);
          } else {
            g_string_append(fixed_json, evt_json);
          }
        } else {
          g_string_append(fixed_json, evt_json);
        }
      } else {
        g_string_append(fixed_json, evt_json);
      }
      
      /* Use storage_ndb_ingest_event_json (same as live notes) instead of LDJSON */
      int ingest_rc = storage_ndb_ingest_event_json(fixed_json->str, NULL);
      if (ingest_rc != 0) {
        failed++;
        if (failed <= 3) {
          g_warning("profile_fetch: ingest FAILED rc=%d for event[%u]: %.100s", ingest_rc, i, evt_json);
        }
      } else {
        ingested++;
      }
      g_string_free(fixed_json, TRUE);
    }
    if (failed > 0) {
      g_warning("[PROFILE] Ingested %u/%u events (%u failed validation)", ingested, jsons->len, failed);
    }
    /* No LDJSON buffer to free: removed to avoid memory spikes */
    /* Now parse events for UI application */
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char*)g_ptr_array_index(jsons, i);
      if (!evt_json) continue;
      GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
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
        }
        deserialized++;
        g_object_unref(evt);
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
    g_idle_add_full(G_PRIORITY_DEFAULT,
                    (GSourceFunc)profile_dispatch_next, g_object_ref(self), 
                    (GDestroyNotify)g_object_unref);
    /* NOTE: Don't unref self here - ctx->self holds the reference and will be freed below */
  } else {
    g_warning("profile_fetch: cannot dispatch next batch - invalid context");
  }
  /* Free ctx - this will unref ctx->self */
  if (ctx && ctx->self) g_object_unref(ctx->self);
  g_free(ctx);
}


static void prepopulate_all_profiles_from_cache(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  void *txn = NULL; char **arr = NULL; int n = 0;
  int brc = storage_ndb_begin_query(&txn);
  if (brc != 0) { g_warning("prepopulate_all_profiles_from_cache: begin_query failed rc=%d", brc); return; }
  const char *filters = "[{\"kinds\":[0]}]"; /* all kind-0 profiles */
  int rc = storage_ndb_query(txn, filters, &arr, &n);
  g_debug("prepopulate_all_profiles_from_cache: query rc=%d count=%d", rc, n);
  if (rc == 0 && arr && n > 0) {
    GPtrArray *items = g_ptr_array_new_with_free_func(profile_apply_item_free);
    for (int i = 0; i < n; i++) {
      const char *evt_json = arr[i];
      if (!evt_json) continue;
      GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
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
        g_object_unref(evt);
      }
    }
    if (items->len > 0) {
      g_debug("prepopulate_all_profiles_from_cache: scheduling %u cached profiles", items->len);
      schedule_apply_profiles(self, items); /* transfers ownership */
      items = NULL;
    } else {
      g_ptr_array_free(items, TRUE);
    }
  }
  storage_ndb_free_results(arr, n);
  storage_ndb_end_query(txn);
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
    if (profile_pane && GNOSTR_IS_PROFILE_PANE(profile_pane)) {
      g_signal_connect(profile_pane, "close-requested",
                       G_CALLBACK(on_profile_pane_close_requested), self);
      /* nostrc-ch2v: Handle mute from profile pane */
      g_signal_connect(profile_pane, "mute-user-requested",
                       G_CALLBACK(on_profile_pane_mute_user_requested), self);
    }
    
    GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
    if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
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

  /* REPOMARK:SCOPE: 4 - Wire GnNostrEventModel "need-profile" signal to enqueue_profile_author() and disable legacy thread_roots/prefetch initialization in gnostr_main_window_init */
  g_signal_connect(self->event_model, "need-profile", G_CALLBACK(on_event_model_need_profile), self);
  /* nostrc-yi2: Calm timeline - connect new items pending signal */
  g_signal_connect(self->event_model, "new-items-pending", G_CALLBACK(on_event_model_new_items_pending), self);
  
  /* Attach model to timeline view (accessed via session view) */
  GtkWidget *timeline = self->session_view ? gnostr_session_view_get_timeline(self->session_view) : NULL;
  if (timeline && G_TYPE_CHECK_INSTANCE_TYPE(timeline, GNOSTR_TYPE_TIMELINE_VIEW)) {
    /* Wrap GListModel in a selection model */
    GtkSelectionModel *selection = GTK_SELECTION_MODEL(
      gtk_single_selection_new(G_LIST_MODEL(self->event_model))
    );
    gnostr_timeline_view_set_model(GNOSTR_TIMELINE_VIEW(timeline), selection);
    g_object_unref(selection); /* View takes ownership */

    /* Connect scroll edge detection for sliding window pagination */
    GtkWidget *scroller = gnostr_timeline_view_get_scrolled_window(GNOSTR_TIMELINE_VIEW(timeline));
    if (scroller && GTK_IS_SCROLLED_WINDOW(scroller)) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
      if (vadj) {
        g_signal_connect(vadj, "value-changed", G_CALLBACK(on_timeline_scroll_value_changed), self);
      }
    }

    /* nostrc-7vm: Connect tab filter changed signal for hashtag/author feeds */
    g_signal_connect(timeline, "tab-filter-changed", G_CALLBACK(on_timeline_tab_filter_changed), self);

    /* Do NOT call refresh here; we refresh once in initial_refresh_timeout_cb to avoid duplicate rebuilds. */
  }
  
  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize profile provider */
  gnostr_profile_provider_init(0); /* Use env/default cap */
  /* LEGITIMATE TIMEOUTS - Periodic stats logging (60s intervals).
   * nostrc-b0h: Audited - diagnostic logging at fixed intervals is appropriate. */
  g_timeout_add_seconds(60, (GSourceFunc)gnostr_profile_provider_log_stats, NULL);
  g_timeout_add_seconds(60, memory_stats_cb, self);
  /* Avatar texture cache is managed by gnostr-avatar-cache module (initialized on first use) */
  /* Initialize liked events cache (NIP-25 reactions) */
  self->liked_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize reconnection flag */
  self->reconnection_in_progress = FALSE;
  /* Pre-populate/apply cached profiles here */
  prepopulate_all_profiles_from_cache(self);
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
  
  /* CRITICAL: Initialize pool and relays BEFORE timeline prepopulation!
   * Timeline prepopulation triggers profile fetches, which need relays in the pool.
   * If we prepopulate first, profile fetches will skip all relays (not in pool yet). */
  start_pool_live(self);
  /* Also start profile subscription if identity is configured */
  start_profile_subscription(self);

  /* NOTE: Periodic refresh disabled - nostrdb ingestion drives UI updates via GnNostrEventModel.
   * This avoids duplicate processing and high memory usage. Initial refresh occurs in
   * initial_refresh_timeout_cb, and subsequent updates stream from nostrdb watchers. */

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
  if (!self->pool) self->pool = gnostr_pool_new();

  /* NIP-42: Install relay AUTH handler so challenges are signed automatically (nostrc-kn38) */
  gnostr_nip42_setup_pool_auth(self->pool);

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

  /* nostrc-bkor: Start gift wrap subscription AFTER state init and DM service setup.
   * This sets user_pubkey_hex from saved settings on app restart. */
  start_gift_wrap_subscription(self);

  /* Seed initial items so Timeline page isn't empty.
   * 150ms delay allows relay pool setup (started above) to begin connecting.
   * nostrc-b0h: Audited - brief delay for async initialization is appropriate. */
  g_timeout_add_once(150, (GSourceOnceFunc)initial_refresh_timeout_cb, self);

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
        (GSourceFunc)periodic_backfill_cb,
        g_object_ref(self),
        (GDestroyNotify)g_object_unref);
  }

  /* DB-only: startup profile prepopulation and backfill are handled above in init */

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
  }
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
  if (self->loading_older) return; /* Already loading */

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
    self->loading_older = TRUE; /* Reuse flag to prevent concurrent loads */
    guint added = gn_nostr_event_model_load_newer(self->event_model, batch);
    (void)added;
    self->loading_older = FALSE;

    /* Trim older events to keep memory bounded */
    guint current = g_list_model_get_n_items(G_LIST_MODEL(self->event_model));
    if (current > max_items) {
      gn_nostr_event_model_trim_older(self->event_model, max_items);
    }
    return;
  }

  /* Trigger load older when within 20% of the bottom */
  gdouble bottom_threshold = upper - page_size - (page_size * 0.2);
  if (value >= bottom_threshold && upper > page_size) {
    self->loading_older = TRUE;
    guint added = gn_nostr_event_model_load_older(self->event_model, batch);
    g_debug("[SCROLL] Loaded %u older events", added);
    self->loading_older = FALSE;

    /* Trim newer events to keep memory bounded */
    guint current = g_list_model_get_n_items(G_LIST_MODEL(self->event_model));
    if (current > max_items) {
      gn_nostr_event_model_trim_newer(self->event_model, max_items);
    }
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
static void on_timeline_tab_filter_changed(GnostrTimelineView *view, guint type, const char *filter_value, gpointer user_data) {
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->event_model) return;

  g_debug("[TAB_FILTER] type=%u filter='%s'", type, filter_value ? filter_value : "(null)");

  GnTimelineQuery *query = NULL;

  switch ((GnTimelineTabType)type) {
    case GN_TIMELINE_TAB_GLOBAL:
      /* Global timeline - kinds 1 and 6, no filter */
      query = gn_timeline_query_new_global();
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
            query = gn_timeline_query_new_for_authors((const char **)followed, n_followed);
            g_debug("[TAB_FILTER] Following tab: %zu followed pubkeys", n_followed);
          }
          g_strfreev(followed);
        }
      }
      if (!query) {
        /* Fallback: not logged in or no follows yet */
        query = gn_timeline_query_new_global();
        g_debug("[TAB_FILTER] Following tab: no contact list, showing global");
      }
      break;

    case GN_TIMELINE_TAB_HASHTAG:
      /* Hashtag filter */
      if (filter_value && *filter_value) {
        query = gn_timeline_query_new_for_hashtag(filter_value);
        g_debug("[TAB_FILTER] Created hashtag query for #%s", filter_value);
      } else {
        query = gn_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_AUTHOR:
      /* Author filter */
      if (filter_value && *filter_value) {
        query = gn_timeline_query_new_for_author(filter_value);
        g_debug("[TAB_FILTER] Created author query for %s", filter_value);
      } else {
        query = gn_timeline_query_new_global();
      }
      break;

    case GN_TIMELINE_TAB_CUSTOM:
      /* Custom filter - fallback to global for now */
      query = gn_timeline_query_new_global();
      break;
  }

  if (query) {
    /* Apply the new query to the model */
    gn_nostr_event_model_set_timeline_query(self->event_model, query);
    gn_nostr_event_model_refresh(self->event_model);
    gn_timeline_query_free(query);
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
  if (timeline && GNOSTR_IS_TIMELINE_VIEW(timeline)) {
    /* Get the internal GtkListView from the timeline and use gtk_list_view_scroll_to
     * which is the GTK4-recommended way to scroll to a specific item.
     * Using adjustment directly doesn't work reliably with ListView because
     * the adjustment bounds may not be updated yet after model changes. */
    GtkWidget *list_view = gnostr_timeline_view_get_list_view(GNOSTR_TIMELINE_VIEW(timeline));
    if (list_view && GTK_IS_LIST_VIEW(list_view)) {
      gtk_list_view_scroll_to(GTK_LIST_VIEW(list_view), 0, GTK_LIST_SCROLL_FOCUS, NULL);
    }
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

  /* Hide new notes indicator immediately */
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_new_notes_count(self->session_view, 0);
  }

  /* Mark user as at top BEFORE flushing so new items insert directly */
  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model)) {
    gn_nostr_event_model_set_user_at_top(self->event_model, TRUE);
  }

  /* Flush pending notes - this now uses batched insertion with single signal */
  if (self->event_model && GN_IS_NOSTR_EVENT_MODEL(self->event_model)) {
    gn_nostr_event_model_flush_pending(self->event_model);
  }

  /* Defer scroll to next main loop iteration to let GTK finish processing
   * the model changes from flush_pending. Using g_idle_add_full with HIGH
   * priority runs before most other idle handlers but after GTK's internal
   * processing completes. This is much faster than the previous 150ms timeout
   * while still avoiding ListView crashes during widget recycling.
   * We ref the window to ensure it stays valid until the idle fires. */
  g_idle_add_full(G_PRIORITY_HIGH, scroll_to_top_idle, g_object_ref(self), NULL);
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
  GtkWidget *composer = gnostr_composer_new();

  /* Connect the post signal to our existing handler */
  g_signal_connect(composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);

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
  const char *content = gnostr_article_composer_get_content(composer);
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
  GtkWidget *composer = gnostr_composer_new();

  /* Connect the post signal to our existing handler */
  g_signal_connect(composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);

  /* Store dialog reference on composer so we can close it after post */
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);

  /* Set context on composer */
  if (context) {
    switch (context->type) {
      case COMPOSE_CONTEXT_REPLY:
        gnostr_composer_set_reply_context(GNOSTR_COMPOSER(composer),
                                          context->reply_to_id,
                                          context->root_id,
                                          context->reply_to_pubkey,
                                          context->display_name);
        break;
      case COMPOSE_CONTEXT_QUOTE:
        gnostr_composer_set_quote_context(GNOSTR_COMPOSER(composer),
                                          context->quote_id,
                                          context->quote_pubkey,
                                          context->nostr_uri,
                                          context->display_name);
        break;
      case COMPOSE_CONTEXT_COMMENT:
        gnostr_composer_set_comment_context(GNOSTR_COMPOSER(composer),
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

static void gnostr_main_window_dispose(GObject *object) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  g_debug("main-window: dispose");

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

  if (self->profile_fetch_cancellable) { g_object_unref(self->profile_fetch_cancellable); self->profile_fetch_cancellable = NULL; }
  if (self->bg_prefetch_cancellable) { g_object_unref(self->bg_prefetch_cancellable); self->bg_prefetch_cancellable = NULL; }
  if (self->pool_cancellable) { g_object_unref(self->pool_cancellable); self->pool_cancellable = NULL; }
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
  if (self->profile_batch_filters) {
    nostr_filters_free(self->profile_batch_filters);
    self->profile_batch_filters = NULL;
  }
  if (self->profile_pool) {
    g_object_unref(self->profile_pool);
    self->profile_pool = NULL;
  }
  if (self->pool) {
    /* Disconnect signal handlers BEFORE unreffing to prevent use-after-free
     * when pending main loop callbacks try to emit on the freed pool */
    if (self->pool_events_handler) {
      g_signal_handler_disconnect(self->pool, self->pool_events_handler);
      self->pool_events_handler = 0;
    }
    /* Disconnect all remaining handlers from this instance (e.g., bg prefetch) */
    g_signal_handlers_disconnect_by_data(self->pool, self);
    g_object_unref(self->pool);
    self->pool = NULL;
  }
  if (self->seen_texts) { g_hash_table_unref(self->seen_texts); self->seen_texts = NULL; }
  if (self->event_model) { g_object_unref(self->event_model); self->event_model = NULL; }
  /* Avatar texture cache cleanup is handled by gnostr-avatar-cache module */
  if (self->liked_events) { g_hash_table_unref(self->liked_events); self->liked_events = NULL; }

  /* Stop gift wrap subscription */
  stop_gift_wrap_subscription(self);
  if (self->gift_wrap_queue) {
    g_ptr_array_free(self->gift_wrap_queue, TRUE);
    self->gift_wrap_queue = NULL;
  }

  /* Stop and cleanup DM service */
  if (self->dm_service) {
    gnostr_dm_service_stop(self->dm_service);
    g_object_unref(self->dm_service);
    self->dm_service = NULL;
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
  g_debug("STARTUP_DEBUG: initial_refresh_timeout_cb ENTER");

  if (self->event_model) {
    gn_nostr_event_model_refresh(self->event_model);
  }

  gnostr_main_window_set_page(self, GNOSTR_MAIN_WINDOW_PAGE_SESSION);

  g_debug("STARTUP_DEBUG: initial_refresh_timeout_cb EXIT");
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
  GSettings *client_settings = g_settings_new("org.gnostr.Client");
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
    g_object_unref(client_settings);
  }
}

/* Context for async sign-and-publish operation */
struct _PublishContext {
  GnostrMainWindow *self;  /* weak ref */
  char *text;              /* owned; original note content */
  GnostrComposer *composer; /* weak ref; for clearing/closing dialog on success */
};

static void publish_context_free(PublishContext *ctx) {
  if (!ctx) return;
  g_free(ctx->text);
  g_free(ctx);
}

/* Callback when unified signer service completes signing */
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  PublishContext *ctx = (PublishContext*)user_data;
  (void)source; /* Not used with unified signer service */

  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    publish_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    char *msg = g_strdup_printf("Signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&error);
    publish_context_free(ctx);
    return;
  }

  g_debug("[PUBLISH] Signed event: %.100s...", signed_event_json);

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    show_toast(self, "Failed to parse signed event");
    nostr_event_free(event);
    g_free(signed_event_json);
    publish_context_free(ctx);
    return;
  }

  /* Get write-capable relay URLs from config (NIP-65: write-only or read+write) */
  GPtrArray *relay_urls = gnostr_get_write_relay_urls();

  /* Extract event properties for validation */
  const char *content = nostr_event_get_content(event);
  gint content_len = content ? (gint)strlen(content) : 0;
  NostrTags *tags = nostr_event_get_tags(event);
  gint tag_count = tags ? (gint)nostr_tags_size(tags) : 0;
  gint64 created_at = nostr_event_get_created_at(event);
  gssize serialized_len = signed_event_json ? (gssize)strlen(signed_event_json) : -1;

  /* Publish to each write relay */
  guint success_count = 0;
  guint fail_count = 0;
  guint limit_skip_count = 0;
  GString *limit_warnings = g_string_new(NULL);

  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = (const char*)g_ptr_array_index(relay_urls, i);

    /* NIP-11: Check relay limitations before publishing */
    GnostrRelayInfo *relay_info = gnostr_relay_info_cache_get(url);
    if (relay_info) {
      GnostrRelayValidationResult *validation = gnostr_relay_info_validate_event(
        relay_info, content, content_len, tag_count, created_at, serialized_len);

      if (!gnostr_relay_validation_result_is_valid(validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(validation);
        if (errors) {
          if (limit_warnings->len > 0) g_string_append(limit_warnings, "\n");
          g_string_append(limit_warnings, errors);
          g_free(errors);
        }
        gnostr_relay_validation_result_free(validation);
        gnostr_relay_info_free(relay_info);
        limit_skip_count++;
        continue;
      }
      gnostr_relay_validation_result_free(validation);

      /* nostrc-23: Check auth_required / payment_required before publishing */
      GnostrRelayValidationResult *pub_validation =
          gnostr_relay_info_validate_for_publishing(relay_info);
      if (!gnostr_relay_validation_result_is_valid(pub_validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(pub_validation);
        if (errors) {
          if (limit_warnings->len > 0) g_string_append(limit_warnings, "\n");
          g_string_append(limit_warnings, errors);
          g_free(errors);
        }
        gnostr_relay_validation_result_free(pub_validation);
        gnostr_relay_info_free(relay_info);
        limit_skip_count++;
        continue;
      }
      gnostr_relay_validation_result_free(pub_validation);
      gnostr_relay_info_free(relay_info);
    }

    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) {
      fail_count++;
      continue;
    }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      success_count++;
    } else {
      g_clear_error(&pub_err);
      fail_count++;
    }
    g_object_unref(relay);
  }

  /* Show result toast */
  if (success_count > 0) {
    char *msg;
    if (limit_skip_count > 0) {
      msg = g_strdup_printf("Published to %u relay%s (%u skipped due to limits)",
                            success_count, success_count == 1 ? "" : "s", limit_skip_count);
    } else {
      msg = g_strdup_printf("Published to %u relay%s", success_count, success_count == 1 ? "" : "s");
    }
    show_toast(self, msg);
    g_free(msg);

    /* nostrc-c0mp: Close compose dialog and clear composer on successful publish */
    if (ctx->composer && GNOSTR_IS_COMPOSER(ctx->composer)) {
      /* Get the dialog reference stored on the composer */
      AdwDialog *dialog = ADW_DIALOG(g_object_get_data(G_OBJECT(ctx->composer), "compose-dialog"));
      if (dialog && ADW_IS_DIALOG(dialog)) {
        adw_dialog_force_close(dialog);
      }
      /* Clear the composer for next use */
      gnostr_composer_clear(ctx->composer);
    }

    /* Switch to timeline tab via session view */
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_show_page(self->session_view, "timeline");
    }
  } else {
    if (limit_skip_count > 0 && limit_warnings->len > 0) {
      /* All relays skipped due to limits - show detailed warning */
      char *msg = g_strdup_printf("Event exceeds relay limits:\n%s", limit_warnings->str);
      show_toast(self, msg);
      g_free(msg);
    } else {
      show_toast(self, "Failed to publish to any relay");
    }
  }

  /* Log limit warnings for debugging */
  if (limit_warnings->len > 0) {
    g_warning("[PUBLISH] Relay limit violations:\n%s", limit_warnings->str);
  }

  /* Cleanup */
  g_string_free(limit_warnings, TRUE);
  nostr_event_free(event);
  g_free(signed_event_json);
  g_ptr_array_free(relay_urls, TRUE);
  publish_context_free(ctx);
}

/* Public wrapper for requesting a repost (kind 6) - must be after PublishContext is defined */
void gnostr_main_window_request_repost(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[REPOST] Request repost of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for repost");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  show_toast(self, "Reposting...");

  /* Build unsigned kind 6 repost event JSON using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 6);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Build tags array: e-tag and p-tag */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<reposted-event-id>", "<relay-hint>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_add_string(builder, ""); /* relay hint - empty for now */
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<original-author-pubkey>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to serialize repost event");
    return;
  }

  g_debug("[REPOST] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(""); /* repost has no text content */

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-09 Event Deletion Implementation ================= */

/* Public wrapper for requesting deletion of a note (kind 5) per NIP-09 */
void gnostr_main_window_request_delete_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[DELETE] Request deletion of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for deletion");
    return;
  }

  /* Verify user is signed in and owns the note */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    show_toast(self, "Sign in to delete notes");
    return;
  }

  /* Security check: Only allow deletion of own notes */
  if (!pubkey_hex || strlen(pubkey_hex) != 64 ||
      g_ascii_strcasecmp(pubkey_hex, self->user_pubkey_hex) != 0) {
    show_toast(self, "Can only delete your own notes");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  show_toast(self, "Deleting note...");

  /* Build unsigned kind 5 deletion event JSON per NIP-09 using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 5);  /* NOSTR_KIND_DELETION */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, ""); /* Optional deletion reason */

  /* Build tags array per NIP-09:
   * - ["e", "<event-id-to-delete>"]
   * - ["k", "<kind-of-deleted-event>"] (kind 1 for text notes)
   */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<event-id-to-delete>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* k-tag: ["k", "1"] to indicate we're deleting a kind 1 note */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "k");
  gnostr_json_builder_add_string(builder, "1");
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to serialize deletion event");
    return;
  }

  g_debug("[DELETE] Unsigned deletion event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(""); /* deletion has no text content */

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-56 Report Implementation ================= */

/* Public wrapper for reporting a note/user (kind 1984) per NIP-56 */
void gnostr_main_window_request_report_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[NIP-56] Request report of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  /* Verify user is signed in */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    show_toast(self, "Sign in to report content");
    return;
  }

  /* Validate pubkey */
  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    show_toast(self, "Invalid target for report");
    return;
  }

  /* Create and show report dialog */
  GnostrReportDialog *dialog = gnostr_report_dialog_new(GTK_WINDOW(self));
  gnostr_report_dialog_set_target(dialog, id_hex, pubkey_hex);

  /* Connect to signals for feedback */
  g_signal_connect_swapped(dialog, "report-sent", G_CALLBACK(gtk_window_destroy), dialog);

  gtk_window_present(GTK_WINDOW(dialog));
}

/* ================= NIP-32 Labeling Implementation ================= */

/* Public wrapper for adding a label to a note (kind 1985) per NIP-32 */
void gnostr_main_window_request_label_note(GtkWidget *window, const char *id_hex, const char *namespace, const char *label, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[NIP-32] Request label of id=%s namespace=%s label=%s",
            id_hex ? id_hex : "(null)",
            namespace ? namespace : "(null)",
            label ? label : "(null)");

  /* Verify user is signed in */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    show_toast(self, "Sign in to add labels");
    return;
  }

  /* Validate inputs */
  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for labeling");
    return;
  }

  if (!namespace || !*namespace || !label || !*label) {
    show_toast(self, "Label and namespace are required");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  /* Build unsigned kind 1985 label event JSON per NIP-32 using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 1985);  /* NOSTR_KIND_LABEL */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Build tags array per NIP-32:
   * - ["L", "<namespace>"]
   * - ["l", "<label>", "<namespace>"]
   * - ["e", "<event-id>"]
   * - ["p", "<event-author-pubkey>"]
   */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* L-tag: ["L", "<namespace>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "L");
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* l-tag: ["l", "<label>", "<namespace>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "l");
  gnostr_json_builder_add_string(builder, label);
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* e-tag: ["e", "<event-id>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<event-author-pubkey>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to create label event");
    return;
  }

  g_debug("[NIP-32] Unsigned label event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(""); /* label has no text content */

  /* Call unified signer service */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-25 Like/Reaction Implementation ================= */

/* Context for async like/reaction operation */
struct _LikeContext {
  GnostrMainWindow *self;       /* weak ref */
  char *event_id_hex;           /* owned; event being liked */
  GnostrNoteCardRow *row;       /* weak ref; row to update on success */
};

static void like_context_free(LikeContext *ctx) {
  if (!ctx) return;
  g_free(ctx->event_id_hex);
  g_free(ctx);
}

/* Callback when unified signer completes signing for like/reaction */
static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  LikeContext *ctx = (LikeContext*)user_data;
  (void)source;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    like_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    char *msg = g_strdup_printf("Like signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&error);
    like_context_free(ctx);
    return;
  }

  g_debug("[LIKE] Signed reaction event: %.100s...", signed_event_json);

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    show_toast(self, "Failed to parse signed reaction event");
    nostr_event_free(event);
    g_free(signed_event_json);
    like_context_free(ctx);
    return;
  }

  /* Get write-capable relay URLs from config (NIP-65: write-only or read+write) */
  GPtrArray *relay_urls = gnostr_get_write_relay_urls();

  /* Extract event properties for NIP-11 validation */
  const char *like_content = nostr_event_get_content(event);
  gint like_content_len = like_content ? (gint)strlen(like_content) : 0;
  NostrTags *like_tags = nostr_event_get_tags(event);
  gint like_tag_count = like_tags ? (gint)nostr_tags_size(like_tags) : 0;
  gint64 like_created_at = nostr_event_get_created_at(event);
  gssize like_serialized_len = signed_event_json ? (gssize)strlen(signed_event_json) : -1;

  /* Publish to each write relay */
  guint success_count = 0;
  guint fail_count = 0;
  guint limit_skip_count = 0;

  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = (const char*)g_ptr_array_index(relay_urls, i);

    /* NIP-11: Check relay limitations before publishing */
    GnostrRelayInfo *relay_info = gnostr_relay_info_cache_get(url);
    if (relay_info) {
      GnostrRelayValidationResult *validation = gnostr_relay_info_validate_event(
        relay_info, like_content, like_content_len, like_tag_count, like_created_at, like_serialized_len);

      if (!gnostr_relay_validation_result_is_valid(validation)) {
        gnostr_relay_validation_result_free(validation);
        gnostr_relay_info_free(relay_info);
        limit_skip_count++;
        continue;
      }
      gnostr_relay_validation_result_free(validation);
      gnostr_relay_info_free(relay_info);
    }

    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) {
      fail_count++;
      continue;
    }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      success_count++;
    } else {
      g_clear_error(&pub_err);
      fail_count++;
    }
    g_object_unref(relay);
  }

  /* Show result toast and update UI */
  if (success_count > 0) {
    if (limit_skip_count > 0) {
      char *msg = g_strdup_printf("Liked! (%u relays skipped)", limit_skip_count);
      show_toast(self, msg);
      g_free(msg);
    } else {
      show_toast(self, "Liked!");
    }

    /* Mark event as liked in local cache */
    if (ctx->event_id_hex && self->liked_events) {
      g_hash_table_insert(self->liked_events, g_strdup(ctx->event_id_hex), GINT_TO_POINTER(1));
    }

    /* Update note card row to show liked state */
    if (ctx->row && GNOSTR_IS_NOTE_CARD_ROW(ctx->row)) {
      gnostr_note_card_row_set_liked(ctx->row, TRUE);
    }

    /* Store reaction in local NostrdB cache */
    if (signed_event_json) {
      int ingest_rc = storage_ndb_ingest_event_json(signed_event_json, NULL);
      if (ingest_rc != 0) {
        g_warning("[LIKE] Failed to store reaction locally");
      } else {
      }
    }
  } else {
    show_toast(self, "Failed to publish reaction");
  }

  /* Cleanup */
  nostr_event_free(event);
  g_free(signed_event_json);
  g_ptr_array_free(relay_urls, TRUE);
  like_context_free(ctx);
}

/* Public function to request a like/reaction (kind 7) - NIP-25
 * @event_kind: the kind of the event being reacted to (for k-tag)
 * @reaction_content: the reaction content ("+" for like, "-" for dislike, or emoji) */
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, GnostrNoteCardRow *row) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Default reaction content to "+" if not specified */
  if (!reaction_content || !*reaction_content) {
    reaction_content = "+";
  }

  g_debug("[LIKE] Request reaction '%s' on id=%s pubkey=%.8s... kind=%d",
            reaction_content,
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)",
            event_kind);

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for reaction");
    return;
  }

  /* Check if already liked (only for "+" reactions) */
  if (strcmp(reaction_content, "+") == 0 && self->liked_events && g_hash_table_contains(self->liked_events, id_hex)) {
    show_toast(self, "Already liked!");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  /* Show appropriate toast based on reaction type */
  if (strcmp(reaction_content, "+") == 0) {
    show_toast(self, "Liking...");
  } else if (strcmp(reaction_content, "-") == 0) {
    show_toast(self, "Reacting...");
  } else {
    char *msg = g_strdup_printf("Reacting with %s...", reaction_content);
    show_toast(self, msg);
    g_free(msg);
  }

  /* Build unsigned kind 7 reaction event JSON (NIP-25) using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NOSTR_KIND_REACTION);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, reaction_content);

  /* Build tags array: e-tag, p-tag, and k-tag per NIP-25 */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<event-id-being-reacted-to>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<pubkey-of-event-author>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  /* k-tag: ["k", "<kind-of-reacted-event>"] per NIP-25 */
  char kind_str[16];
  snprintf(kind_str, sizeof(kind_str), "%d", event_kind > 0 ? event_kind : 1);
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "k");
  gnostr_json_builder_add_string(builder, kind_str);
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to serialize reaction event");
    return;
  }

  g_debug("[LIKE] Unsigned reaction event: %s", event_json);

  /* Create async context */
  LikeContext *ctx = g_new0(LikeContext, 1);
  ctx->self = self;
  ctx->event_id_hex = g_strdup(id_hex);
  ctx->row = row; /* weak ref - may be invalid by callback time */

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_like_event_complete,
    ctx
  );
  g_free(event_json);
}

static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Validate input */
  if (!text || !*text) {
    show_toast(self, "Cannot post empty note");
    return;
  }

  /* Check if signer is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available - please sign in");
    return;
  }

  show_toast(self, "Signing...");

  /* Build unsigned event JSON using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* NIP-22: Check if this is a comment (kind 1111) - takes precedence over reply/quote */
  gboolean is_comment = (composer && GNOSTR_IS_COMPOSER(composer) &&
                         gnostr_composer_is_comment(GNOSTR_COMPOSER(composer)));

  if (is_comment) {
    const char *comment_root_id = gnostr_composer_get_comment_root_id(GNOSTR_COMPOSER(composer));
    int comment_root_kind = gnostr_composer_get_comment_root_kind(GNOSTR_COMPOSER(composer));
    const char *comment_root_pubkey = gnostr_composer_get_comment_root_pubkey(GNOSTR_COMPOSER(composer));

    g_debug("[PUBLISH] Building NIP-22 comment event: root_id=%s root_kind=%d pubkey=%.8s...",
            comment_root_id ? comment_root_id : "(null)",
            comment_root_kind,
            comment_root_pubkey ? comment_root_pubkey : "(null)");

    /* Set kind 1111 for comment */
    gnostr_json_builder_set_key(builder, "kind");
    gnostr_json_builder_add_int(builder, 1111);

    gnostr_json_builder_set_key(builder, "created_at");
    gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
    gnostr_json_builder_set_key(builder, "content");
    gnostr_json_builder_add_string(builder, text);

    /* Build tags array */
    gnostr_json_builder_set_key(builder, "tags");
    gnostr_json_builder_begin_array(builder);

    /* NIP-22 requires these tags:
     * - ["K", "<root-kind>"] - kind of the root event
     * - ["E", "<root-id>", "<relay>", "<pubkey>"] - root event reference
     * - ["P", "<root-pubkey>"] - root event author
     */

    /* K tag: root event kind */
    char kind_str[16];
    g_snprintf(kind_str, sizeof(kind_str), "%d", comment_root_kind);
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "K");
    gnostr_json_builder_add_string(builder, kind_str);
    gnostr_json_builder_end_array(builder);

    /* E tag: root event reference */
    if (comment_root_id && strlen(comment_root_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "E");
      gnostr_json_builder_add_string(builder, comment_root_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      if (comment_root_pubkey && strlen(comment_root_pubkey) == 64) {
        gnostr_json_builder_add_string(builder, comment_root_pubkey); /* author hint */
      }
      gnostr_json_builder_end_array(builder);
    }

    /* P tag: root event author */
    if (comment_root_pubkey && strlen(comment_root_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "P");
      gnostr_json_builder_add_string(builder, comment_root_pubkey);
      gnostr_json_builder_end_array(builder);
    }

    gnostr_json_builder_end_array(builder); /* end tags */
    gnostr_json_builder_end_object(builder);

    char *event_json = gnostr_json_builder_finish(builder);
    g_object_unref(builder);

    if (!event_json) {
      show_toast(self, "Failed to build event JSON");
      return;
    }

    g_debug("[PUBLISH] Unsigned NIP-22 comment event: %s", event_json);

    /* Create async context and sign */
    PublishContext *ctx = g_new0(PublishContext, 1);
    ctx->self = self;
    ctx->text = g_strdup(text);
    ctx->composer = composer; /* weak ref for closing dialog on success */

    gnostr_sign_event_async(
      event_json,
      "",              /* current_user: ignored */
      "gnostr",        /* app_id: ignored */
      NULL,            /* cancellable */
      on_sign_event_complete,
      ctx
    );
    g_free(event_json);
    return;
  }

  /* Regular kind 1 text note */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 1);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, text);

  /* Build tags array */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* Check if this is a reply - add NIP-10 threading tags (only for kind 1, not comments) */
  if (!is_comment && composer && GNOSTR_IS_COMPOSER(composer) && gnostr_composer_is_reply(GNOSTR_COMPOSER(composer))) {
    const char *reply_to_id = gnostr_composer_get_reply_to_id(GNOSTR_COMPOSER(composer));
    const char *root_id = gnostr_composer_get_root_id(GNOSTR_COMPOSER(composer));
    const char *reply_to_pubkey = gnostr_composer_get_reply_to_pubkey(GNOSTR_COMPOSER(composer));

    g_debug("[PUBLISH] Building reply event: reply_to=%s root=%s pubkey=%.8s...",
            reply_to_id ? reply_to_id : "(null)",
            root_id ? root_id : "(null)",
            reply_to_pubkey ? reply_to_pubkey : "(null)");

    /* NIP-10 recommends using positional markers for replies:
     * - ["e", "<root-id>", "", "root"] for the thread root
     * - ["e", "<reply-id>", "", "reply"] for the direct parent
     * - ["p", "<pubkey>"] for all mentioned pubkeys
     */

    /* Add root e-tag (always present for replies) */
    if (root_id && strlen(root_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "e");
      gnostr_json_builder_add_string(builder, root_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_add_string(builder, "root");
      gnostr_json_builder_end_array(builder);
    }

    /* Add reply e-tag if different from root (nested reply) */
    if (reply_to_id && strlen(reply_to_id) == 64 &&
        (!root_id || strcmp(reply_to_id, root_id) != 0)) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "e");
      gnostr_json_builder_add_string(builder, reply_to_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_add_string(builder, "reply");
      gnostr_json_builder_end_array(builder);
    }

    /* Add p-tag for the author being replied to */
    if (reply_to_pubkey && strlen(reply_to_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "p");
      gnostr_json_builder_add_string(builder, reply_to_pubkey);
      gnostr_json_builder_end_array(builder);
    }
  }

  /* Check if this is a quote post - add q-tag and p-tag per NIP-18 (only for kind 1, not comments) */
  if (!is_comment && composer && GNOSTR_IS_COMPOSER(composer) && gnostr_composer_is_quote(GNOSTR_COMPOSER(composer))) {
    const char *quote_id = gnostr_composer_get_quote_id(GNOSTR_COMPOSER(composer));
    const char *quote_pubkey = gnostr_composer_get_quote_pubkey(GNOSTR_COMPOSER(composer));

    g_debug("[PUBLISH] Building quote post: quote_id=%s pubkey=%.8s...",
            quote_id ? quote_id : "(null)",
            quote_pubkey ? quote_pubkey : "(null)");

    /* q-tag: ["q", "<quoted-event-id>", "<relay-hint>"] */
    if (quote_id && strlen(quote_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "q");
      gnostr_json_builder_add_string(builder, quote_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_end_array(builder);
    }

    /* p-tag: ["p", "<quoted-author-pubkey>"] */
    if (quote_pubkey && strlen(quote_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "p");
      gnostr_json_builder_add_string(builder, quote_pubkey);
      gnostr_json_builder_end_array(builder);
    }
  }

  /* NIP-14: Add subject tag if present */
  if (composer && GNOSTR_IS_COMPOSER(composer)) {
    const char *subject = gnostr_composer_get_subject(GNOSTR_COMPOSER(composer));
    if (subject && *subject) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "subject");
      gnostr_json_builder_add_string(builder, subject);
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added subject tag: %s", subject);
    }
  }

  /* NIP-92: Add imeta tags for uploaded media */
  if (composer && GNOSTR_IS_COMPOSER(composer)) {
    gsize media_count = gnostr_composer_get_uploaded_media_count(GNOSTR_COMPOSER(composer));
    if (media_count > 0) {
      GnostrComposerMedia **media_list = gnostr_composer_get_uploaded_media(GNOSTR_COMPOSER(composer));
      for (gsize i = 0; i < media_count && media_list && media_list[i]; i++) {
        GnostrComposerMedia *m = media_list[i];
        if (!m->url) continue;

        /* Build imeta tag: ["imeta", "url <url>", "m <mime>", "x <sha256>", "size <bytes>"] */
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "imeta");

        /* url field (required) */
        char *url_field = g_strdup_printf("url %s", m->url);
        gnostr_json_builder_add_string(builder, url_field);
        g_free(url_field);

        /* m field (MIME type) */
        if (m->mime_type && *m->mime_type) {
          char *mime_field = g_strdup_printf("m %s", m->mime_type);
          gnostr_json_builder_add_string(builder, mime_field);
          g_free(mime_field);
        }

        /* x field (SHA-256 hash) */
        if (m->sha256 && *m->sha256) {
          char *hash_field = g_strdup_printf("x %s", m->sha256);
          gnostr_json_builder_add_string(builder, hash_field);
          g_free(hash_field);
        }

        /* size field */
        if (m->size > 0) {
          char *size_field = g_strdup_printf("size %" G_GINT64_FORMAT, m->size);
          gnostr_json_builder_add_string(builder, size_field);
          g_free(size_field);
        }

        gnostr_json_builder_end_array(builder);
        g_debug("[PUBLISH] Added imeta tag for: %s (type=%s, sha256=%.16s...)",
                m->url, m->mime_type ? m->mime_type : "?",
                m->sha256 ? m->sha256 : "?");
      }
    }
  }

  /* NIP-40: Add expiration tag if set */
  if (composer && GNOSTR_IS_COMPOSER(composer)) {
    gint64 expiration = gnostr_composer_get_expiration(GNOSTR_COMPOSER(composer));
    if (expiration > 0) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "expiration");
      char expiration_str[32];
      g_snprintf(expiration_str, sizeof(expiration_str), "%" G_GINT64_FORMAT, expiration);
      gnostr_json_builder_add_string(builder, expiration_str);
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added expiration tag: %s", expiration_str);
    }
  }

  /* NIP-36: Add content-warning tag if note is marked as sensitive */
  if (composer && GNOSTR_IS_COMPOSER(composer)) {
    if (gnostr_composer_is_sensitive(GNOSTR_COMPOSER(composer))) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "content-warning");
      /* Empty reason - users can customize in future */
      gnostr_json_builder_add_string(builder, "");
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added content-warning tag (sensitive content)");
    }
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to build event JSON");
    return;
  }

  g_debug("[PUBLISH] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(text);
  ctx->composer = composer; /* weak ref for closing dialog on success */

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,      /* event_json */
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );

  g_free(event_json);
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
    g_timeout_add(500, profile_dispatch_next, g_object_ref(self));
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
        self->profile_fetch_source_id = g_timeout_add(delay, profile_fetch_fire_idle, g_object_ref(self));
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
    g_idle_add_full(G_PRIORITY_DEFAULT, profile_dispatch_next, g_object_ref(self), (GDestroyNotify)g_object_unref);
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

  g_free((gpointer)authors);
  /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
  return G_SOURCE_REMOVE;
}

static gboolean periodic_backfill_cb(gpointer data) { (void)data; return G_SOURCE_CONTINUE; }

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
    g_object_unref(self->pool_cancellable);
    self->pool_cancellable = NULL;

    /* LEGITIMATE TIMEOUT - Allow cancellation to complete before restart.
     * 100ms gives goroutines time to clean up connections gracefully.
     * nostrc-b0h: Audited - brief delay for async cleanup is appropriate. */
    g_timeout_add(100, (GSourceFunc)on_relay_config_changed_restart, self);
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
        /* retry_pool_live expects a ref; reuse the one we hold */
        g_timeout_add_seconds(5, retry_pool_live, self);
        self->reconnection_in_progress = FALSE;
        return;
    }
    g_clear_error(&err);

    g_debug("[RELAY] Relays connected, starting live subscription");

    GError *sub_error = NULL;
    GNostrSubscription *sub = gnostr_pool_subscribe(self->pool, filters, &sub_error);
    nostr_filters_free(filters);

    if (!sub) {
        g_warning("live: pool_subscribe failed: %s - retrying in 5 seconds",
                  sub_error ? sub_error->message : "(unknown)");
        g_clear_error(&sub_error);
        g_timeout_add_seconds(5, retry_pool_live, self);
        self->reconnection_in_progress = FALSE;
        return;
    }

    self->live_sub = sub;
    g_signal_connect(sub, "event", G_CALLBACK(on_pool_sub_event), self);
    g_signal_connect(sub, "eose", G_CALLBACK(on_pool_sub_eose), self);
    g_debug("[RELAY] Live subscription started successfully");
    self->reconnection_in_progress = FALSE;

    if (self->health_check_source_id == 0) {
        self->health_check_source_id = g_timeout_add_seconds(30, check_relay_health, self);
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
  if (self->pool_cancellable) {
    g_cancellable_cancel(self->pool_cancellable);
    g_object_unref(self->pool_cancellable);
    self->pool_cancellable = NULL;
  }
  self->pool_cancellable = g_cancellable_new();

  /* Build live URLs and filters: subscribe to all required kinds for persistence-first operation.
   * No limit on subscription since all events go into nostrdb - UI models handle their own windowing. */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;

  const int live_kinds[] = {0, 1, 5, 6, 7, 16, 1111};
  build_urls_and_filters_for_kinds(self,
                                  live_kinds,
                                  G_N_ELEMENTS(live_kinds),
                                  &urls,
                                  &url_count,
                                  &filters,
                                  0);  /* No limit - nostrdb handles storage */
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
  g_debug("[RELAY] Initializing %zu relays in pool", self->live_url_count);
  gnostr_pool_sync_relays(self->pool, (const gchar **)self->live_urls, self->live_url_count);
  g_debug("[RELAY] ✓ All relays initialized");
  /* Close previous subscription if any */
  if (self->live_sub) {
    gnostr_subscription_close(self->live_sub);
    g_clear_object(&self->live_sub);
  }

  /* nostrc-p2f6: Connect all relays BEFORE subscribing.
   * Previously we subscribed immediately after sync_relays(), but relays
   * start disconnected — pool_subscribe requires at least one connected relay. */
  g_debug("[RELAY] Connecting %zu relays...", self->live_url_count);
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
    GNostrRelay *relay = g_list_model_get_item(G_LIST_MODEL(relay_store), i);
    if (!relay) continue;
    /* Check if relay is present (added to pool = considered "connected" for health check) */
    if (gnostr_pool_get_relay(self->pool, gnostr_relay_get_url(relay)) != NULL) {
      connected_count++;
    } else {
      disconnected_count++;
    }
    g_object_unref(relay);
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
    gn_nostr_event_model_refresh(self->event_model);
  }
  
  return G_SOURCE_CONTINUE;
}

/* Retry live subscription after failure */
static gboolean retry_pool_live(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    return G_SOURCE_REMOVE;
  }
  start_pool_live(self);
  return G_SOURCE_REMOVE;
}

/* Live subscription event handler: ingest individual events into nostrdb. */
static void on_pool_sub_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data) {
  (void)sub;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !event_json) return;

  /* Parse event to get kind for filtering */
  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
    if (evt) nostr_event_free(evt);
    return;
  }

  int kind = nostr_event_get_kind(evt);
  /* Ingest timeline events and NIP-34 git events (30617 repos, 1617 patches, 1621 issues, 1622 replies) */
  if (!(kind == 0 || kind == 1 || kind == 5 || kind == 6 || kind == 7 || kind == 16 || kind == 1111 ||
        kind == 30617 || kind == 1617 || kind == 1621 || kind == 1622)) {
    nostr_event_free(evt);
    return;
  }

  const char *id = nostr_event_get_id(evt);
  if (!id || strlen(id) != 64) {
    nostr_event_free(evt);
    return;
  }

  /* Ingest to nostrdb for persistence; UI models subscribe to nostrdb and update from there. */
  int ingest_rc = storage_ndb_ingest_event_json(event_json, NULL);
  if (ingest_rc != 0) {
    g_debug("[INGEST] Failed to ingest event %.8s kind=%d: rc=%d json_len=%zu",
            id, kind, ingest_rc, strlen(event_json));
  }

  nostr_event_free(evt);
}

/* Live subscription EOSE handler */
static void on_pool_sub_eose(GNostrSubscription *sub, gpointer user_data) {
  (void)sub;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  g_debug("[RELAY] Live subscription received EOSE");
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
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return NULL;
  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);
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
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
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
    if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len) == 0 && json) {
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
  char *filter_json = g_strdup_printf(
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

  g_free(filter_json);
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
  if (thread_view && GNOSTR_IS_THREAD_VIEW(thread_view)) {
    if (is_panel_visible(self) && !gnostr_session_view_is_showing_profile(self->session_view)) {
      gnostr_thread_view_update_profiles(GNOSTR_THREAD_VIEW(thread_view));
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
