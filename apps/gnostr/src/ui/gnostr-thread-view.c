#include "gnostr-thread-view.h"
#include "note_card_row.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-profile-provider.h"
#include "../storage_ndb.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "../util/relays.h"
#include "../util/utils.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr_simple_pool.h"
#include <nostr/nip19/nip19.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <string.h>
#include <time.h>
#include <json.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-thread-view.ui"

/* Check if user is logged in by checking GSettings current-npub.
 * Returns TRUE if logged in, FALSE otherwise. */
static gboolean is_user_logged_in(void) {
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return FALSE;
  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);
  gboolean logged_in = (npub && *npub);
  g_free(npub);
  return logged_in;
}

/* Maximum thread depth to display */
#define MAX_THREAD_DEPTH 10

/* Maximum events to fetch for a thread */
#define MAX_THREAD_EVENTS 100

/* Thread event item for internal use */
typedef struct {
  char *id_hex;
  char *pubkey_hex;
  char *content;
  char *root_id;
  char *parent_id;
  char *root_relay_hint;   /* NIP-10 relay hint for root event */
  char *parent_relay_hint; /* NIP-10 relay hint for parent event */
  gint64 created_at;
  guint depth;
  /* Profile info (resolved asynchronously) */
  char *display_name;
  char *handle;
  char *avatar_url;
  char *nip05;
} ThreadEventItem;

static void thread_event_item_free(ThreadEventItem *item) {
  if (!item) return;
  g_free(item->id_hex);
  g_free(item->pubkey_hex);
  g_free(item->content);
  g_free(item->root_id);
  g_free(item->parent_id);
  g_free(item->root_relay_hint);
  g_free(item->parent_relay_hint);
  g_free(item->display_name);
  g_free(item->handle);
  g_free(item->avatar_url);
  g_free(item->nip05);
  g_free(item);
}

struct _GnostrThreadView {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root_box;
  GtkWidget *header_box;
  GtkWidget *btn_close;
  GtkWidget *title_label;
  GtkWidget *scroll_window;
  GtkWidget *thread_list_box;
  GtkWidget *loading_box;
  GtkWidget *loading_spinner;
  GtkWidget *empty_box;
  GtkWidget *empty_label;

  /* State */
  char *focus_event_id;
  char *thread_root_id;
  GHashTable *events_by_id;    /* id_hex -> ThreadEventItem* (owned) */
  GPtrArray *sorted_events;    /* ThreadEventItem* (borrowed refs) */
  GCancellable *fetch_cancellable;
  /* Uses gnostr_get_shared_query_pool() instead of per-widget pool */
  gboolean is_loading;

  /* Profile fetch tracking */
  GHashTable *profiles_requested; /* pubkey_hex -> gboolean */

  /* nostrc-46g: Track ancestor event IDs we've already attempted to fetch
   * to prevent duplicate requests and enable proper chain traversal */
  GHashTable *ancestors_fetched; /* event_id_hex -> gboolean */
  guint ancestor_fetch_depth;    /* Current chain traversal depth */

  /* nostrc-50t: nostrdb subscription for live thread updates */
  uint64_t ndb_sub_thread;       /* Subscription ID for thread events */
  guint rebuild_pending_id;      /* Timeout source for debounced UI rebuild */
};

G_DEFINE_TYPE(GnostrThreadView, gnostr_thread_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLOSE_REQUESTED,
  SIGNAL_NOTE_ACTIVATED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_NEED_PROFILE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void load_thread(GnostrThreadView *self);
static void rebuild_thread_ui(GnostrThreadView *self);
static void set_loading_state(GnostrThreadView *self, gboolean loading);
static void fetch_thread_from_relays(GnostrThreadView *self);
static void on_root_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void setup_thread_subscription(GnostrThreadView *self);
static void teardown_thread_subscription(GnostrThreadView *self);

/* Helper: convert hex string to 32-byte binary */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i*2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Helper: convert 32-byte binary to hex string */
static void bytes_to_hex(const unsigned char *bin, char *hex) {
  static const char hexchars[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex[i*2] = hexchars[(bin[i] >> 4) & 0x0f];
    hex[i*2 + 1] = hexchars[bin[i] & 0x0f];
  }
  hex[64] = '\0';
}

/* Context for NIP-10 tag parsing callback */
typedef struct {
  char **root_id;
  char **reply_id;
  char **root_relay_hint;
  char **reply_relay_hint;
  char *first_e_id;
  char *first_e_relay;
  char *last_e_id;
  char *last_e_relay;
} Nip10ParseContext;

/* Helper: validate and duplicate relay URL */
static char *dup_relay_hint(const char *url) {
  if (!url || !*url) return NULL;
  /* Basic validation: must start with ws:// or wss:// */
  if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) return NULL;
  return g_strdup(url);
}

/* Callback for processing each tag in the tags array */
static bool parse_nip10_tag_callback(size_t index, const char *tag_json, void *user_data) {
  (void)index;
  Nip10ParseContext *ctx = (Nip10ParseContext *)user_data;
  if (!tag_json || !ctx) return true;

  /* Validate tag is an array */
  if (!nostr_json_is_array_str(tag_json)) return true;

  /* Get tag type (first element) */
  char *tag_type = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 0, &tag_type) != 0 || !tag_type) {
    return true;
  }

  /* Only process "e" tags */
  if (strcmp(tag_type, "e") != 0) {
    free(tag_type);
    return true;
  }
  free(tag_type);

  /* Get event ID (second element) */
  char *event_id = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 1, &event_id) != 0 || !event_id) {
    return true;
  }

  if (strlen(event_id) != 64) {
    free(event_id);
    return true;
  }

  /* Get relay hint (third element) - NIP-10 relay hint */
  char *relay_hint = NULL;
  nostr_json_get_array_string(tag_json, NULL, 2, &relay_hint);

  /* Check for marker (NIP-10 preferred markers) - fourth element */
  char *marker = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 3, &marker) == 0 && marker && *marker) {
    if (strcmp(marker, "root") == 0) {
      g_free(*ctx->root_id);
      *ctx->root_id = g_strdup(event_id);
      if (ctx->root_relay_hint) {
        g_free(*ctx->root_relay_hint);
        *ctx->root_relay_hint = dup_relay_hint(relay_hint);
      }
    } else if (strcmp(marker, "reply") == 0) {
      g_free(*ctx->reply_id);
      *ctx->reply_id = g_strdup(event_id);
      if (ctx->reply_relay_hint) {
        g_free(*ctx->reply_relay_hint);
        *ctx->reply_relay_hint = dup_relay_hint(relay_hint);
      }
    }
    free(marker);
    free(event_id);
    free(relay_hint);
    return true;
  }
  free(marker);

  /* Fall back to positional interpretation */
  if (!ctx->first_e_id) {
    ctx->first_e_id = g_strdup(event_id);
    g_free(ctx->first_e_relay);
    ctx->first_e_relay = dup_relay_hint(relay_hint);
  }
  g_free(ctx->last_e_id);
  ctx->last_e_id = g_strdup(event_id);
  g_free(ctx->last_e_relay);
  ctx->last_e_relay = dup_relay_hint(relay_hint);

  free(event_id);
  free(relay_hint);
  return true;
}

/* Parse NIP-10 tags from a nostr event to get root and reply IDs with relay hints */
static void parse_nip10_from_json_full(const char *json_str, char **root_id, char **reply_id,
                                        char **root_relay_hint, char **reply_relay_hint) {
  *root_id = NULL;
  *reply_id = NULL;
  if (root_relay_hint) *root_relay_hint = NULL;
  if (reply_relay_hint) *reply_relay_hint = NULL;
  if (!json_str) return;

  if (!nostr_json_is_valid(json_str)) return;

  Nip10ParseContext ctx = {
    .root_id = root_id,
    .reply_id = reply_id,
    .root_relay_hint = root_relay_hint,
    .reply_relay_hint = reply_relay_hint,
    .first_e_id = NULL,
    .first_e_relay = NULL,
    .last_e_id = NULL,
    .last_e_relay = NULL
  };

  /* Iterate through tags array */
  nostr_json_array_foreach(json_str, "tags", parse_nip10_tag_callback, &ctx);

  /* If no markers found, use positional (NIP-10 fallback):
   * - First e-tag = root
   * - Last e-tag = reply target (event being replied to)
   * When there's only one e-tag (first == last), the event is a direct reply
   * to that event, so both root and reply should point to it.
   * nostrc-5b8: Fix single e-tag case where reply_id was incorrectly left NULL */
  if (!*root_id && ctx.first_e_id) {
    *root_id = g_strdup(ctx.first_e_id);
    if (root_relay_hint && ctx.first_e_relay) {
      *root_relay_hint = g_strdup(ctx.first_e_relay);
    }
  }
  if (!*reply_id && ctx.last_e_id) {
    /* Any e-tag (even if same as root) indicates this is a reply */
    *reply_id = g_strdup(ctx.last_e_id);
    if (reply_relay_hint && ctx.last_e_relay) {
      *reply_relay_hint = g_strdup(ctx.last_e_relay);
    }
  }
  /* nostrc-mef: NIP-10 "root-only" marker case.
   * When an event has a "root" marker but NO "reply" marker, it means
   * the event is a direct reply to the root. Set reply_id = root_id. */
  if (!*reply_id && *root_id) {
    *reply_id = g_strdup(*root_id);
    if (reply_relay_hint && root_relay_hint && *root_relay_hint) {
      *reply_relay_hint = g_strdup(*root_relay_hint);
    }
  }

  g_free(ctx.first_e_id);
  g_free(ctx.first_e_relay);
  g_free(ctx.last_e_id);
  g_free(ctx.last_e_relay);
}

/* Parse NIP-10 tags from a nostr event to get root and reply IDs (legacy wrapper) */
static void parse_nip10_from_json(const char *json_str, char **root_id, char **reply_id) {
  parse_nip10_from_json_full(json_str, root_id, reply_id, NULL, NULL);
}

/* Dispose */
static void gnostr_thread_view_dispose(GObject *obj) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(obj);

  /* nostrc-50t: Teardown nostrdb subscription */
  teardown_thread_subscription(self);

  /* Cancel pending fetch */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }

  /* Cancel pending rebuild timeout */
  if (self->rebuild_pending_id > 0) {
    g_source_remove(self->rebuild_pending_id);
    self->rebuild_pending_id = 0;
  }

  /* Clear hash tables */
  g_clear_pointer(&self->events_by_id, g_hash_table_unref);
  g_clear_pointer(&self->profiles_requested, g_hash_table_unref);
  g_clear_pointer(&self->ancestors_fetched, g_hash_table_unref);

  /* Clear array (items are borrowed, owned by hash table) */
  if (self->sorted_events) {
    g_ptr_array_free(self->sorted_events, TRUE);
    self->sorted_events = NULL;
  }

  /* Shared query pool is managed globally - do not clear here */

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_THREAD_VIEW);
  G_OBJECT_CLASS(gnostr_thread_view_parent_class)->dispose(obj);
}

/* Finalize */
static void gnostr_thread_view_finalize(GObject *obj) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(obj);
  g_clear_pointer(&self->focus_event_id, g_free);
  g_clear_pointer(&self->thread_root_id, g_free);
  G_OBJECT_CLASS(gnostr_thread_view_parent_class)->finalize(obj);
}

/* Signal handlers */
static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  (void)btn;
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

/* Note card signal handlers */
static void on_note_open_profile(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  (void)row;
  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void on_note_view_thread(GnostrNoteCardRow *row, const char *root_event_id, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  (void)row;
  /* Navigate to the new thread root */
  gnostr_thread_view_set_thread_root(self, root_event_id);
}

/* NIP-56: Handler for report-note-requested signal - relay to main window */
static void on_note_report_requested(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  (void)user_data;
  /* Walk up the widget tree to find the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to report note */
      extern void gnostr_main_window_request_report_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
      gnostr_main_window_request_report_note(widget, id_hex, pubkey_hex);
      break;
    }
  }
}

/* Class init */
static void gnostr_thread_view_class_init(GnostrThreadViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);

  obj_class->dispose = gnostr_thread_view_dispose;
  obj_class->finalize = gnostr_thread_view_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, root_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, header_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, btn_close);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, title_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, scroll_window);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, thread_list_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, loading_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, empty_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, empty_label);

  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new(
    "close-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_NOTE_ACTIVATED] = g_signal_new(
    "note-activated",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
    "open-profile",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_NEED_PROFILE] = g_signal_new(
    "need-profile",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

/* Instance init */
static void gnostr_thread_view_init(GnostrThreadView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize state */
  self->events_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                             (GDestroyNotify)thread_event_item_free);
  self->sorted_events = g_ptr_array_new();
  self->profiles_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* nostrc-46g: Track fetched ancestors to prevent duplicate requests and enable chain traversal */
  self->ancestors_fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->ancestor_fetch_depth = 0;
  /* Uses shared query pool from gnostr_get_shared_query_pool() */

  /* Connect close button */
  if (self->btn_close) {
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
  }

  /* Initial state */
  set_loading_state(self, FALSE);
}

/* Public API */
GtkWidget *gnostr_thread_view_new(void) {
  return g_object_new(GNOSTR_TYPE_THREAD_VIEW, NULL);
}

void gnostr_thread_view_set_focus_event(GnostrThreadView *self, const char *event_id_hex) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!event_id_hex || strlen(event_id_hex) != 64) {
    g_warning("[THREAD_VIEW] Invalid event ID");
    return;
  }

  /* Store focus event */
  g_free(self->focus_event_id);
  self->focus_event_id = g_strdup(event_id_hex);

  /* Load the thread */
  load_thread(self);
}

void gnostr_thread_view_set_thread_root(GnostrThreadView *self, const char *root_event_id_hex) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!root_event_id_hex || strlen(root_event_id_hex) != 64) {
    g_warning("[THREAD_VIEW] Invalid root event ID");
    return;
  }

  /* Clear existing data */
  gnostr_thread_view_clear(self);

  /* Store root ID */
  g_free(self->thread_root_id);
  self->thread_root_id = g_strdup(root_event_id_hex);

  /* Also set as focus if no focus set */
  if (!self->focus_event_id) {
    self->focus_event_id = g_strdup(root_event_id_hex);
  }

  /* Load the thread */
  load_thread(self);
}

void gnostr_thread_view_clear(GnostrThreadView *self) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  /* nostrc-50t: Teardown nostrdb subscription when clearing */
  teardown_thread_subscription(self);

  /* Cancel pending fetch */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }

  /* Cancel pending rebuild timeout */
  if (self->rebuild_pending_id > 0) {
    g_source_remove(self->rebuild_pending_id);
    self->rebuild_pending_id = 0;
  }

  /* Clear events */
  if (self->events_by_id) {
    g_hash_table_remove_all(self->events_by_id);
  }
  if (self->sorted_events) {
    g_ptr_array_set_size(self->sorted_events, 0);
  }
  if (self->profiles_requested) {
    g_hash_table_remove_all(self->profiles_requested);
  }
  /* nostrc-46g: Clear ancestor tracking on view clear */
  if (self->ancestors_fetched) {
    g_hash_table_remove_all(self->ancestors_fetched);
  }
  self->ancestor_fetch_depth = 0;

  /* Clear UI */
  if (self->thread_list_box) {
    GtkWidget *child = gtk_widget_get_first_child(self->thread_list_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->thread_list_box), child);
      child = next;
    }
  }

  /* Clear IDs */
  g_clear_pointer(&self->focus_event_id, g_free);
  g_clear_pointer(&self->thread_root_id, g_free);

  set_loading_state(self, FALSE);
}

void gnostr_thread_view_refresh(GnostrThreadView *self) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (self->focus_event_id || self->thread_root_id) {
    load_thread(self);
  }
}

const char *gnostr_thread_view_get_focus_event_id(GnostrThreadView *self) {
  g_return_val_if_fail(GNOSTR_IS_THREAD_VIEW(self), NULL);
  return self->focus_event_id;
}

const char *gnostr_thread_view_get_thread_root_id(GnostrThreadView *self) {
  g_return_val_if_fail(GNOSTR_IS_THREAD_VIEW(self), NULL);
  return self->thread_root_id;
}

/* Internal: set loading state */
static void set_loading_state(GnostrThreadView *self, gboolean loading) {
  self->is_loading = loading;

  if (self->loading_box) {
    gtk_widget_set_visible(self->loading_box, loading);
  }
  if (self->loading_spinner) {
    if (loading) {
      gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    } else {
      gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
    }
  }
  if (self->scroll_window) {
    gtk_widget_set_visible(self->scroll_window, !loading);
  }
  if (self->empty_box) {
    gtk_widget_set_visible(self->empty_box, FALSE);
  }
}

/* Internal: show empty state */
static void show_empty_state(GnostrThreadView *self, const char *message) {
  set_loading_state(self, FALSE);

  if (self->scroll_window) {
    gtk_widget_set_visible(self->scroll_window, FALSE);
  }
  if (self->empty_box) {
    gtk_widget_set_visible(self->empty_box, TRUE);
  }
  if (self->empty_label && message) {
    gtk_label_set_text(GTK_LABEL(self->empty_label), message);
  }
}

/* Internal: add event to hash table */
static ThreadEventItem *add_event_from_json(GnostrThreadView *self, const char *json_str) {
  if (!json_str || !*json_str) return NULL;

  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, json_str) != 0) {
    if (evt) nostr_event_free(evt);
    return NULL;
  }

  const char *id = nostr_event_get_id(evt);
  if (!id || strlen(id) != 64) {
    nostr_event_free(evt);
    return NULL;
  }

  /* Check if already exists */
  if (g_hash_table_contains(self->events_by_id, id)) {
    nostr_event_free(evt);
    return g_hash_table_lookup(self->events_by_id, id);
  }

  /* Create new item */
  ThreadEventItem *item = g_new0(ThreadEventItem, 1);
  item->id_hex = g_strdup(id);
  item->pubkey_hex = g_strdup(nostr_event_get_pubkey(evt));
  item->content = g_strdup(nostr_event_get_content(evt));
  item->created_at = (gint64)nostr_event_get_created_at(evt);

  /* Parse NIP-10 tags with relay hints */
  parse_nip10_from_json_full(json_str, &item->root_id, &item->parent_id,
                              &item->root_relay_hint, &item->parent_relay_hint);

  nostr_event_free(evt);

  /* Add to hash table (owns the item) */
  g_hash_table_insert(self->events_by_id, item->id_hex, item);

  return item;
}

/* Internal: fetch profile for pubkey using profile provider.
 * If not found in cache/nostrdb, emits "need-profile" signal to request fetch from relays.
 * nostrc-oz5: Always populate profile fields on the item, even if we've already requested
 * the profile for another item from the same author. The profiles_requested hash only
 * prevents duplicate relay fetch requests, not duplicate cache lookups. */
static void fetch_profile_for_event(GnostrThreadView *self, ThreadEventItem *item) {
  if (!item || !item->pubkey_hex) return;

  /* Check if we've already requested this profile from relays */
  gboolean already_requested = g_hash_table_contains(self->profiles_requested, item->pubkey_hex);

  /* Always try to get profile from provider (checks cache + nostrdb).
   * Each ThreadEventItem has its own profile fields that need populating,
   * even if another item from the same author was already processed. */
  GnostrProfileMeta *meta = gnostr_profile_provider_get(item->pubkey_hex);
  if (meta) {
    /* Profile found - populate the item (freeing any stale values first) */
    if (meta->display_name && *meta->display_name) {
      g_free(item->display_name);
      item->display_name = g_strdup(meta->display_name);
    } else if (meta->name && *meta->name && !item->display_name) {
      item->display_name = g_strdup(meta->name);
    }
    if (meta->name && *meta->name) {
      g_free(item->handle);
      item->handle = g_strdup_printf("@%s", meta->name);
    }
    if (meta->picture && *meta->picture) {
      g_free(item->avatar_url);
      item->avatar_url = g_strdup(meta->picture);
    }
    if (meta->nip05 && *meta->nip05) {
      g_free(item->nip05);
      item->nip05 = g_strdup(meta->nip05);
    }
    gnostr_profile_meta_free(meta);
  } else if (!already_requested) {
    /* Profile not in cache/db and we haven't requested yet - request fetch from relays */
    g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, item->pubkey_hex);
  }

  /* Track that we've requested this profile (to prevent duplicate relay fetches) */
  if (!already_requested) {
    g_hash_table_insert(self->profiles_requested, g_strdup(item->pubkey_hex), GINT_TO_POINTER(1));
  }
}

/* Compare function for sorting by created_at */
static gint compare_events_by_time(gconstpointer a, gconstpointer b) {
  const ThreadEventItem *item_a = *(const ThreadEventItem **)a;
  const ThreadEventItem *item_b = *(const ThreadEventItem **)b;

  if (item_a->created_at < item_b->created_at) return -1;
  if (item_a->created_at > item_b->created_at) return 1;
  return 0;
}

/* Calculate depth for each event in the thread */
static void calculate_thread_depths(GnostrThreadView *self) {
  /* Find root (no parent or self-referencing root) */
  const char *root_id = self->thread_root_id;

  /* First pass: find actual root if not set */
  if (!root_id) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->events_by_id);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      ThreadEventItem *item = (ThreadEventItem *)value;
      if (!item->parent_id && !item->root_id) {
        root_id = item->id_hex;
        break;
      }
    }
  }

  /* Build parent->children map */
  GHashTable *children_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                                    (GDestroyNotify)g_ptr_array_unref);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    const char *parent = item->parent_id ? item->parent_id : item->root_id;

    if (parent) {
      GPtrArray *children = g_hash_table_lookup(children_map, parent);
      if (!children) {
        children = g_ptr_array_new();
        g_hash_table_insert(children_map, (gpointer)parent, children);
      }
      g_ptr_array_add(children, item);
    }
  }

  /* BFS to assign depths */
  GQueue *queue = g_queue_new();

  /* Start with root items (items without parent in our set) */
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    const char *parent = item->parent_id ? item->parent_id : item->root_id;

    /* If no parent, or parent not in our set, this is a root */
    if (!parent || !g_hash_table_contains(self->events_by_id, parent)) {
      item->depth = 0;
      g_queue_push_tail(queue, item);
    }
  }

  while (!g_queue_is_empty(queue)) {
    ThreadEventItem *item = g_queue_pop_head(queue);
    GPtrArray *children = g_hash_table_lookup(children_map, item->id_hex);

    if (children) {
      for (guint i = 0; i < children->len; i++) {
        ThreadEventItem *child = g_ptr_array_index(children, i);
        child->depth = item->depth + 1;
        if (child->depth > MAX_THREAD_DEPTH) {
          child->depth = MAX_THREAD_DEPTH;
        }
        g_queue_push_tail(queue, child);
      }
    }
  }

  g_queue_free(queue);
  g_hash_table_unref(children_map);
}

/* Internal: rebuild sorted events array */
static void rebuild_sorted_events(GnostrThreadView *self) {
  g_ptr_array_set_size(self->sorted_events, 0);

  /* Calculate depths first */
  calculate_thread_depths(self);

  /* Add all events to array */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_ptr_array_add(self->sorted_events, value);
  }

  /* Sort by created_at */
  g_ptr_array_sort(self->sorted_events, compare_events_by_time);
}

/* Internal: create note card for an event */
static GtkWidget *create_note_card_for_item(GnostrThreadView *self, ThreadEventItem *item) {
  GnostrNoteCardRow *row = gnostr_note_card_row_new();

  /* Fetch profile if not already done */
  fetch_profile_for_event(self, item);

  /* Set author info */
  const char *display = item->display_name;
  const char *handle = item->handle;
  if (!display && !handle && item->pubkey_hex) {
    /* Fallback to truncated pubkey */
    char fallback[20];
    snprintf(fallback, sizeof(fallback), "%.8s...", item->pubkey_hex);
    gnostr_note_card_row_set_author(row, fallback, NULL, item->avatar_url);
  } else {
    gnostr_note_card_row_set_author(row, display, handle, item->avatar_url);
  }

  /* Set timestamp */
  gnostr_note_card_row_set_timestamp(row, item->created_at, NULL);

  /* Set content */
  gnostr_note_card_row_set_content(row, item->content);

  /* Set depth */
  gnostr_note_card_row_set_depth(row, item->depth);

  /* Set IDs */
  gnostr_note_card_row_set_ids(row, item->id_hex, item->root_id, item->pubkey_hex);

  /* Set thread info */
  gboolean is_reply = (item->parent_id != NULL);
  gnostr_note_card_row_set_thread_info(row, item->root_id, item->parent_id, NULL, is_reply);

  /* Set NIP-05 if available */
  if (item->nip05 && item->pubkey_hex) {
    gnostr_note_card_row_set_nip05(row, item->nip05, item->pubkey_hex);
  }

  /* Set login state for authentication-required buttons */
  gnostr_note_card_row_set_logged_in(row, is_user_logged_in());

  /* Connect signals */
  g_signal_connect(row, "open-profile", G_CALLBACK(on_note_open_profile), self);
  g_signal_connect(row, "view-thread-requested", G_CALLBACK(on_note_view_thread), self);
  g_signal_connect(row, "report-note-requested", G_CALLBACK(on_note_report_requested), self);

  /* Highlight focus event */
  if (self->focus_event_id && g_strcmp0(item->id_hex, self->focus_event_id) == 0) {
    gtk_widget_add_css_class(GTK_WIDGET(row), "thread-focus-note");
  }

  return GTK_WIDGET(row);
}

/* Internal: rebuild UI from sorted events */
static void rebuild_thread_ui(GnostrThreadView *self) {
  if (!self->thread_list_box) return;

  /* Clear existing widgets */
  GtkWidget *child = gtk_widget_get_first_child(self->thread_list_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->thread_list_box), child);
    child = next;
  }

  /* Rebuild sorted array */
  rebuild_sorted_events(self);

  if (self->sorted_events->len == 0) {
    show_empty_state(self, "No messages in this thread");
    return;
  }

  /* Update title */
  if (self->title_label) {
    char title[64];
    snprintf(title, sizeof(title), "Thread (%u notes)", self->sorted_events->len);
    gtk_label_set_text(GTK_LABEL(self->title_label), title);
  }

  /* Add note cards */
  for (guint i = 0; i < self->sorted_events->len; i++) {
    ThreadEventItem *item = g_ptr_array_index(self->sorted_events, i);
    GtkWidget *card = create_note_card_for_item(self, item);
    gtk_box_append(GTK_BOX(self->thread_list_box), card);
  }

  /* Show the scroll window */
  set_loading_state(self, FALSE);
  if (self->scroll_window) {
    gtk_widget_set_visible(self->scroll_window, TRUE);
  }

  /* Scroll to focus event if set */
  if (self->focus_event_id) {
    /* Find the focus event index */
    for (guint i = 0; i < self->sorted_events->len; i++) {
      ThreadEventItem *item = g_ptr_array_index(self->sorted_events, i);
      if (g_strcmp0(item->id_hex, self->focus_event_id) == 0) {
        /* Scroll to this position after layout */
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll_window));
        if (vadj && self->sorted_events->len > 0) {
          /* Approximate position based on index */
          double fraction = (double)i / (double)self->sorted_events->len;
          double range = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_lower(vadj);
          gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj) + fraction * range);
        }
        break;
      }
    }
  }
}

/* Forward declaration */
static void fetch_missing_ancestors(GnostrThreadView *self);

/* Callback for relay query completion */
static void on_thread_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);

  if (!GNOSTR_IS_THREAD_VIEW(self)) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("[THREAD_VIEW] Query failed: %s", error->message);
      show_empty_state(self, "Failed to load thread");
    }
    g_error_free(error);
    return;
  }

  if (!results || results->len == 0) {
    g_debug("[THREAD_VIEW] No events found from relays");
    if (g_hash_table_size(self->events_by_id) == 0) {
      show_empty_state(self, "Thread not found");
    } else {
      rebuild_thread_ui(self);
    }
    if (results) g_ptr_array_unref(results);
    return;
  }

  g_debug("[THREAD_VIEW] Received %u events from relays", results->len);

  /* Add events to our collection */
  for (guint i = 0; i < results->len; i++) {
    const char *json = g_ptr_array_index(results, i);
    if (json) {
      /* Ingest into nostrdb for future use */
      storage_ndb_ingest_event_json(json, NULL);
      add_event_from_json(self, json);
    }
  }

  g_ptr_array_unref(results);

  /* Rebuild UI */
  rebuild_thread_ui(self);

  /* Check if new events reference ancestors we don't have yet */
  fetch_missing_ancestors(self);
}

/* Callback for root event fetch completion */
static void on_root_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);

  if (!GNOSTR_IS_THREAD_VIEW(self)) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("[THREAD_VIEW] Root fetch failed: %s", error->message);
    }
    g_error_free(error);
    return;
  }

  if (results && results->len > 0) {
    g_debug("[THREAD_VIEW] Received %u root/ancestor events from relays", results->len);

    /* Add events to our collection */
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (json) {
        /* Ingest into nostrdb for future use */
        storage_ndb_ingest_event_json(json, NULL);
        add_event_from_json(self, json);
      }
    }

    /* Rebuild UI with new events */
    rebuild_thread_ui(self);

    /* Check if new events reference ancestors we don't have yet */
    fetch_missing_ancestors(self);
  }

  if (results) g_ptr_array_unref(results);
}

/* Callback for missing ancestor fetch completion.
 * nostrc-46g: Improved to continue chain traversal until root is reached. */
static void on_missing_ancestors_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);

  if (!GNOSTR_IS_THREAD_VIEW(self)) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("[THREAD_VIEW] Missing ancestors fetch failed: %s", error->message);
    }
    g_error_free(error);
    /* nostrc-46g: Even on error, try to continue chain traversal with what we have */
    fetch_missing_ancestors(self);
    return;
  }

  gboolean found_new_events = FALSE;

  if (results && results->len > 0) {
    g_debug("[THREAD_VIEW] Fetched %u missing ancestor events", results->len);

    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (json) {
        storage_ndb_ingest_event_json(json, NULL);
        ThreadEventItem *item = add_event_from_json(self, json);
        if (item) {
          found_new_events = TRUE;
        }
      }
    }

    /* Rebuild UI with new events */
    rebuild_thread_ui(self);
  }

  if (results) g_ptr_array_unref(results);

  /* nostrc-46g: Continue chain traversal if we found new events.
   * New events may reference additional ancestors we need to fetch. */
  if (found_new_events) {
    fetch_missing_ancestors(self);
  } else {
    g_debug("[THREAD_VIEW] No new ancestor events found, chain traversal complete");
  }
}

/* nostrc-46g: Maximum depth for ancestor chain traversal to prevent infinite loops */
#define MAX_ANCESTOR_FETCH_DEPTH 50

/* Helper to add relay hint URL to array if valid and not already present */
static void add_relay_hint_if_unique(GPtrArray *relay_arr, const char *hint) {
  if (!hint || !*hint) return;
  /* Basic validation: must start with ws:// or wss:// */
  if (strncmp(hint, "ws://", 5) != 0 && strncmp(hint, "wss://", 6) != 0) return;

  /* Check for duplicates */
  for (guint i = 0; i < relay_arr->len; i++) {
    if (g_strcmp0(g_ptr_array_index(relay_arr, i), hint) == 0) return;
  }
  g_ptr_array_add(relay_arr, g_strdup(hint));
}

/* Internal: fetch any missing parent/root events referenced by loaded events.
 * This is called after receiving new events to ensure complete thread chains.
 * nostrc-46g: Improved to track already-fetched ancestors and properly traverse
 * the full chain to the root event.
 * nostrc-7r5: Now uses NIP-10 relay hints from e-tags to query hinted relays. */
static void fetch_missing_ancestors(GnostrThreadView *self) {
  if (!self || g_hash_table_size(self->events_by_id) == 0) return;

  /* nostrc-46g: Check depth limit to prevent infinite traversal */
  if (self->ancestor_fetch_depth >= MAX_ANCESTOR_FETCH_DEPTH) {
    g_debug("[THREAD_VIEW] Reached max ancestor fetch depth (%d), stopping chain traversal",
            MAX_ANCESTOR_FETCH_DEPTH);
    return;
  }

  /* Collect missing event IDs and their relay hints */
  GPtrArray *missing_ids = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *relay_hints = g_ptr_array_new_with_free_func(g_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;

    /* Check parent_id - need to fetch if not in events and not already attempted */
    if (item->parent_id && strlen(item->parent_id) == 64 &&
        !g_hash_table_contains(self->events_by_id, item->parent_id) &&
        !g_hash_table_contains(self->ancestors_fetched, item->parent_id)) {
      /* Check if we already have this in our missing list */
      gboolean found = FALSE;
      for (guint i = 0; i < missing_ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(missing_ids, i), item->parent_id) == 0) {
          found = TRUE;
          break;
        }
      }
      if (!found) {
        g_ptr_array_add(missing_ids, g_strdup(item->parent_id));
        /* nostrc-7r5: Collect relay hint for parent */
        add_relay_hint_if_unique(relay_hints, item->parent_relay_hint);
        /* nostrc-46g: Mark as attempted to prevent duplicate requests */
        g_hash_table_insert(self->ancestors_fetched, g_strdup(item->parent_id), GINT_TO_POINTER(1));
      }
    }

    /* Check root_id - need to fetch if not in events and not already attempted */
    if (item->root_id && strlen(item->root_id) == 64 &&
        !g_hash_table_contains(self->events_by_id, item->root_id) &&
        !g_hash_table_contains(self->ancestors_fetched, item->root_id)) {
      gboolean found = FALSE;
      for (guint i = 0; i < missing_ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(missing_ids, i), item->root_id) == 0) {
          found = TRUE;
          break;
        }
      }
      if (!found) {
        g_ptr_array_add(missing_ids, g_strdup(item->root_id));
        /* nostrc-7r5: Collect relay hint for root */
        add_relay_hint_if_unique(relay_hints, item->root_relay_hint);
        /* nostrc-46g: Mark as attempted to prevent duplicate requests */
        g_hash_table_insert(self->ancestors_fetched, g_strdup(item->root_id), GINT_TO_POINTER(1));
      }
    }
  }

  if (missing_ids->len == 0) {
    g_ptr_array_unref(missing_ids);
    g_ptr_array_unref(relay_hints);
    g_debug("[THREAD_VIEW] No more missing ancestors to fetch, chain complete");
    return;
  }

  /* nostrc-46g: Increment depth counter for chain traversal tracking */
  self->ancestor_fetch_depth++;

  /* nostrc-7r5: Log relay hints being used */
  if (relay_hints->len > 0) {
    g_debug("[THREAD_VIEW] Fetching %u missing ancestor events (depth %u) with %u relay hints",
            missing_ids->len, self->ancestor_fetch_depth, relay_hints->len);
  } else {
    g_debug("[THREAD_VIEW] Fetching %u missing ancestor events (depth %u), no relay hints",
            missing_ids->len, self->ancestor_fetch_depth);
  }

  /* Build filter with missing IDs */
  NostrFilter *filter = nostr_filter_new();
  int kinds[2] = { 1, 1111 };
  nostr_filter_set_kinds(filter, kinds, 2);

  for (guint i = 0; i < missing_ids->len; i++) {
    nostr_filter_add_id(filter, g_ptr_array_index(missing_ids, i));
  }
  nostr_filter_set_limit(filter, MAX_THREAD_EVENTS);

  g_ptr_array_unref(missing_ids);

  /* nostrc-7r5: Build relay URL list - hinted relays first, then configured relays */
  GPtrArray *all_relays = g_ptr_array_new_with_free_func(g_free);

  /* Add relay hints first (higher priority) */
  for (guint i = 0; i < relay_hints->len; i++) {
    g_ptr_array_add(all_relays, g_strdup(g_ptr_array_index(relay_hints, i)));
  }
  g_ptr_array_unref(relay_hints);

  /* Add configured read relays (fallback) */
  GPtrArray *config_relays = gnostr_get_read_relay_urls();
  for (guint i = 0; i < config_relays->len; i++) {
    add_relay_hint_if_unique(all_relays, g_ptr_array_index(config_relays, i));
  }
  g_ptr_array_unref(config_relays);

  /* Convert to const char** array */
  const char **urls = g_new0(const char*, all_relays->len);
  for (guint i = 0; i < all_relays->len; i++) {
    urls[i] = g_ptr_array_index(all_relays, i);
  }

  /* Query relays (reuse existing cancellable) */
  if (!self->fetch_cancellable) {
    self->fetch_cancellable = g_cancellable_new();
  }

  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    all_relays->len,
    filter,
    self->fetch_cancellable,
    on_missing_ancestors_done,
    self
  );

  nostr_filter_free(filter);
  g_free(urls);
  g_ptr_array_unref(all_relays);
}

/* Internal: fetch thread from relays */
static void fetch_thread_from_relays(GnostrThreadView *self) {
  if (!self->thread_root_id && !self->focus_event_id) return;

  /* Cancel previous fetch */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }
  self->fetch_cancellable = g_cancellable_new();

  const char *root = self->thread_root_id ? self->thread_root_id : self->focus_event_id;
  const char *focus = self->focus_event_id;

  /* Get read-capable relay URLs for fetching (NIP-65) */
  GPtrArray *relay_arr = gnostr_get_read_relay_urls();

  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Query 1: Fetch all replies and comments (events with #e tag referencing root)
   * NIP-22: kind 1111 is for comments, which use E tag (uppercase) for root reference */
  NostrFilter *filter_replies = nostr_filter_new();
  int kinds[2] = { 1, 1111 }; /* Text notes and NIP-22 comments */
  nostr_filter_set_kinds(filter_replies, kinds, 2);
  nostr_filter_tags_append(filter_replies, "e", root, NULL);
  nostr_filter_set_limit(filter_replies, MAX_THREAD_EVENTS);

  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    relay_arr->len,
    filter_replies,
    self->fetch_cancellable,
    on_thread_query_done,
    self
  );

  nostr_filter_free(filter_replies);

  /* Query 2: Fetch root event and focus event by ID (they may not reference themselves) */
  NostrFilter *filter_ids = nostr_filter_new();
  nostr_filter_set_kinds(filter_ids, kinds, 2);  /* Include both kind 1 and 1111 */

  /* Add root ID */
  nostr_filter_add_id(filter_ids, root);

  /* Add focus ID if different from root */
  if (focus && g_strcmp0(focus, root) != 0) {
    nostr_filter_add_id(filter_ids, focus);
  }

  /* Also fetch any parent IDs we know about from loaded events */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    if (item->parent_id && !g_hash_table_contains(self->events_by_id, item->parent_id)) {
      nostr_filter_add_id(filter_ids, item->parent_id);
    }
    if (item->root_id && !g_hash_table_contains(self->events_by_id, item->root_id)) {
      nostr_filter_add_id(filter_ids, item->root_id);
    }
  }

  nostr_filter_set_limit(filter_ids, MAX_THREAD_EVENTS);

  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    relay_arr->len,
    filter_ids,
    self->fetch_cancellable,
    on_root_fetch_done,
    self
  );

  nostr_filter_free(filter_ids);

  /* Query 3: NIP-22 comments use uppercase E tag for root event reference */
  NostrFilter *filter_nip22 = nostr_filter_new();
  int comment_kind[1] = { 1111 };
  nostr_filter_set_kinds(filter_nip22, comment_kind, 1);
  nostr_filter_tags_append(filter_nip22, "E", root, NULL);
  nostr_filter_set_limit(filter_nip22, MAX_THREAD_EVENTS);

  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    relay_arr->len,
    filter_nip22,
    self->fetch_cancellable,
    on_thread_query_done,  /* Reuse same callback */
    self
  );

  nostr_filter_free(filter_nip22);
  g_free(urls);
  g_ptr_array_unref(relay_arr);
}

/* Internal: load a single event by ID from nostrdb and add to collection.
 * Returns the ThreadEventItem if found, NULL otherwise. */
static ThreadEventItem *load_event_by_id(GnostrThreadView *self, const char *id_hex) {
  if (!id_hex || strlen(id_hex) != 64) return NULL;

  /* Check if already loaded */
  ThreadEventItem *existing = g_hash_table_lookup(self->events_by_id, id_hex);
  if (existing) return existing;

  unsigned char id32[32];
  if (!hex_to_bytes_32(id_hex, id32)) return NULL;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return NULL;

  ThreadEventItem *item = NULL;
  char *json = NULL;
  int len = 0;
  if (storage_ndb_get_note_by_id(txn, id32, &json, &len) == 0 && json) {
    item = add_event_from_json(self, json);
  }
  storage_ndb_end_query(txn);

  return item;
}

/* Internal: recursively load parent chain from nostrdb (NIP-10).
 * Walks up parent_id or root_id references to load all ancestor events. */
static void load_parent_chain(GnostrThreadView *self, ThreadEventItem *item, int depth) {
  if (!item || depth > MAX_THREAD_DEPTH) return;

  /* Load parent event (reply marker takes precedence) */
  const char *parent_id = item->parent_id ? item->parent_id : item->root_id;
  if (parent_id && strlen(parent_id) == 64) {
    ThreadEventItem *parent = load_event_by_id(self, parent_id);
    if (parent) {
      load_parent_chain(self, parent, depth + 1);
    }
  }

  /* Also ensure root is loaded if different from parent */
  if (item->root_id && (!parent_id || g_strcmp0(item->root_id, parent_id) != 0)) {
    load_event_by_id(self, item->root_id);
  }
}

/* Internal: load thread from nostrdb and relays */
static void load_thread(GnostrThreadView *self) {
  const char *focus_id = self->focus_event_id;
  const char *root_id = self->thread_root_id;

  if (!focus_id && !root_id) {
    show_empty_state(self, "No thread selected");
    return;
  }

  /* nostrc-46g: Reset ancestor tracking for new thread load */
  if (self->ancestors_fetched) {
    g_hash_table_remove_all(self->ancestors_fetched);
  }
  self->ancestor_fetch_depth = 0;

  set_loading_state(self, TRUE);

  /* First, try to load focus event from nostrdb */
  ThreadEventItem *focus_item = NULL;
  if (focus_id) {
    focus_item = load_event_by_id(self, focus_id);

    /* If we found the focus event, extract root_id from it */
    if (focus_item && focus_item->root_id && !self->thread_root_id) {
      self->thread_root_id = g_strdup(focus_item->root_id);
    }
  }

  /* Load the root event if we know it */
  root_id = self->thread_root_id;
  if (root_id && (!focus_id || g_strcmp0(root_id, focus_id) != 0)) {
    load_event_by_id(self, root_id);
  }

  /* Load parent chain from focus event to find all ancestors (NIP-10) */
  if (focus_item) {
    load_parent_chain(self, focus_item, 0);
  }

  /* Query nostrdb for events referencing this thread root */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) == 0 && txn) {
    const char *query_root = self->thread_root_id ? self->thread_root_id : focus_id;

    /* Build filter JSON to find all replies to the root (kind 1 and NIP-22 kind 1111) */
    char filter_json[512];
    snprintf(filter_json, sizeof(filter_json),
             "[{\"kinds\":[1,1111],\"#e\":[\"%s\"],\"limit\":%d}]",
             query_root, MAX_THREAD_EVENTS);

    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter_json, &results, &count) == 0 && results) {
      for (int i = 0; i < count; i++) {
        if (results[i]) {
          add_event_from_json(self, results[i]);
        }
      }
      storage_ndb_free_results(results, count);
    }

    /* Also query for events referencing the focus event specifically
     * (in case it's a mid-thread note with its own replies) */
    if (focus_id && g_strcmp0(focus_id, query_root) != 0) {
      snprintf(filter_json, sizeof(filter_json),
               "[{\"kinds\":[1,1111],\"#e\":[\"%s\"],\"limit\":%d}]",
               focus_id, MAX_THREAD_EVENTS);

      results = NULL;
      count = 0;
      if (storage_ndb_query(txn, filter_json, &results, &count) == 0 && results) {
        for (int i = 0; i < count; i++) {
          if (results[i]) {
            add_event_from_json(self, results[i]);
          }
        }
        storage_ndb_free_results(results, count);
      }
    }

    storage_ndb_end_query(txn);
  }

  /* Show what we have from local DB */
  if (g_hash_table_size(self->events_by_id) > 0) {
    rebuild_thread_ui(self);
  }

  /* nostrc-50t: Setup nostrdb subscription for live updates */
  setup_thread_subscription(self);

  /* Fetch more from relays */
  fetch_thread_from_relays(self);
}

/* Internal: update profile info for a single ThreadEventItem from provider cache */
static void update_item_profile_from_cache(ThreadEventItem *item) {
  if (!item || !item->pubkey_hex) return;

  GnostrProfileMeta *meta = gnostr_profile_provider_get(item->pubkey_hex);
  if (!meta) return;

  /* Always update profile fields when we have new data.
   * This ensures late-arriving profiles are properly displayed.
   * (nostrc-k8pd fix) */
  if (meta->display_name && *meta->display_name) {
    g_free(item->display_name);
    item->display_name = g_strdup(meta->display_name);
  } else if (meta->name && *meta->name && !item->display_name) {
    g_free(item->display_name);
    item->display_name = g_strdup(meta->name);
  }
  if (meta->name && *meta->name) {
    g_free(item->handle);
    item->handle = g_strdup_printf("@%s", meta->name);
  }
  if (meta->picture && *meta->picture) {
    g_free(item->avatar_url);
    item->avatar_url = g_strdup(meta->picture);
  }
  if (meta->nip05 && *meta->nip05) {
    g_free(item->nip05);
    item->nip05 = g_strdup(meta->nip05);
  }

  gnostr_profile_meta_free(meta);
}

/* Internal: update a single note card widget with profile info */
static void update_note_card_profile(GnostrNoteCardRow *row, ThreadEventItem *item) {
  if (!row || !item) return;

  const char *display = item->display_name;
  const char *handle = item->handle;

  if (!display && !handle && item->pubkey_hex) {
    /* Fallback to truncated pubkey */
    char fallback[20];
    snprintf(fallback, sizeof(fallback), "%.8s...", item->pubkey_hex);
    gnostr_note_card_row_set_author(row, fallback, NULL, item->avatar_url);
  } else {
    gnostr_note_card_row_set_author(row, display, handle, item->avatar_url);
  }

  if (item->nip05 && item->pubkey_hex) {
    gnostr_note_card_row_set_nip05(row, item->nip05, item->pubkey_hex);
  }
}

void gnostr_thread_view_update_profiles(GnostrThreadView *self) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!self->thread_list_box || !self->sorted_events) return;

  /* Iterate through displayed cards and update profile info */
  GtkWidget *child = gtk_widget_get_first_child(self->thread_list_box);
  guint idx = 0;

  while (child && idx < self->sorted_events->len) {
    if (GNOSTR_IS_NOTE_CARD_ROW(child)) {
      ThreadEventItem *item = g_ptr_array_index(self->sorted_events, idx);
      if (item) {
        /* Re-fetch profile from cache */
        update_item_profile_from_cache(item);
        /* Update the card */
        update_note_card_profile(GNOSTR_NOTE_CARD_ROW(child), item);
      }
      idx++;
    }
    child = gtk_widget_get_next_sibling(child);
  }
}

/* ========== nostrc-50t: nostrdb subscription for live thread updates ========== */

/* Debounce interval for UI rebuild after receiving new events (ms) */
#define THREAD_REBUILD_DEBOUNCE_MS 150

/* Timeout callback to rebuild the UI after receiving new events */
static gboolean on_rebuild_debounce_timeout(gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self)) return G_SOURCE_REMOVE;

  self->rebuild_pending_id = 0;

  /* Rebuild UI with newly arrived events */
  rebuild_thread_ui(self);

  /* Check if new events reference ancestors we don't have yet */
  fetch_missing_ancestors(self);

  return G_SOURCE_REMOVE;
}

/* Schedule a debounced UI rebuild */
static void schedule_thread_rebuild(GnostrThreadView *self) {
  if (self->rebuild_pending_id > 0) {
    /* Already scheduled, don't reschedule */
    return;
  }

  self->rebuild_pending_id = g_timeout_add(THREAD_REBUILD_DEBOUNCE_MS,
                                           on_rebuild_debounce_timeout, self);
}

/* Callback for nostrdb subscription - called when new thread events arrive */
static void on_ndb_thread_batch(uint64_t subid, const uint64_t *note_keys,
                                 guint n_keys, gpointer user_data) {
  (void)subid;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self) || !note_keys || n_keys == 0) return;

  g_debug("[THREAD_VIEW] Received %u events from nostrdb subscription", n_keys);

  gboolean found_new = FALSE;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

  for (guint i = 0; i < n_keys; i++) {
    uint64_t key = note_keys[i];

    /* Get note pointer from key */
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, key);
    if (!note) continue;

    /* Get event ID */
    const unsigned char *id_bin = storage_ndb_note_id(note);
    if (!id_bin) continue;

    char id_hex[65];
    storage_ndb_hex_encode(id_bin, id_hex);

    /* Skip if we already have this event */
    if (g_hash_table_contains(self->events_by_id, id_hex)) continue;

    /* Get pubkey */
    const unsigned char *pk_bin = storage_ndb_note_pubkey(note);
    if (!pk_bin) continue;

    char pk_hex[65];
    storage_ndb_hex_encode(pk_bin, pk_hex);

    /* Get content */
    const char *content = storage_ndb_note_content(note);

    /* Get created_at */
    uint32_t created_at = storage_ndb_note_created_at(note);

    /* Get NIP-10 thread info with relay hints (nostrc-7r5) */
    char *root_id = NULL;
    char *reply_id = NULL;
    char *root_relay_hint = NULL;
    char *reply_relay_hint = NULL;
    storage_ndb_note_get_nip10_thread_full(note, &root_id, &reply_id,
                                            &root_relay_hint, &reply_relay_hint);

    /* Create new item */
    ThreadEventItem *item = g_new0(ThreadEventItem, 1);
    item->id_hex = g_strdup(id_hex);
    item->pubkey_hex = g_strdup(pk_hex);
    item->content = content ? g_strdup(content) : g_strdup("");
    item->created_at = (gint64)created_at;
    item->root_id = root_id;               /* Takes ownership */
    item->parent_id = reply_id;            /* Takes ownership */
    item->root_relay_hint = root_relay_hint;     /* Takes ownership (nostrc-7r5) */
    item->parent_relay_hint = reply_relay_hint;  /* Takes ownership (nostrc-7r5) */

    /* Add to hash table (owns the item) */
    g_hash_table_insert(self->events_by_id, item->id_hex, item);
    found_new = TRUE;

    g_debug("[THREAD_VIEW] Added event %.16s... from subscription", id_hex);
  }

  storage_ndb_end_query(txn);

  if (found_new) {
    /* Schedule debounced UI rebuild */
    schedule_thread_rebuild(self);
  }
}

/* Setup nostrdb subscription for thread events */
static void setup_thread_subscription(GnostrThreadView *self) {
  if (!self) return;

  /* Teardown any existing subscription */
  teardown_thread_subscription(self);

  const char *root_id = self->thread_root_id ? self->thread_root_id : self->focus_event_id;
  if (!root_id || strlen(root_id) != 64) return;

  /* Build filter for events referencing the thread root.
   * Subscribe to kind 1 (notes) and kind 1111 (NIP-22 comments) with #e tag = root.
   * Note: nostrdb filter format uses JSON. */
  char filter_json[256];
  snprintf(filter_json, sizeof(filter_json),
           "{\"kinds\":[1,1111],\"#e\":[\"%s\"],\"limit\":%d}",
           root_id, MAX_THREAD_EVENTS);

  self->ndb_sub_thread = gn_ndb_subscribe(filter_json, on_ndb_thread_batch, self, NULL);

  if (self->ndb_sub_thread > 0) {
    g_debug("[THREAD_VIEW] Created nostrdb subscription %" G_GUINT64_FORMAT " for root %s",
            (guint64)self->ndb_sub_thread, root_id);
  } else {
    g_warning("[THREAD_VIEW] Failed to create nostrdb subscription for root %s", root_id);
  }
}

/* Teardown nostrdb subscription */
static void teardown_thread_subscription(GnostrThreadView *self) {
  if (!self) return;

  if (self->ndb_sub_thread > 0) {
    g_debug("[THREAD_VIEW] Unsubscribing from nostrdb subscription %" G_GUINT64_FORMAT,
            (guint64)self->ndb_sub_thread);
    gn_ndb_unsubscribe(self->ndb_sub_thread);
    self->ndb_sub_thread = 0;
  }
}
