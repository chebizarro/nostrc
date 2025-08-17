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
/* NIP-19 helpers */
#include "../../../nips/nip19/include/nip19.h"
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
static void show_toast(GnostrMainWindow *self, const char *msg);
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json);
static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data);

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
  
  /* Profile subscription */
  gulong profile_sub_id;        /* signal handler ID for profile events */
  GCancellable *profile_sub_cancellable; /* cancellable for profile sub */
  
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
};

/* Forward declaration for async dialog completion */
static void relays_alert_done(GtkAlertDialog *d, GAsyncResult *res, gpointer data);
static void on_relays_save(GtkButton *b, gpointer user_data);

/* Relay Manager dialog context */
typedef struct {
  GtkWindow *win;
  GtkListView *list;
  GtkSelectionModel *sel;
  GtkStringList *model;
  GtkEntry *entry;
  GtkButton *btn_add;
  GtkButton *btn_remove;
  GtkButton *btn_save;
  gboolean dirty;
  GPtrArray *orig; /* owned; snapshot of sorted initial relays */
  GtkLabel *status; /* optional status label */
} RelaysCtx;

/* Pairing dialog context */
typedef struct {
  GnostrMainWindow *self; /* strong ref */
  GtkWindow *win;         /* dialog window */
  GtkEntry *entry_uri;    /* uri input */
  GtkButton *btn_connect; /* connect button */
  GtkLabel *lbl_status;   /* status label */
} PairCtx;

/* Save window size to GSettings if schema is present */
static void relays_save_window_size(GtkWindow *win) {
  if (!win) return;
  int w = 0, h = 0;
  gtk_window_get_default_size(win, &w, &h);
  if (w <= 0 || h <= 0) return;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return;
  GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
  if (!schema) return;
  GSettings *settings = g_settings_new("org.gnostr.gnostr");
  g_settings_set_int(settings, "relays-window-width", w);
  g_settings_set_int(settings, "relays-window-height", h);
  g_object_unref(settings);
  g_settings_schema_unref(schema);
}

/* ---- Profile subscription management ---- */

/* Cancel any existing profile subscription */
static void cancel_profile_subscription(GnostrMainWindow *self) {
  if (!self->pool || !self->profile_sub_id) return;
  
  g_cancellable_cancel(self->profile_sub_cancellable);
  g_clear_object(&self->profile_sub_cancellable);
  self->profile_sub_id = 0;
}

/* Handle incoming profile events from persistent subscription */
static void on_profile_event(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  
  for (guint i = 0; i < batch->len; i++) {
    const char *json = g_ptr_array_index(batch, i);
    if (!json) continue;

    NostrEvent *evt = nostr_event_new();
    if (!evt) continue;
    if (nostr_event_deserialize(evt, json) != 0) { nostr_event_free(evt); continue; }

    if (nostr_event_get_kind(evt) == 0) {  // kind-0: profile metadata
      const char *pk = nostr_event_get_pubkey(evt);
      const char *content = nostr_event_get_content(evt);
      if (pk && content) {
        update_meta_from_profile_json(self, pk, content);
      }
    }

    nostr_event_free(evt);
  }
}

/* Start a persistent subscription for profile updates */
static void start_profile_subscription(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->pool) return;
  
  // Cancel any existing subscription first
  cancel_profile_subscription(self);
  
  // Get current identity
  g_autofree char *npub = client_settings_get_current_npub();
  if (!npub || !*npub) return;
  
  uint8_t pub32[32] = {0};
  if (nostr_nip19_decode_npub(npub, pub32) != 0) return;
  
  g_autofree char *pub_hex = hex_encode_lower(pub32, 32);
  if (!pub_hex || strlen(pub_hex) != 64) return;
  
  // Create filters set for kind-0 events from our pubkey
  NostrFilters *filters = nostr_filters_new();
  if (!filters) return;
  NostrFilter *filter = nostr_filter_new();
  if (!filter) { nostr_filters_free(filters); return; }
  nostr_filter_add_kind(filter, 0);
  nostr_filter_add_author(filter, pub_hex);
  nostr_filters_add(filters, filter);
  /* filter shell can be freed after adding; internals are retained by filters */
  nostr_filter_free(filter);
  
  // Get relay URLs
  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relays);
  if (relays->len == 0) {
    g_ptr_array_free(relays, TRUE);
    nostr_filter_free(filter);
    return;
  }
  
  // Convert to const char** array
  const char **urls = g_new0(const char*, relays->len + 1);
  for (guint i = 0; i < relays->len; i++) {
    urls[i] = g_strdup(g_ptr_array_index(relays, i));
  }
  
  // Create new cancellable for this subscription
  self->profile_sub_cancellable = g_cancellable_new();
  
  // Connect to pool events for this subscription
  self->profile_sub_id = g_signal_connect_data(
    self->pool, "events", G_CALLBACK(on_profile_event), g_object_ref(self),
    (GClosureNotify)g_object_unref, G_CONNECT_AFTER);
  
  // Start the subscription
  gnostr_simple_pool_subscribe_many_async(
    self->pool,
    urls,
    relays->len,
    filters,
    self->profile_sub_cancellable,
    on_pool_subscribe_done,
    g_object_ref(self)
  );
  
  // Cleanup
  for (guint i = 0; i < relays->len; i++) {
    g_free((char*)urls[i]);
  }
  g_free(urls);
  g_ptr_array_free(relays, TRUE);
  // Note: filters are kept alive by the subscription
}

/* ---- Profile fetch (single-shot query for kind-0 of current identity) ---- */
static void gnostr_profile_fetch_async(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  /* Read current identity */
  g_autofree char *npub = client_settings_get_current_npub();
  if (!npub || !*npub) { g_message("profile_fetch: no current npub set"); return; }
  uint8_t pub32[32] = {0};
  if (nostr_nip19_decode_npub(npub, pub32) != 0) {
    g_warning("profile_fetch: failed to decode npub");
    return;
  }
  g_autofree char *pub_hex = hex_encode_lower(pub32, 32);
  if (!pub_hex || strlen(pub_hex) != 64) { g_warning("profile_fetch: bad hex"); return; }

  /* Build filter: kind=0, authors=[pub_hex], limit=1 */
  NostrFilters *filters = nostr_filters_new();
  if (!filters) return;
  NostrFilter *f = nostr_filter_new();
  if (!f) { nostr_filters_free(filters); return; }
  nostr_filter_add_kind(f, 0);
  nostr_filter_add_author(f, pub_hex);
  nostr_filter_set_limit(f, 1);
  (void)nostr_filters_add(filters, f);
  /* f contents moved; free the shell safely */
  nostr_filter_free(f);

  /* Collect relay URLs */
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(arr);
  if (arr->len == 0) {
    g_ptr_array_free(arr, TRUE);
    nostr_filters_free(filters);
    g_message("profile_fetch: no relays configured");
    return;
  }
  size_t url_count = arr->len;
  const char **urls = g_new0(const char*, url_count);
  for (guint i = 0; i < url_count; i++) urls[i] = g_strdup((const char*)g_ptr_array_index(arr, i));
  g_ptr_array_free(arr, TRUE);

  /* Ensure pool/cancellable and events handler */
  if (!self->pool) self->pool = gnostr_simple_pool_new();
  if (!self->pool_cancellable) self->pool_cancellable = g_cancellable_new();
  if (!self->pool_events_handler) {
    self->pool_events_handler = g_signal_connect(self->pool, "events", G_CALLBACK(on_profile_event), self);
  }

  show_toast(self, "Fetching profile…");
  gnostr_simple_pool_subscribe_many_async(self->pool, urls, url_count, filters, self->pool_cancellable, on_pool_subscribe_done, self);
  /* Free duplicated url strings and array; filters kept alive for subscription lifetime */
  for (size_t i = 0; i < url_count; i++) g_free((char*)urls[i]);
  g_free((void*)urls);
}

static gboolean relays_on_close_request(GtkWindow *win, gpointer user_data) {
  (void)user_data;
  relays_save_window_size(win);
  RelaysCtx *ctx = (RelaysCtx*)g_object_get_data(G_OBJECT(win), "relays-ctx");
  if (!ctx || !ctx->dirty) return FALSE;
#if GTK_CHECK_VERSION(4,10,0)
  GtkAlertDialog *dlg = gtk_alert_dialog_new("You have unsaved changes.");
  gtk_alert_dialog_set_detail(dlg, "Do you want to save your relay list before closing?");
  const char *btns[] = { "_Cancel", "_Discard", "_Save", NULL };
  gtk_alert_dialog_set_buttons(dlg, btns);
  gtk_alert_dialog_choose(dlg, GTK_WINDOW(win), NULL,
    (GAsyncReadyCallback)relays_alert_done, win);
  return TRUE; /* stop default close, we'll decide in callback */
#else
  /* Without GtkAlertDialog, just block close if dirty */
  return TRUE;
#endif
}

static void relays_alert_done(GtkAlertDialog *d, GAsyncResult *res, gpointer data) {
  GtkWindow *w = GTK_WINDOW(data);
  GError *err = NULL;
  int idx = gtk_alert_dialog_choose_finish(d, res, &err);
  RelaysCtx *c = (RelaysCtx*)g_object_get_data(G_OBJECT(w), "relays-ctx");
  if (err) { g_error_free(err); g_object_unref(d); return; }
  if (!c) { g_object_unref(d); return; }
  if (idx == 2) { /* Save */
    on_relays_save(NULL, c);
  } else if (idx == 1) { /* Discard */
    gtk_window_destroy(w);
  } else {
    /* Cancel: do nothing */
  }
  g_object_unref(d);
}

/* Forward declarations needed early */
typedef struct {
  GnostrMainWindow *self; /* strong ref */
  GPtrArray *lines;       /* transfer full */
} IdleApplyCtx;

static gboolean apply_timeline_lines_idle(gpointer user_data);
/* Forward: periodic backfill */
static gboolean periodic_backfill_cb(gpointer data);
/* NostrdB helpers */
static char *build_backfill_filters_json(GnostrMainWindow *self, int limit);
static void ndb_backfill_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable);
static void start_pool_backfill(GnostrMainWindow *self, int limit);
/* Lightweight UI event payload captured from NostrEvent (needed early) */
typedef struct UiEventRow {
  char *id;
  char *parent_id; /* from 'e' tag if present */
  char *pubkey;
  int64_t created_at;
  char *content;
} UiEventRow;
/* Helpers used by backfill worker */
static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */);
static void ui_event_row_free(gpointer p);
static void start_pool_live_idle_cb(gpointer data);
static void start_pool_backfill_idle_cb(gpointer data);
/* Settings persistence */
static void gnostr_load_settings(GnostrMainWindow *self);
/* Profile fetch */
static void gnostr_profile_fetch_async(GnostrMainWindow *self);

/* Client settings helpers */
static void client_settings_set_current_npub(const char *npub) {
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return;
  GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.Client", TRUE);
  if (!schema) return;
  GSettings *settings = g_settings_new("org.gnostr.Client");
  g_settings_set_string(settings, "current-npub", npub ? npub : "");
  g_object_unref(settings);
  g_settings_schema_unref(schema);
}

static char *client_settings_get_current_npub(void) {
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.Client", TRUE);
  if (!schema) return NULL;
  GSettings *settings = g_settings_new("org.gnostr.Client");
  gchar *s = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);
  g_settings_schema_unref(schema);
  if (s && *s) return s;
  g_clear_pointer(&s, g_free);
  return NULL;
}

/* Small helper: hex-encode bytes to lowercase hex string */
static char *hex_encode_lower(const uint8_t *buf, size_t len) {
  static const char *hex = "0123456789abcdef";
  if (!buf || len == 0) return g_strdup("");
  char *out = g_malloc0(len * 2 + 1);
  for (size_t i = 0; i < len; i++) { out[2*i] = hex[(buf[i] >> 4) & 0xF]; out[2*i+1] = hex[buf[i] & 0xF]; }
  return out;
}
static void gnostr_save_settings(GnostrMainWindow *self);
static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit);
/* Relay list item factory */
static void relay_item_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer user_data);

/* ---- NostrdB helpers ---- */

typedef struct {
  GnostrMainWindow *self; /* strong ref */
  int limit;
} NdbBackfillCtx;

static void ndb_backfill_ctx_free(gpointer p) {
  NdbBackfillCtx *d = (NdbBackfillCtx*)p;
  if (!d) return;
  if (d->self) g_object_unref(d->self);
  g_free(d);
}

static void ndb_backfill_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  (void)source; (void)cancellable;
  NdbBackfillCtx *d = (NdbBackfillCtx*)task_data;
  if (!d || !GNOSTR_IS_MAIN_WINDOW(d->self)) { g_task_return_boolean(task, FALSE); return; }
  char *filters_json = build_backfill_filters_json(d->self, d->limit);
  if (!filters_json) { g_task_return_boolean(task, FALSE); return; }
  void *txn = NULL; char **arr = NULL; int n = 0;
  if (storage_ndb_begin_query(&txn) != 1) { free(filters_json); g_task_return_boolean(task, FALSE); return; }
  int rc = storage_ndb_query(txn, filters_json, &arr, &n);
  free(filters_json);
  if (rc != 0) {
    storage_ndb_end_query(txn);
    g_task_return_boolean(task, FALSE);
    return;
  }
  /* Build UiEventRow list by deserializing events */
  GPtrArray *rows = g_ptr_array_new_with_free_func(ui_event_row_free);
  for (int i = 0; i < n; i++) {
    const char *js = arr[i]; if (!js) continue;
    NostrEvent *evt = nostr_event_new();
    if (!evt) continue;
    if (nostr_event_deserialize(evt, js) != 0) { nostr_event_free(evt); continue; }
    UiEventRow *r = g_new0(UiEventRow, 1);
    /* id */
    const char *id_hex = evt->id; /* struct is public */
    if (id_hex && *id_hex) r->id = g_strdup(id_hex);
    /* pubkey */
    const char *pk = nostr_event_get_pubkey(evt);
    if (pk) r->pubkey = g_strdup(pk);
    r->created_at = nostr_event_get_created_at(evt);
    /* content & metadata cache for profiles */
    const char *content = nostr_event_get_content(evt);
    if (content && *content) r->content = g_utf8_make_valid(content, -1);
    int kind = nostr_event_get_kind(evt);
    if (kind == 0 && pk && content) {
      update_meta_from_profile_json(d->self, pk, content);
    }
    /* parent from last 'e' tag if present */
    NostrTags *tags = (NostrTags*)nostr_event_get_tags(evt);
    if (tags) {
      for (ssize_t j = (ssize_t)nostr_tags_size(tags) - 1; j >= 0; j--) {
        NostrTag *t = nostr_tags_get(tags, (size_t)j);
        const char *k = nostr_tag_get_key(t);
        if (k && g_strcmp0(k, "e") == 0) {
          const char *val = nostr_tag_get_value(t);
          if (val && *val) { r->parent_id = g_strndup(val, 64); break; }
        }
      }
    }
    g_ptr_array_add(rows, r);
    nostr_event_free(evt);
  }
  storage_ndb_free_results(arr, n);
  storage_ndb_end_query(txn);
  /* Schedule apply on main loop */
  schedule_apply_events(d->self, rows);
  g_task_return_boolean(task, TRUE);
}

/* Start a backfill using NostrdB (replaces relay backfill). */
static void start_pool_backfill(GnostrMainWindow *self, int limit) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  GTask *t = g_task_new(self, NULL, NULL, NULL);
  NdbBackfillCtx *ctx = g_new0(NdbBackfillCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->limit = limit;
  g_task_set_task_data(t, ctx, ndb_backfill_ctx_free);
  g_task_run_in_thread(t, ndb_backfill_worker);
  g_object_unref(t);
}
static void relay_item_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer user_data);

G_DEFINE_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

/* Build a JSON array string of filters equivalent to build_urls_and_filters() output. */
static char *build_backfill_filters_json(GnostrMainWindow *self, int limit) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return NULL;
  int lim = limit > 0 ? limit : (int)self->default_limit;
  json_t *arr = json_array();
  if (!arr) return NULL;
  /* f1: notes (kind=1) with optional since */
  {
    json_t *f1 = json_object();
    json_t *kinds = json_array(); json_array_append_new(kinds, json_integer(1));
    json_object_set_new(f1, "kinds", kinds);
    json_object_set_new(f1, "limit", json_integer(lim));
    if (self->use_since) {
      time_t now = time(NULL);
      int64_t since = (int64_t)now - (int64_t)self->since_seconds;
      if (since < 0) since = 0;
      json_object_set_new(f1, "since", json_integer((json_int_t)since));
    }
    json_array_append_new(arr, f1);
  }
  /* f0: profiles (kind=0) no since */
  {
    json_t *f0 = json_object();
    json_t *kinds = json_array(); json_array_append_new(kinds, json_integer(0));
    json_object_set_new(f0, "kinds", kinds);
    int lim0 = lim > 0 ? lim : 100; if (lim0 <= 0) lim0 = 100;
    json_object_set_new(f0, "limit", json_integer(lim0));
    json_array_append_new(arr, f0);
  }
  char *s = json_dumps(arr, JSON_COMPACT);
  json_decref(arr);
  return s;
}

static void gnostr_main_window_dispose(GObject *obj) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(obj);
  /* Early teardown: stop timers, cancel async, disconnect signals */
  if (self->backfill_source_id) {
    g_source_remove(self->backfill_source_id);
    self->backfill_source_id = 0;
  }
  if (self->pool) {
    if (self->pool_events_handler) {
      g_signal_handler_disconnect(self->pool, self->pool_events_handler);
      self->pool_events_handler = 0;
    }
  }
  if (self->pool_cancellable) {
    g_cancellable_cancel(self->pool_cancellable);
  }
  if (self->nodes_by_id) {
    g_hash_table_destroy(self->nodes_by_id);
    self->nodes_by_id = NULL;
  }
  if (self->meta_by_pubkey) {
    g_hash_table_destroy(self->meta_by_pubkey);
    self->meta_by_pubkey = NULL;
  }
  if (self->thread_roots) {
    g_object_unref(self->thread_roots);
    self->thread_roots = NULL;
  }
  /* Free nip46 session early */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->dispose(obj);
}

/* Factory callbacks for relay list */
static void relay_item_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer user_data) {
  (void)f; (void)user_data;
  GtkWidget *lbl = gtk_label_new("");
  gtk_list_item_set_child(item, lbl);
}

static void relay_item_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer user_data) {
  (void)f;
  GtkStringList *model = GTK_STRING_LIST(user_data);
  GtkWidget *lbl = gtk_list_item_get_child(item);
  guint pos = gtk_list_item_get_position(item);
  const char *s = gtk_string_list_get_string(model, pos);
  gtk_label_set_text(GTK_LABEL(lbl), s ? s : "");
}

/* Helper: schedule a batch of lines to be applied on the main loop. Transfers ownership of 'lines'. */
static void schedule_apply_lines(GnostrMainWindow *self, GPtrArray *lines) {
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self) || !lines) return;
  IdleApplyCtx *c = g_new0(IdleApplyCtx, 1);
  c->self = g_object_ref(self);
  c->lines = lines; /* transfer */
  g_message("timeline_refresh_worker: scheduling main-loop apply (lines=%u)", lines->len);
  if (g_getenv("GNOSTR_USE_IDLE")) {
    g_idle_add_full(G_PRIORITY_DEFAULT, (GSourceFunc)apply_timeline_lines_idle, c, NULL);
  } else {
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, (GSourceFunc)apply_timeline_lines_idle, c, NULL);
  }
}

/* Helper: post batch if size or time thresholds are reached. Resets batch on post. */
static void maybe_post_batch(GnostrMainWindow *self,
                             GPtrArray **pbatch,
                             gint64 *plast_post_us,
                             const gint64 interval_us,
                             const guint batch_max) {
  if (!self || !pbatch || !*pbatch || !plast_post_us) return;
  GPtrArray *batch = *pbatch;
  const gint64 now_us = g_get_monotonic_time();
  if (batch->len == 0) return;
  if (batch->len >= batch_max || now_us - *plast_post_us >= interval_us) {
    schedule_apply_lines(self, batch);
    *pbatch = g_ptr_array_new_with_free_func(g_free);
    *plast_post_us = now_us;
  }
}

static void gnostr_main_window_finalize(GObject *obj) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(obj);
  /* Objects should already be quiesced in dispose; free leftovers */
  if (self->pool_cancellable) {
    g_clear_object(&self->pool_cancellable);
  }
  if (self->live_filters) {
    nostr_filters_free(self->live_filters);
    self->live_filters = NULL;
  }
  if (self->pool) {
    g_clear_object(&self->pool);
  }
  if (self->seen_texts) {
    g_hash_table_destroy(self->seen_texts);
    self->seen_texts = NULL;
  }
  if (self->seen_ids) {
    g_hash_table_destroy(self->seen_ids);
    self->seen_ids = NULL;
  }
  if (self->nip46_session) {
    /* Should be NULL from dispose, but guard and free */
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }
  g_weak_ref_clear(&self->timeline_ref);
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_MAIN_WINDOW);
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->finalize(obj);
}

/* Build URLs and Filters based on env/settings. Caller must g_free(out_urls) array pointer, but not the strings. */
static void build_urls_and_filters(GnostrMainWindow *self, const char ***out_urls, size_t *out_count, NostrFilters **out_filters, int limit) {
  g_return_if_fail(out_urls && out_count && out_filters);
  const char *env_relays = g_getenv("GNOSTR_RELAYS");
  static const char *defaults[] = { "wss://relay.damus.io", "wss://nos.lol" };
  /* Prefer relays from config if present */
  gboolean used_config_relays = FALSE;
  {
    gchar *cfg = gnostr_config_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, &err)) {
      gsize n = 0;
      gchar **list = g_key_file_get_string_list(kf, "relays", "urls", &n, NULL);
      if (list && n > 0) {
        *out_urls = g_new0(const char*, n);
        *out_count = n;
        for (gsize i = 0; i < n; i++) {
          const char *s = list[i] ? list[i] : "";
          if (*s) ((char**)*out_urls)[i] = g_strdup(s);
          else ((char**)*out_urls)[i] = g_strdup("");
        }
        used_config_relays = TRUE;
      }
      if (list) g_strfreev(list);
    } else {
      g_clear_error(&err);
    }
    g_key_file_unref(kf);
    g_free(cfg);
  }
  if (!used_config_relays) {
  if (env_relays && *env_relays) {
    /* Split and trim each entry; skip empties */
    gchar **list = g_strsplit(env_relays, ",", -1);
    GPtrArray *clean = g_ptr_array_new_with_free_func(g_free);
    for (gchar **p = list; p && *p; ++p) {
      gchar *item = g_strdup(*p ? *p : "");
      if (item) {
        gchar *trimmed = g_strstrip(item); /* in-place trim */
        if (trimmed && *trimmed) {
          /* normalize: if starts with "ws://" or "wss://" keep; else accept as-is */
          g_ptr_array_add(clean, g_strdup(trimmed));
        }
      }
      g_free(item);
    }
    g_strfreev(list);
    if (clean->len == 0) {
      g_ptr_array_free(clean, TRUE);
      *out_urls = g_new0(const char*, G_N_ELEMENTS(defaults));
      *out_count = G_N_ELEMENTS(defaults);
      for (size_t i = 0; i < *out_count; i++) ((char**)*out_urls)[i] = g_strdup(defaults[i]);
    } else {
      *out_urls = g_new0(const char*, clean->len);
      *out_count = clean->len;
      for (guint i = 0; i < clean->len; i++) {
        ((char**)*out_urls)[i] = g_strdup((const char*)g_ptr_array_index(clean, i));
      }
      g_ptr_array_free(clean, TRUE);
    }
  } else {
    *out_urls = g_new0(const char*, G_N_ELEMENTS(defaults));
    *out_count = G_N_ELEMENTS(defaults);
    for (size_t i = 0; i < *out_count; i++) ((char**)*out_urls)[i] = g_strdup(defaults[i]);
  }
  }

  /* Build two filters:
   *  - f1: notes (kind=1) with limit and optional since
   *  - f0: profiles (kind=0) with limit but NO since (profiles are often older and would be filtered out)
   */
  int lim = limit > 0 ? limit : (int)self->default_limit;
  NostrFilters *fs = nostr_filters_new();

  /* f1: notes */
  {
    NostrFilter *f1 = nostr_filter_new();
    int kinds1[] = { 1 };
    nostr_filter_set_kinds(f1, kinds1, 1);
    nostr_filter_set_limit(f1, lim);
    if (self->use_since) {
      time_t now = time(NULL);
      int64_t since = (int64_t)now - (int64_t)self->since_seconds;
      if (since < 0) since = 0;
      nostr_filter_set_since_i64(f1, since);
      g_message("build_urls_and_filters: notes kinds=[1] limit=%d since=%lld (use_since=1)", lim, (long long)since);
    } else {
      g_message("build_urls_and_filters: notes kinds=[1] limit=%d (use_since=0)", lim);
    }
    nostr_filters_add(fs, f1);
  }

  /* f0: profiles (no since) */
  {
    NostrFilter *f0 = nostr_filter_new();
    int kinds0[] = { 0 };
    nostr_filter_set_kinds(f0, kinds0, 1);
    /* Keep profile fetch lightweight */
    int lim0 = lim > 0 ? lim : 100;
    if (lim0 <= 0) lim0 = 100;
    nostr_filter_set_limit(f0, lim0);
    g_message("build_urls_and_filters: profiles kinds=[0] limit=%d (no since)", lim0);
    nostr_filters_add(fs, f0);
  }
  *out_filters = fs;
}


static void ui_event_row_free(gpointer p) {
  UiEventRow *r = (UiEventRow*)p;
  if (!r) return;
  g_free(r->id);
  g_free(r->parent_id);
  g_free(r->pubkey);
  g_free(r->content);
  g_free(r);
}

/* -------- User metadata cache (pubkey -> display/handle/avatar) -------- */
typedef struct {
  char *display;
  char *handle;
  char *avatar_url;
} UserMeta;

static void user_meta_free(gpointer p) {
  UserMeta *m = (UserMeta*)p;
  if (!m) return;
  g_free(m->display);
  g_free(m->handle);
  g_free(m->avatar_url);
  g_free(m);
}

/* hex helpers */
static int hex_nibble(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return 10+(c-'a'); if(c>='A'&&c<='F')return 10+(c-'A'); return -1; }
static int hex_to_bytes_exact(const char *hex, unsigned char *out, size_t outlen){ if(!hex||!out) return -1; size_t n=strlen(hex); if(n!=outlen*2) return -1; for(size_t i=0;i<outlen;i++){ int h=hex_nibble(hex[2*i]); int l=hex_nibble(hex[2*i+1]); if(h<0||l<0) return -1; out[i]=(unsigned char)((h<<4)|l);} return 0; }

static char *short_bech(const char *bech, guint front, guint back) {
  if (!bech) return g_strdup("");
  size_t n = strlen(bech);
  if (n <= front + back + 3) return g_strdup(bech);
  GString *s = g_string_new(NULL);
  g_string_append_len(s, bech, front);
  g_string_append(s, "…");
  g_string_append_len(s, bech + (n - back), back);
  return g_string_free(s, FALSE);
}

/* Derive a reasonable handle from pubkey (npub short) if no profile */
static char *derive_handle_from_pubkey_hex(const char *pubkey_hex) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return g_strdup("@anon");
  /* Use a short hex moniker: @<first8>…<last6> */
  char first8[9] = {0}; memcpy(first8, pubkey_hex, 8);
  const char *tail = pubkey_hex + 64 - 6;
  GString *h = g_string_new("@");
  g_string_append(h, first8);
  g_string_append(h, "…");
  g_string_append_len(h, tail, 6);
  return g_string_free(h, FALSE);
}

static UserMeta *ensure_user_meta(GHashTable *ht, const char *pubkey_hex) {
  if (!ht || !pubkey_hex) return NULL;
  UserMeta *m = (UserMeta*)g_hash_table_lookup(ht, pubkey_hex);
  if (!m) {
    m = g_new0(UserMeta, 1);
    /* Defaults */
    m->display = g_strdup("Anonymous");
    m->handle = derive_handle_from_pubkey_hex(pubkey_hex);
    m->avatar_url = g_strdup("");
    g_hash_table_insert(ht, g_strdup(pubkey_hex), m);
  }
  return m;
}

/* Update cache from kind-0 profile content (JSON) */
static void update_meta_from_profile_json(GnostrMainWindow *self, const char *pubkey_hex, const char *content_json) {
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !content_json) return;
  json_error_t jerr; json_t *root = json_loads(content_json, 0, &jerr);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return; }
  const char *name = NULL, *display_name = NULL, *picture = NULL, *username = NULL;
  json_t *v = NULL;
  if ((v = json_object_get(root, "display_name")) && json_is_string(v)) display_name = json_string_value(v);
  if (!display_name && (v = json_object_get(root, "displayName")) && json_is_string(v)) display_name = json_string_value(v);
  if ((v = json_object_get(root, "name")) && json_is_string(v)) name = json_string_value(v);
  if ((v = json_object_get(root, "username")) && json_is_string(v)) username = json_string_value(v);
  if ((v = json_object_get(root, "picture")) && json_is_string(v)) picture = json_string_value(v);
  UserMeta *m = ensure_user_meta(self->meta_by_pubkey, pubkey_hex);
  if (display_name && *display_name) { g_free(m->display); m->display = g_strdup(display_name); }
  const char *handle_src = NULL;
  if (username && *username) handle_src = username; else if (name && *name) handle_src = name;
  if (handle_src && *handle_src) { g_free(m->handle); m->handle = g_strconcat("@", handle_src, NULL); }
  if (picture && *picture) {
    g_free(m->avatar_url);
    m->avatar_url = g_strdup(picture);
  } else {
    /* RoboHash fallback using pubkey hex for deterministic avatar */
    g_free(m->avatar_url);
    m->avatar_url = g_strdup_printf("https://robohash.org/%s.png?size=80x80&set=set1", pubkey_hex);
  }
  json_decref(root);
  /* Refresh any existing TimelineItems authored by this pubkey */
  if (self->nodes_by_id && self->meta_by_pubkey) {
    GHashTableIter it; gpointer key=NULL, val=NULL; g_hash_table_iter_init(&it, self->nodes_by_id);
    while (g_hash_table_iter_next(&it, &key, &val)) {
      gpointer obj = val;
      if (!G_IS_OBJECT(obj)) continue;
      gchar *item_pk = NULL;
      g_object_get(obj, "pubkey", &item_pk, NULL);
      if (item_pk && g_strcmp0(item_pk, pubkey_hex) == 0) {
        const UserMeta *um = (const UserMeta*)g_hash_table_lookup(self->meta_by_pubkey, pubkey_hex);
        if (um) {
          g_object_set(obj,
                       "display-name", um->display ? um->display : "Anonymous",
                       "handle",       um->handle  ? um->handle  : "@anon",
                       "avatar-url",   um->avatar_url ? um->avatar_url : "",
                       NULL);
        }
      }
      g_free(item_pk);
    }
  }
}

typedef struct IdleApplyEventsCtx {
  GnostrMainWindow *self;
  GPtrArray *rows; /* UiEventRow* */
} IdleApplyEventsCtx;

static void idle_apply_events_ctx_free(IdleApplyEventsCtx *c) {
  if (!c) return;
  if (c->rows) g_ptr_array_free(c->rows, TRUE);
  g_free(c);
}

/* Insert or link TimelineItem into thread graph */
static void thread_graph_insert(GnostrMainWindow *self, UiEventRow *r) {
  if (!self || !r) return;
  if (!self->thread_roots) {
    extern GType timeline_item_get_type(void);
    self->thread_roots = g_list_store_new(timeline_item_get_type());
  }
  if (!self->nodes_by_id) self->nodes_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  extern GType timeline_item_get_type(void);
  GType ti = timeline_item_get_type();
  if (!ti) return;
  /* Resolve metadata for author */
  const char *pk = r->pubkey;
  const char *disp = "Anonymous";
  const char *handle = "@anon";
  const char *avatar = "";
  if (pk && *pk) {
    if (!self->meta_by_pubkey)
      self->meta_by_pubkey = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, user_meta_free);
    UserMeta *m = ensure_user_meta(self->meta_by_pubkey, pk);
    if (m) {
      disp = m->display ? m->display : disp;
      handle = m->handle ? m->handle : handle;
      avatar = m->avatar_url ? m->avatar_url : "";
    }
  }
  /* Create TimelineItem for this event with resolved metadata */
  gpointer item = g_object_new(ti,
                               "display-name", disp,
                               "handle", handle,
                               "avatar-url", avatar,
                               "timestamp", "", /* TODO: pretty ts */
                               "content", r->content ? r->content : "",
                               "depth", 0,
                               NULL);
  if (r->id) g_object_set(item, "id", r->id, NULL);
  if (r->pubkey) g_object_set(item, "pubkey", r->pubkey, NULL);
  g_object_set(item, "created-at", (gint64)r->created_at, NULL);

  /* Try to link to parent if known */
  TimelineItem *parent = NULL;
  if (r->parent_id && *r->parent_id) {
    parent = (TimelineItem*)g_hash_table_lookup(self->nodes_by_id, r->parent_id);
  }
  if (parent) {
    /* depth = parent.depth + 1; root-id = parent.root-id if set else parent.id */
    guint pdepth = 0; gchar *proot = NULL; gchar *pid = NULL;
    g_object_get(parent, "depth", &pdepth, "root-id", &proot, "id", &pid, NULL);
    guint cdepth = pdepth + 1;
    const char *root_id = (proot && *proot) ? proot : pid;
    g_object_set(item, "depth", cdepth, NULL);
    if (root_id && *root_id) g_object_set(item, "root-id", root_id, NULL);
    g_free(proot); g_free(pid);
    gnostr_timeline_item_add_child(parent, (TimelineItem*)item);
  } else {
    /* Insert as a new root: depth=0, root-id=self id if available */
    if (r->id && *r->id) g_object_set(item, "root-id", r->id, NULL);
    g_list_store_insert(self->thread_roots, 0, item);
  }
  /* Register id mapping after insertion to allow future children */
  if (r->id && !g_hash_table_contains(self->nodes_by_id, r->id))
    g_hash_table_insert(self->nodes_by_id, g_strdup(r->id), item);
  /* Keep item alive via roots/parent children store; no unref here */
}

static gboolean apply_timeline_events_idle(gpointer user_data) {
  IdleApplyEventsCtx *c = (IdleApplyEventsCtx*)user_data;
  GnostrMainWindow *self = c ? c->self : NULL;
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self) || !c->rows) { idle_apply_events_ctx_free(c); return G_SOURCE_REMOVE; }
  GtkWidget *timeline = g_weak_ref_get(&self->timeline_ref);
  if (!timeline || !GTK_IS_WIDGET(timeline) || !gtk_widget_get_root(timeline)) {
    /* Delay until widget ready */
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, apply_timeline_events_idle, c, NULL);
    if (timeline && G_IS_OBJECT(timeline)) g_object_unref(timeline);
    return G_SOURCE_REMOVE;
  }
  guint applied = 0;
  for (guint i = 0; i < c->rows->len; i++) {
    UiEventRow *r = (UiEventRow*)g_ptr_array_index(c->rows, i);
    /* Dedup by id */
    if (r->id && strlen(r->id) == 64) {
      if (!self->seen_ids) self->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      if (g_hash_table_contains(self->seen_ids, r->id)) continue;
      g_hash_table_add(self->seen_ids, g_strdup(r->id));
    }
    thread_graph_insert(self, r);
    applied++;
  }
  g_message("apply_timeline_events_idle: applied=%u", applied);
  if (timeline && G_IS_OBJECT(timeline)) g_object_unref(timeline);
  idle_apply_events_ctx_free(c);
  return G_SOURCE_REMOVE;
}

static void schedule_apply_events(GnostrMainWindow *self, GPtrArray *rows /* UiEventRow* */) {
  if (!self || !rows) { if (rows) g_ptr_array_free(rows, TRUE); return; }
  IdleApplyEventsCtx *c = g_new0(IdleApplyEventsCtx, 1);
  c->self = self;
  c->rows = rows;
  g_idle_add_full(G_PRIORITY_DEFAULT, apply_timeline_events_idle, c, NULL);
}

/* Convert pool event batch directly into thread graph */
static void on_pool_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !batch) return;
  g_message("on_pool_events: batch len=%u", batch->len);
  GPtrArray *rows = g_ptr_array_new_with_free_func(ui_event_row_free);
  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *evt = (NostrEvent*)g_ptr_array_index(batch, i);
    /* Handle kind-0 profile events by updating metadata cache */
    int kind = nostr_event_get_kind(evt);
    if (kind == 0) {
      const char *pk0 = nostr_event_get_pubkey(evt);
      const char *content0 = nostr_event_get_content(evt);
      g_message("on_pool_events: got kind-0 profile (pk=%s) content_present=%s", pk0 ? pk0 : "(null)", (content0 && *content0) ? "yes" : "no");
      if (pk0 && content0) update_meta_from_profile_json(self, pk0, content0);
      continue;
    }
    UiEventRow *r = g_new0(UiEventRow, 1);
    const char *eid = nostr_event_get_id(evt);
    const char *pubkey = nostr_event_get_pubkey(evt);
    int64_t ts = nostr_event_get_created_at(evt);
    const char *content = nostr_event_get_content(evt);
    r->id = eid ? g_strndup(eid, 64) : NULL;
    r->pubkey = pubkey ? g_strndup(pubkey, 64) : NULL;
    r->created_at = ts;
    r->content = g_utf8_make_valid(content, -1);
    /* parent from last 'e' tag if present */
    NostrTags *tags = (NostrTags*)nostr_event_get_tags(evt);
    if (tags) {
      for (ssize_t j = (ssize_t)nostr_tags_size(tags) - 1; j >= 0; j--) {
        NostrTag *t = nostr_tags_get(tags, (size_t)j);
        const char *k = nostr_tag_get_key(t);
        if (k && g_strcmp0(k, "e") == 0) {
          const char *val = nostr_tag_get_value(t);
          if (val && *val) { r->parent_id = g_strndup(val, 64); break; }
        }
      }
    }
    /* Ingest into NostrdB: serialize event JSON and store */
    char *json = nostr_event_serialize(evt);
    if (json) {
      int irc = storage_ndb_ingest_event_json(json, NULL);
      if (irc != 0) {
        /* LN_OK is typically 0; if API differs, this log still helps */
        g_debug("ndb ingest rc=%d", irc);
      }
      free(json);
    }
    g_ptr_array_add(rows, r);
  }
  schedule_apply_events(self, rows);
}

static void on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  gboolean ok = gnostr_simple_pool_subscribe_many_finish(GNOSTR_SIMPLE_POOL(source), res, &error);
  if (!ok || error) {
    g_warning("SimplePool subscribe_many failed: %s", error ? error->message : "unknown");
    g_clear_error(&error);
  } else {
    g_message("SimplePool subscribe_many started");
  }
}


static void start_pool_live(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  if (!self->pool) self->pool = gnostr_simple_pool_new();
  if (!self->pool_cancellable) self->pool_cancellable = g_cancellable_new();
  if (self->pool_events_handler) {
    g_signal_handler_disconnect(self->pool, self->pool_events_handler);
    self->pool_events_handler = 0;
  }
  // Clean up any existing subscriptions
  if (self->pool_cancellable) {
    g_cancellable_cancel(self->pool_cancellable);
    g_clear_object(&self->pool_cancellable);
  }
  // Clean up profile subscription
  cancel_profile_subscription(self);
  self->pool_events_handler = g_signal_connect(self->pool, "events", G_CALLBACK(on_pool_events), self);
  const char **urls = NULL; size_t url_count = 0; NostrFilters *filters = NULL;
  build_urls_and_filters(self, &urls, &url_count, &filters, (int)self->default_limit);
  /* Do not free previous live_filters here to avoid races; allow leak until shutdown. */
  self->live_filters = filters; /* take ownership for lifetime of live subscription */
  gnostr_simple_pool_subscribe_many_async(self->pool, urls, url_count, self->live_filters, self->pool_cancellable, on_pool_subscribe_done, self);
  /* Free duplicated url strings and array; filters owned by libnostr after async begins? Keep it alive here by not freeing immediately. */
  for (size_t i = 0; i < url_count; i++) g_free((char*)urls[i]);
  g_free((void*)urls);
  /* Keep filters: the async thread borrows them. They will be freed on process exit; alternatively store to free on finalize if needed. */
}

/* (legacy SimplePool backfill removed; using NostrdB worker) */

/* Env helper */
static guint getenv_uint_default(const char *name, guint def) {
  const char *v = g_getenv(name);
  if (!v || !*v) return def;
  long long x = g_ascii_strtoll(v, NULL, 10);
  if (x <= 0) return def;
  if (x > G_MAXUINT) return G_MAXUINT;
  return (guint)x;
}

typedef struct {
  GnostrMainWindow *self; /* strong ref */
  GtkWidget *win;         /* settings window */
  GtkWidget *w_limit;
  GtkWidget *w_batch;
  GtkWidget *w_interval;
  GtkWidget *w_quiet;
  GtkWidget *w_hard;
  GtkWidget *w_use_since;
  GtkWidget *w_since;
  GtkWidget *w_backfill_interval;
} SettingsCtx;

static void settings_ctx_free(SettingsCtx *ctx) {
  if (!ctx) return;
  if (ctx->self) g_object_unref(ctx->self);
  g_free(ctx);
}

static void on_settings_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsCtx *ctx = (SettingsCtx*)user_data;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) return;
  ctx->self->default_limit     = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_limit));
  ctx->self->batch_max         = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_batch));
  ctx->self->post_interval_ms  = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_interval));
  ctx->self->eose_quiet_ms     = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_quiet));
  ctx->self->per_relay_hard_ms = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_hard));
  ctx->self->use_since         = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->w_use_since));
  ctx->self->since_seconds     = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_since));
  ctx->self->backfill_interval_sec = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->w_backfill_interval));
  /* restart periodic refresh timer */
  if (ctx->self->backfill_source_id) {
    g_source_remove(ctx->self->backfill_source_id);
    ctx->self->backfill_source_id = 0;
  }
  if (ctx->self->backfill_interval_sec > 0) {
    ctx->self->backfill_source_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
        ctx->self->backfill_interval_sec,
        (GSourceFunc)periodic_backfill_cb,
        g_object_ref(ctx->self),
        (GDestroyNotify)g_object_unref);
  }
  /* Restart live streaming with new settings.
   * IMPORTANT: The subscribe thread holds a borrowed reference to the provided
   * GCancellable. Clearing/unrefing it here can cause UAF. Instead, swap in a
   * fresh cancellable for the new subscription and only cancel the old one. */
  if (ctx->self->pool_cancellable) {
    GCancellable *old = ctx->self->pool_cancellable; /* keep alive */
    ctx->self->pool_cancellable = g_cancellable_new();
    g_cancellable_cancel(old);
    /* Do NOT unref old here: thread may still read it. */
  } else {
    ctx->self->pool_cancellable = g_cancellable_new();
  }
  /* Do not free live_filters here: async worker may still be borrowing them. */
  /* Stagger re-start to allow previous worker to observe cancel. */
  g_idle_add_once((GSourceOnceFunc)start_pool_live_idle_cb, g_object_ref(ctx->self));
  /* Schedule immediate backfill shortly after live start */
  typedef struct { GnostrMainWindow *self; int limit; } BF;
  BF *bf = g_new0(BF, 1); bf->self = g_object_ref(ctx->self); bf->limit = (int)ctx->self->default_limit;
  g_timeout_add_once(50, (GSourceOnceFunc)start_pool_backfill_idle_cb, bf);
  /* Persist settings */
  gnostr_save_settings(ctx->self);
  show_toast(ctx->self, "Settings saved");
  if (ctx->win) gtk_window_destroy(GTK_WINDOW(ctx->win));
}

static void start_pool_live_idle_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  start_pool_live(self);
  g_object_unref(self);
}

static void start_pool_backfill_idle_cb(gpointer data) {
  typedef struct { GnostrMainWindow *self; int limit; } BF;
  BF *bf = (BF*)data;
  if (bf && GNOSTR_IS_MAIN_WINDOW(bf->self))
    start_pool_backfill(bf->self, bf->limit);
  if (bf && bf->self) g_object_unref(bf->self);
  g_free(bf);
}

/* -- Settings persistence (GKeyFile) -- */
/* moved to util/relays.c: gnostr_config_path */

static void gnostr_load_settings(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gchar *path = gnostr_config_path();
  GKeyFile *kf = g_key_file_new();
  GError *error = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
    g_clear_error(&error);
    g_key_file_unref(kf);
    g_free(path);
    return;
  }
  const char *grp = "gnostr";
  if (g_key_file_has_group(kf, grp)) {
    self->default_limit        = (guint)g_key_file_get_integer(kf, grp, "default_limit", NULL);
    self->batch_max            = (guint)g_key_file_get_integer(kf, grp, "batch_max", NULL);
    self->post_interval_ms     = (guint)g_key_file_get_integer(kf, grp, "post_interval_ms", NULL);
    self->eose_quiet_ms        = (guint)g_key_file_get_integer(kf, grp, "eose_quiet_ms", NULL);
    self->per_relay_hard_ms    = (guint)g_key_file_get_integer(kf, grp, "per_relay_hard_ms", NULL);
    self->use_since            = g_key_file_get_boolean(kf, grp, "use_since", NULL);
    self->since_seconds        = (guint)g_key_file_get_integer(kf, grp, "since_seconds", NULL);
    self->backfill_interval_sec= (guint)g_key_file_get_integer(kf, grp, "backfill_interval_sec", NULL);
  }
  g_key_file_unref(kf);
  g_free(path);
}

static void gnostr_save_settings(GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  gchar *path = gnostr_config_path();
  GKeyFile *kf = g_key_file_new();
  /* Load existing file to preserve other groups (e.g., [relays]) */
  GError *error = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error)) {
    /* It's fine if it doesn't exist; we'll create a new one. */
    g_clear_error(&error);
  }
  const char *grp = "gnostr";
  g_key_file_set_integer(kf, grp, "default_limit",         (gint)self->default_limit);
  g_key_file_set_integer(kf, grp, "batch_max",              (gint)self->batch_max);
  g_key_file_set_integer(kf, grp, "post_interval_ms",       (gint)self->post_interval_ms);
  g_key_file_set_integer(kf, grp, "eose_quiet_ms",          (gint)self->eose_quiet_ms);
  g_key_file_set_integer(kf, grp, "per_relay_hard_ms",      (gint)self->per_relay_hard_ms);
  g_key_file_set_boolean(kf, grp, "use_since",              self->use_since);
  g_key_file_set_integer(kf, grp, "since_seconds",          (gint)self->since_seconds);
  g_key_file_set_integer(kf, grp, "backfill_interval_sec",  (gint)self->backfill_interval_sec);
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (!g_file_set_contents(path, data, len, &error)) {
    g_warning("failed to save settings: %s", error ? error->message : "unknown");
    g_clear_error(&error);
  }
  g_free(data);
  g_free(path);
  g_key_file_unref(kf);
}

static void on_settings_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsCtx *ctx = (SettingsCtx*)user_data;
  if (ctx && ctx->win) gtk_window_destroy(GTK_WINDOW(ctx->win));
}

/* moved to util/relays.c: relay url validation and load/save helpers */

static void relays_ctx_free(RelaysCtx *c){
  if (!c) return;
  if (c->win) {
    /* Window is owned elsewhere and this ctx is freed via window's destroy notify. */
    c->win = NULL;
  }
  c->list = NULL;
  c->sel = NULL;
  c->model = NULL;
  c->entry = NULL;
  if (c->orig) {
    for (guint i = 0; i < c->orig->len; i++) g_free(g_ptr_array_index(c->orig, i));
    g_ptr_array_free(c->orig, TRUE);
    c->orig = NULL;
  }
  g_free(c);
}

/* Forward declarations for signal handlers */
static void relays_model_items_changed(GListModel *m, guint pos, guint removed, guint added, gpointer data);
static gboolean relays_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data);

/* Case-insensitive compare for qsort */
static int cmp_ascii_nocase(gconstpointer a, gconstpointer b) {
  const char *sa = *(const char *const*)a;
  const char *sb = *(const char *const*)b;
  if (!sa && !sb) return 0;
  if (!sa) return -1;
  if (!sb) return 1;
  return g_ascii_strcasecmp(sa, sb);
}

/* Collect strings from model into new GPtrArray (owned), optionally sorted. */
static GPtrArray* relays_collect(RelaysCtx *c, gboolean sort) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  if (!c || !c->model) return arr;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
  for (guint i = 0; i < n; i++) {
    const char *s = gtk_string_list_get_string(c->model, i);
    if (s) g_ptr_array_add(arr, g_strdup(s));
  }
  if (sort) qsort(arr->pdata, arr->len, sizeof(gpointer), cmp_ascii_nocase);
  return arr;
}

/* Replace model contents with sorted snapshot. */
static void relays_resort_model(RelaysCtx *c) {
  if (!c || !c->model) return;
  GPtrArray *arr = relays_collect(c, TRUE);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
  /* Build C array of const char* for splice */
  const char **vals = g_new0(const char*, arr->len + 1);
  for (guint i = 0; i < arr->len; i++) vals[i] = (const char*)g_ptr_array_index(arr, i);
  vals[arr->len] = NULL; /* NULL-terminate for GTK 4 splice API */
  gtk_string_list_splice(c->model, 0, n, vals);
  g_free(vals);
  /* gtk_string_list_splice copies strings; free our array */
  g_ptr_array_free(arr, TRUE);
}

/* Compare current model against ctx->orig to update dirty/save button. */
static void relays_update_dirty(RelaysCtx *c) {
  if (!c) return;
  gboolean dirty = FALSE;
  GPtrArray *now = relays_collect(c, TRUE);
  if (!c->orig || now->len != c->orig->len) {
    dirty = TRUE;
  } else {
    for (guint i = 0; i < now->len; i++) {
      const char *a = (const char*)g_ptr_array_index(now, i);
      const char *b = (const char*)g_ptr_array_index(c->orig, i);
      if (g_strcmp0(a, b) != 0) { dirty = TRUE; break; }
    }
  }
  c->dirty = dirty;
  if (c->btn_save) gtk_widget_set_sensitive(GTK_WIDGET(c->btn_save), dirty);
  g_ptr_array_free(now, TRUE);
}

static void relays_update_status(RelaysCtx *c) {
  if (!c) return;
  guint n = 0;
  if (c->model) n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
  if (c->status) {
    gchar *markup = g_markup_printf_escaped("<small>%u relay%s • %s</small>", n, n==1?"":"s", c->dirty?"Unsaved changes":"Saved");
    gtk_label_set_markup(c->status, markup);
    g_free(markup);
  }
  if (c->win) {
    gtk_window_set_title(c->win, c->dirty ? "Relays*" : "Relays");
  }
}

static void on_relays_add(GtkButton *b, gpointer user_data) {
  (void)b;
  RelaysCtx *c = (RelaysCtx*)user_data;
  const char *url = gtk_editable_get_text(GTK_EDITABLE(c->entry));
  gchar *norm = gnostr_normalize_relay_url(url);
  if (norm) {
    /* Check for duplicates */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
    gboolean exists = FALSE;
    for (guint i = 0; i < n; i++) {
      const char *s = gtk_string_list_get_string(c->model, i);
      if (g_strcmp0(s, norm) == 0) { exists = TRUE; break; }
    }
    if (exists) {
      GtkWindow *parent = gtk_window_get_transient_for(c->win);
      if (GNOSTR_IS_MAIN_WINDOW(parent)) {
        show_toast(GNOSTR_MAIN_WINDOW(parent), "Relay already in the list");
      }
    } else {
      gtk_string_list_append(c->model, norm);
      relays_resort_model(c);
      relays_update_dirty(c);
      relays_update_status(c);
    }
    gtk_editable_set_text(GTK_EDITABLE(c->entry), "");
    g_free(norm);
  } else {
    /* Show a toast on the transient main window */
    GtkWindow *parent = gtk_window_get_transient_for(c->win);
    if (GNOSTR_IS_MAIN_WINDOW(parent)) {
      show_toast(GNOSTR_MAIN_WINDOW(parent), "Invalid relay URL. Use ws:// or wss://");
    }
  }
  /* Update button sensitivity after change */
  if (c->btn_add) gtk_widget_set_sensitive(GTK_WIDGET(c->btn_add), FALSE);
}

static void on_relays_remove(GtkButton *b, gpointer user_data) {
  (void)b;
  RelaysCtx *c = (RelaysCtx*)user_data;
  if (!c->sel) return;
  guint pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(c->sel));
  if (pos != GTK_INVALID_LIST_POSITION) gtk_string_list_remove(c->model, pos);
  /* Update remove sensitivity */
  if (c->btn_remove) {
    pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(c->sel));
    gtk_widget_set_sensitive(GTK_WIDGET(c->btn_remove), pos != GTK_INVALID_LIST_POSITION);
  }
  relays_resort_model(c);
  relays_update_dirty(c);
  relays_update_status(c);
}

static void on_relays_save(GtkButton *b, gpointer user_data) {
  (void)b;
  RelaysCtx *c = (RelaysCtx*)user_data;
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
  g_message("relay_manager: preparing to save %u entries", n);
  for (guint i = 0; i < n; i++) {
    const char *s = gtk_string_list_get_string(c->model, i);
    if (s && *s) {
      g_message("relay_manager: save[%u] %s", i, s);
      g_ptr_array_add(arr, g_strdup(s));
    }
  }
  gnostr_save_relays_from(arr);
  g_ptr_array_free(arr, TRUE);
  show_toast(GNOSTR_MAIN_WINDOW(gtk_window_get_transient_for(c->win)), "Relays saved");
  /* Avoid re-entrant close-request prompt by clearing dirty before destroy */
  c->dirty = FALSE;
  relays_update_status(c);
  gtk_window_destroy(c->win);
}

/* ---- NIP-46 Pairing UI and async worker ---- */

static void pair_ctx_free(PairCtx *c) {
  if (!c) return;
  if (c->self) g_object_unref(c->self);
  c->win = NULL; c->entry_uri = NULL; c->btn_connect = NULL; c->lbl_status = NULL;
  g_free(c);
}

typedef struct {
  char *uri;                    /* in */
  char *requested_perms;        /* in */
  /* out */
  NostrNip46Session *session;   /* created on success */
  char *npub;                   /* derived from remote pubkey */
  char *error;                  /* error message */
} PairWorker;

static void pair_worker_free(PairWorker *w) {
  if (!w) return;
  g_free(w->uri);
  g_free(w->requested_perms);
  if (w->session) nostr_nip46_session_free(w->session);
  g_free(w->npub);
  g_free(w->error);
  g_free(w);
}

/* minimal hex -> bytes helper */
static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex) return FALSE;
  size_t n = strlen(hex);
  if (n != 64) return FALSE;
  for (size_t i = 0; i < 32; i++) {
    char c1 = hex[2*i], c2 = hex[2*i+1];
    int v1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : -1;
    int v2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : -1;
    if (v1 < 0 || v2 < 0) return FALSE;
    out[i] = (uint8_t)((v1 << 4) | v2);
  }
  return TRUE;
}

static void pair_remote_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  (void)source; (void)cancellable;
  PairWorker *w = (PairWorker*)task_data;
  if (!w || !w->uri) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid worker args"); return; }
  NostrNip46Session *s = nostr_nip46_client_new();
  if (!s) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "failed to allocate nip46 session"); return; }
  int rc = nostr_nip46_client_connect(s, w->uri, w->requested_perms ? w->requested_perms : "get_public_key,sign_event,encrypt,decrypt");
  if (rc != 0) {
    nostr_nip46_session_free(s);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "Failed to connect to remote signer");
    return;
  }
  char *pub_hex = NULL;
  rc = nostr_nip46_client_get_public_key(s, &pub_hex);
  if (rc != 0 || !pub_hex) {
    nostr_nip46_session_free(s);
    if (pub_hex) free(pub_hex);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Remote signer did not provide a public key");
    return;
  }
  uint8_t pk[32] = {0};
  if (!hex_to_bytes32(pub_hex, pk)) {
    nostr_nip46_session_free(s);
    free(pub_hex);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid public key from remote signer");
    return;
  }
  char *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0 || !npub) {
    nostr_nip46_session_free(s);
    free(pub_hex);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to encode npub");
    return;
  }
  free(pub_hex);
  w->session = s;
  w->npub = npub;
  g_task_return_boolean(task, TRUE);
}

static void pair_remote_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  PairCtx *ctx = (PairCtx*)user_data;
  GTask *task = G_TASK(res);
  PairWorker *w = (PairWorker*)g_task_get_task_data(task);
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(task, &error);
  if (!ctx || !ctx->self) { if (error) g_error_free(error); if (w) pair_worker_free(w); pair_ctx_free(ctx); return; }
  if (!ok || error) {
    const char *msg = error && error->message ? error->message : "Pairing failed";
    if (ctx->lbl_status) gtk_label_set_text(ctx->lbl_status, msg);
    if (ctx->btn_connect) gtk_widget_set_sensitive(GTK_WIDGET(ctx->btn_connect), TRUE);
    show_toast(ctx->self, msg);
    g_clear_error(&error);
    if (w) pair_worker_free(w);
    return;
  }
  /* Success: adopt session, persist npub, fetch profile */
  if (ctx->self->nip46_session) {
    nostr_nip46_session_free(ctx->self->nip46_session);
  }
  ctx->self->nip46_session = w->session; /* take ownership */
  w->session = NULL;
  client_settings_set_current_npub(w->npub);
  gchar *msg = g_strdup_printf("Paired remote signer: %s", w->npub);
  show_toast(ctx->self, msg);
  g_free(msg);
  gnostr_profile_fetch_async(ctx->self);
  start_profile_subscription(ctx->self);
  if (ctx->win) gtk_window_destroy(ctx->win);
  pair_worker_free(w);
  pair_ctx_free(ctx);
}

static void on_pair_cancel(GtkButton *b, gpointer user_data) {
  (void)b;
  PairCtx *ctx = (PairCtx*)user_data;
  if (ctx && ctx->win) gtk_window_destroy(ctx->win);
}

static void on_pair_connect(GtkButton *b, gpointer user_data) {
  (void)b;
  PairCtx *ctx = (PairCtx*)user_data;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) return;
  const char *uri = gtk_editable_get_text(GTK_EDITABLE(ctx->entry_uri));
  if (!uri || !*uri) { if (ctx->lbl_status) gtk_label_set_text(ctx->lbl_status, "Enter a bunker:// or nostrconnect:// URI"); return; }
  if (ctx->btn_connect) gtk_widget_set_sensitive(GTK_WIDGET(ctx->btn_connect), FALSE);
  if (ctx->lbl_status) gtk_label_set_text(ctx->lbl_status, "Connecting…");
  GTask *task = g_task_new(NULL, NULL, pair_remote_complete, ctx);
  PairWorker *w = g_new0(PairWorker, 1);
  w->uri = g_strdup(uri);
  w->requested_perms = g_strdup("get_public_key,sign_event,encrypt,decrypt");
  g_task_set_task_data(task, w, (GDestroyNotify)pair_worker_free);
  g_task_run_in_thread(task, pair_remote_worker);
  g_object_unref(task);
}

static void on_relays_cancel(GtkButton *b, gpointer user_data) {
  (void)b;
  RelaysCtx *c = (RelaysCtx*)user_data;
  gtk_window_destroy(c->win);
}

static void relays_update_buttons(RelaysCtx *c) {
  if (!c) return;
  /* Add enabled only when entry has a valid relay url */
  gboolean can_add = gnostr_is_valid_relay_url(gtk_editable_get_text(GTK_EDITABLE(c->entry)));
  if (c->btn_add) gtk_widget_set_sensitive(GTK_WIDGET(c->btn_add), can_add);
  /* Remove enabled only when a row is selected */
  guint pos = GTK_INVALID_LIST_POSITION;
  if (c->sel) pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(c->sel));
  if (c->btn_remove) gtk_widget_set_sensitive(GTK_WIDGET(c->btn_remove), pos != GTK_INVALID_LIST_POSITION);
  /* Save only when dirty */
  if (c->btn_save) gtk_widget_set_sensitive(GTK_WIDGET(c->btn_save), c->dirty);
  relays_update_status(c);
}

static void on_relays_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  RelaysCtx *c = (RelaysCtx*)user_data;
  /* Visual validation */
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(c->entry));
  gboolean ok = gnostr_is_valid_relay_url(txt);
  if (ok) gtk_widget_remove_css_class(GTK_WIDGET(c->entry), "error");
  else gtk_widget_add_css_class(GTK_WIDGET(c->entry), "error");
  relays_update_buttons(c);
}

static void on_relays_selection_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)obj; (void)pspec;
  RelaysCtx *c = (RelaysCtx*)user_data;
  relays_update_buttons(c);
}

static void on_relays_clicked(GtkButton *button, GnostrMainWindow *self) {
  (void)button;
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  GtkBuilder *b = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-relay-manager.ui");
  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(b, "relay_manager_window"));
  if (!win) { g_warning("relay_manager_window not found in builder"); g_object_unref(b); return; }
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  GtkListView *list = GTK_LIST_VIEW(gtk_builder_get_object(b, "relay_list"));
  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(b, "relay_entry"));
  GtkButton *btn_add = GTK_BUTTON(gtk_builder_get_object(b, "btn_add"));
  GtkButton *btn_remove = GTK_BUTTON(gtk_builder_get_object(b, "btn_remove"));
  GtkButton *btn_save = GTK_BUTTON(gtk_builder_get_object(b, "btn_save"));
  GtkButton *btn_cancel = GTK_BUTTON(gtk_builder_get_object(b, "btn_cancel"));
  GtkLabel *status = GTK_LABEL(gtk_builder_get_object(b, "status_label"));
  /* Restore window size from GSettings if available */
  {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (src) {
      GSettingsSchema *sch = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
      if (sch) {
        GSettings *gs = g_settings_new("org.gnostr.gnostr");
        int w = g_settings_get_int(gs, "relays-window-width");
        int h = g_settings_get_int(gs, "relays-window-height");
        if (w > 0 && h > 0) gtk_window_set_default_size(win, w, h);
        g_object_unref(gs);
        g_settings_schema_unref(sch);
      }
    }
  }
  /* Build model */
  GtkStringList *model = gtk_string_list_new(NULL);
  /* Load from config */
  GPtrArray *buf = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(buf);
  g_message("relay_manager: loaded %u into buffer", buf->len);
  for (guint i = 0; i < buf->len; i++) {
    const char *u = (const char*)g_ptr_array_index(buf, i);
    g_message("relay_manager: add to model [%u]: %s", i, u);
    gtk_string_list_append(model, u);
  }
  g_ptr_array_free(buf, TRUE);
  /* Sort initial contents */
  {
    RelaysCtx tmp = {0}; tmp.model = model; relays_resort_model(&tmp);
  }
  /* Create selection and set on list */
  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(model));
  gtk_list_view_set_model(list, GTK_SELECTION_MODEL(sel));
  /* Setup factory */
  GtkListItemFactory *fac = gtk_signal_list_item_factory_new();
  g_signal_connect(fac, "setup", G_CALLBACK(relay_item_setup), NULL);
  g_signal_connect(fac, "bind", G_CALLBACK(relay_item_bind), model);
  gtk_list_view_set_factory(list, GTK_LIST_ITEM_FACTORY(fac));
  g_object_unref(fac);
  /* Context */
  RelaysCtx *ctx = g_new0(RelaysCtx, 1);
  ctx->win = g_object_ref(win);
  ctx->list = list;
  ctx->sel = GTK_SELECTION_MODEL(sel);
  ctx->model = model;
  ctx->entry = entry;
  ctx->btn_add = btn_add;
  ctx->btn_remove = btn_remove;
  ctx->btn_save = btn_save;
  ctx->status = status;
  /* Snapshot original (sorted) list for dirty tracking */
  ctx->orig = relays_collect(ctx, TRUE);
  ctx->dirty = FALSE;
  g_signal_connect(btn_add, "clicked", G_CALLBACK(on_relays_add), ctx);
  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_relays_remove), ctx);
  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_relays_save), ctx);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_relays_cancel), ctx);
  g_signal_connect(entry, "changed", G_CALLBACK(on_relays_entry_changed), ctx);
  g_signal_connect(entry, "activate", G_CALLBACK(on_relays_add), ctx);
  g_signal_connect(sel, "notify::selected", G_CALLBACK(on_relays_selection_changed), ctx);
  /* Track model changes for dirty flag */
  g_signal_connect(model, "items-changed", G_CALLBACK(relays_model_items_changed), ctx);
  /* Styling */
  gtk_widget_add_css_class(GTK_WIDGET(btn_save), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "destructive-action");
  gtk_widget_add_css_class(GTK_WIDGET(list), "card");
  /* Keyboard shortcuts: Esc=close, Ctrl+Enter=save, Delete=remove */
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(relays_key_pressed), ctx);
  gtk_widget_add_controller(GTK_WIDGET(win), keys);
  /* Initialize button states */
  relays_update_buttons(ctx);
  relays_update_status(ctx);
  /* Store context before wiring close handler so it can read 'dirty' */
  g_object_set_data_full(G_OBJECT(win), "relays-ctx", ctx, (GDestroyNotify)relays_ctx_free);
  /* Save size on close / confirm discard */
  g_signal_connect(win, "close-request", G_CALLBACK(relays_on_close_request), NULL);
  g_object_unref(b);
  gtk_window_present(GTK_WINDOW(win));
  gtk_widget_grab_focus(GTK_WIDGET(entry));
}

/* Signal handlers (C-callable) */
static void relays_model_items_changed(GListModel *m, guint pos, guint removed, guint added, gpointer data) {
  (void)m; (void)pos; (void)removed; (void)added;
  RelaysCtx *c = (RelaysCtx*)data;
  relays_update_dirty(c);
  relays_update_buttons(c);
}

static gboolean relays_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
  (void)c; (void)keycode;
  RelaysCtx *rc = (RelaysCtx*)data;
  if (keyval == GDK_KEY_Escape) { gtk_window_destroy(rc->win); return TRUE; }
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
    on_relays_save(NULL, rc); return TRUE; }
  if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace)) { on_relays_remove(NULL, rc); return TRUE; }
  return FALSE;
}

static void on_settings_clicked(GtkButton *button, GnostrMainWindow *self) {
  (void)button;
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  GtkBuilder *b = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-settings-dialog.ui");
  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(b, "settings_window"));
  if (!win) { g_warning("settings_window not found in builder"); g_object_unref(b); return; }
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  /* Lookup controls */
  GtkWidget *w_limit   = GTK_WIDGET(gtk_builder_get_object(b, "w_limit"));
  GtkWidget *w_batch   = GTK_WIDGET(gtk_builder_get_object(b, "w_batch"));
  GtkWidget *w_interval= GTK_WIDGET(gtk_builder_get_object(b, "w_interval"));
  GtkWidget *w_quiet   = GTK_WIDGET(gtk_builder_get_object(b, "w_quiet"));
  GtkWidget *w_hard    = GTK_WIDGET(gtk_builder_get_object(b, "w_hard"));
  GtkWidget *w_use_since = GTK_WIDGET(gtk_builder_get_object(b, "w_use_since"));
  GtkWidget *w_since   = GTK_WIDGET(gtk_builder_get_object(b, "w_since"));
  GtkWidget *w_backfill= GTK_WIDGET(gtk_builder_get_object(b, "w_backfill"));
  GtkWidget *btn_save  = GTK_WIDGET(gtk_builder_get_object(b, "btn_save"));
  GtkWidget *btn_cancel= GTK_WIDGET(gtk_builder_get_object(b, "btn_cancel"));
  if (!w_limit || !w_batch || !w_interval || !w_quiet || !w_hard || !w_use_since || !w_since || !w_backfill || !btn_save || !btn_cancel) {
    g_warning("settings builder missing required widgets");
    g_object_unref(b);
    return;
  }
  /* Initialize values from current settings */
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_limit), self->default_limit);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_batch), self->batch_max);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_interval), self->post_interval_ms);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_quiet), self->eose_quiet_ms);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_hard), self->per_relay_hard_ms);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w_use_since), self->use_since);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_since), self->since_seconds);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_backfill), self->backfill_interval_sec);

  /* Create ctx and wire signals */
  SettingsCtx *ctx = g_new0(SettingsCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->win = GTK_WIDGET(win);
  ctx->w_limit = w_limit;
  ctx->w_batch = w_batch;
  ctx->w_interval = w_interval;
  ctx->w_quiet = w_quiet;
  ctx->w_hard = w_hard;
  ctx->w_use_since = w_use_since;
  ctx->w_since = w_since;
  ctx->w_backfill_interval = w_backfill;
  g_object_set_data_full(G_OBJECT(win), "settings-ctx", ctx, (GDestroyNotify)settings_ctx_free);
  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_settings_save_clicked), ctx);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_settings_cancel_clicked), ctx);
  /* Builder can be dropped; widgets are now owned by the window */
  g_object_unref(b);
  gtk_window_present(GTK_WINDOW(win));
}

/* Finish handler for GetPublicKey from local signer */
static void on_avatar_get_pubkey_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  GError *error = NULL;
  char *npub = NULL;
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_finish(proxy, &npub, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("Avatar GetPublicKey failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Local Signer not available");
      gtk_alert_dialog_set_detail(dlg, "Could not contact org.nostr.Signer. Is the signer running?");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    g_clear_error(&error);
    return;
  }
  if (npub && *npub) {
    gchar *msg = g_strdup_printf("Signed in: %s", npub);
    show_toast(self, msg);
    g_free(msg);
    /* Persist identity */
    client_settings_set_current_npub(npub);
    /* Fetch profile metadata for this identity and subscribe for updates */
    gnostr_profile_fetch_async(self);
    start_profile_subscription(self);
  } else {
    show_toast(self, "Signer returned empty public key");
  }
  g_free(npub);
}

/* Avatar popover: Login with Local Signer clicked */
static void on_avatar_login_local_clicked(GtkButton *button, GnostrMainWindow *self) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  /* Close the popover if present */
  GtkWidget *w = GTK_WIDGET(button);
  GtkPopover *pop = GTK_POPOVER(gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER));
  if (pop) gtk_popover_popdown(pop);
  /* Acquire proxy (sync, fast) and call GetPublicKey async */
  GError *err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&err);
  if (!proxy) {
    g_warning("signer proxy get failed: %s", err ? err->message : "unknown");
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Local Signer not found");
      gtk_alert_dialog_set_detail(dlg, "Install or start the signer application to login.");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    g_clear_error(&err);
    return;
  }
  show_toast(self, "Contacting local signer…");
  nostr_org_nostr_signer_call_get_public_key(proxy, NULL, on_avatar_get_pubkey_done, self);
}

/* Avatar popover: Pair Remote Signer (NIP-46) clicked */
static void on_avatar_pair_remote_clicked(GtkButton *button, GnostrMainWindow *self) {
  (void)button;
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  /* Close the popover if present */
  GtkWidget *w = GTK_WIDGET(button);
  GtkPopover *pop = GTK_POPOVER(gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER));
  if (pop) gtk_popover_popdown(pop);
  /* Build a simple dialog for entering the pairing URI */
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_title(win, "Pair Remote Signer (NIP-46)");
  gtk_window_set_default_size(win, 520, 140);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *lbl = gtk_label_new("Paste bunker:// or nostrconnect:// URI:");
  GtkWidget *entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), "bunker://");
  GtkWidget *status = gtk_label_new("");
  gtk_widget_add_css_class(status, "dim-label");
  GtkWidget *btnrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_connect = gtk_button_new_with_label("Connect");
  gtk_box_append(GTK_BOX(btnrow), btn_cancel);
  gtk_box_append(GTK_BOX(btnrow), btn_connect);
  gtk_box_append(GTK_BOX(box), lbl);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_box_append(GTK_BOX(box), status);
  gtk_box_append(GTK_BOX(box), btnrow);
  gtk_window_set_child(win, box);
  PairCtx *ctx = g_new0(PairCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->win = win;
  ctx->entry_uri = GTK_ENTRY(entry);
  ctx->btn_connect = GTK_BUTTON(btn_connect);
  ctx->lbl_status = GTK_LABEL(status);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_pair_cancel), ctx);
  g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_pair_connect), ctx);
  /* Destroy notify ensures context freed when window is closed by WM */
  g_object_set_data_full(G_OBJECT(win), "pair-ctx", ctx, (GDestroyNotify)pair_ctx_free);
  gtk_window_present(win);
}

static void toast_hide_cb(gpointer data) {
  GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(data);
  if (win && win->toast_revealer)
    gtk_revealer_set_reveal_child(GTK_REVEALER(win->toast_revealer), FALSE);
}

static void show_toast(GnostrMainWindow *self, const char *msg) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (!self->toast_revealer || !self->toast_label) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg ? msg : "");
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 2.5s */
  g_timeout_add_once(2500, (GSourceOnceFunc)toast_hide_cb, self);
}

/* Forward declaration to satisfy initial_refresh */
static gboolean initial_refresh_cb(gpointer data);

/* Trampoline with correct type for g_timeout_add_once */
static void initial_refresh_timeout_cb(gpointer data) {
  (void)initial_refresh_cb(data);
}

/* Periodic backfill callback. data is a strong ref to GnostrMainWindow (freed by destroy notify). */
static gboolean periodic_backfill_cb(gpointer data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return G_SOURCE_REMOVE;
  g_message("backfill: triggering timeline refresh (every %u sec)", self->backfill_interval_sec);
  start_pool_backfill(self, (int)self->default_limit);
  return G_SOURCE_CONTINUE;
}

static gboolean initial_refresh_cb(gpointer data) {
  GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(data);
  if (!win || !GTK_IS_WIDGET(win->timeline)) {
    g_message("initial refresh skipped: timeline not ready");
    return G_SOURCE_REMOVE;
  }
  g_message("initial refresh: starting backfill");
  start_pool_backfill(win, (int)win->default_limit);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GnostrMainWindow *self; /* strong ref, owned */
  int limit;
} RefreshTaskData;

/* (IdleApplyCtx typedef moved earlier) */

static void idle_apply_ctx_free(IdleApplyCtx *c) {
  if (!c) return;
  if (c->lines) {
    g_debug("idle free: unref lines ptr=%p", (void*)c->lines);
    g_ptr_array_unref(c->lines);
  }
  if (c->self) {
    g_debug("idle free: unref self ptr=%p", (void*)c->self);
    g_object_unref(c->self);
  }
  g_free(c);
}

/* Forward declare idle applier for trampoline */
static gboolean apply_timeline_lines_idle(gpointer user_data);

/* Trampoline to safely reschedule apply on a timeout with correct callback type */
static void apply_timeline_lines_timeout_cb(gpointer user_data) {
  (void)apply_timeline_lines_idle(user_data);
}

static gboolean apply_timeline_lines_idle(gpointer user_data) {
  IdleApplyCtx *c = (IdleApplyCtx*)user_data;
  GnostrMainWindow *self = c ? c->self : NULL;
  GPtrArray *lines = c ? c->lines : NULL;
  g_message("apply_timeline_lines_idle: entry (self=%p, lines=%p)", (void*)self, (void*)lines);
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self) || !lines) {
    g_debug("apply_timeline_lines_idle: missing self or lines (self=%p lines=%p)", (void*)self, (void*)lines);
    idle_apply_ctx_free(c);
    return G_SOURCE_REMOVE;
  }
  GtkWidget *timeline = g_weak_ref_get(&self->timeline_ref);
  if (!timeline || !GTK_IS_WIDGET(timeline) || !gtk_widget_get_root(timeline)) {
    /* Widget not yet in a realized hierarchy; retry shortly instead of dropping */
    g_debug("timeline not ready (tl=%p); will retry apply of %u lines", (void*)timeline, lines->len);
    g_timeout_add_once(100, (GSourceOnceFunc)apply_timeline_lines_timeout_cb, c);
    if (timeline && G_IS_OBJECT(timeline)) g_object_unref(timeline);
    return G_SOURCE_REMOVE;
  }
  if (GNOSTR_IS_TIMELINE_VIEW(timeline)) {
    g_message("apply_timeline_lines_idle: applying %u lines to timeline=%p (type=%s)",
              lines->len, (void*)timeline, G_OBJECT_TYPE_NAME(timeline));
    const gboolean skip_dedup = g_getenv("GNOSTR_SKIP_DEDUP") != NULL;
    const gboolean always_allow = g_getenv("GNOSTR_ALWAYS_ALLOW") != NULL;
    guint before_count = 0;
    if (GNOSTR_IS_TIMELINE_VIEW(timeline)) {
      /* Peek current count for diagnostics */
      GnostrTimelineView *tv = GNOSTR_TIMELINE_VIEW(timeline);
      /* Access internal string_model only via public API; we don't have one.
         We'll get count after each prepend via its own log. */
      (void)tv;
    }
    if (skip_dedup) g_warning("GNOSTR_SKIP_DEDUP is set: bypassing deduplication");
    if (always_allow) g_warning("GNOSTR_ALWAYS_ALLOW is set: forcing allow of all rows");
    guint applied = 0, skipped = 0;
    for (guint i = 0; i < lines->len; i++) {
      const char *raw = (const char*)g_ptr_array_index(lines, i);
      const char *tab = raw ? strchr(raw, '\t') : NULL;
      const char *id = NULL;
      const char *text = raw;
      if (tab) {
        /* Expect format: "<id>\t<text>" */
        id = raw;
        text = tab + 1;
      }
      /* Initialize sets on demand */
      if (!self->seen_ids) self->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      if (!self->seen_texts) self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

      gboolean allow = TRUE;
      if (skip_dedup || always_allow) {
        allow = TRUE;
      } else {
      if (id && g_str_has_prefix(id, "-") == FALSE && strlen(id) == 64) {
        if (g_hash_table_contains(self->seen_ids, id)) {
          allow = FALSE;
          g_debug("apply: skip duplicate id %.12s...", id);
        } else {
          g_hash_table_add(self->seen_ids, g_strndup(id, 64));
        }
      } else {
        /* Fallback dedup by text if no id */
        if (g_hash_table_contains(self->seen_texts, text)) {
          allow = FALSE;
          g_debug("apply: skip duplicate text %.32s", text);
        } else {
          g_hash_table_add(self->seen_texts, g_strdup(text));
        }
      } }
      if (allow) {
        g_debug("apply: insert root item text=%.60s", text ? text : "");
        if (!self->thread_roots) {
          extern GType timeline_item_get_type(void);
          self->thread_roots = g_list_store_new(timeline_item_get_type());
        }
        GType ti = timeline_item_get_type();
        if (ti) {
          gpointer item = g_object_new(ti,
                                       "display-name", "Anonymous",
                                       "handle", "@anon",
                                       "timestamp", "now",
                                       "content", text ? text : "",
                                       "depth", 0,
                                       NULL);
          /* Record id mapping if present */
          if (id && strlen(id) == 64) {
            g_object_set(item, "id", id, NULL);
            if (!self->nodes_by_id)
              self->nodes_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            if (!g_hash_table_contains(self->nodes_by_id, id))
              g_hash_table_insert(self->nodes_by_id, g_strndup(id, 64), item);
          }
          g_list_store_insert(self->thread_roots, 0, item);
          /* If we inserted into roots after timeline set, view will update */
          applied++;
          g_object_unref(item);
        }
      } else {
        skipped++;
      }
    }
    g_message("apply_timeline_lines_idle: done applying (applied=%u skipped=%u)", applied, skipped);
  } else {
    g_warning("timeline widget is not GnostrTimelineView: type=%s", G_OBJECT_TYPE_NAME(timeline));
  }
  if (timeline && G_IS_OBJECT(timeline)) g_object_unref(timeline);
  idle_apply_ctx_free(c);
  g_message("apply_timeline_lines_idle: exit");
  return G_SOURCE_REMOVE;
}

static void refresh_task_data_free(RefreshTaskData *d) {
  if (!d) return;
  if (d->self) g_object_unref(d->self);
  g_free(d);
}

/* Worker: for now, synthesize placeholder lines. Later, query relays via libnostr. */
static void timeline_refresh_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  (void)source; (void)cancellable;
  RefreshTaskData *d = (RefreshTaskData*)task_data;
  /* Legacy accumulator retained (unused for posting) */
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  g_message("timeline_refresh_worker: start (limit=%d)", d ? d->limit : -1);
  /* Streaming batch state */
  GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);
  const guint batch_max = d && d->self ? GNOSTR_MAIN_WINDOW(d->self)->batch_max : 5;
  const gint64 interval_us = (gint64)((d && d->self ? GNOSTR_MAIN_WINDOW(d->self)->post_interval_ms : 150) * G_TIME_SPAN_MILLISECOND);
  gint64 last_post_us = g_get_monotonic_time();

#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
  /* Real path: connect to each relay, subscribe, drain events until EOSE/CLOSED, format lines. */
  /* Allow override relays via GNOSTR_RELAYS=ws://a,ws://b */
  const char *env_relays = g_getenv("GNOSTR_RELAYS");
  gchar **relay_list = NULL;
  const char *default_urls[] = {
    "wss://relay.damus.io",
    "wss://nos.lol"
  };
  size_t url_count = 0;
  if (env_relays && *env_relays) {
    relay_list = g_strsplit(env_relays, ",", -1);
    /* count */
    for (gchar **p = relay_list; p && *p; ++p) url_count++;
    g_message("worker: using GNOSTR_RELAYS (%zu)", url_count);
  } else {
    url_count = sizeof(default_urls)/sizeof(default_urls[0]);
  }

  int total_collected = 0;
  /* Defer teardown to avoid blocking between relays */
  typedef struct { NostrSubscription *sub; NostrRelay *relay; NostrFilters *fs; } CleanupItem;
  GPtrArray *deferred = g_ptr_array_new_with_free_func(g_free);

  for (size_t i = 0; i < url_count; ++i) {
    const char *url = env_relays ? relay_list[i] : default_urls[i];
    if (!url || !*url) {
      g_message("worker: relay %zu/%zu skipped (empty)", i+1, url_count);
      continue;
    }
    g_message("worker: relay %zu/%zu url=%s", i+1, url_count, url);
    g_message("worker: connecting %s", url);
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, url, &err);
    if (!relay) {
      g_ptr_array_add(batch, g_strdup_printf("-\t[error] relay_new %s: %s", url, err ? err->message : "unknown"));
      maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
      if (err) free_error(err);
      continue;
    }
    if (!nostr_relay_connect(relay, &err)) {
      g_ptr_array_add(batch, g_strdup_printf("-\t[error] connect %s: %s", url, err ? err->message : "unknown"));
      maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
      if (err) free_error(err);
      nostr_relay_free(relay);
      continue;
    }

    /* Build filters per-relay to avoid shared ownership pitfalls */
    /* Build filters: kind=1 notes with a sensible window and limit */
    NostrFilter *f = nostr_filter_new();
    int kinds[] = { 1 };
    nostr_filter_set_kinds(f, kinds, 1);
    int lim = d && d->limit > 0 ? d->limit : (d && d->self ? (int)GNOSTR_MAIN_WINDOW(d->self)->default_limit : 30);
    if (total_collected >= lim) {
      g_message("worker: global limit reached (%d), stopping before %s", lim, url);
      break;
    }
    int per_relay_lim = lim - total_collected;
    /* Some relays apply limit per-subscription; request only remaining needed */
    nostr_filter_set_limit(f, per_relay_lim);
    /* Canonical: when asking for recent events with a limit, omit 'since'.
       Allow opting-in via GNOSTR_USE_SINCE and optional GNOSTR_SINCE_SECONDS. */
    if (d && d->self && GNOSTR_MAIN_WINDOW(d->self)->use_since) {
      int window_secs = (int)GNOSTR_MAIN_WINDOW(d->self)->since_seconds;
      time_t now = time(NULL);
      int64_t since = (int64_t)now - window_secs;
      if (since < 0) since = 0;
      nostr_filter_set_since_i64(f, since);
      g_message("worker: using since=%ld seconds ago (env)", (long)window_secs);
    }
    NostrFilters *fs = nostr_filters_new();
    nostr_filters_add(fs, f);

    GoContext *bg = go_context_background();
    NostrSubscription *sub = (NostrSubscription*)nostr_relay_prepare_subscription(relay, bg, fs);
    if (!sub) {
      g_ptr_array_add(batch, g_strdup_printf("-\t[error] prepare sub %s", url));
      maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      nostr_filters_free(fs);
      continue;
    }
    if (!nostr_subscription_fire(sub, &err)) {
      g_ptr_array_add(batch, g_strdup_printf("-\t[error] fire sub %s: %s", url, err ? err->message : "unknown"));
      maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
      if (err) free_error(err);
      nostr_subscription_close(sub, NULL);
      nostr_subscription_free(sub);
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      nostr_filters_free(fs);
      continue;
    }

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose   = nostr_subscription_get_eose_channel(sub);
    GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

    /* Drain events; on EOSE, keep only a short grace period to catch stragglers. */
    gboolean eose_seen = FALSE;
    gint64 eose_time_us = 0;
    int collected = 0; /* per-relay count */
    const gint64 started_us = g_get_monotonic_time();
    const guint hard_ms = (d && d->self) ? GNOSTR_MAIN_WINDOW(d->self)->per_relay_hard_ms : 5000;
    const gint64 per_relay_hard_us = started_us + (hard_ms * G_TIME_SPAN_MILLISECOND);
    /* If busy, extend briefly when events arrive, but cap to 750ms after last event post-EOSE */
    const guint eose_quiet_ms = (d && d->self) ? GNOSTR_MAIN_WINDOW(d->self)->eose_quiet_ms : 150;
    gint64 quiet_deadline_us = g_get_monotonic_time() + (eose_quiet_ms * G_TIME_SPAN_MILLISECOND);
    while (TRUE) {
      /* CLOSED wins: exit immediately */
      void *data = NULL;
      if (ch_closed && go_channel_try_receive(ch_closed, &data) == 0) {
        const char *reason = (const char *)data;
        g_ptr_array_add(batch, g_strdup_printf("%s\t[%s] CLOSED: %s", "-", url, reason ? reason : ""));
        maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
        break;
      }

      /* Drain as many events as are immediately available */
      gboolean any_event = FALSE;
      while (ch_events && go_channel_try_receive(ch_events, &data) == 0) {
        any_event = TRUE;
        NostrEvent *evt = (NostrEvent *)data;
        const char *pubkey = nostr_event_get_pubkey(evt);
        const char *content = nostr_event_get_content(evt);
        int64_t ts = nostr_event_get_created_at(evt);
        if (!content) content = "";
        /* Ensure valid UTF-8 to prevent GTK crashes */
        gchar *one = g_utf8_make_valid(content, -1);
        /* Truncate to first line */
        gchar *nl = one ? strchr(one, '\n') : NULL;
        if (nl) *nl = '\0';
        /* Limit display length to 160 UTF-8 chars */
        if (one && g_utf8_strlen(one, -1) > 160) {
          gchar *tmp = g_utf8_substring(one, 0, 160);
          g_free(one);
          one = tmp;
        }
        const char *eid = nostr_event_get_id(evt);
        if (!eid) eid = "-";
        gchar *row = g_strdup_printf("%s\t[%s] %s | %s (%ld)", eid, url, pubkey ? pubkey : "(anon)", one ? one : "", (long)ts);
        g_debug("worker: row eid=%.12s... text=%.40s", eid, one ? one : "");
        g_ptr_array_add(batch, row);
        maybe_post_batch(d->self, &batch, &last_post_us, interval_us, batch_max);
        collected++;
        if (one) g_free(one);
        if (collected >= lim) {
          g_debug("worker: %s reached limit=%d; breaking", url, lim);
          any_event = FALSE; /* prevent extending deadlines */
          break;
        }
      }
      if (any_event) {
        /* Extend quiet deadline a bit after receiving events to allow small bursts */
        quiet_deadline_us = g_get_monotonic_time() + (eose_quiet_ms * G_TIME_SPAN_MILLISECOND);
      }

      /* Observe EOSE but don't break until grace period elapses */
      data = NULL;
      if (!eose_seen && ch_eose && go_channel_try_receive(ch_eose, &data) == 0) {
        eose_seen = TRUE;
        eose_time_us = g_get_monotonic_time();
        g_debug("worker: %s EOSE seen", url);
      }

      /* Exit conditions */
      const gint64 now_us = g_get_monotonic_time();
      if (collected >= per_relay_lim) {
        /* Reached limit; flush any pending items before breaking */
        if (batch && batch->len > 0 && d && d->self) {
          g_message("timeline_refresh_worker: flushing remaining (limit hit, lines=%u)", batch->len);
          schedule_apply_lines(d->self, batch);
          batch = g_ptr_array_new_with_free_func(g_free);
          last_post_us = now_us;
        }
        break;
      }
      if (eose_seen) {
        /* After EOSE, exit after quiet period of ~150ms without new events */
        if (!any_event && now_us > quiet_deadline_us) {
          if (batch && batch->len > 0 && d && d->self) {
            g_message("timeline_refresh_worker: flushing remaining (EOSE quiet, lines=%u)", batch->len);
            schedule_apply_lines(d->self, batch);
            batch = g_ptr_array_new_with_free_func(g_free);
            last_post_us = now_us;
          }
          break;
        }
        /* Hard-cap maximum wait after EOSE to 1500ms */
        if (eose_time_us && now_us - eose_time_us > (1500 * G_TIME_SPAN_MILLISECOND)) {
          if (batch && batch->len > 0 && d && d->self) {
            g_message("timeline_refresh_worker: flushing remaining (EOSE hard-cap, lines=%u)", batch->len);
            schedule_apply_lines(d->self, batch);
            batch = g_ptr_array_new_with_free_func(g_free);
            last_post_us = now_us;
          }
          break;
        }
      }
      /* Also hard-cap per relay duration to avoid indefinite loops on busy relays */
      if (now_us > per_relay_hard_us) {
        g_debug("worker: %s hard timeout reached (%.2fs)", url, (per_relay_hard_us - started_us) / 1000000.0);
        break;
      }

      /* if nothing urgent, yield briefly */
      g_usleep(1000 * 5); /* 5ms */
    }

    /* Flush any per-relay leftovers before teardown */
    if (batch && batch->len > 0 && d && d->self) {
      g_message("timeline_refresh_worker: flushing remaining (relay end, lines=%u)", batch->len);
      schedule_apply_lines(d->self, batch);
      batch = g_ptr_array_new_with_free_func(g_free);
      last_post_us = g_get_monotonic_time();
    }
    total_collected += collected;
    g_message("worker: %s collected=%d (per-relay lim=%d, total=%d/%d)", url, collected, per_relay_lim, total_collected, lim);
    g_message("worker: moving on from %s", url);
    g_message("worker: finished relay %zu/%zu, total so far %d/%d", i+1, url_count, total_collected, lim);
    /* Minimal immediate action: close subscription to stop further events */
    nostr_subscription_close(sub, NULL);
    /* Defer heavy teardown to post-loop to avoid blocking here */
    CleanupItem *ci = g_new0(CleanupItem, 1);
    ci->sub = sub;
    ci->relay = relay;
    ci->fs = fs;
    g_ptr_array_add(deferred, ci);
  }
#endif

#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
  /* Drain deferred cleanup after processing all relays */
  if (deferred) {
    for (guint i = 0; i < deferred->len; ++i) {
      CleanupItem *ci = (CleanupItem*)g_ptr_array_index(deferred, i);
      if (!ci) continue;
      if (ci->sub) nostr_subscription_free(ci->sub);
      if (ci->relay) {
        nostr_relay_disconnect(ci->relay);
        nostr_relay_free(ci->relay);
      }
      if (ci->fs) nostr_filters_free(ci->fs);
    }
    g_ptr_array_free(deferred, TRUE);
    g_message("worker: all relays processed, total=%d", total_collected);
  }
#endif

  /* Post any remaining batch */
  if (batch && batch->len > 0 && d && d->self) {
    schedule_apply_lines(d->self, batch);
    batch = NULL; /* transferred */
  }
  /* Free legacy accumulator if still allocated */
  if (lines) g_ptr_array_unref(lines);
  g_task_return_boolean(task, TRUE);
  g_message("timeline_refresh_worker: done");
}

static void timeline_refresh_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = G_TASK(res);
  RefreshTaskData *d = (RefreshTaskData*)g_task_get_task_data(task);
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(task, &error);
  /* Get window from our task data (strong ref we manage) */
  GnostrMainWindow *self = d ? d->self : NULL;
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self)) {
    g_warning("timeline_refresh_complete: self missing or invalid");
    return;
  }
  /* Re-enable button (guard against stale/destroyed widget) */
  if (self->btn_refresh && G_IS_OBJECT(self->btn_refresh) && GTK_IS_WIDGET(self->btn_refresh))
    gtk_widget_set_sensitive(self->btn_refresh, TRUE);
  if (!ok || error) {
    show_toast(self, error && error->message ? error->message : "Failed to load timeline");
    g_clear_error(&error);
  } else {
    g_message("timeline_refresh_complete: worker signaled ok");
    show_toast(self, "Timeline updated");
  }
  /* Do not unref self: owned by application; GTask manages a temporary ref. */
}

static void timeline_refresh_async(GnostrMainWindow *self, int limit) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (self->btn_refresh) gtk_widget_set_sensitive(self->btn_refresh, FALSE);
  show_toast(self, "Refreshing timeline...");
  /* Create task without a source object; we manage a strong ref in task data */
  GTask *task = g_task_new(NULL, NULL, timeline_refresh_complete, NULL);
  RefreshTaskData *d = g_new0(RefreshTaskData, 1);
  d->self = g_object_ref(self);
  d->limit = limit;
  g_task_set_task_data(task, d, (GDestroyNotify)refresh_task_data_free);
  g_task_run_in_thread(task, timeline_refresh_worker);
  g_object_unref(task);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  start_pool_backfill(self, (int)self->default_limit);
}

/* No proxy signals: org.nostr.Signer.xml does not define approval signals. */

typedef struct {
  NostrSignerProxy *proxy; /* not owned */
  char *event_json;        /* owned */
  char *current_user;      /* owned */
  char *app_id;            /* owned */
} PostCtx;

static void post_ctx_free(PostCtx *ctx){
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx->current_user);
  g_free(ctx->app_id);
  g_free(ctx);
}

static void on_sign_event_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GError *error = NULL;
  char *signature = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signature, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("SignEvent async failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    /* UI feedback */
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Signing failed");
      gtk_alert_dialog_set_detail(dlg, "The signer could not sign your event. Check your identity and permissions.");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    PostCtx *ctx = (PostCtx*)g_object_steal_data(G_OBJECT(self), "postctx-temp");
    post_ctx_free(ctx);
    return;
  }
  g_message("signature: %s", signature ? signature : "<null>");
  g_free(signature);
  PostCtx *ctx = (PostCtx*)g_object_steal_data(G_OBJECT(self), "postctx-temp");
  post_ctx_free(ctx);
}

static void on_get_pubkey_done(GObject *source, GAsyncResult *res, gpointer user_data){
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  PostCtx *ctx = (PostCtx*)g_object_get_data(G_OBJECT(self), "postctx-temp");
  GError *error = NULL;
  char *npub = NULL;
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_finish(proxy, &npub, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("GetPublicKey failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Identity unavailable");
      gtk_alert_dialog_set_detail(dlg, "No signing identity is configured. Import or select an identity in GNostr Signer.");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    post_ctx_free(ctx);
    return;
  }
  g_message("using identity npub=%s", npub ? npub : "<null>");
  /* Ensure SignEvent uses the same identity returned here */
  g_clear_pointer(&ctx->current_user, g_free);
  ctx->current_user = g_strdup(npub ? npub : "");
  g_free(npub);
  /* Proceed to SignEvent */
  g_message("calling SignEvent (async)... json-len=%zu", strlen(ctx->event_json));
  nostr_org_nostr_signer_call_sign_event(
      proxy,
      ctx->event_json,
      ctx->current_user,
      ctx->app_id,
      NULL,
      on_sign_event_done,
      self);
}

static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data) {
  g_message("on_composer_post_requested enter composer=%p user_data=%p", (void*)composer, user_data);
  if (!GNOSTR_IS_COMPOSER(composer)) { g_warning("composer instance invalid"); return; }
  if (!GNOSTR_IS_MAIN_WINDOW(user_data)) { g_warning("main window user_data invalid"); return; }
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!text || !*text) {
    g_message("empty post ignored");
    return;
  }
  g_message("post-requested: %s", text);

  // Build minimal Nostr event JSON (kind=1) for signing
  time_t now = time(NULL);
  gchar *escaped = g_strescape(text, NULL);
  gchar *event_json = g_strdup_printf("{\"kind\":1,\"created_at\":%ld,\"tags\":[],\"content\":\"%s\"}", (long)now, escaped ? escaped : "");
  g_free(escaped);

  GError *error = NULL;
  g_message("acquiring signer proxy...");
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
  if (!proxy) {
    g_warning("Signer proxy unavailable: %s", error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(event_json);
    return;
  }

  // Ensure the service is actually present on the bus; otherwise avoid long timeouts
  const gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!owner || !*owner) {
    g_warning("Signer service is not running (no name owner). Start gnostr-signer-daemon and retry.");
    g_free(event_json);
    return;
  }

  // For SignEvent, allow sufficient time for user approval in the signer UI
  g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(proxy), 600000); // 10 minutes

  PostCtx *ctx = g_new0(PostCtx, 1);
  ctx->proxy = proxy;
  ctx->event_json = event_json; /* take ownership */
  ctx->current_user = g_strdup(""); // will be set to npub after GetPublicKey
  ctx->app_id = g_strdup("org.gnostr.Client");
  g_object_set_data_full(G_OBJECT(self), "postctx-temp", ctx, (GDestroyNotify)post_ctx_free);

  /* Pre-check identity exists and is configured */
  g_message("calling GetPublicKey (async) to verify identity...");
  nostr_org_nostr_signer_call_get_public_key(
      proxy,
      NULL,
      on_get_pubkey_done,
      self);

  // TODO: assemble full event (id/sig/pubkey), publish to relay, push into timeline
}

static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_main_window_dispose;
  gobj_class->finalize = gnostr_main_window_finalize;
  /* Ensure custom widget types are registered before template parsing */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_COMPOSER);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_relays);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_menu);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_avatar);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, avatar_popover);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, composer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_label);
  gtk_widget_class_bind_template_callback(widget_class, on_settings_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_relays_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_avatar_login_local_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_avatar_pair_remote_clicked);
}

static void gnostr_main_window_init(GnostrMainWindow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_return_if_fail(self->composer != NULL);
  /* Initialize weak refs to template children needed in async paths */
  g_weak_ref_init(&self->timeline_ref, self->timeline);
  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Initialize metadata cache */
  self->meta_by_pubkey = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, user_meta_free);
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
  /* Ensure avatar menu button has a popover attached (from template) */
  if (self->btn_avatar && self->avatar_popover) {
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->btn_avatar), GTK_WIDGET(self->avatar_popover));
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
  /* Initialize tree model for timeline view */
  if (self->timeline && GNOSTR_IS_TIMELINE_VIEW(self->timeline)) {
    extern GType timeline_item_get_type(void);
    self->thread_roots = g_list_store_new(timeline_item_get_type());
    self->nodes_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gnostr_timeline_view_set_tree_roots(GNOSTR_TIMELINE_VIEW(self->timeline), G_LIST_MODEL(self->thread_roots));
  } else {
    g_warning("timeline is not a GnostrTimelineView at init: type=%s", self->timeline ? G_OBJECT_TYPE_NAME(self->timeline) : "(null)");
  }
  /* Seed initial items so Timeline page isn't empty */
  g_timeout_add_once(150, (GSourceOnceFunc)initial_refresh_timeout_cb, self);

  /* If backfill requested via env, start periodic timer */
  if (self->backfill_interval_sec > 0) {
    self->backfill_source_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
        self->backfill_interval_sec,
        (GSourceFunc)periodic_backfill_cb,
        g_object_ref(self),
        (GDestroyNotify)g_object_unref);
  }

  /* Start live streaming via SimplePool */
  start_pool_live(self);
  /* If identity is already set, fetch its profile metadata and subscribe for updates */
  gnostr_profile_fetch_async(self);
  start_profile_subscription(self);
}

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}
