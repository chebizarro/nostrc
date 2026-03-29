#ifndef GNOSTR_MAIN_WINDOW_PRIVATE_H
#define GNOSTR_MAIN_WINDOW_PRIVATE_H

#include "gnostr-main-window.h"
#include <stddef.h>
#include <stdint.h>
#include <gio/gio.h>
#include <nostr-gtk-1.0/gnostr-composer.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

typedef struct NostrNip46Session NostrNip46Session;
typedef struct _GNostrPool GNostrPool;
typedef struct _GNostrPoolMultiSub GNostrPoolMultiSub;
typedef struct _GnostrDmService GnostrDmService;
typedef struct _GnNostrEventModel GnNostrEventModel;
typedef struct _NostrGtkProfilePane NostrGtkProfilePane;
typedef struct _NostrGtkThreadView NostrGtkThreadView;
typedef struct _GnostrArticleReader GnostrArticleReader;
typedef struct _GnostrPageDiscover GnostrPageDiscover;
typedef struct _GnostrSearchResultsView GnostrSearchResultsView;
typedef struct _GnostrNotificationsView GnostrNotificationsView;
typedef struct _GnostrClassifiedsView GnostrClassifiedsView;
typedef struct _GnostrRepoBrowser GnostrRepoBrowser;
typedef struct _GnostrSignerService GnostrSignerService;
typedef struct _NostrGtkTimelineView NostrGtkTimelineView;

typedef enum {
  COMPOSE_CONTEXT_NONE,
  COMPOSE_CONTEXT_REPLY,
  COMPOSE_CONTEXT_QUOTE,
  COMPOSE_CONTEXT_COMMENT
} ComposeContextType;

typedef struct {
  ComposeContextType type;
  char *reply_to_id;
  char *root_id;
  char *reply_to_pubkey;
  char *display_name;
  char *quote_id;
  char *quote_pubkey;
  char *nostr_uri;
  char *comment_root_id;
  int comment_root_kind;
  char *comment_root_pubkey;
} ComposeContext;

typedef struct {
  char *pubkey_hex;
  char *content_json;
} ProfileApplyCtx;

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
  guint64      startup_profile_throttle_until_us; /* monotonic deadline for startup throttling */
  guint        startup_profile_batch_size; /* small startup batch size for visible rows */
  guint        startup_profile_max_concurrent; /* startup concurrency cap */
  guint        startup_profile_inter_batch_delay_ms; /* pacing gap between startup batches */

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
  GNostrPoolMultiSub *live_multi_sub; /* owned; multi-relay live subscription */
  GCancellable    *pool_cancellable; /* owned */
  NostrFilters    *live_filters; /* owned; current live filter set */
  gulong           pool_events_handler; /* signal handler id */
  gboolean         reconnection_in_progress; /* prevent concurrent reconnection attempts */
  guint            health_check_source_id;   /* GLib source id for relay health check */
  const char     **live_urls;          /* owned array pointer + strings */
  size_t           live_url_count;     /* number of current live relays */
  
  /* Backpressure monitoring */
  guint64          ingest_events_received;   /* total events received from relays */
  guint64          ingest_events_dropped;    /* events dropped due to queue full */
  guint64          ingest_events_processed;  /* events successfully ingested to NDB */
  gint64           last_backpressure_warn_us; /* last backpressure warning timestamp */

  /* Sequential profile batch dispatch state */
  GNostrPool      *profile_pool;          /* owned; GObject pool for profile fetching */
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
  gboolean        startup_live_eose_seen;         /* at least one live relay reached EOSE */
  gboolean        startup_gift_wrap_started;      /* startup gift-wrap subscription already started */

  /* NIP-17 DM Service for decryption and conversation management */
  GnostrDmService *dm_service;           /* owned; handles gift wrap decryption */

  /* DM UI signal handler ids (0 when not connected) */
  gulong dm_inbox_open_handler_id;
  gulong dm_inbox_compose_handler_id;
  gulong dm_conv_back_handler_id;
  gulong dm_conv_send_handler_id;
  gulong dm_conv_send_file_handler_id;
  gulong dm_conv_open_profile_handler_id;
  gulong dm_service_message_handler_id;

  /* Window controllers */
  GtkEventController *key_controller;

  /* Live relay switching (nostrc-36y.4) */
  gulong           relay_change_handler_id; /* relay config change handler */

  /* Liked events cache (NIP-25 reactions) */
  GHashTable      *liked_events;  /* owned; key=event_id_hex (char*), value=unused (GINT_TO_POINTER(1)) */

  /* nostrc-61s.6: Background operation mode */
  gboolean         background_mode_enabled; /* hide on close instead of quit */

  /* nostrc-mzab: Background NDB ingestion to avoid blocking main thread.
   * Events are queued here and consumed by a dedicated ingestion thread,
   * so ndb_process_event_with() never blocks the GTK main loop. */
  GAsyncQueue     *ingest_queue;           /* owned; char* JSON strings */
  GThread         *ingest_thread;          /* owned; background ingestion worker */
  gboolean         ingest_running;         /* atomic; FALSE signals thread to exit */
};


void gnostr_main_window_show_toast_internal(GnostrMainWindow *self, const char *message);
void gnostr_main_window_handle_composer_post_requested(NostrGtkComposer *composer, const char *text, gpointer user_data);
void gnostr_main_window_on_relays_clicked_internal(GtkButton *btn, gpointer user_data);
void gnostr_main_window_on_settings_clicked_internal(GtkButton *btn, gpointer user_data);
void gnostr_main_window_on_compose_requested_internal(GnostrSessionView *session_view, gpointer user_data);
void gnostr_main_window_on_show_about_activated_internal(GSimpleAction *action, GVariant *param, gpointer user_data);
void gnostr_main_window_on_show_preferences_activated_internal(GSimpleAction *action, GVariant *param, gpointer user_data);
void gnostr_main_window_install_actions_internal(GnostrMainWindow *self);
void gnostr_main_window_open_compose_dialog_internal(GnostrMainWindow *self, ComposeContext *context);
void gnostr_main_window_compose_context_free_internal(ComposeContext *ctx);
void gnostr_main_window_compose_article(GtkWidget *window);
void gnostr_main_window_connect_dm_handlers_internal(GnostrMainWindow *self);
void gnostr_main_window_navigate_to_dm_conversation_internal(GnostrMainWindow *self, const char *peer_pubkey);
void gnostr_main_window_on_profile_pane_close_requested_internal(NostrGtkProfilePane *pane, gpointer user_data);
void gnostr_main_window_on_profile_pane_mute_user_requested_internal(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_profile_pane_follow_requested_internal(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_profile_pane_message_requested_internal(NostrGtkProfilePane *pane, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_thread_view_close_requested_internal(NostrGtkThreadView *view, gpointer user_data);
void gnostr_main_window_on_thread_view_need_profile_internal(NostrGtkThreadView *view, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_thread_view_open_profile_internal(NostrGtkThreadView *view, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_article_reader_close_requested_internal(GnostrArticleReader *reader, gpointer user_data);
void gnostr_main_window_on_article_reader_open_profile_internal(GnostrArticleReader *reader, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_article_reader_open_url_internal(GnostrArticleReader *reader, const char *url, gpointer user_data);
void gnostr_main_window_on_article_reader_share_internal(GnostrArticleReader *reader, const char *naddr_uri, gpointer user_data);
void gnostr_main_window_on_article_reader_zap_internal(GnostrArticleReader *reader, const char *event_id, const char *pubkey_hex, const char *lud16, gpointer user_data);
void gnostr_main_window_on_discover_open_profile_internal(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_discover_copy_npub_internal(GnostrPageDiscover *page, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_discover_open_communities_internal(GnostrPageDiscover *page, gpointer user_data);
void gnostr_main_window_on_discover_open_article_internal(GnostrPageDiscover *page, const char *event_id, gint kind, gpointer user_data);
void gnostr_main_window_on_discover_zap_article_internal(GnostrPageDiscover *page, const char *event_id, const char *pubkey_hex, const char *lud16, gpointer user_data);
void gnostr_main_window_on_discover_search_hashtag_internal(GnostrPageDiscover *page, const char *hashtag, gpointer user_data);
void gnostr_main_window_on_search_open_note_internal(GnostrSearchResultsView *view, const char *event_id_hex, gpointer user_data);
void gnostr_main_window_on_search_open_profile_internal(GnostrSearchResultsView *view, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_notification_open_note_internal(GnostrNotificationsView *view, const char *note_id, gpointer user_data);
void gnostr_main_window_on_notification_open_profile_internal(GnostrNotificationsView *view, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_classifieds_open_profile_internal(GnostrClassifiedsView *view, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_classifieds_contact_seller_internal(GnostrClassifiedsView *view, const char *pubkey_hex, const char *lud16, gpointer user_data);
void gnostr_main_window_on_classifieds_listing_clicked_internal(GnostrClassifiedsView *view, const char *event_id, const char *naddr, gpointer user_data);
void gnostr_main_window_on_repo_selected_internal(GnostrRepoBrowser *browser, const char *repo_id, gpointer user_data);
void gnostr_main_window_on_clone_requested_internal(GnostrRepoBrowser *browser, const char *clone_url, gpointer user_data);
void gnostr_main_window_on_repo_refresh_requested_internal(GnostrRepoBrowser *browser, gpointer user_data);
void gnostr_main_window_on_repo_browser_need_profile_internal(GnostrRepoBrowser *browser, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_on_repo_browser_open_profile_internal(GnostrRepoBrowser *browser, const char *pubkey_hex, gpointer user_data);
void gnostr_main_window_start_pool_live_internal(GnostrMainWindow *self);
void gnostr_main_window_start_profile_subscription_internal(GnostrMainWindow *self);
void gnostr_main_window_on_relay_config_changed_internal(gpointer user_data);
void gnostr_main_window_build_urls_and_filters_for_kinds_internal(GnostrMainWindow *self,
                                                                  const int *kinds,
                                                                  size_t n_kinds,
                                                                  const char ***out_urls,
                                                                  size_t *out_count,
                                                                  NostrFilters **out_filters,
                                                                  int limit);
void gnostr_main_window_free_urls_owned_internal(const char **urls, size_t count);
gboolean gnostr_main_window_ingest_queue_push_internal(GnostrMainWindow *self, gchar *json);
void gnostr_main_window_schedule_apply_profiles_internal(GnostrMainWindow *self, GPtrArray *items);
void gnostr_main_window_prepopulate_all_profiles_from_cache_internal(GnostrMainWindow *self);
void gnostr_main_window_enqueue_profile_author_internal(GnostrMainWindow *self, const char *pubkey_hex);
void gnostr_main_window_update_meta_from_profile_json_internal(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json);
void gnostr_main_window_refresh_thread_view_profiles_if_visible_internal(GnostrMainWindow *self);
void gnostr_main_window_get_property_internal(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
void gnostr_main_window_set_property_internal(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
void gnostr_main_window_dispose_internal(GObject *object);
void gnostr_main_window_bind_template_internal(GtkWidgetClass *widget_class);
void gnostr_main_window_update_login_ui_state_internal(GnostrMainWindow *self);
char *gnostr_main_window_get_current_user_pubkey_hex_internal(void);
void gnostr_main_window_start_gift_wrap_subscription_internal(GnostrMainWindow *self);
void gnostr_main_window_stop_gift_wrap_subscription_internal(GnostrMainWindow *self);
void gnostr_main_window_note_startup_live_eose_internal(GnostrMainWindow *self);
void gnostr_main_window_on_avatar_login_clicked_internal(GtkButton *btn, gpointer user_data);
void gnostr_main_window_on_signer_state_changed_internal(GnostrSignerService *signer,
                                                         guint old_state,
                                                         guint new_state,
                                                         gpointer user_data);
void gnostr_main_window_on_avatar_logout_clicked_internal(GtkButton *btn, gpointer user_data);
void gnostr_main_window_on_view_profile_requested_internal(GnostrSessionView *sv, gpointer user_data);
void gnostr_main_window_on_account_switch_requested_internal(GnostrSessionView *view,
                                                             const char *npub,
                                                             gpointer user_data);
void gnostr_main_window_init_widget_state_internal(GnostrMainWindow *self);
void gnostr_main_window_init_runtime_state_internal(GnostrMainWindow *self);
void gnostr_main_window_init_dm_internal(GnostrMainWindow *self);
void gnostr_main_window_on_event_model_need_profile_internal(GnNostrEventModel *model,
                                                         const char *pubkey_hex,
                                                         gpointer user_data);
void gnostr_main_window_on_timeline_scroll_value_changed_internal(GtkAdjustment *adj,
                                                                 gpointer user_data);
void gnostr_main_window_on_event_model_new_items_pending_internal(GnNostrEventModel *model,
                                                                  guint count,
                                                                  gpointer user_data);
void gnostr_main_window_on_timeline_tab_filter_changed_internal(NostrGtkTimelineView *view,
                                                                guint type,
                                                                const char *filter_value,
                                                                gpointer user_data);
void gnostr_main_window_on_new_notes_clicked_internal(GtkButton *btn, gpointer user_data);
void gnostr_main_window_connect_session_view_signals_internal(GnostrMainWindow *self,
                                                             GCallback settings_cb,
                                                             GCallback relays_cb,
                                                             GCallback reconnect_cb,
                                                             GCallback login_cb,
                                                             GCallback logout_cb,
                                                             GCallback view_profile_cb,
                                                             GCallback account_switch_cb,
                                                             GCallback new_notes_cb,
                                                             GCallback compose_cb);
void gnostr_main_window_connect_child_view_signals_internal(GnostrMainWindow *self,
                                                           GCallback profile_close_cb,
                                                           GCallback profile_mute_cb,
                                                           GCallback profile_follow_cb,
                                                           GCallback profile_message_cb,
                                                           GCallback thread_close_cb,
                                                           GCallback thread_need_profile_cb,
                                                           GCallback thread_open_profile_cb,
                                                           GCallback article_close_cb,
                                                           GCallback article_open_profile_cb,
                                                           GCallback article_open_url_cb,
                                                           GCallback article_share_cb,
                                                           GCallback article_zap_cb,
                                                           GCallback repo_selected_cb,
                                                           GCallback clone_requested_cb,
                                                           GCallback repo_refresh_cb,
                                                           GCallback repo_need_profile_cb,
                                                           GCallback repo_open_profile_cb);
void gnostr_main_window_connect_page_signals_internal(GnostrMainWindow *self,
                                                     GCallback discover_open_profile_cb,
                                                     GCallback discover_copy_npub_cb,
                                                     GCallback discover_open_communities_cb,
                                                     GCallback discover_open_article_cb,
                                                     GCallback discover_zap_article_cb,
                                                     GCallback discover_search_hashtag_cb,
                                                     GCallback search_open_note_cb,
                                                     GCallback search_open_profile_cb,
                                                     GCallback notification_open_note_cb,
                                                     GCallback notification_open_profile_cb,
                                                     GCallback classifieds_open_profile_cb,
                                                     GCallback classifieds_contact_seller_cb,
                                                     GCallback classifieds_listing_clicked_cb);
void gnostr_main_window_connect_window_signals_internal(GnostrMainWindow *self,
                                                       GCallback close_request_cb,
                                                       GCallback key_pressed_cb);
void gnostr_main_window_on_stack_visible_child_changed_internal(GObject *stack,
                                                                GParamSpec *pspec,
                                                                gpointer user_data);
void gnostr_main_window_run_startup_bootstrap_internal(GnostrMainWindow *self,
                                                      GCallback need_profile_cb,
                                                      GCallback new_items_pending_cb,
                                                      GCallback scroll_cb,
                                                      GCallback tab_filter_cb);
void gnostr_main_window_restore_session_services_internal(GnostrMainWindow *self);
void gnostr_main_window_initial_refresh_timeout_cb_internal(gpointer data);
void gnostr_main_window_run_startup_stage2_internal(gpointer data);
gpointer gnostr_main_window_ingest_thread_func_internal(gpointer data);

/* Panel management helpers (used by extracted signal handler modules) */
void gnostr_main_window_show_profile_panel_internal(GnostrMainWindow *self);
void gnostr_main_window_show_thread_panel_internal(GnostrMainWindow *self);
void gnostr_main_window_show_article_panel_internal(GnostrMainWindow *self);
void gnostr_main_window_hide_panel_internal(GnostrMainWindow *self);
gboolean gnostr_main_window_is_panel_visible_internal(GnostrMainWindow *self);

G_END_DECLS

#endif /* GNOSTR_MAIN_WINDOW_PRIVATE_H */
