#include "gnostr-main-window.h"
#include "gnostr-composer.h"
#include "gnostr-timeline-view.h"
#include "gnostr-profile-pane.h"
#include "gnostr-profile-provider.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-row.h"
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
#include "libnostr_errors.h"
/* JSON interface */
#include "json.h"
/* Relays helpers */
#include "../util/relays.h"
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
static void on_event_model_need_profile(GnNostrEventModel *model, const char *pubkey_hex, gpointer user_data);
static void on_timeline_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_refresh_clicked(GtkButton *btn, gpointer user_data);
static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data);
static void on_relays_clicked(GtkButton *btn, gpointer user_data);
static void on_settings_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_login_local_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_pair_remote_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_sign_out_clicked(GtkButton *btn, gpointer user_data);
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data);
static void on_profile_pane_close_requested(GnostrProfilePane *pane, gpointer user_data);
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
  GtkWidget *btn_refresh;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
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
};

/* Old LRU functions removed - now using profile provider */

/* ---- Memory stats logging and cache pruning ---- */

/* Cache size limits to prevent unbounded memory growth */
#define AVATAR_CACHE_MAX 1000
#define SEEN_TEXTS_MAX 10000

static gboolean memory_stats_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_CONTINUE;
  
  guint seen_texts_size = self->seen_texts ? g_hash_table_size(self->seen_texts) : 0;
  guint profile_queue = self->profile_fetch_queue ? self->profile_fetch_queue->len : 0;
  guint avatar_size = self->avatar_tex_cache ? g_hash_table_size(self->avatar_tex_cache) : 0;
  guint model_items = self->event_model ? g_list_model_get_n_items(G_LIST_MODEL(self->event_model)) : 0;
  
  g_message("[MEMORY] model=%u seen_texts=%u avatars=%u profile_q=%u",
          model_items, seen_texts_size, avatar_size, profile_queue);
  
  /* Prune caches if they exceed limits to prevent unbounded memory growth */
  gboolean pruned = FALSE;
  
  /* Prune avatar_tex_cache - clear entirely if too large */
  if (avatar_size > AVATAR_CACHE_MAX) {
    g_message("[MEMORY] Pruning avatar_tex_cache: %u -> 0", avatar_size);
    g_hash_table_remove_all(self->avatar_tex_cache);
    pruned = TRUE;
  }
  
  /* Prune seen_texts - clear if too large */
  if (seen_texts_size > SEEN_TEXTS_MAX) {
    g_message("[MEMORY] Pruning seen_texts: %u -> 0", seen_texts_size);
    g_hash_table_remove_all(self->seen_texts);
    pruned = TRUE;
  }
  
  if (pruned) {
    g_message("[MEMORY] Cache pruning complete");
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

static void on_relays_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Relays settings (stub)");
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
  /* Wire buttons */
  GtkWidget *btn_cancel = GTK_WIDGET(gtk_builder_get_object(builder, "btn_cancel"));
  if (btn_cancel) g_signal_connect(btn_cancel, "clicked", G_CALLBACK(settings_on_close_clicked), win);
  GtkWidget *btn_save = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save"));
  if (btn_save) g_signal_connect(btn_save, "clicked", G_CALLBACK(settings_on_close_clicked), win);
  /* Auto-unref builder when window is destroyed */
  g_signal_connect(win, "destroy", G_CALLBACK(g_object_unref), builder);
  gtk_window_present(win);
}

static void on_avatar_login_local_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Login with Local Signer (stub)");
}

static void on_avatar_pair_remote_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Pair Remote Signer (stub)");
}

static void on_avatar_sign_out_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Signed out (stub)");
}

/* Profile pane signal handlers */
static void on_note_card_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  (void)row;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
  
  g_message("[UI] Opening profile pane for %.*s...", 8, pubkey_hex);
  
  /* Check if profile pane is already showing this profile */
  extern const char* gnostr_profile_pane_get_current_pubkey(GnostrProfilePane *pane);
  if (GNOSTR_IS_PROFILE_PANE(self->profile_pane)) {
    const char *current = gnostr_profile_pane_get_current_pubkey(GNOSTR_PROFILE_PANE(self->profile_pane));
    if (current && strcmp(current, pubkey_hex) == 0) {
      /* Already showing this profile, just ensure pane is visible */
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->profile_revealer), TRUE);
      return;
    }
  }
  
  /* Show the profile pane */
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

  g_message("[REPLY] Request reply to id=%s root=%s pubkey=%.8s...",
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
  g_message("prepopulate_all_profiles_from_cache: query rc=%d count=%d", rc, n);
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
      g_message("prepopulate_all_profiles_from_cache: scheduling %u cached profiles", items->len);
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
  /* Report HTTP avatar support availability */
#ifdef HAVE_SOUP3
  g_message("http: libsoup3 enabled; avatar HTTP fetch active");
#else
  g_warning("http: libsoup3 NOT enabled; avatar HTTP fetch disabled");
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
  /* Build app menu for header button */
  if (self->btn_menu) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Quit", "app.quit");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_menu), G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_message("connecting post-requested handler on composer=%p", (void*)self->composer);
  g_signal_connect(self->composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);
  if (self->btn_refresh) {
    g_signal_connect(self->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), self);
  }
  /* Connect profile pane signals */
  if (self->profile_pane && GNOSTR_IS_PROFILE_PANE(self->profile_pane)) {
    g_signal_connect(self->profile_pane, "close-requested", 
                     G_CALLBACK(on_profile_pane_close_requested), self);
    g_message("connected profile pane close-requested signal");
  }
  if (self->btn_avatar) {
    /* Ensure avatar button is interactable */
    gtk_widget_set_sensitive(self->btn_avatar, TRUE);
    gtk_widget_set_tooltip_text(self->btn_avatar, "Login / Account");
  }
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
      g_message("[INIT] Starting live subscriptions (GNOSTR_LIVE=TRUE)");
      start_pool_live(self);
      /* Also start profile subscription if identity is configured */
      start_profile_subscription(self);
      
      /* NOTE: Periodic refresh disabled - nostrdb ingestion drives UI updates via GnNostrEventModel.
       * This avoids duplicate processing and high memory usage. Initial refresh occurs in
       * initial_refresh_timeout_cb, and subsequent updates stream from nostrdb watchers. */
    }
  }
  
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

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}

/* ---- GObject type boilerplate and template binding ---- */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void gnostr_main_window_dispose(GObject *object) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  g_debug("main-window: dispose");
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
  
  /* Shutdown profile provider */
  gnostr_profile_provider_shutdown();
  
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
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  /* Bind expected template children (IDs must match the UI file) */
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, profile_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, profile_pane);
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
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_label);
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
  g_message("STARTUP_DEBUG: initial_refresh_timeout_cb ENTER");

  if (self->event_model) {
    gn_nostr_event_model_refresh(self->event_model);
  }

  g_message("STARTUP_DEBUG: initial_refresh_timeout_cb EXIT");
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

static void gnostr_load_settings(GnostrMainWindow *self) { (void)self; /* TODO: load persisted settings */ }

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Refreshing…");
}

/* Context for async sign-and-publish operation */
typedef struct {
  GnostrMainWindow *self;  /* weak ref */
  char *text;              /* owned; original note content */
} PublishContext;

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

  g_message("[PUBLISH] Signed event: %.100s...", signed_event_json);

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

  /* Get relay URLs from config */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_urls);
  if (relay_urls->len == 0) {
    /* Fallback defaults */
    g_ptr_array_add(relay_urls, g_strdup("wss://relay.primal.net/"));
    g_ptr_array_add(relay_urls, g_strdup("wss://relay.damus.io/"));
    g_ptr_array_add(relay_urls, g_strdup("wss://nos.lol/"));
  }

  /* Publish to each relay */
  guint success_count = 0;
  guint fail_count = 0;
  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = (const char*)g_ptr_array_index(relay_urls, i);
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) {
      fail_count++;
      continue;
    }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_message("[PUBLISH] Failed to connect to %s: %s", url, conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      g_message("[PUBLISH] Published to %s", url);
      success_count++;
    } else {
      g_message("[PUBLISH] Publish failed to %s: %s", url, pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      fail_count++;
    }
    g_object_unref(relay);
  }

  /* Show result toast */
  if (success_count > 0) {
    char *msg = g_strdup_printf("Published to %u relay%s", success_count, success_count == 1 ? "" : "s");
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
    show_toast(self, "Failed to publish to any relay");
  }

  /* Cleanup */
  nostr_event_free(event);
  g_free(signed_event_json);
  g_ptr_array_free(relay_urls, TRUE);
  publish_context_free(ctx);
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

    g_message("[PUBLISH] Building reply event: reply_to=%s root=%s pubkey=%.8s...",
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

  json_object_set_new(event_obj, "tags", tags);

  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  if (!event_json) {
    show_toast(self, "Failed to build event JSON");
    return;
  }

  g_message("[PUBLISH] Unsigned event: %s", event_json);

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

  /* Load relays from config */
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(arr);
  if (arr->len == 0) {
    /* Provide a sensible default if none configured */
    g_ptr_array_add(arr, g_strdup("wss://relay.primal.net/"));
    g_ptr_array_add(arr, g_strdup("wss://relay.damus.io/"));
    g_ptr_array_add(arr, g_strdup("wss://relay.sharegap.net/"));
    g_ptr_array_add(arr, g_strdup("wss://nos.lol/"));
    g_ptr_array_add(arr, g_strdup("wss://purplepag.es/"));
    g_ptr_array_add(arr, g_strdup("wss://relay.nostr.band/"));
    g_ptr_array_add(arr, g_strdup("wss://indexer.coracle.social/"));
  }

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

  /* Build live URLs and filters: subscribe to all required kinds for persistence-first operation. */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;

  const int live_kinds[] = {0, 1, 5, 6, 7, 16, 1111};
  build_urls_and_filters_for_kinds(self,
                                  live_kinds,
                                  G_N_ELEMENTS(live_kinds),
                                  &urls,
                                  &url_count,
                                  &filters,
                                  (int)self->default_limit);
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
   * We call ensure_relay() here BEFORE starting subscriptions to populate the pool.
   * This is acceptable because start_pool_live() runs early at startup, not on main loop yet. */
  NostrSimplePool *c_pool = GNOSTR_SIMPLE_POOL(self->pool)->pool;
  g_message("[RELAY] Initializing %zu relays in pool (pool=%p)", self->live_url_count, (void*)c_pool);
  for (size_t i = 0; i < self->live_url_count; i++) {
    if (self->live_urls[i] && *self->live_urls[i]) {
      nostr_simple_pool_ensure_relay(c_pool, self->live_urls[i]);
      g_debug("[RELAY] Added relay %s to pool", self->live_urls[i]);
    }
  }
  g_message("[RELAY] ✓ All relays initialized (pool now has %zu relays)", c_pool->relay_count);
  /* Hook up events signal exactly once */
  if (self->pool_events_handler == 0) {
    self->pool_events_handler = g_signal_connect(self->pool, "events", G_CALLBACK(on_pool_events), self);
  }

  g_message("[RELAY] Starting live subscription to %zu relays", self->live_url_count);
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
  g_message("start_bg_profile_prefetch: paginate %zu relay(s) interval=%ums", url_count, interval);
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
  
  /* Also log the underlying C pool relay count and goroutine count to detect growth */
  NostrSimplePool *c_pool = GNOSTR_SIMPLE_POOL(self->pool)->pool;
  size_t c_pool_count = c_pool ? c_pool->relay_count : 0;
  int goroutine_count = go_get_active_count();
  /* Get ingest stats for memory diagnostics */
  guint64 ingest_count = storage_ndb_get_ingest_count();
  guint64 ingest_mb = storage_ndb_get_ingest_bytes() / (1024 * 1024);
  
  g_message("relay_health: status - %u connected, %u disconnected (total %u, c_pool=%zu, goroutines=%d, ingested=%" G_GUINT64_FORMAT ", ingest_mb=%" G_GUINT64_FORMAT ")",
          connected_count, disconnected_count, relay_urls->len, c_pool_count, goroutine_count, ingest_count, ingest_mb);
  
  /* If ALL relays are disconnected, trigger reconnection (not just any) */
  if (disconnected_count > 0 && connected_count == 0) {
    g_warning("relay_health: ALL %u relay(s) disconnected - triggering reconnection", 
              disconnected_count);
    
    /* Restart the live subscription to reconnect */
    start_pool_live(self);
  } else if (disconnected_count > 0) {
    g_message("relay_health: %u relay(s) disconnected but %u still connected - not reconnecting",
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
  g_message("[RELAY] Retrying subscription after failure");
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
    g_message("[RELAY] ✓ Live subscription started successfully");
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

static char *client_settings_get_current_npub(void) { return NULL; }

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
