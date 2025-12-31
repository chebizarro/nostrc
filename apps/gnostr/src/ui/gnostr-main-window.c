#include "gnostr-main-window.h"
#include "gnostr-composer.h"
#include "gnostr-timeline-view.h"
#include "../ipc/signer_ipc.h"
#include <gio/gio.h>
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
/* JSON interface */
#include "json.h"
/* Relays helpers */
#include "../util/relays.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
/* NIP-19 helpers */
#include "nostr/nip19/nip19.h"
/* NIP-46 client (remote signer pairing) */
#include "nostr/nip46/nip46_client.h"

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
typedef struct UiEventRow UiEventRow;
static void ui_event_row_free(gpointer p);
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static gboolean periodic_backfill_cb(gpointer data);
static void on_refresh_clicked(GtkButton *btn, gpointer user_data);
static void on_composer_post_requested(GnostrComposer *composer, gpointer user_data);
static void on_relays_clicked(GtkButton *btn, gpointer user_data);
static void on_settings_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_login_local_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_pair_remote_clicked(GtkButton *btn, gpointer user_data);
static void on_avatar_sign_out_clicked(GtkButton *btn, gpointer user_data);
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
/* Helpers to build TimelineItem rows from events */
static void append_note_from_event(GnostrMainWindow *self, NostrEvent *evt);
static void derive_identity_from_meta(GnostrMainWindow *self, const char *pubkey_hex,
                                     char **out_display, char **out_handle, char **out_avatar_url);
static char *format_timestamp_approx(gint64 created_at);

/* Avatar HTTP downloader (libsoup) helpers declared later, after struct definition */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json);
static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_pool_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);
static void on_bg_prefetch_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);

/* Demand-driven profile fetch helpers */
static void enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex);
static gboolean profile_fetch_fire_idle(gpointer data);
static void on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void profile_dispatch_next(GnostrMainWindow *self);

/* Forward decl: relay URL builder used by profile fetch */
static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit);

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
  g_message("apply_profiles_idle: applied=%u (scheduled=%u)", applied, c->items->len);
  idle_apply_profiles_ctx_free(c);
  return G_SOURCE_REMOVE;
}

static void schedule_apply_profiles(GnostrMainWindow *self, GPtrArray *items /* ProfileApplyCtx* */) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !items) { if (items) g_ptr_array_free(items, TRUE); return; }
  IdleApplyProfilesCtx *c = g_new0(IdleApplyProfilesCtx, 1);
  c->self = g_object_ref(self);
  c->items = items; /* transfer */
  g_message("schedule_apply_profiles: posting %u item(s) to main loop", items->len);
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, apply_profiles_idle, c, NULL);
}

static gboolean profile_apply_on_main(gpointer data) {
  ProfileApplyCtx *c = (ProfileApplyCtx*)data;
  if (c && c->pubkey_hex && c->content_json) {
    g_message("profile_apply_on_main: applying pubkey=%.*s… content_len=%zu", 8, c->pubkey_hex, strlen(c->content_json));
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
  GtkWidget *btn_refresh;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
  /* Session state */
  GHashTable *seen_texts; /* owned; keys are g_strdup(text), values unused */
  GHashTable *seen_ids;   /* owned; keys are g_strdup(event id hex), values unused */
  /* Thread model */
  GListStore *thread_roots;    /* owned; element-type TimelineItem */
  GHashTable *nodes_by_id;     /* owned; key: id string -> TimelineItem* (weak) */
  /* Metadata cache: key=pubkey hex (string), value=UserMeta* (owned) */
  GHashTable *meta_by_ptr;     /* owned; maps ptr to nodes */
  GHashTable *meta_by_id;       /* owned; maps event_id to nodes */
  GHashTable *meta_by_pubkey;   /* owned; maps pubkey to UserMeta */
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
 
  /* Sequential profile batch dispatch state */
  GPtrArray      *profile_batches;       /* owned; elements: GPtrArray* of char* authors */
  guint           profile_batch_pos;     /* next batch index */
  const char    **profile_batch_urls;    /* owned array pointer; strings not owned */
  size_t          profile_batch_url_count;

  /* Debounced local NostrDB profile sweep */
  guint           ndb_sweep_source_id;   /* GLib source id, 0 if none */
  guint           ndb_sweep_debounce_ms; /* default ~150ms */
};

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
  if (!self->profile_fetch_queue)
    self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  /* Dedup linear scan (queue is expected to stay small) */
  for (guint i = 0; i < self->profile_fetch_queue->len; i++) {
    const char *s = (const char*)g_ptr_array_index(self->profile_fetch_queue, i);
    if (g_strcmp0(s, pubkey_hex) == 0) goto schedule_only; /* already queued */
  }
  g_ptr_array_add(self->profile_fetch_queue, g_strdup(pubkey_hex));
  g_message("profile_enqueue: +1 author %.*s… (queue=%u)", 8, pubkey_hex, self->profile_fetch_queue->len);
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
  self->profile_fetch_source_id = 0;
  if (!self->pool) self->pool = gnostr_simple_pool_new();
  if (!self->profile_fetch_queue || self->profile_fetch_queue->len == 0) return G_SOURCE_REMOVE;
  /* Snapshot and clear queue */
  GPtrArray *authors = self->profile_fetch_queue;
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  /* Build relay URLs */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *dummy = NULL;
  build_urls_and_filters(self, &urls, &url_count, &dummy, 0 /* unused for profiles */);
  if (!urls || url_count == 0) {
    g_message("profile_fetch: no relays configured; dropping %u author(s)", authors->len);
    g_ptr_array_free(authors, TRUE);
    if (dummy) nostr_filters_free(dummy);
    return G_SOURCE_REMOVE;
  }
  /* Build batch list but dispatch sequentially (EOSE-gated) */
  const guint total = authors->len;
  const guint batch_sz = 16; /* start conservative */
  const guint n_batches = (total + batch_sz - 1) / batch_sz;
  g_message("profile_fetch: queueing %u author(s) across %zu relay(s) into %u batch(es)", total, url_count, n_batches);
  /* Init/clear prior sequence if any (should be none normally) */
  if (self->profile_batches) {
    for (guint i = 0; i < self->profile_batches->len; i++) {
      GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
      if (b) g_ptr_array_free(b, TRUE);
    }
    g_ptr_array_free(self->profile_batches, TRUE);
    self->profile_batches = NULL;
  }
  /* Do not set a free-func: we'll free each batch when its callback completes,
     and clean up any remaining (if canceled) at sequence end. */
  self->profile_batches = g_ptr_array_new();
  self->profile_batch_pos = 0;
  /* Capture relay URLs for the whole sequence (free array pointer at end only) */
  self->profile_batch_urls = urls; /* take ownership of array pointer */
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
  profile_dispatch_next(self);
  return G_SOURCE_REMOVE;
}

typedef struct ProfileBatchCtx {
  GnostrMainWindow *self;      /* strong ref */
  GPtrArray        *batch;     /* owned; char* */
} ProfileBatchCtx;

static void on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrSimplePool *pool = GNOSTR_SIMPLE_POOL(source); (void)pool;
  ProfileBatchCtx *ctx = (ProfileBatchCtx*)user_data;
  GError *error = NULL;
  GPtrArray *jsons = gnostr_simple_pool_fetch_profiles_by_authors_finish(GNOSTR_SIMPLE_POOL(source), res, &error);
  if (error) {
    g_warning("profile_fetch: finish error: %s", error->message);
    g_clear_error(&error);
  }
  /* Update cache/UI from returned events */
  if (jsons) {
    guint deserialized = 0, dispatched = 0;
    GPtrArray *items = g_ptr_array_new_with_free_func(profile_apply_item_free);
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
    g_message("profile_fetch: batch summary; json=%u deserialized=%u dispatched=%u",
              jsons->len, deserialized, dispatched);
    if (items->len > 0 && GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
      schedule_apply_profiles(ctx->self, items); /* transfers ownership */
      items = NULL;
    }
    if (items) g_ptr_array_free(items, TRUE);
    g_ptr_array_free(jsons, TRUE);
  }
  else {
    g_message("profile_fetch: batch returned NULL results");
  }
  /* Done with this batch's author list */
  if (ctx && ctx->batch) g_ptr_array_free(ctx->batch, TRUE);
  /* Advance to next batch */
  if (ctx && GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    GnostrMainWindow *self = ctx->self;
    profile_dispatch_next(self);
    g_object_unref(self);
  }
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

static void derive_identity_from_meta(GnostrMainWindow *self, const char *pubkey_hex,
                                     char **out_display, char **out_handle, char **out_avatar_url) {
  if (out_display) *out_display = NULL;
  if (out_handle) *out_handle = NULL;
  if (out_avatar_url) *out_avatar_url = NULL;
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !self->meta_by_pubkey) return;
  const char *content_json = (const char*)g_hash_table_lookup(self->meta_by_pubkey, pubkey_hex);
  if (!content_json || !*content_json) return;
  json_error_t jerr;
  json_t *root = json_loads(content_json, 0, &jerr);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return; }
  const char *display = NULL, *handle = NULL, *picture = NULL;
  json_t *jdisplay = json_object_get(root, "display_name");
  json_t *jhandle  = json_object_get(root, "name");
  json_t *jpic     = json_object_get(root, "picture");
  if (json_is_string(jdisplay)) display = json_string_value(jdisplay);
  if (json_is_string(jhandle))  handle  = json_string_value(jhandle);
  if (json_is_string(jpic))     picture = json_string_value(jpic);
  if (out_display && display) *out_display = g_strdup(display);
  if (out_handle && handle) {
    if (handle[0] == '@') *out_handle = g_strdup(handle);
    else *out_handle = g_strdup_printf("@%s", handle);
  }
  if (out_avatar_url && picture) *out_avatar_url = g_strdup(picture);
  json_decref(root);
}

static void append_note_from_event(GnostrMainWindow *self, NostrEvent *evt) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !evt || !self->thread_roots) return;
  const char *content = nostr_event_get_content(evt);
  const char *pubkey  = nostr_event_get_pubkey(evt);
  const char *id_hex  = nostr_event_get_id(evt);
  gint64 created_at   = nostr_event_get_created_at(evt);
  if (!content) content = "";
  if (!pubkey || !id_hex) return;

  /* Identity from cached profile, if available */
  char *display = NULL, *handle = NULL, *avatar_url = NULL;
  derive_identity_from_meta(self, pubkey, &display, &handle, &avatar_url);

  /* Friendly timestamp string for initial bind (view recomputes from created_at too) */
  g_autofree char *ts = format_timestamp_approx(created_at);

  /* Build TimelineItem */
  GObject *item = g_object_new(timeline_item_get_type(),
                               "display-name", display ? display : "",
                               "handle",       handle  ? handle  : "",
                               "timestamp",    ts      ? ts      : "",
                               "content",      content,
                               "depth",        0,
                               NULL);
  /* Set remaining metadata */
  g_object_set(item,
               "id",          id_hex,
               "root-id",     id_hex, /* no threading yet: root=self */
               "pubkey",      pubkey,
               "created-at",  created_at,
               "avatar-url",  avatar_url ? avatar_url : "",
               NULL);

  /* Append to roots model */
  g_list_store_append(self->thread_roots, item);
  g_object_unref(item);

  /* Optional: prefetch avatar in background */
  if (avatar_url && *avatar_url) gnostr_avatar_prefetch(avatar_url);

  g_free(display);
  g_free(handle);
  g_free(avatar_url);
}

static void prepopulate_text_notes_from_cache(GnostrMainWindow *self, guint limit) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  void *txn = NULL; char **arr = NULL; int n = 0;
  if (storage_ndb_begin_query(&txn) != 0) { g_warning("prepopulate_text_notes_from_cache: begin_query failed"); return; }
  /* Build filters: kind 1 with limit */
  g_autofree char *filters = g_strdup_printf("[{\"kinds\":[1],\"limit\":%u}]", limit > 0 ? limit : 30);
  int rc = storage_ndb_query(txn, filters, &arr, &n);
  g_message("prepopulate_text_notes_from_cache: query rc=%d count=%d", rc, n);
  if (rc == 0 && arr && n > 0) {
    /* Insert in reverse so newest ends up at top if model shows append order */
    for (int i = 0; i < n; i++) {
      const char *evt_json = arr[i];
      if (!evt_json) continue;
      NostrEvent *evt = nostr_event_new();
      if (evt && nostr_event_deserialize(evt, evt_json) == 0) {
        if (nostr_event_get_kind(evt) == 1) {
          append_note_from_event(self, evt);
        }
      } else {
        if (evt_json) {
          size_t len = strlen(evt_json);
          char snippet[121]; size_t copy = len < 120 ? len : 120; memcpy(snippet, evt_json, copy); snippet[copy] = '\0';
          g_warning("prepopulate_text_notes_from_cache: deserialize failed json='%s'%s", snippet, len > 120 ? "…" : "");
        }
      }
      if (evt) nostr_event_free(evt);
    }
  }
  storage_ndb_free_results(arr, n);
  storage_ndb_end_query(txn);
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
  g_message("main_window_init: btn_avatar=%p avatar_popover=%p popover_now=%p",
            (void*)self->btn_avatar, (void*)self->avatar_popover, (void*)init_pop);
  if (self->btn_avatar && self->avatar_popover) {
    /* Unconditionally associate the popover to avoid ambiguity */
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->btn_avatar), GTK_WIDGET(self->avatar_popover));
  }
  g_return_if_fail(self->composer != NULL);
  /* Initialize weak refs to template children needed in async paths */
  g_weak_ref_init(&self->timeline_ref, self->timeline);
  /* Prepare timeline roots model and attach to view so subsequent inserts render */
  if (!self->thread_roots) {
    extern GType timeline_item_get_type(void);
    self->thread_roots = g_list_store_new(timeline_item_get_type());
    if (self->timeline && G_TYPE_CHECK_INSTANCE_TYPE(self->timeline, GNOSTR_TYPE_TIMELINE_VIEW)) {
      g_message("main_window_init: setting timeline roots model (%p)", (void*)self->thread_roots);
      gnostr_timeline_view_set_tree_roots(GNOSTR_TIMELINE_VIEW(self->timeline), G_LIST_MODEL(self->thread_roots));
    } else {
      g_debug("main_window_init: timeline widget not a GnostrTimelineView (type=%s)", self->timeline ? G_OBJECT_TYPE_NAME(self->timeline) : "(null)");
    }
  }
  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize id -> node index for thread linking */
  if (!self->nodes_by_id) {
    self->nodes_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  /* Initialize metadata cache */
  self->meta_by_pubkey = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, user_meta_free);
  /* Initialize avatar texture cache */
  self->avatar_tex_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
  /* Do NOT pre-populate/apply cached profiles here; we defer to a debounced
   * sweep after timeline items are appended (see initial_refresh_timeout_cb and on_pool_events).
   */
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
  if (self->btn_avatar) {
    /* Ensure avatar button is interactable */
    gtk_widget_set_sensitive(self->btn_avatar, TRUE);
    gtk_widget_set_tooltip_text(self->btn_avatar, "Login / Account");
  }
  /* Ensure Timeline page is visible initially */
  if (self->stack && self->timeline && GTK_IS_STACK(self->stack)) {
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->timeline);
  }
  /* Seed initial items so Timeline page isn't empty */
  g_timeout_add_once(150, (GSourceOnceFunc)initial_refresh_timeout_cb, self);

  /* Optional: enable live subscriptions at startup when GNOSTR_LIVE is set */
  {
    const char *live = g_getenv("GNOSTR_LIVE");
    if (live && *live && g_strcmp0(live, "0") != 0) {
      g_message("main_window_init: GNOSTR_LIVE set; starting live subscriptions");
      start_pool_live(self);
      /* Also start profile subscription if identity is configured */
      start_profile_subscription(self);
    }
  }

  /* Init demand-driven profile fetch state */
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  self->profile_fetch_source_id = 0;
  self->profile_fetch_debounce_ms = 150;
  self->profile_fetch_cancellable = g_cancellable_new();

  /* Init debounced NostrDB profile sweep */
  self->ndb_sweep_source_id = 0;
  self->ndb_sweep_debounce_ms = 150;
  if (!self->pool) self->pool = gnostr_simple_pool_new();

  /* Init background prefetch defaults and kick it off */
  self->bg_prefetch_handler = 0;
  self->bg_prefetch_cancellable = g_cancellable_new();
  self->bg_prefetch_interval_ms = 250;
  start_bg_profile_prefetch(self);

  /* Optional: insert a synthetic timeline event when GNOSTR_SYNTH is set (for wiring validation) */
  {
    const char *synth = g_getenv("GNOSTR_SYNTH");
    if (synth && *synth && g_strcmp0(synth, "0") != 0) {
      g_message("main_window_init: GNOSTR_SYNTH set; inserting synthetic timeline event");
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

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}

/* ---- GObject type boilerplate and template binding ---- */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void gnostr_main_window_dispose(GObject *object) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(object);
  if (self->profile_fetch_cancellable) { g_object_unref(self->profile_fetch_cancellable); self->profile_fetch_cancellable = NULL; }
  if (self->bg_prefetch_cancellable) { g_object_unref(self->bg_prefetch_cancellable); self->bg_prefetch_cancellable = NULL; }
  if (self->pool_cancellable) { g_object_unref(self->pool_cancellable); self->pool_cancellable = NULL; }
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
    g_free((gpointer)self->profile_batch_urls);
    self->profile_batch_urls = NULL;
    self->profile_batch_url_count = 0;
  }
  if (self->pool) { g_object_unref(self->pool); self->pool = NULL; }
  if (self->seen_texts) { g_hash_table_unref(self->seen_texts); self->seen_texts = NULL; }
  if (self->seen_ids) { g_hash_table_unref(self->seen_ids); self->seen_ids = NULL; }
  if (self->nodes_by_id) { g_hash_table_unref(self->nodes_by_id); self->nodes_by_id = NULL; }
  if (self->meta_by_pubkey) { g_hash_table_unref(self->meta_by_pubkey); self->meta_by_pubkey = NULL; }
  if (self->avatar_tex_cache) { g_hash_table_unref(self->avatar_tex_cache); self->avatar_tex_cache = NULL; }
  if (self->thread_roots) { g_object_unref(self->thread_roots); self->thread_roots = NULL; }
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->dispose(object);
}

static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_main_window_dispose;
  /* Ensure custom template child types are registered before parsing template */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_COMPOSER);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  /* Bind expected template children (IDs must match the UI file) */
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
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

static void gnostr_main_window_init(GnostrMainWindow *self);

/* ---- Minimal stub implementations to satisfy build and support cached profiles path ---- */
static void initial_refresh_timeout_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  /* Populate recent text notes from local cache so the timeline is not empty */
  guint limit = self->default_limit ? self->default_limit : 30;
  prepopulate_text_notes_from_cache(self, limit);
  /* After items exist, sweep local DB for any cached profiles (debounced) */
  schedule_ndb_profile_sweep(self);
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

static void on_composer_post_requested(GnostrComposer *composer, gpointer user_data) {
  (void)composer;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  show_toast(self, "Post requested (stub)");
}

static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit) {
  if (out_urls) *out_urls = NULL;
  if (out_count) *out_count = 0;
  if (out_filters) *out_filters = NULL;
  (void)self;

  /* Load relays from config */
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(arr);
  if (arr->len == 0) {
    /* Provide a sensible default if none configured */
    //g_ptr_array_add(arr, g_strdup("wss://relay.damus.io"));
    g_ptr_array_add(arr, g_strdup("wss://relay.sharegap.net"));
}
  const char **urls = NULL; size_t n = arr->len;
  if (n > 0) {
    urls = g_new0(const char*, n);
    for (guint i = 0; i < arr->len; i++) urls[i] = (const char*)g_ptr_array_index(arr, i);
  }
  if (out_urls) *out_urls = urls;
  if (out_count) *out_count = n;

  /* Build a single filter for kind-1 timeline */
  if (out_filters) {
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kind1 = 1;
    nostr_filter_set_kinds(f, &kind1, 1);
    if (limit > 0) nostr_filter_set_limit(f, limit);
    /* Optional since window */
    /* We only use since if explicitly enabled to avoid missing older cached content */
    /* This relies on self fields; guard against NULL */
    if (self && self->use_since && self->since_seconds > 0) {
      time_t now = time(NULL);
      int64_t since = (int64_t)(now - (time_t)self->since_seconds);
      if (since > 0) nostr_filter_set_since_i64(f, since);
    }
    nostr_filters_add(fs, f);
    *out_filters = fs;
  }

  /* The strings inside arr are owned by arr; transfer pointers to urls, keep them alive by not freeing arr contents separately */
  if (arr) g_ptr_array_free(arr, FALSE); /* do not free contained strings */
}

static void profile_dispatch_next(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  /* Nothing to do? Clean up sequence if finished */
  if (!self->profile_batches || self->profile_batch_pos >= self->profile_batches->len) {
    if (self->profile_batches) {
      g_message("profile_fetch: sequence complete (batches=%u)", self->profile_batches->len);
    } else {
      g_message("profile_fetch: sequence complete (no batches)");
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
      g_free((gpointer)self->profile_batch_urls);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
    return;
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
      g_free((gpointer)self->profile_batch_urls);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
    return;
  }

  /* Take next batch */
  GPtrArray *batch = g_ptr_array_index(self->profile_batches, self->profile_batch_pos);
  self->profile_batch_pos++;
  if (!batch || batch->len == 0) {
    if (batch) g_ptr_array_free(batch, TRUE);
    /* Continue to next */
    profile_dispatch_next(self);
    return;
  }

  /* Prepare authors array (borrow strings) */
  size_t n = batch->len;
  const char **authors = g_new0(const char*, n);
  for (guint i = 0; i < n; i++) authors[i] = (const char*)g_ptr_array_index(batch, i);

  ProfileBatchCtx *ctx = g_new0(ProfileBatchCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->batch = batch; /* ownership transferred; freed in callback */

  int limit_per_author = 0; /* no limit; filter limit is total, not per author */
  g_message("profile_fetch: dispatching batch %u/%u (authors=%zu)",
            self->profile_batch_pos, self->profile_batches ? self->profile_batches->len : 0, n);
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
}

static gboolean periodic_backfill_cb(gpointer data) { (void)data; return G_SOURCE_CONTINUE; }

static void start_pool_live(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->pool) self->pool = gnostr_simple_pool_new();
  if (!self->pool_cancellable) self->pool_cancellable = g_cancellable_new();

  /* Build live URLs and filters: text notes (kind 1), optional limit/since */
  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;
  build_urls_and_filters(self, &urls, &url_count, &filters, (int)self->default_limit);
  if (!urls || url_count == 0 || !filters) {
    g_message("start_pool_live: no relay URLs or filters; skipping live start");
    if (filters) nostr_filters_free(filters);
    if (urls) { g_free((gpointer)urls); }
    return;
  }

  /* Hook up events signal exactly once */
  if (self->pool_events_handler == 0) {
    self->pool_events_handler = g_signal_connect(self->pool, "events", G_CALLBACK(on_pool_events), self);
  }

  g_message("start_pool_live: subscribing to %zu relay(s) for kind=1", url_count);
  gnostr_simple_pool_subscribe_many_async(self->pool, urls, url_count, filters, self->pool_cancellable, on_pool_subscribe_done, g_object_ref(self));

  /* Caller owns arrays and filters after async setup */
  nostr_filters_free(filters);
  g_free((gpointer)urls);
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
    if (urls) g_free((gpointer)urls);
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
  g_free((gpointer)urls);
}

/* Live stream setup completion */
static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GError *error = NULL;
  gboolean ok = gnostr_simple_pool_subscribe_many_finish(GNOSTR_SIMPLE_POOL(source), res, &error);
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!ok) {
    g_warning("live: subscribe_many finish error: %s", error ? error->message : "(unknown)");
  } else {
    g_message("live: subscribe_many started");
  }
  if (error) g_clear_error(&error);
  if (self) g_object_unref(self);
}

/* Main handler for live batches: append notes to timeline and queue profile fetches */
static void on_pool_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !batch) return;

  guint appended = 0, enqueued_profiles = 0;
  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *evt = (NostrEvent*)batch->pdata[i];
    if (!evt) continue;
    int kind = nostr_event_get_kind(evt);
    if (kind != 1) continue;
    const char *id = nostr_event_get_id(evt);
    if (!id || strlen(id) != 64) continue;
    /* Dedup on id */
    if (!self->seen_ids) self->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (g_hash_table_contains(self->seen_ids, id)) {
      continue;
    }
    g_hash_table_insert(self->seen_ids, g_strdup(id), GINT_TO_POINTER(1));
    append_note_from_event(self, evt);
    appended++;
    const char *pk = nostr_event_get_pubkey(evt);
    if (pk && strlen(pk) == 64) { enqueue_profile_author(self, pk); enqueued_profiles++; }
  }
  if (appended > 0) {
    g_message("live: appended %u new note(s); enqueued %u profile(s)", appended, enqueued_profiles);
    /* Also sweep local cache (debounced) to apply any already-cached profiles */
    schedule_ndb_profile_sweep(self);
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
  if (enq > 0) g_message("bg-prefetch: enqueued %u profile(s)", enq);
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

/* Sweep current timeline items, query NostrDB for profiles by pubkey, and apply them */
static void apply_profiles_for_current_items_from_ndb(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->thread_roots) return;
  GListModel *model = G_LIST_MODEL(self->thread_roots);
  guint n = g_list_model_get_n_items(model);
  if (n == 0) { g_debug("ndb_profile_sweep: 0 items; skip"); return; }
  /* collect unique pubkeys */
  GHashTable *uniq = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n; i++) {
    GObject *item = g_list_model_get_item(model, i);
    if (!item) continue;
    gchar *pk = NULL; g_object_get(item, "pubkey", &pk, NULL);
    if (pk && strlen(pk) == 64) {
      if (!g_hash_table_contains(uniq, pk)) g_hash_table_insert(uniq, g_strdup(pk), GINT_TO_POINTER(1));
    }
    if (pk) g_free(pk);
    g_object_unref(item);
  }
  guint count = g_hash_table_size(uniq);
  if (count == 0) { g_hash_table_unref(uniq); g_debug("ndb_profile_sweep: no pubkeys"); return; }
  void *txn = NULL; int brc = storage_ndb_begin_query(&txn);
  if (brc != 0) { g_warning("ndb_profile_sweep: begin_query rc=%d", brc); g_hash_table_unref(uniq); return; }
  GHashTableIter it; gpointer key, val; guint applied = 0, present = 0;
  g_hash_table_iter_init(&it, uniq);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const char *pkhex = (const char*)key; (void)val;
    uint8_t pk32[32]; if (!hex_to_bytes32(pkhex, pk32)) continue;
    char *pjson = NULL; int plen = 0;
    int prc = storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen);
    if (prc == 0 && pjson && plen > 0) {
      /* Debug: verify length vs strnlen and NUL termination */
      size_t slen = strnlen(pjson, (size_t)plen + 1);
      char lastc = '\0';
      if ((size_t)plen < slen) {
        /* unexpected: strnlen found longer than reported plen; clamp */
        slen = (size_t)plen;
      }
      if ((size_t)plen <= slen) {
        lastc = pjson[plen];
      }
      g_message("ndb_profile_sweep: json buf len=%d strnlen=%zu nul_at_plen=%s last_byte=0x%02x", plen, slen,
                (lastc == '\0' ? "yes" : "no"), (unsigned char)lastc);
      /* Jansson expects exact JSON length; exclude trailing NUL if present */
      size_t eff_len = slen; /* strnlen(pjson, plen) computed above */
      json_error_t jerr;
      json_t *root = json_loadb(pjson, eff_len, 0, &jerr);
      if (root) {
        json_t *content_json = json_object_get(root, "content");
        if (content_json && json_is_string(content_json)) {
          const char *content = json_string_value(content_json);
          update_meta_from_profile_json(self, pkhex, content);
          applied++;
        } else {
          g_debug("ndb_profile_sweep: profile event missing content for %.*s…", 8, pkhex);
        }
      } else {
        /* Emit head/tail snippets to detect truncation */
        int head_len = plen < 120 ? plen : 120;
        int tail_len = plen < 120 ? plen : 120;
        char head[121]; memcpy(head, pjson, head_len); head[head_len] = '\0';
        const char *tail_src = pjson + (eff_len > 120 ? (eff_len - 120) : 0);
        char tail[121]; memcpy(tail, tail_src, tail_len); tail[tail_len] = '\0';
        g_debug("ndb_profile_sweep: invalid event JSON for %.*s… err=%s plen=%d eff_len=%zu head='%s'%s tail='%s'",
                8, pkhex, jerr.text, plen, eff_len, head, (eff_len > 120 ? "…" : ""), tail);
      }
      if (root) json_decref(root);
      free(pjson);
    }
  }
  storage_ndb_end_query(txn);
  g_hash_table_unref(uniq);
  g_message("ndb_profile_sweep: items=%u unique_pubkeys=%u profiles_found=%u applied_calls=%u", n, count, present, applied);
}

/* Debounced schedule: coalesce multiple triggers into one sweep */
static gboolean ndb_sweep_timeout_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;
  /* clear id before running to allow re-scheduling after */
  self->ndb_sweep_source_id = 0;
  apply_profiles_for_current_items_from_ndb(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void schedule_ndb_profile_sweep(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (self->ndb_sweep_source_id) return; /* already scheduled */
  guint delay = self->ndb_sweep_debounce_ms ? self->ndb_sweep_debounce_ms : 150;
  /* take a strong ref; timeout will unref */
  GnostrMainWindow *ref = g_object_ref(self);
  self->ndb_sweep_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, delay, ndb_sweep_timeout_cb, ref, NULL);
  g_message("ndb_profile_sweep: scheduled in %ums", delay);
}

/* Parses content_json minimally and stores by pubkey in meta cache */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !content_json) return;
  if (!self->meta_by_pubkey) return;
  /* Store a copy of the JSON as the value. Replace if existing */
  gpointer old = g_hash_table_lookup(self->meta_by_pubkey, pubkey_hex);
  if (old) {
    /* replace */
    g_hash_table_replace(self->meta_by_pubkey, g_strdup(pubkey_hex), g_strdup(content_json));
  } else {
    g_hash_table_insert(self->meta_by_pubkey, g_strdup(pubkey_hex), g_strdup(content_json));
  }
  /* Parse profile JSON and update any existing timeline items with this pubkey */
  const char *display = NULL, *handle = NULL, *picture = NULL;
  json_error_t jerr;
  json_t *root = json_loads(content_json, 0, &jerr);
  if (root && json_is_object(root)) {
    json_t *jdisplay = json_object_get(root, "display_name");
    json_t *jhandle  = json_object_get(root, "name");
    json_t *jpic     = json_object_get(root, "picture");
    if (json_is_string(jdisplay)) display = json_string_value(jdisplay);
    if (json_is_string(jhandle))  handle  = json_string_value(jhandle);
    if (json_is_string(jpic))     picture = json_string_value(jpic);
  }

  guint updated = 0;
  if (self->thread_roots) {
    GListModel *model = G_LIST_MODEL(self->thread_roots);
    guint n = g_list_model_get_n_items(model);
    g_debug("profile_apply: scanning %u timeline item(s) for pubkey %.*s…", n, 8, pubkey_hex);
    for (guint i = 0; i < n; i++) {
      GObject *item = g_list_model_get_item(model, i);
      if (!item) continue;
      gchar *pk = NULL;
      g_object_get(item, "pubkey", &pk, NULL);
      g_debug("profile_apply: item[%u] pubkey=%s", i, pk ? pk : "(null)");
      if (pk && g_ascii_strcasecmp(pk, pubkey_hex) == 0) {
        /* Apply properties; prefix handle with @ if missing */
        const char *eff_handle = handle;
        g_autofree char *prefixed = NULL;
        if (eff_handle && *eff_handle) {
          if (eff_handle[0] == '@') {
            prefixed = g_strdup(eff_handle);
          } else {
            prefixed = g_strdup_printf("@%s", eff_handle);
          }
        }
        if (display && *display)
          g_object_set(item, "display-name", display, NULL);
        if (prefixed)
          g_object_set(item, "handle", prefixed, NULL);
        if (picture && *picture)
          g_object_set(item, "avatar-url", picture, NULL);
        updated++;
      }
      if (pk) g_free(pk);
      g_object_unref(item);
    }
  }
  else {
    g_debug("profile_apply: thread_roots is NULL; cannot scan timeline items");
  }
  g_message("profile_apply: pubkey=%.*s… updated_items=%u display=%s handle=%s avatar=%s",
            8, pubkey_hex,
            updated,
            display && *display ? display : "",
            handle && *handle ? handle : "",
            picture && *picture ? picture : "");
  if (root) json_decref(root);
}
