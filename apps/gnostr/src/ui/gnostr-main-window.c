#define G_LOG_DOMAIN "gnostr-main-window"

#include "gnostr-main-window.h"
#include "gnostr-composer.h"
#include "gnostr-timeline-view.h"
#include "gnostr-profile-pane.h"
#include "gnostr-thread-view.h"
#include "gnostr-profile-provider.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-row.h"
#include "gnostr-dm-service.h"
#include "gnostr-notifications-view.h"
#include "gnostr-notification-row.h"
#include "page-discover.h"
#include "gnostr-search-results-view.h"
#include "note_card_row.h"
#include "../ipc/signer_ipc.h"
#include "../model/gn-nostr-event-model.h"
#include "../model/gn-nostr-event-item.h"
#include "../model/gn-nostr-profile.h"
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <time.h>
/* Metadata helpers */
#include <jansson.h>
/* SimplePool GObject wrapper for live streaming/backfill */
#include "nostr_simple_pool.h"
#include "nostr-event.h"
#include "nostr-filter.h"
/* Canonical JSON helpers (for nostr_event_from_json, etc.) */
#include "nostr-json.h"
/* NostrdB storage */
#include "../storage_ndb.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "libnostr_errors.h"
/* Nostr event kinds */
#include "nostr-kinds.h"
/* JSON interface */
#include "json.h"
/* Relays helpers */
#include "../util/relays.h"
/* NIP-11 relay information */
#include "../util/relay_info.h"
/* NIP-51 mute list */
#include "../util/mute_list.h"
/* NIP-51 settings sync */
#include "../util/nip51_settings.h"
/* Blossom server settings (kind 10063) */
#include "../util/blossom_settings.h"
#include "gnostr-login.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
/* NIP-19 helpers */
#include "nostr/nip19/nip19.h"
/* NIP-10 threading */
#include "nip10.h"
/* NIP-46 client (remote signer pairing) */
#include "nostr/nip46/nip46_client.h"
/* Nostr utilities */
#include "nostr-utils.h"

/* Implement as-if SimplePool is fully functional; guarded to avoid breaking builds until wired. */
#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "channel.h"
#include "error.h"
#include "context.h"
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

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
static void on_new_notes_clicked(GtkButton *btn, gpointer user_data);
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_refresh_clicked(GtkButton *btn, gpointer user_data);
static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data);
static void on_relays_clicked(GtkButton *btn, gpointer user_data);
static void on_settings_clicked(GtkButton *btn, gpointer user_data);
static void on_show_mute_list_activated(GSimpleAction *action, GVariant *param, gpointer user_data);
static void on_avatar_login_local_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_pair_remote_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_sign_out_clicked(GtkButton *btn, gpointer user_data);
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_close_requested(GnostrProfilePane *pane, gpointer user_data);
/* Forward declaration for ESC key handler to close profile sidebar */
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
/* Forward declaration for close-request handler (nostrc-61s.6: background mode) */
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
/* Forward declarations for repost/quote/like signal handlers */
static void on_note_card_repost_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_quote_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
static void on_note_card_like_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data);
/* Forward declarations for publish context (needed by repost function) */
typedef struct _PublishContext PublishContext;
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);
/* Forward declarations for like context and callback (NIP-25) */
typedef struct _LikeContext LikeContext;
static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);
/* Forward declarations for public repost/quote/like functions (defined after PublishContext) */
void gnostr_main_window_request_repost(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
void gnostr_main_window_request_quote(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, GnostrNoteCardRow *row);
static void user_meta_free(gpointer p);
static void show_toast(GnostrMainWindow *self, const char *msg);
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
static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_pool_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);
static void on_bg_prefetch_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);
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
  if (applied > 0) {
    g_debug("[PROFILE] Applied %u profiles to UI", applied);
  }
  idle_apply_profiles_ctx_free(c);
  return G_SOURCE_REMOVE;
}

static void schedule_apply_profiles(GnostrMainWindow *self, GPtrArray *items /* ProfileApplyCtx* */) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !items) { if (items) g_ptr_array_free(items, TRUE); return; }
  IdleApplyProfilesCtx *c = g_new0(IdleApplyProfilesCtx, 1);
  c->self = g_object_ref(self);
  c->items = items; /* transfer */
  g_debug("[PROFILE] Scheduling %u profiles for UI update", items->len);
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, apply_profiles_idle, c, NULL);
}

static gboolean profile_apply_on_main(gpointer data) {
  ProfileApplyCtx *c = (ProfileApplyCtx*)data;
  if (c && c->pubkey_hex && c->content_json) {
    g_debug("[PROFILE] Applying profile %.*s...", 8, c->pubkey_hex);
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next) {
      if (GNOSTR_IS_MAIN_WINDOW(l->data)) {
        update_meta_from_profile_json(GNOSTR_MAIN_WINDOW(l->data), c->pubkey_hex, c->content_json);
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
struct _GnostrMainWindow {
  GtkApplicationWindow parent_instance;
  // Template children
  GtkWidget *stack;
  GtkWidget *timeline;
  GWeakRef timeline_ref; /* weak ref to avoid UAF in async */
  GtkWidget *timeline_overlay;
  GtkWidget *profile_revealer;
  GtkWidget *profile_pane;
  GtkWidget *thread_revealer;
  GtkWidget *thread_view;
  GtkWidget *btn_settings;
  GtkWidget *btn_relays;
  GtkWidget *btn_menu;
  GtkWidget *btn_avatar;
  GtkWidget *avatar_popover;
  GtkWidget *lbl_signin_status;
  GtkWidget *lbl_profile_name;
  GtkWidget *btn_login_local;
  GtkWidget *btn_pair_remote;
  GtkWidget *btn_sign_out;
  GtkWidget *composer;
  GtkWidget *dm_inbox;
  GtkWidget *notifications_view;
  GtkWidget *btn_refresh;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
  /* nostrc-yi2: Calm timeline - new notes indicator */
  GtkWidget *new_notes_revealer;
  GtkWidget *btn_new_notes;
  GtkWidget *lbl_new_notes_count;
  /* Session state */
  GHashTable *seen_texts; /* owned; keys are g_strdup(text), values unused */
  /* GListModel-based timeline (primary data source) */
  GnNostrEventModel *event_model; /* owned; reactive model over nostrdb */
  guint model_refresh_pending;    /* debounced refresh source id, 0 if none */
  /* In-memory avatar texture cache: key=url (string), value=GdkTexture* */
  GHashTable *avatar_tex_cache;
  
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
  /* SimplePool live stream */
  GnostrSimplePool *pool;       /* owned */
  GCancellable    *pool_cancellable; /* owned */
  NostrFilters    *live_filters; /* owned; current live filter set */
  gulong           pool_events_handler; /* signal handler id */
  gboolean         reconnection_in_progress; /* prevent concurrent reconnection attempts */
  guint            health_check_source_id;   /* GLib source id for relay health check */
  const char     **live_urls;          /* owned array pointer + strings */
  size_t           live_url_count;     /* number of current live relays */
 
  /* Sequential profile batch dispatch state */
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

/* Old LRU functions removed - now using profile provider */

/* ---- Memory stats logging and cache pruning ---- */

/* Cache size limits to prevent unbounded memory growth */
#define AVATAR_CACHE_MAX 1000
#define SEEN_TEXTS_MAX 10000
#define LIKED_EVENTS_MAX 5000

static gboolean memory_stats_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_CONTINUE;
  
  guint seen_texts_size = self->seen_texts ? g_hash_table_size(self->seen_texts) : 0;
  guint profile_queue = self->profile_fetch_queue ? self->profile_fetch_queue->len : 0;
  guint avatar_size = self->avatar_tex_cache ? g_hash_table_size(self->avatar_tex_cache) : 0;
  guint model_items = self->event_model ? g_list_model_get_n_items(G_LIST_MODEL(self->event_model)) : 0;
  guint liked_events_size = self->liked_events ? g_hash_table_size(self->liked_events) : 0;

  g_debug("[MEMORY] model=%u seen_texts=%u avatars=%u profile_q=%u liked=%u",
          model_items, seen_texts_size, avatar_size, profile_queue, liked_events_size);
  
  /* Prune caches if they exceed limits to prevent unbounded memory growth */
  gboolean pruned = FALSE;
  
  /* Prune avatar_tex_cache - clear entirely if too large */
  if (avatar_size > AVATAR_CACHE_MAX) {
    g_debug("[MEMORY] Pruning avatar_tex_cache: %u -> 0", avatar_size);
    g_hash_table_remove_all(self->avatar_tex_cache);
    pruned = TRUE;
  }
  
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
  g_debug("[PROFILE] Queued author %.*s... (queue size: %u)", 8, pubkey_hex, self->profile_fetch_queue->len);
schedule_only:
  /* Debounce triggering */
  if (self->profile_fetch_source_id) {
    /* already scheduled; let it fire */
  } else {
    guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
    /* schedule timeout -> idle trampoline to preserve callback types */
    GnostrMainWindow *ref = g_object_ref(self);
    self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, delay, (GSourceFunc)profile_fetch_fire_idle, ref, (GDestroyNotify)g_object_unref);
  }
}

/* ---- Toast helpers and UI signal handlers ---- */
static gboolean hide_toast_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;
  if (self->toast_revealer && GTK_IS_REVEALER(self->toast_revealer))
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), FALSE);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void show_toast(GnostrMainWindow *self, const char *msg) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (self->toast_label && GTK_IS_LABEL(self->toast_label))
    gtk_label_set_text(GTK_LABEL(self->toast_label), msg ? msg : "");
  if (self->toast_revealer && GTK_IS_REVEALER(self->toast_revealer))
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* auto-hide after 2s */
  g_timeout_add_once(2000, (GSourceOnceFunc)hide_toast_cb, g_object_ref(self));
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
};

static void relay_manager_ctx_free(RelayManagerCtx *ctx) {
  if (!ctx) return;
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
    g_object_unref(ctx->fetch_cancellable);
  }
  if (ctx->relay_types) {
    g_hash_table_destroy(ctx->relay_types);
  }
  g_free(ctx->selected_url);
  g_free(ctx);
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
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(container)) != NULL) {
    gtk_widget_unparent(child);
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
  if (nips_flowbox) {
    relay_manager_clear_container(nips_flowbox);

    if (info->supported_nips && info->supported_nips_count > 0) {
      for (gsize i = 0; i < info->supported_nips_count; i++) {
        GtkWidget *badge = relay_manager_create_nip_badge(info->supported_nips[i]);
        gtk_flow_box_append(GTK_FLOW_BOX(nips_flowbox), badge);
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

  gtk_stack_set_visible_child_name(stack, "info");
}

static void on_relay_info_fetched(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->builder) return;

  GError *err = NULL;
  GnostrRelayInfo *info = gnostr_relay_info_fetch_finish(result, &err);

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (!stack) {
    if (info) gnostr_relay_info_free(info);
    g_clear_error(&err);
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
    return;
  }

  if (!info) {
    GtkLabel *error_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_error_label"));
    if (error_label) gtk_label_set_text(error_label, "Failed to parse relay info");
    gtk_stack_set_visible_child_name(stack, "error");
    return;
  }

  relay_manager_populate_info(ctx, info);
  gnostr_relay_info_free(info);
}

static void relay_manager_fetch_info(RelayManagerCtx *ctx, const gchar *url) {
  if (!ctx || !url) return;

  /* Cancel any pending fetch */
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
    g_object_unref(ctx->fetch_cancellable);
  }
  ctx->fetch_cancellable = g_cancellable_new();

  g_free(ctx->selected_url);
  ctx->selected_url = g_strdup(url);

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "loading");

  gnostr_relay_info_fetch_async(url, ctx->fetch_cancellable, on_relay_info_fetched, ctx);
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
  if (ctx && ctx->builder) {
    g_object_unref(ctx->builder);
    ctx->builder = NULL;
  }
  relay_manager_ctx_free(ctx);
}

/* Structure to hold row widget references */
typedef struct {
  GtkWidget *name_label;
  GtkWidget *url_label;
  GtkWidget *status_icon;
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

  /* Status indicator (colored dot) */
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
  ctx->window = win;
  ctx->builder = builder;
  ctx->modified = FALSE;
  ctx->relay_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

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
  GtkWidget *relay_entry = GTK_WIDGET(gtk_builder_get_object(builder, "relay_entry"));

  if (btn_add) g_signal_connect(btn_add, "clicked", G_CALLBACK(relay_manager_on_add_clicked), ctx);
  if (btn_remove) g_signal_connect(btn_remove, "clicked", G_CALLBACK(relay_manager_on_remove_clicked), ctx);
  if (btn_save) g_signal_connect(btn_save, "clicked", G_CALLBACK(relay_manager_on_save_clicked), ctx);
  if (btn_cancel) g_signal_connect(btn_cancel, "clicked", G_CALLBACK(relay_manager_on_cancel_clicked), ctx);
  if (btn_retry) g_signal_connect(btn_retry, "clicked", G_CALLBACK(relay_manager_on_retry_clicked), ctx);
  if (relay_entry) g_signal_connect(relay_entry, "activate", G_CALLBACK(relay_manager_on_entry_activate), ctx);

  /* Update status and cleanup on destroy */
  relay_manager_update_status(ctx);
  g_signal_connect(win, "destroy", G_CALLBACK(relay_manager_on_destroy), ctx);

  gtk_window_present(win);
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

  gboolean is_logged_in = (ctx->main_window->user_pubkey_hex != NULL &&
                           ctx->main_window->user_pubkey_hex[0] != '\0');

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
    gtk_widget_set_margin_end(box, 12);
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

  /* Check if user is logged in and update mute list visibility */
  gboolean is_logged_in = (self->user_pubkey_hex != NULL && self->user_pubkey_hex[0] != '\0');
  GtkWidget *mute_login_required = GTK_WIDGET(gtk_builder_get_object(builder, "mute_login_required"));
  GtkWidget *mute_content = GTK_WIDGET(gtk_builder_get_object(builder, "mute_content"));
  if (mute_login_required) gtk_widget_set_visible(mute_login_required, !is_logged_in);
  if (mute_content) gtk_widget_set_visible(mute_content, is_logged_in);

  /* Load current settings values (General panel) */
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

  /* Context is freed when window is destroyed */
  g_signal_connect(win, "destroy", G_CALLBACK(on_settings_dialog_destroy), ctx);
  gtk_window_present(win);
}

/* Forward declaration for updating login UI state */
static void update_login_ui_state(GnostrMainWindow *self);

/* Signal handler for when user successfully signs in via login dialog */
static void on_login_signed_in(GnostrLogin *login, const char *npub, gpointer user_data) {
  (void)login;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[AUTH] User signed in: %s", npub ? npub : "(null)");

  /* Update user_pubkey_hex from npub */
  if (npub && g_str_has_prefix(npub, "npub1")) {
    uint8_t pubkey_bytes[32];
    if (nostr_nip19_decode_npub(npub, pubkey_bytes) == 0) {
      g_free(self->user_pubkey_hex);
      self->user_pubkey_hex = hex_encode_lower(pubkey_bytes, 32);
    }
  }

  /* Update UI to show signed-in state */
  update_login_ui_state(self);

  /* Start gift wrap subscription for encrypted DMs */
  start_gift_wrap_subscription(self);

  /* Load NIP-65 relay list for the user */
  if (self->user_pubkey_hex) {
    g_debug("[AUTH] Loading NIP-65 relay list for user %.*s...", 8, self->user_pubkey_hex);
    gnostr_nip65_load_on_login_async(self->user_pubkey_hex, NULL, NULL);

    /* Load Blossom server list (kind 10063) */
    g_debug("[AUTH] Loading Blossom server list (kind 10063) for user %.*s...", 8, self->user_pubkey_hex);
    gnostr_blossom_settings_load_from_relays_async(self->user_pubkey_hex, NULL, NULL);

    /* Auto-sync NIP-51 settings if enabled */
    gnostr_nip51_settings_auto_sync_on_login(self->user_pubkey_hex);
  }

  /* Close the avatar popover */
  if (self->avatar_popover && GTK_IS_POPOVER(self->avatar_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->avatar_popover));
  }

  show_toast(self, "Signed in successfully");
}

/* Opens the login dialog */
static void open_login_dialog(GnostrMainWindow *self) {
  GnostrLogin *login = gnostr_login_new(GTK_WINDOW(self));
  g_signal_connect(login, "signed-in", G_CALLBACK(on_login_signed_in), self);
  gtk_window_present(GTK_WINDOW(login));
}

static void on_avatar_login_local_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close popover and open login dialog */
  if (self->avatar_popover && GTK_IS_POPOVER(self->avatar_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->avatar_popover));
  }
  open_login_dialog(self);
}

static void on_avatar_pair_remote_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close popover and open login dialog (it has both options) */
  if (self->avatar_popover && GTK_IS_POPOVER(self->avatar_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->avatar_popover));
  }
  open_login_dialog(self);
}

static void on_avatar_sign_out_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Stop gift wrap subscription when user signs out */
  stop_gift_wrap_subscription(self);

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

  /* Update UI */
  update_login_ui_state(self);

  /* Clear gift wrap queue */
  if (self->gift_wrap_queue) {
    g_ptr_array_set_size(self->gift_wrap_queue, 0);
  }

  /* Close popover */
  if (self->avatar_popover && GTK_IS_POPOVER(self->avatar_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->avatar_popover));
  }

  show_toast(self, "Signed out");
}

/* Update the avatar popover UI based on sign-in state */
static void update_login_ui_state(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return;

  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);

  gboolean signed_in = npub && *npub;

  if (self->lbl_signin_status && GTK_IS_LABEL(self->lbl_signin_status)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_signin_status),
                       signed_in ? "Signed in" : "Not signed in");
  }

  if (self->lbl_profile_name && GTK_IS_LABEL(self->lbl_profile_name)) {
    if (signed_in && npub) {
      /* Show truncated npub */
      char *display = g_strdup_printf("%.16s...", npub);
      gtk_label_set_text(GTK_LABEL(self->lbl_profile_name), display);
      g_free(display);
    } else {
      gtk_label_set_text(GTK_LABEL(self->lbl_profile_name), "");
    }
  }

  /* Show/hide buttons based on state */
  if (self->btn_login_local && GTK_IS_WIDGET(self->btn_login_local)) {
    gtk_widget_set_visible(self->btn_login_local, !signed_in);
  }
  if (self->btn_pair_remote && GTK_IS_WIDGET(self->btn_pair_remote)) {
    gtk_widget_set_visible(self->btn_pair_remote, !signed_in);
  }
  if (self->btn_sign_out && GTK_IS_WIDGET(self->btn_sign_out)) {
    gtk_widget_set_visible(self->btn_sign_out, signed_in);
  }

  g_free(npub);
}

/* Profile pane signal handlers */
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;

  g_debug("[UI] Profile click for %.*s...", 8, pubkey_hex);

  /* Check if profile pane is currently visible */
  gboolean sidebar_visible = gtk_revealer_get_reveal_child(GTK_REVEALER(self->profile_revealer));

  /* Check if profile pane is already showing this profile */
  extern const char* gnostr_profile_pane_get_current_pubkey(GnostrProfilePane *pane);
  if (GNOSTR_IS_PROFILE_PANE(self->profile_pane)) {
    const char *current = gnostr_profile_pane_get_current_pubkey(GNOSTR_PROFILE_PANE(self->profile_pane));
    if (sidebar_visible && current && strcmp(current, pubkey_hex) == 0) {
      /* Same profile clicked while sidebar is visible - toggle OFF (close sidebar) */
      g_debug("[UI] Toggle: closing profile pane (same profile clicked)");
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), FALSE);
      return;
    }
  }

  /* Different profile or sidebar was closed - show the profile pane */
  g_debug("[UI] Toggle: showing profile pane for %.*s...", 8, pubkey_hex);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), TRUE);
  
  /* Set the pubkey on the profile pane */
  if (GNOSTR_IS_PROFILE_PANE(self->profile_pane)) {
    gnostr_profile_pane_set_pubkey(GNOSTR_PROFILE_PANE(self->profile_pane), pubkey_hex);
    
    /* Query nostrdb directly for profile using optimized lookup */
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0) {
      uint8_t pk32[32];
      gboolean found = FALSE;

      if (hex_to_bytes32(pubkey_hex, pk32)) {
        char *event_json = NULL;
        int event_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &event_json, &event_len) == 0 && event_json) {
          /* storage_ndb_get_profile_by_pubkey returns full event JSON, extract content field */
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, event_json) == 0) {
            const char *content = nostr_event_get_content(evt);
            if (content && *content) {
              extern void gnostr_profile_pane_update_from_json(GnostrProfilePane *pane, const char *json);
              gnostr_profile_pane_update_from_json(GNOSTR_PROFILE_PANE(self->profile_pane), content);
              g_debug("[PROFILE] Loaded profile for %.8s from nostrdb", pubkey_hex);
              found = TRUE;
            }
          }
          if (evt) nostr_event_free(evt);
          free(event_json);
        }
      }

      if (!found) {
        /* Profile not in DB, enqueue for fetching from relays */
        g_debug("[PROFILE] Profile %.8s not in nostrdb, enqueueing for fetch", pubkey_hex);
        enqueue_profile_author(self, pubkey_hex);
      }
      storage_ndb_end_query(txn);
    }
  }
}

static void on_profile_pane_close_requested(GnostrProfilePane *pane, gpointer user_data) {
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[UI] Closing profile pane");
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), FALSE);
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

/* ESC key handler to close profile sidebar */
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Escape) {
    /* Close thread view if it's open (takes priority) */
    if (self->thread_revealer && GTK_IS_REVEALER(self->thread_revealer) &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(self->thread_revealer))) {
      g_debug("[UI] ESC pressed: closing thread view");
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->thread_revealer), FALSE);
      if (self->thread_view && GNOSTR_IS_THREAD_VIEW(self->thread_view)) {
        gnostr_thread_view_clear(GNOSTR_THREAD_VIEW(self->thread_view));
      }
      return GDK_EVENT_STOP;
    }
    /* Close profile sidebar if it's open */
    if (self->profile_revealer && GTK_IS_REVEALER(self->profile_revealer) &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(self->profile_revealer))) {
      g_debug("[UI] ESC pressed: closing profile sidebar");
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), FALSE);
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

/* Public wrapper for opening profile pane (called from timeline view) */
void gnostr_main_window_open_profile(GtkWidget *window, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  on_note_card_open_profile(NULL, pubkey_hex, self);
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
          json_error_t err;
          json_t *meta = json_loads(meta_json, 0, &err);
          if (meta) {
            json_t *name_obj = json_object_get(meta, "display_name");
            if (!name_obj || !json_is_string(name_obj)) {
              name_obj = json_object_get(meta, "name");
            }
            if (name_obj && json_is_string(name_obj)) {
              const char *n = json_string_value(name_obj);
              if (n && *n) display_name = g_strdup(n);
            }
            json_decref(meta);
          }
          /* Note: meta_json is owned by store, do not free */
        }
      }
      storage_ndb_end_query(txn);
    }
  }

  /* Set the reply context on the composer */
  if (self->composer && GNOSTR_IS_COMPOSER(self->composer)) {
    gnostr_composer_set_reply_context(GNOSTR_COMPOSER(self->composer),
                                      id_hex, root_id, pubkey_hex,
                                      display_name ? display_name : "@user");
  }
  g_free(display_name);

  /* Switch to composer tab */
  if (self->stack && GTK_IS_STACK(self->stack)) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "compose");
  }
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

  /* Convert event ID hex to note1 bech32 URI */
  uint8_t id32[32];
  if (!hex_to_bytes32(id_hex, id32)) {
    show_toast(self, "Invalid event ID format");
    return;
  }

  char *note_bech32 = NULL;
  int encode_rc = nostr_nip19_encode_note(id32, &note_bech32);
  if (encode_rc != 0 || !note_bech32) {
    show_toast(self, "Failed to encode note ID");
    return;
  }

  /* Build nostr: URI */
  char *nostr_uri = g_strdup_printf("nostr:%s", note_bech32);
  free(note_bech32);

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
          json_error_t err;
          json_t *meta = json_loads(meta_json, 0, &err);
          if (meta) {
            json_t *name_obj = json_object_get(meta, "display_name");
            if (!name_obj || !json_is_string(name_obj)) {
              name_obj = json_object_get(meta, "name");
            }
            if (name_obj && json_is_string(name_obj)) {
              const char *n = json_string_value(name_obj);
              if (n && *n) display_name = g_strdup(n);
            }
            json_decref(meta);
          }
        }
      }
      storage_ndb_end_query(txn);
    }
  }

  /* Set the quote context on the composer */
  if (self->composer && GNOSTR_IS_COMPOSER(self->composer)) {
    gnostr_composer_set_quote_context(GNOSTR_COMPOSER(self->composer),
                                      id_hex, pubkey_hex, nostr_uri,
                                      display_name ? display_name : "@user");
  }
  g_free(display_name);
  g_free(nostr_uri);

  /* Switch to composer tab */
  if (self->stack && GTK_IS_STACK(self->stack)) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "compose");
  }
}

/* Signal handler for repost-requested from note card */
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
static void on_note_card_like_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gnostr_main_window_request_like(GTK_WIDGET(self), id_hex, pubkey_hex, row);
}

/* Forward declaration for thread view close handler */
static void on_thread_view_close_requested(GnostrThreadView *view, gpointer user_data);
static void on_thread_view_open_profile(GnostrThreadView *view, const char *pubkey_hex, gpointer user_data);

/* Public wrapper for viewing a thread (called from timeline view) */
void gnostr_main_window_view_thread(GtkWidget *window, const char *root_event_id) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  if (!root_event_id || strlen(root_event_id) != 64) {
    g_warning("[THREAD] Invalid root event ID for thread view");
    return;
  }

  g_debug("[THREAD] View thread requested for root=%s", root_event_id);

  /* Show thread view panel */
  if (!self->thread_view || !GNOSTR_IS_THREAD_VIEW(self->thread_view)) {
    g_warning("[THREAD] Thread view widget not available");
    show_toast(self, "Thread view not available");
    return;
  }

  /* Set the thread root and load the thread */
  gnostr_thread_view_set_thread_root(GNOSTR_THREAD_VIEW(self->thread_view), root_event_id);

  /* Reveal the thread panel */
  if (self->thread_revealer && GTK_IS_REVEALER(self->thread_revealer)) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->thread_revealer), TRUE);
  }

  /* Hide profile pane if visible to avoid overlap */
  if (self->profile_revealer && GTK_IS_REVEALER(self->profile_revealer)) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), FALSE);
  }
}

/* Handler for thread view close button */
static void on_thread_view_close_requested(GnostrThreadView *view, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Hide thread panel */
  if (self->thread_revealer && GTK_IS_REVEALER(self->thread_revealer)) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->thread_revealer), FALSE);
  }

  /* Clear thread view to free resources */
  if (self->thread_view && GNOSTR_IS_THREAD_VIEW(self->thread_view)) {
    gnostr_thread_view_clear(GNOSTR_THREAD_VIEW(self->thread_view));
  }
}

/* Handler for open profile from thread view */
static void on_thread_view_open_profile(GnostrThreadView *view, const char *pubkey_hex, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  (void)view;

  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Close thread view first */
  on_thread_view_close_requested(GNOSTR_THREAD_VIEW(self->thread_view), self);

  /* Open profile pane */
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
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
  
  /* CRITICAL FIX: Don't fetch profiles if pool isn't initialized with relays
   * Without GNOSTR_LIVE=TRUE, the pool and relays aren't set up, and profile
   * fetching will fail anyway. Skip it to avoid hanging on relay config loading. */
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
    /* Queue empty - silent */
    return G_SOURCE_REMOVE;
  }
  g_debug("[PROFILE] Fetching profiles for %u authors", self->profile_fetch_queue->len);
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
        json_error_t jerr;
        json_t *root = json_loadb(pjson, strnlen(pjson, plen), 0, &jerr);
        if (root) {
          json_t *content_json = json_object_get(root, "content");
          if (content_json && json_is_string(content_json)) {
            update_meta_from_profile_json(self, pkhex, json_string_value(content_json));
            cached_applied++;
          }
          json_decref(root);
        }
        free(pjson);
      }
    }
    storage_ndb_end_query(txn);
  }
  
  if (cached_applied > 0) {
    g_debug("[PROFILE] ✓ %u cached profiles loaded from DB", cached_applied);
  }
  
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
  
  /* NOTE: Relays should already be in the pool from start_pool_live().
   * If not, the async fetch will skip unavailable relays. */
  /* Build batch list but dispatch sequentially (EOSE-gated) */
  const guint total = authors->len;
  const guint batch_sz = 100; /* Increased from 16 to reduce inter-batch issues */
  const guint n_batches = (total + batch_sz - 1) / batch_sz;
  g_debug("[PROFILE] Fetching %u authors from %zu relays (%u batches)", total, url_count, n_batches);
  
  /* Check for stale batch state (shouldn't happen, but be defensive) */
  if (self->profile_batches) {
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
    
    g_debug("[PROFILE] Stale state cleared, proceeding with new fetch");
    /* Fall through to create new batch sequence */
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
  GnostrSimplePool *pool = GNOSTR_SIMPLE_POOL(source); (void)pool;
  ProfileBatchCtx *ctx = (ProfileBatchCtx*)user_data;
  
  if (!ctx) {
    g_critical("profile_fetch: callback ctx is NULL!");
    return;
  }
  
  GError *error = NULL;
  GPtrArray *jsons = gnostr_simple_pool_fetch_profiles_by_authors_finish(GNOSTR_SIMPLE_POOL(source), res, &error);
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
        NostrEvent *evt = nostr_event_new();
        if (evt && nostr_event_deserialize(evt, evt_json) == 0) {
          const char *pk = nostr_event_get_pubkey(evt);
          if (pk) g_hash_table_add(unique_pks, g_strdup(pk));
        }
        if (evt) nostr_event_free(evt);
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
      NostrEvent *evt = nostr_event_new();
      if (evt && nostr_event_deserialize(evt, evt_json) == 0) {
        const char *pk_hex = nostr_event_get_pubkey(evt);
        const char *content = nostr_event_get_content(evt);
        if (pk_hex && content) {
          /* Collect for bulk dispatch */
          ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
          pctx->pubkey_hex = g_strdup(pk_hex);
          pctx->content_json = g_strdup(content);
          g_ptr_array_add(items, pctx);
          dispatched++;
        }
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
      if (evt) {
        deserialized++;
        nostr_event_free(evt);
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
      NostrEvent *evt = nostr_event_new();
      if (evt && nostr_event_deserialize(evt, evt_json) == 0) {
        if (nostr_event_get_kind(evt) == 0) {
          const char *pk_hex = nostr_event_get_pubkey(evt);
          const char *content = nostr_event_get_content(evt);
          if (pk_hex && content) {
            ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
            pctx->pubkey_hex = g_strdup(pk_hex);
            pctx->content_json = g_strdup(content);
            g_ptr_array_add(items, pctx);
          }
        }
      }
      if (evt) nostr_event_free(evt);
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
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_refresh),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Refresh Timeline", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_relays),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Manage Relays", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_settings),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Settings", -1);
  /* Report HTTP avatar support availability */
#ifdef HAVE_SOUP3
  g_debug("http: libsoup3 enabled; avatar HTTP fetch active");
#else
  g_debug("http: libsoup3 NOT enabled; avatar HTTP fetch disabled");
#endif
  /* Sanity logging and guard for avatar popover attachment */
  GtkPopover *init_pop = NULL;
  if (self->btn_avatar) init_pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(self->btn_avatar));
  g_debug("[INIT] Avatar button setup");
  if (self->btn_avatar && self->avatar_popover) {
    /* Unconditionally associate the popover to avoid ambiguity */
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->btn_avatar), GTK_WIDGET(self->avatar_popover));
  }
  g_return_if_fail(self->composer != NULL);
  /* Initialize weak refs to template children needed in async paths */
  g_weak_ref_init(&self->timeline_ref, self->timeline);
  /* NEW: Initialize GListModel-based event model */
  self->event_model = gn_nostr_event_model_new();
  g_debug("[INIT] Created GnNostrEventModel");
  
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
  
  /* Attach model to timeline view */
  if (self->timeline && G_TYPE_CHECK_INSTANCE_TYPE(self->timeline, GNOSTR_TYPE_TIMELINE_VIEW)) {
    g_debug("[INIT] Attaching GnNostrEventModel to timeline view");
    /* Wrap GListModel in a selection model */
    GtkSelectionModel *selection = GTK_SELECTION_MODEL(
      gtk_single_selection_new(G_LIST_MODEL(self->event_model))
    );
    gnostr_timeline_view_set_model(GNOSTR_TIMELINE_VIEW(self->timeline), selection);
    g_object_unref(selection); /* View takes ownership */

    /* Connect scroll edge detection for sliding window pagination */
    GtkWidget *scroller = gnostr_timeline_view_get_scrolled_window(GNOSTR_TIMELINE_VIEW(self->timeline));
    if (scroller && GTK_IS_SCROLLED_WINDOW(scroller)) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
      if (vadj) {
        g_signal_connect(vadj, "value-changed", G_CALLBACK(on_timeline_scroll_value_changed), self);
        g_debug("[INIT] Connected scroll edge detection for sliding window");
      }
    }

    /* Do NOT call refresh here; we refresh once in initial_refresh_timeout_cb to avoid duplicate rebuilds. */
  }
  
  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize profile provider */
  gnostr_profile_provider_init(0); /* Use env/default cap */
  /* Profile provider stats logging */
  g_timeout_add_seconds(60, (GSourceFunc)gnostr_profile_provider_log_stats, NULL);
  /* Memory stats logging */
  g_timeout_add_seconds(60, memory_stats_cb, self);
  /* Initialize avatar texture cache */
  self->avatar_tex_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
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
  g_debug("[LIVE_RELAY] Registered relay change handler (id=%lu)", self->relay_change_handler_id);
  /* Build app menu for header button */
  if (self->btn_menu) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Quit", "app.quit");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_menu), G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_debug("connecting post-requested handler on composer=%p", (void*)self->composer);
  g_signal_connect(self->composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);
  if (self->btn_refresh) {
    g_signal_connect(self->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), self);
  }
  /* nostrc-yi2: Calm timeline - connect new notes button click */
  if (self->btn_new_notes) {
    g_signal_connect(self->btn_new_notes, "clicked", G_CALLBACK(on_new_notes_clicked), self);
  }
  /* Connect profile pane signals */
  if (self->profile_pane && GNOSTR_IS_PROFILE_PANE(self->profile_pane)) {
    g_signal_connect(self->profile_pane, "close-requested",
                     G_CALLBACK(on_profile_pane_close_requested), self);
    g_debug("connected profile pane close-requested signal");
  }
  /* Connect thread view signals */
  if (self->thread_view && GNOSTR_IS_THREAD_VIEW(self->thread_view)) {
    g_signal_connect(self->thread_view, "close-requested",
                     G_CALLBACK(on_thread_view_close_requested), self);
    g_signal_connect(self->thread_view, "open-profile",
                     G_CALLBACK(on_thread_view_open_profile), self);
    g_debug("connected thread view signals");
  }
  /* Add key event controller for ESC to close profile sidebar */
  {
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);
    g_debug("connected ESC key handler for profile sidebar");
  }
  if (self->btn_avatar) {
    /* Ensure avatar button is interactable */
    gtk_widget_set_sensitive(self->btn_avatar, TRUE);
    gtk_widget_set_tooltip_text(self->btn_avatar, "Login / Account");
  }
  /* Initialize login UI state from saved settings */
  update_login_ui_state(self);
  /* Ensure Timeline page is visible initially */
  if (self->stack && self->timeline && GTK_IS_STACK(self->stack)) {
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->timeline);
  }
  
  /* CRITICAL: Initialize pool and relays BEFORE timeline prepopulation!
   * Timeline prepopulation triggers profile fetches, which need relays in the pool.
   * If we prepopulate first, profile fetches will skip all relays (not in pool yet). */
  {
    const char *live = g_getenv("GNOSTR_LIVE");
    if (live && *live && g_strcmp0(live, "0") != 0) {
      g_debug("[INIT] Starting live subscriptions (GNOSTR_LIVE=TRUE)");
      start_pool_live(self);
      /* Also start profile subscription if identity is configured */
      start_profile_subscription(self);

      /* NOTE: Periodic refresh disabled - nostrdb ingestion drives UI updates via GnNostrEventModel.
       * This avoids duplicate processing and high memory usage. Initial refresh occurs in
       * initial_refresh_timeout_cb, and subsequent updates stream from nostrdb watchers. */
    }
  }

  /* Start gift wrap (NIP-59) subscription if user is signed in.
   * This is a nostrdb subscription (not relay), so it works regardless of GNOSTR_LIVE.
   * Gift wraps are encrypted messages addressed to the current user. */
  start_gift_wrap_subscription(self);
  
  /* Seed initial items so Timeline page isn't empty */
  g_timeout_add_once(150, (GSourceOnceFunc)initial_refresh_timeout_cb, self);

  /* Init demand-driven profile fetch state */
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  self->profile_fetch_source_id = 0;
  self->profile_fetch_debounce_ms = 150;
  self->profile_fetch_cancellable = g_cancellable_new();
  self->profile_fetch_active = 0;
  self->profile_fetch_max_concurrent = 3; /* Limit concurrent fetches to reduce goroutine count */

  /* Init debounced NostrDB profile sweep */
  self->ndb_sweep_source_id = 0;
  self->ndb_sweep_debounce_ms = 1000; /* 1 second - prevents transaction contention */
  if (!self->pool) self->pool = gnostr_simple_pool_new();

  /* Init gift wrap (NIP-59) subscription state */
  self->sub_gift_wrap = 0;
  self->user_pubkey_hex = NULL;
  self->gift_wrap_queue = NULL; /* Created lazily when first gift wrap arrives */

  /* Init NIP-17 DM service and wire to inbox view */
  self->dm_service = gnostr_dm_service_new();
  if (self->dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(self->dm_inbox)) {
    gnostr_dm_service_set_inbox_view(self->dm_service, GNOSTR_DM_INBOX_VIEW(self->dm_inbox));
    g_debug("[DM_SERVICE] Connected DM service to inbox view");
  }

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

  /* If backfill requested via env, start periodic timer */
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
    /* Initialize label */
    if (self->lbl_signin_status && GTK_IS_LABEL(self->lbl_signin_status)) {
      gtk_label_set_text(GTK_LABEL(self->lbl_signin_status), signed_in ? "Signed in" : "Not signed in");
    }
    if (self->btn_login_local && GTK_IS_WIDGET(self->btn_login_local))
      gtk_widget_set_sensitive(self->btn_login_local, !signed_in);
    if (self->btn_pair_remote && GTK_IS_WIDGET(self->btn_pair_remote))
      gtk_widget_set_sensitive(self->btn_pair_remote, !signed_in);
    if (self->btn_sign_out && GTK_IS_WIDGET(self->btn_sign_out))
      gtk_widget_set_sensitive(self->btn_sign_out, signed_in);
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

  if (count > 0) {
    /* Update label and show indicator */
    char *label = g_strdup_printf("%u new note%s", count, count == 1 ? "" : "s");
    if (self->lbl_new_notes_count && GTK_IS_LABEL(self->lbl_new_notes_count)) {
      gtk_label_set_text(GTK_LABEL(self->lbl_new_notes_count), label);
    }
    g_free(label);

    if (self->new_notes_revealer && GTK_IS_REVEALER(self->new_notes_revealer)) {
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->new_notes_revealer), TRUE);
    }
  } else {
    /* Hide indicator when count is 0 */
    if (self->new_notes_revealer && GTK_IS_REVEALER(self->new_notes_revealer)) {
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->new_notes_revealer), FALSE);
    }
  }
}

/* nostrc-9f4: Idle callback to scroll timeline to top after model changes complete */
static gboolean scroll_to_top_idle(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;

  if (self->timeline && G_TYPE_CHECK_INSTANCE_TYPE(self->timeline, GNOSTR_TYPE_TIMELINE_VIEW)) {
    GtkWidget *scroller = gnostr_timeline_view_get_scrolled_window(GNOSTR_TIMELINE_VIEW(self->timeline));
    if (scroller && GTK_IS_SCROLLED_WINDOW(scroller)) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
      if (vadj) {
        gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
      }
    }
  }
  return G_SOURCE_REMOVE;
}

/* nostrc-yi2: Calm timeline - handle new notes indicator button click */
static void on_new_notes_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->event_model) return;

  /* Flush pending notes - this schedules model changes via idle */
  gn_nostr_event_model_flush_pending(self->event_model);

  /* nostrc-9f4: Defer scroll to idle to avoid GTK4 ListView crash.
   * The model flush schedules items_changed via g_idle_add, so we need to
   * scroll AFTER that emission completes. Using g_idle_add_full with lower
   * priority ensures our scroll runs after the model's default-priority idle. */
  g_idle_add_full(G_PRIORITY_LOW, scroll_to_top_idle, self, NULL);

  /* Hide indicator */
  if (self->new_notes_revealer && GTK_IS_REVEALER(self->new_notes_revealer)) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->new_notes_revealer), FALSE);
  }
}
GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}

/* ---- GObject type boilerplate and template binding ---- */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void gnostr_main_window_dispose(GObject *object) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  g_debug("main-window: dispose");

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
  if (self->pool) { g_object_unref(self->pool); self->pool = NULL; }
  if (self->seen_texts) { g_hash_table_unref(self->seen_texts); self->seen_texts = NULL; }
  if (self->event_model) { g_object_unref(self->event_model); self->event_model = NULL; }
  if (self->avatar_tex_cache) { g_hash_table_unref(self->avatar_tex_cache); self->avatar_tex_cache = NULL; }
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
  /* Ensure custom template child types are registered before parsing template */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_COMPOSER);
  g_type_ensure(GNOSTR_TYPE_PROFILE_PANE);
  g_type_ensure(GNOSTR_TYPE_DM_INBOX_VIEW);
  g_type_ensure(GNOSTR_TYPE_THREAD_VIEW);
  g_type_ensure(GNOSTR_TYPE_NOTIFICATIONS_VIEW);
  g_type_ensure(GNOSTR_TYPE_NOTIFICATION_ROW);
  g_type_ensure(GNOSTR_TYPE_PAGE_DISCOVER);
  g_type_ensure(GNOSTR_TYPE_SEARCH_RESULTS_VIEW);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  /* Bind expected template children (IDs must match the UI file) */
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, profile_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, profile_pane);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, thread_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, thread_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_relays);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_menu);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_avatar);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, avatar_popover);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, lbl_signin_status);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, lbl_profile_name);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_login_local);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_pair_remote);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_sign_out);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, composer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, dm_inbox);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, notifications_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_label);
  /* nostrc-yi2: Calm timeline - new notes indicator */
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, new_notes_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_new_notes);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, lbl_new_notes_count);
  /* Bind template callbacks referenced by the UI file */
  gtk_widget_class_bind_template_callback(widget_class, on_relays_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_settings_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_avatar_login_local_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_avatar_pair_remote_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_avatar_sign_out_clicked);
}

/* ---- Minimal stub implementations to satisfy build and support cached profiles path ---- */
static void initial_refresh_timeout_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  g_debug("STARTUP_DEBUG: initial_refresh_timeout_cb ENTER");

  if (self->event_model) {
    gn_nostr_event_model_refresh(self->event_model);
  }

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

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Refreshing…");
}

/* Context for async sign-and-publish operation */
struct _PublishContext {
  GnostrMainWindow *self;  /* weak ref */
  char *text;              /* owned; original note content */
};

static void publish_context_free(PublishContext *ctx) {
  if (!ctx) return;
  g_free(ctx->text);
  g_free(ctx);
}

/* Callback when DBus sign_event completes */
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  PublishContext *ctx = (PublishContext*)user_data;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    publish_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;
  NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

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
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json);
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
        g_debug("[PUBLISH] Skipping %s due to limit violations: %s", url, errors ? errors : "unknown");
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
      gnostr_relay_info_free(relay_info);
    }

    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) {
      fail_count++;
      continue;
    }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("[PUBLISH] Failed to connect to %s: %s", url, conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      g_debug("[PUBLISH] Published to %s", url);
      success_count++;
    } else {
      g_debug("[PUBLISH] Publish failed to %s: %s", url, pub_err ? pub_err->message : "unknown");
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

    /* Clear composer text on success */
    if (self->composer && GNOSTR_IS_COMPOSER(self->composer)) {
      gnostr_composer_clear(GNOSTR_COMPOSER(self->composer));
    }

    /* Switch to timeline tab */
    if (self->stack && GTK_IS_STACK(self->stack)) {
      gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "timeline");
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

  /* Get signer proxy */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    char *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    return;
  }

  show_toast(self, "Reposting...");

  /* Build unsigned kind 6 repost event JSON */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(6));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(""));

  /* Build tags array: e-tag and p-tag */
  json_t *tags = json_array();

  /* e-tag: ["e", "<reposted-event-id>", "<relay-hint>"] */
  json_t *e_tag = json_array();
  json_array_append_new(e_tag, json_string("e"));
  json_array_append_new(e_tag, json_string(id_hex));
  json_array_append_new(e_tag, json_string("")); /* relay hint - empty for now */
  json_array_append_new(tags, e_tag);

  /* p-tag: ["p", "<original-author-pubkey>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(pubkey_hex));
    json_array_append_new(tags, p_tag);
  }

  json_object_set_new(event_obj, "tags", tags);

  /* Serialize and sign via signer proxy */
  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  if (!event_json) {
    show_toast(self, "Failed to serialize repost event");
    return;
  }

  g_debug("[REPOST] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(""); /* repost has no text content */

  /* Call signer asynchronously */
  nostr_org_nostr_signer_call_sign_event(
    proxy,
    event_json,
    "",              /* current_user: empty = use default */
    "gnostr",        /* app_id */
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

  /* Get signer proxy */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    char *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    return;
  }

  show_toast(self, "Deleting note...");

  /* Build unsigned kind 5 deletion event JSON per NIP-09 */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(5));  /* NOSTR_KIND_DELETION */
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string("")); /* Optional deletion reason */

  /* Build tags array per NIP-09:
   * - ["e", "<event-id-to-delete>"]
   * - ["k", "<kind-of-deleted-event>"] (kind 1 for text notes)
   */
  json_t *tags = json_array();

  /* e-tag: ["e", "<event-id-to-delete>"] */
  json_t *e_tag = json_array();
  json_array_append_new(e_tag, json_string("e"));
  json_array_append_new(e_tag, json_string(id_hex));
  json_array_append_new(tags, e_tag);

  /* k-tag: ["k", "1"] to indicate we're deleting a kind 1 note */
  json_t *k_tag = json_array();
  json_array_append_new(k_tag, json_string("k"));
  json_array_append_new(k_tag, json_string("1"));
  json_array_append_new(tags, k_tag);

  json_object_set_new(event_obj, "tags", tags);

  /* Serialize and sign via signer proxy */
  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  if (!event_json) {
    show_toast(self, "Failed to serialize deletion event");
    return;
  }

  g_debug("[DELETE] Unsigned deletion event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(""); /* deletion has no text content */

  /* Call signer asynchronously */
  nostr_org_nostr_signer_call_sign_event(
    proxy,
    event_json,
    "",              /* current_user: empty = use default */
    "gnostr",        /* app_id */
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

/* Callback when DBus sign_event completes for like/reaction */
static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  LikeContext *ctx = (LikeContext*)user_data;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    like_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;
  NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

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
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json);
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
        gchar *errors = gnostr_relay_validation_result_format_errors(validation);
        g_debug("[LIKE] Skipping %s due to limit violations: %s", url, errors ? errors : "unknown");
        g_free(errors);
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
      g_debug("[LIKE] Failed to connect to %s: %s", url, conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      g_debug("[LIKE] Published reaction to %s", url);
      success_count++;
    } else {
      g_debug("[LIKE] Publish failed to %s: %s", url, pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      fail_count++;
    }
    g_object_unref(relay);
  }

  /* Show result toast and update UI */
  if (success_count > 0) {
    if (limit_skip_count > 0) {
      char *msg = g_strdup_printf("Liked! (%u relays skipped due to limits)", limit_skip_count);
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
        g_warning("[LIKE] Failed to ingest reaction event to local cache");
      } else {
        g_debug("[LIKE] Reaction event stored in local cache");
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

/* Public function to request a like/reaction (kind 7) - NIP-25 */
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, GnostrNoteCardRow *row) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[LIKE] Request like of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    show_toast(self, "Invalid event ID for like");
    return;
  }

  /* Check if already liked */
  if (self->liked_events && g_hash_table_contains(self->liked_events, id_hex)) {
    show_toast(self, "Already liked!");
    return;
  }

  /* Get signer proxy */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    char *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    return;
  }

  show_toast(self, "Liking...");

  /* Build unsigned kind 7 reaction event JSON (NIP-25) */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(NOSTR_KIND_REACTION));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string("+")); /* "+" = like reaction */

  /* Build tags array: e-tag and p-tag per NIP-25 */
  json_t *tags = json_array();

  /* e-tag: ["e", "<event-id-being-reacted-to>"] */
  json_t *e_tag = json_array();
  json_array_append_new(e_tag, json_string("e"));
  json_array_append_new(e_tag, json_string(id_hex));
  json_array_append_new(tags, e_tag);

  /* p-tag: ["p", "<pubkey-of-event-author>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(pubkey_hex));
    json_array_append_new(tags, p_tag);
  }

  /* k-tag: ["k", "<kind-of-reacted-event>"] per NIP-25 */
  json_t *k_tag = json_array();
  json_array_append_new(k_tag, json_string("k"));
  json_array_append_new(k_tag, json_string("1"));  /* text note - could be enhanced to pass actual kind */
  json_array_append_new(tags, k_tag);

  json_object_set_new(event_obj, "tags", tags);

  /* Serialize and sign via signer proxy */
  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

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

  /* Call signer asynchronously */
  nostr_org_nostr_signer_call_sign_event(
    proxy,
    event_json,
    "",              /* current_user: empty = use default */
    "gnostr",        /* app_id */
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

  /* Get signer proxy */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    char *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    return;
  }

  show_toast(self, "Signing...");

  /* Build unsigned event JSON */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(1));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(text));

  /* Build tags array */
  json_t *tags = json_array();

  /* Check if this is a reply - add NIP-10 threading tags */
  if (composer && GNOSTR_IS_COMPOSER(composer) && gnostr_composer_is_reply(GNOSTR_COMPOSER(composer))) {
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
      json_t *root_tag = json_array();
      json_array_append_new(root_tag, json_string("e"));
      json_array_append_new(root_tag, json_string(root_id));
      json_array_append_new(root_tag, json_string("")); /* relay hint */
      json_array_append_new(root_tag, json_string("root"));
      json_array_append_new(tags, root_tag);
    }

    /* Add reply e-tag if different from root (nested reply) */
    if (reply_to_id && strlen(reply_to_id) == 64 &&
        (!root_id || strcmp(reply_to_id, root_id) != 0)) {
      json_t *reply_tag = json_array();
      json_array_append_new(reply_tag, json_string("e"));
      json_array_append_new(reply_tag, json_string(reply_to_id));
      json_array_append_new(reply_tag, json_string("")); /* relay hint */
      json_array_append_new(reply_tag, json_string("reply"));
      json_array_append_new(tags, reply_tag);
    }

    /* Add p-tag for the author being replied to */
    if (reply_to_pubkey && strlen(reply_to_pubkey) == 64) {
      json_t *p_tag = json_array();
      json_array_append_new(p_tag, json_string("p"));
      json_array_append_new(p_tag, json_string(reply_to_pubkey));
      json_array_append_new(tags, p_tag);
    }
  }

  /* Check if this is a quote post - add q-tag and p-tag per NIP-18 */
  if (composer && GNOSTR_IS_COMPOSER(composer) && gnostr_composer_is_quote(GNOSTR_COMPOSER(composer))) {
    const char *quote_id = gnostr_composer_get_quote_id(GNOSTR_COMPOSER(composer));
    const char *quote_pubkey = gnostr_composer_get_quote_pubkey(GNOSTR_COMPOSER(composer));

    g_debug("[PUBLISH] Building quote post: quote_id=%s pubkey=%.8s...",
            quote_id ? quote_id : "(null)",
            quote_pubkey ? quote_pubkey : "(null)");

    /* q-tag: ["q", "<quoted-event-id>", "<relay-hint>"] */
    if (quote_id && strlen(quote_id) == 64) {
      json_t *q_tag = json_array();
      json_array_append_new(q_tag, json_string("q"));
      json_array_append_new(q_tag, json_string(quote_id));
      json_array_append_new(q_tag, json_string("")); /* relay hint */
      json_array_append_new(tags, q_tag);
    }

    /* p-tag: ["p", "<quoted-author-pubkey>"] */
    if (quote_pubkey && strlen(quote_pubkey) == 64) {
      json_t *p_tag = json_array();
      json_array_append_new(p_tag, json_string("p"));
      json_array_append_new(p_tag, json_string(quote_pubkey));
      json_array_append_new(tags, p_tag);
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
        json_t *imeta_tag = json_array();
        json_array_append_new(imeta_tag, json_string("imeta"));

        /* url field (required) */
        char *url_field = g_strdup_printf("url %s", m->url);
        json_array_append_new(imeta_tag, json_string(url_field));
        g_free(url_field);

        /* m field (MIME type) */
        if (m->mime_type && *m->mime_type) {
          char *mime_field = g_strdup_printf("m %s", m->mime_type);
          json_array_append_new(imeta_tag, json_string(mime_field));
          g_free(mime_field);
        }

        /* x field (SHA-256 hash) */
        if (m->sha256 && *m->sha256) {
          char *hash_field = g_strdup_printf("x %s", m->sha256);
          json_array_append_new(imeta_tag, json_string(hash_field));
          g_free(hash_field);
        }

        /* size field */
        if (m->size > 0) {
          char *size_field = g_strdup_printf("size %" G_GINT64_FORMAT, m->size);
          json_array_append_new(imeta_tag, json_string(size_field));
          g_free(size_field);
        }

        json_array_append_new(tags, imeta_tag);
        g_debug("[PUBLISH] Added imeta tag for: %s (type=%s, sha256=%.16s...)",
                m->url, m->mime_type ? m->mime_type : "?",
                m->sha256 ? m->sha256 : "?");
      }
    }
  }

  json_object_set_new(event_obj, "tags", tags);

  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  if (!event_json) {
    show_toast(self, "Failed to build event JSON");
    return;
  }

  g_debug("[PUBLISH] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = self;
  ctx->text = g_strdup(text);

  /* Call signer asynchronously */
  nostr_org_nostr_signer_call_sign_event(
    proxy,
    event_json,      /* event_json */
    "",              /* current_user: empty = use default */
    "gnostr",        /* app_id */
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
  
  /* Limit concurrent fetches to prevent goroutine explosion */
  if (self->profile_fetch_active >= self->profile_fetch_max_concurrent) {
    g_debug("profile_fetch: at max concurrent (%u/%u), deferring batch",
            self->profile_fetch_active, self->profile_fetch_max_concurrent);
    /* Re-schedule after a delay to check again */
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
    
    /* CRITICAL: Check if there are queued authors waiting and trigger a new fetch */
    if (self->profile_fetch_queue && self->profile_fetch_queue->len > 0) {
      g_debug("profile_fetch: ✅ SEQUENCE COMPLETE - %u authors queued, scheduling new fetch in 150ms",
             self->profile_fetch_queue->len);
      /* Schedule a new fetch for the queued authors */
      if (!self->profile_fetch_source_id) {
        guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
        self->profile_fetch_source_id = g_timeout_add(delay, profile_fetch_fire_idle, g_object_ref(self));
      } else {
        g_warning("profile_fetch: fetch already scheduled (source_id=%u), not scheduling again", self->profile_fetch_source_id);
      }
    } else {
      g_debug("profile_fetch: ✅ SEQUENCE COMPLETE - no authors queued");
    }
    /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
    return G_SOURCE_REMOVE;
  }

  if (!self->pool) self->pool = gnostr_simple_pool_new();
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
    /* Continue to next - NOTE: We're already in a timeout callback, so we need to schedule another one */
    g_timeout_add_full(G_PRIORITY_DEFAULT, 0, profile_dispatch_next, g_object_ref(self), (GDestroyNotify)g_object_unref);
    /* NOTE: Don't unref - GLib handles it via g_timeout_add_full's GDestroyNotify */
    return G_SOURCE_REMOVE;
  }

  /* Prepare authors array (borrow strings) */
  size_t n = batch->len;
  const char **authors = g_new0(const char*, n);
  for (guint i = 0; i < n; i++) authors[i] = (const char*)g_ptr_array_index(batch, i);

  ProfileBatchCtx *ctx = g_new0(ProfileBatchCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->batch = batch; /* ownership transferred; freed in callback */

  int limit_per_author = 0; /* no limit; filter limit is total, not per author */
  g_debug("[PROFILE] Dispatching batch %u/%u (%zu authors, active=%u/%u)",
          self->profile_batch_pos, self->profile_batches ? self->profile_batches->len : 0, n,
          self->profile_fetch_active, self->profile_fetch_max_concurrent);
  
  /* Increment active fetch counter */
  self->profile_fetch_active++;
  
  gnostr_simple_pool_fetch_profiles_by_authors_async(
      self->pool,
      self->profile_batch_urls,
      self->profile_batch_url_count,
      (const char* const*)authors,
      n,
      limit_per_author,
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

    gnostr_simple_pool_sync_relays(self->pool, urls, read_relays->len);
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

    /* Schedule restart after a brief delay to allow cancellation to complete */
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

static void start_pool_live(GnostrMainWindow *self) {
  /* Removed noisy debug */
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Prevent concurrent reconnection attempts */
  if (self->reconnection_in_progress) {
    g_debug("[RELAY] Reconnection already in progress, skipping");
    return;
  }
  self->reconnection_in_progress = TRUE;

  if (!self->pool) self->pool = gnostr_simple_pool_new();

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
  gnostr_simple_pool_sync_relays(self->pool, self->live_urls, self->live_url_count);
  g_debug("[RELAY] ✓ All relays initialized");
  /* Hook up events signal exactly once */
  if (self->pool_events_handler == 0) {
    self->pool_events_handler = g_signal_connect(self->pool, "events", G_CALLBACK(on_pool_events), self);
  }

  g_debug("[RELAY] Starting live subscription to %zu relays", self->live_url_count);
  gnostr_simple_pool_subscribe_many_async(self->pool,
                                         self->live_urls,
                                         self->live_url_count,
                                         filters,
                                         self->pool_cancellable,
                                         on_pool_subscribe_done,
                                         g_object_ref(self));

  /* Caller owns arrays and filters after async setup */
  nostr_filters_free(filters);
}

static void start_profile_subscription(GnostrMainWindow *self) {
  /* Optional: one-time fetch of current profile if signed in. We'll rely on demand-driven fetch otherwise. */
  (void)self; /* Intentionally minimal at this stage. */
}

static void start_bg_profile_prefetch(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->pool) self->pool = gnostr_simple_pool_new();
  if (!self->bg_prefetch_cancellable) self->bg_prefetch_cancellable = g_cancellable_new();

  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;
  /* Use same filter as live timeline (kind 1), but paginate to sweep authors and queue profiles */
  build_urls_and_filters(self, &urls, &url_count, &filters, (int)self->default_limit);
  if (!urls || url_count == 0 || !filters) {
    if (filters) nostr_filters_free(filters);
    if (urls) free_urls_owned(urls, url_count);
    return;
  }

  /* Connect a temporary events handler for prefetch-only author enqueue */
  g_signal_connect(self->pool, "events", G_CALLBACK(on_bg_prefetch_events), self);
  guint interval = self->bg_prefetch_interval_ms ? self->bg_prefetch_interval_ms : 250;
  g_debug("start_bg_profile_prefetch: paginate %zu relay(s) interval=%ums", url_count, interval);
  /* Build a standalone filter for paginator: kind=1 with same since/limit */
  NostrFilter *pf = nostr_filter_new();
  int kind1 = 1;
  nostr_filter_set_kinds(pf, &kind1, 1);
  if (self->default_limit > 0) nostr_filter_set_limit(pf, (int)self->default_limit);
  if (self->use_since && self->since_seconds > 0) {
    time_t now = time(NULL);
    int64_t since = (int64_t)(now - (time_t)self->since_seconds);
    if (since > 0) nostr_filter_set_since_i64(pf, since);
  }
  gnostr_simple_pool_paginate_with_interval_async(self->pool, urls, url_count, pf, interval, self->bg_prefetch_cancellable, NULL, NULL);
  nostr_filter_free(pf);
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
    g_debug("relay_health: reconnection in progress, skipping check");
    return G_SOURCE_CONTINUE;
  }
  
  /* Get list of relay URLs from the pool */
  GPtrArray *relay_urls = gnostr_simple_pool_get_relay_urls(self->pool);
  if (!relay_urls || relay_urls->len == 0) {
    g_debug("relay_health: no relays in pool");
    if (relay_urls) g_ptr_array_unref(relay_urls);
    return G_SOURCE_CONTINUE;
  }
  
  /* Check connection status of each relay */
  guint disconnected_count = 0;
  guint connected_count = 0;
  
  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = g_ptr_array_index(relay_urls, i);
    if (!url) continue;
    
    gboolean is_connected = gnostr_simple_pool_is_relay_connected(self->pool, url);
    if (is_connected) {
      connected_count++;
      g_debug("relay_health: %s is CONNECTED", url);
    } else {
      disconnected_count++;
      g_warning("relay_health: %s is DISCONNECTED", url);
    }
  }
  
  /* Log relay health and goroutine count to detect growth */
  int goroutine_count = go_get_active_count();
  /* Get ingest stats for memory diagnostics */
  guint64 ingest_count = storage_ndb_get_ingest_count();
  guint64 ingest_mb = storage_ndb_get_ingest_bytes() / (1024 * 1024);

  g_debug("relay_health: status - %u connected, %u disconnected (total %u, goroutines=%d, ingested=%" G_GUINT64_FORMAT ", ingest_mb=%" G_GUINT64_FORMAT ")",
          connected_count, disconnected_count, relay_urls->len, goroutine_count, ingest_count, ingest_mb);
  
  /* If ALL relays are disconnected, trigger reconnection (not just any) */
  if (disconnected_count > 0 && connected_count == 0) {
    g_warning("relay_health: ALL %u relay(s) disconnected - triggering reconnection", 
              disconnected_count);
    
    /* Restart the live subscription to reconnect */
    start_pool_live(self);
  } else if (disconnected_count > 0) {
    g_debug("relay_health: %u relay(s) disconnected but %u still connected - not reconnecting",
              disconnected_count, connected_count);
  }
  
  g_ptr_array_unref(relay_urls);
  return G_SOURCE_CONTINUE; /* Keep checking every interval */
}

/* Periodic model refresh to pick up new events from nostrdb */
static gboolean periodic_model_refresh(gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    return G_SOURCE_REMOVE;
  }
  
  if (self->event_model) {
    g_debug("[MODEL] Periodic refresh triggered");
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
  g_debug("[RELAY] Retrying subscription after failure");
  start_pool_live(self);
  return G_SOURCE_REMOVE;
}

/* Live stream setup completion */
static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GError *error = NULL;
  gboolean ok = gnostr_simple_pool_subscribe_many_finish(GNOSTR_SIMPLE_POOL(source), res, &error);
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  
  /* Clear reconnection flag */
  if (GNOSTR_IS_MAIN_WINDOW(self)) {
    self->reconnection_in_progress = FALSE;
  }
  
  if (!ok) {
    g_warning("live: subscribe_many failed: %s - retrying in 5 seconds",
              error ? error->message : "(unknown)");
    /* Retry after 5 seconds */
    g_timeout_add_seconds(5, retry_pool_live, g_object_ref(self));
  } else {
    g_debug("[RELAY] ✓ Live subscription started successfully");
    /* Start periodic health check (every 30 seconds) - only if not already running */
    if (GNOSTR_IS_MAIN_WINDOW(self) && self->health_check_source_id == 0) {
      self->health_check_source_id = g_timeout_add_seconds(30, check_relay_health, self);
    }
  }
  if (error) g_clear_error(&error);
  if (self) g_object_unref(self);
}

/* Main handler for live batches: ingest events into nostrdb (UI follows via subscriptions). */
static void on_pool_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !batch) return;

  guint ingested = 0;
  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *evt = (NostrEvent*)batch->pdata[i];
    if (!evt) continue;

    int kind = nostr_event_get_kind(evt);
    if (!(kind == 0 || kind == 1 || kind == 5 || kind == 6 || kind == 7 || kind == 16 || kind == 1111)) {
      continue;
    }

    const char *id = nostr_event_get_id(evt);
    if (!id || strlen(id) != 64) continue;

    /* Ingest to nostrdb for persistence; UI models subscribe to nostrdb and update from there. */
    char *evt_json = nostr_event_serialize_compact(evt);
    if (evt_json) {
      int ingest_rc = storage_ndb_ingest_event_json(evt_json, NULL);
      if (ingest_rc != 0) {
        g_debug("[INGEST] Failed to ingest event %.8s kind=%d: rc=%d json_len=%zu",
                id, kind, ingest_rc, strlen(evt_json));
      } else {
        ingested++;
      }
      free(evt_json);
    } else {
      g_debug("[INGEST] Failed to serialize event %.8s kind=%d", id, kind);
    }
  }

  if (ingested > 0) {
    g_debug("[INGEST] Ingested %u event(s) from relays (batch_len=%u)", ingested, batch->len);
  }
}

/* Background paginator event handler: only enqueue authors for profile fetch */
static void on_bg_prefetch_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !batch) return;
  guint enq = 0;
  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *evt = (NostrEvent*)batch->pdata[i];
    if (!evt) continue;
    if (nostr_event_get_kind(evt) != 1) continue;
    const char *pk = nostr_event_get_pubkey(evt);
    if (pk && strlen(pk) == 64) { enqueue_profile_author(self, pk); enq++; }
  }
  if (enq > 0) g_debug("[PROFILE] Background prefetch queued %u authors", enq);
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
 * Returns newly allocated string or NULL if not signed in. Caller must free. */
static char *get_current_user_pubkey_hex(void) {
  char *npub = client_settings_get_current_npub();
  if (!npub) return NULL;

  /* Decode bech32 npub to get raw pubkey bytes */
  uint8_t pubkey_bytes[32];

  /* Use NIP-19 decoder to convert npub to bytes */
  int decode_result = nostr_nip19_decode_npub(npub, pubkey_bytes);
  g_free(npub);

  if (decode_result != 0) {
    g_warning("[GIFTWRAP] Failed to decode npub to pubkey");
    return NULL;
  }

  /* Convert to hex string */
  char *hex = g_malloc0(65);
  storage_ndb_hex_encode(pubkey_bytes, hex);
  return hex;
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

/* Parses content_json and stores in profile provider, then updates the event model */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !content_json) return;
  
  /* Update profile provider cache */
  gnostr_profile_provider_update(pubkey_hex, content_json);
  
  /* Update GnNostrEventModel items */
  extern void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json);
  if (self->event_model) {
    gn_nostr_event_model_update_profile(G_OBJECT(self->event_model), pubkey_hex, content_json);
  }
}
