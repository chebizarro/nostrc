#include "gnostr-thread-view.h"
#include "note_card_row.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-profile-provider.h"
#include "../storage_ndb.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "../model/gn-nostr-event-item.h"
#include "../model/gn-nostr-profile.h"
#include "../util/relays.h"
#include "../util/utils.h"
#include "nostr_event.h"
#include "nostr-json.h"
#include "nostr_filter.h"
#include "nostr_nip19.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <string.h>
#include <time.h>
#include "nostr_json.h"
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

/* Maximum iterations for iterative child discovery */
#define MAX_CHILD_DISCOVERY_ITERATIONS 5

/* Thread event item for internal use */
typedef struct {
  char *id_hex;
  char *pubkey_hex;
  char *content;
  char *root_id;
  char *parent_id;
  char *root_relay_hint;   /* NIP-10 relay hint for root event */
  char *parent_relay_hint; /* NIP-10 relay hint for parent event */
  GPtrArray *mentioned_pubkeys; /* nostrc-hb7c: p-tag pubkeys for NIP-65 fetch */
  gint64 created_at;
  guint depth;
  /* Profile info (resolved asynchronously) */
  char *display_name;
  char *handle;
  char *avatar_url;
  char *nip05;
} ThreadEventItem;

/* ThreadNode: Graph node representing an event with its relationships */
struct _ThreadNode {
  ThreadEventItem *event;      /* Borrowed reference to event data */
  GPtrArray *child_ids;        /* Array of child event ID strings (owned) */
  char *parent_id;             /* Direct parent event ID (owned) */
  guint depth;                 /* Distance from root */
  gboolean is_focus_path;      /* TRUE if on path from focus to root */
  gboolean is_collapsed;       /* TRUE if branch is collapsed */
  guint child_count;           /* Total descendants (for collapse indicator) */
};

/* ThreadGraph: Complete bidirectional graph of thread */
struct _ThreadGraph {
  GHashTable *nodes;           /* event_id -> ThreadNode* (owned) */
  char *root_id;               /* Discovered thread root */
  char *focus_id;              /* User's focus event */
  GPtrArray *render_order;     /* Events in tree traversal order (borrowed refs) */
};

static void thread_node_free(ThreadNode *node) {
  if (!node) return;
  /* event is borrowed, don't free */
  if (node->child_ids) g_ptr_array_unref(node->child_ids);
  g_free(node->parent_id);
  g_free(node);
}

static ThreadNode *thread_node_new(ThreadEventItem *event) {
  ThreadNode *node = g_new0(ThreadNode, 1);
  node->event = event;
  node->child_ids = g_ptr_array_new_with_free_func(g_free);
  node->parent_id = event->parent_id ? g_strdup(event->parent_id) : NULL;
  node->depth = 0;
  node->is_focus_path = FALSE;
  node->is_collapsed = FALSE;
  node->child_count = 0;
  return node;
}

static void thread_graph_free(ThreadGraph *graph) {
  if (!graph) return;
  if (graph->nodes) g_hash_table_unref(graph->nodes);
  if (graph->render_order) g_ptr_array_unref(graph->render_order);
  g_free(graph->root_id);
  g_free(graph->focus_id);
  g_free(graph);
}

static ThreadGraph *thread_graph_new(void) {
  ThreadGraph *graph = g_new0(ThreadGraph, 1);
  graph->nodes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                        (GDestroyNotify)thread_node_free);
  graph->render_order = g_ptr_array_new();
  return graph;
}

static void thread_event_item_free(ThreadEventItem *item) {
  if (!item) return;
  g_free(item->id_hex);
  g_free(item->pubkey_hex);
  g_free(item->content);
  g_free(item->root_id);
  g_free(item->parent_id);
  g_free(item->root_relay_hint);
  g_free(item->parent_relay_hint);
  if (item->mentioned_pubkeys) g_ptr_array_unref(item->mentioned_pubkeys);
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
  GtkWidget *thread_list_view;  /* GtkListView for virtualized scrolling */
  GtkWidget *loading_box;

  /* GtkListView model and selection (nostrc-evz1) */
  GListStore *thread_model;
  GtkSelectionModel *thread_selection;
  GtkWidget *loading_spinner;
  GtkWidget *empty_box;
  GtkWidget *empty_label;
  GtkWidget *missing_events_banner;  /* nostrc-x3b: banner for missing ancestors */
  GtkWidget *missing_events_stack;
  GtkWidget *missing_events_spinner;
  GtkWidget *missing_events_icon;
  GtkWidget *missing_events_label;
  gboolean is_fetching_missing;      /* TRUE while actively fetching missing ancestors */

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

  /* Thread graph for bidirectional traversal (parents, children, siblings) */
  ThreadGraph *thread_graph;
  /* Track event IDs we've already queried for children */
  GHashTable *children_fetched;  /* event_id_hex -> gboolean */
  guint child_discovery_iteration; /* Current iteration of child discovery */

  /* nostrc-50t: nostrdb subscription for live thread updates */
  uint64_t ndb_sub_thread;       /* Subscription ID for thread events */
  guint rebuild_pending_id;      /* Timeout source for debounced UI rebuild */

  /* nostrc-hl6: Track pubkeys we've fetched NIP-65 relay lists for */
  GHashTable *nip65_pubkeys_fetched;  /* pubkey_hex -> gboolean */

  /* nostrc-59nk: Disposal guard flag to prevent async callbacks from modifying widgets */
  gboolean disposed;
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
static ThreadEventItem *add_event_from_json(GnostrThreadView *self, const char *json_str);
static void on_root_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void setup_thread_subscription(GnostrThreadView *self);
static void teardown_thread_subscription(GnostrThreadView *self);
static void fetch_children_from_relays(GnostrThreadView *self);
static void build_thread_graph(GnostrThreadView *self);
static void mark_focus_path(GnostrThreadView *self);
static guint count_descendants(GnostrThreadView *self, const char *event_id);
/* nostrc-evz1: GtkListView factory callbacks */
static void thread_factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);
static void thread_factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);
static void thread_factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);

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
static gboolean parse_nip10_tag_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;
  Nip10ParseContext *ctx = (Nip10ParseContext *)user_data;
  if (!tag_json || !ctx) return true;

  /* Validate tag is an array */
  if (!gnostr_json_is_array_str(tag_json)) return true;

  /* Get tag type (first element) */
  char *tag_type = NULL;
  tag_type = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_type) {
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
  event_id = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
  if (!event_id) {
    return true;
  }

  if (strlen(event_id) != 64) {
    free(event_id);
    return true;
  }

  /* Get relay hint (third element) - NIP-10 relay hint */
  char *relay_hint = NULL;
  relay_hint = gnostr_json_get_array_string(tag_json, NULL, 2, NULL);

  /* Check for marker (NIP-10 preferred markers) - fourth element */
  char *marker = NULL;
  if ((marker = gnostr_json_get_array_string(tag_json, NULL, 3, NULL)) != NULL && marker && *marker) {
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

  if (!gnostr_json_is_valid(json_str)) return;

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
  gnostr_json_array_foreach(json_str, "tags", parse_nip10_tag_callback, &ctx);

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

/* nostrc-hb7c: Callback for extracting p-tags (mentioned pubkeys) */
static gboolean parse_ptag_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;  /* unused */
  GPtrArray *pubkeys = (GPtrArray *)user_data;
  if (!tag_json || !pubkeys) return true;

  /* Check if this is a p-tag */
  char *tag_type = NULL;
  tag_type = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_type) {
    return true;
  }
  if (strcmp(tag_type, "p") != 0 && strcmp(tag_type, "P") != 0) {
    free(tag_type);
    return true;
  }
  free(tag_type);

  /* Get the pubkey (second element) */
  char *pubkey = NULL;
  pubkey = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
  if (!pubkey) {
    return true;
  }

  /* Validate pubkey length */
  if (strlen(pubkey) != 64) {
    free(pubkey);
    return true;
  }

  /* Check for duplicates */
  for (guint i = 0; i < pubkeys->len; i++) {
    if (g_strcmp0(g_ptr_array_index(pubkeys, i), pubkey) == 0) {
      free(pubkey);
      return true;
    }
  }

  g_ptr_array_add(pubkeys, g_strdup(pubkey));
  free(pubkey);
  return true;
}

/* nostrc-hb7c: Extract p-tag pubkeys from event JSON */
static GPtrArray *extract_ptags_from_json(const char *json_str) {
  GPtrArray *pubkeys = g_ptr_array_new_with_free_func(g_free);
  if (!json_str || !gnostr_json_is_valid(json_str)) {
    return pubkeys;
  }
  gnostr_json_array_foreach(json_str, "tags", parse_ptag_callback, pubkeys);
  return pubkeys;
}

/* Dispose */
static void gnostr_thread_view_dispose(GObject *obj) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(obj);

  /* nostrc-59nk: Mark as disposed FIRST to prevent async callbacks from modifying widgets */
  self->disposed = TRUE;

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
  g_clear_pointer(&self->children_fetched, g_hash_table_unref);
  g_clear_pointer(&self->nip65_pubkeys_fetched, g_hash_table_unref);

  /* Clear thread graph */
  if (self->thread_graph) {
    thread_graph_free(self->thread_graph);
    self->thread_graph = NULL;
  }

  /* Clear array (items are borrowed, owned by hash table) */
  if (self->sorted_events) {
    g_ptr_array_free(self->sorted_events, TRUE);
    self->sorted_events = NULL;
  }

  /* nostrc-evz1: Clear GListStore and selection model */
  g_clear_object(&self->thread_selection);
  g_clear_object(&self->thread_model);

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
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, thread_list_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, loading_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, empty_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, empty_label);
  /* nostrc-x3b: missing events banner */
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, missing_events_banner);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, missing_events_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, missing_events_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, missing_events_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrThreadView, missing_events_label);

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
  /* Track fetched children for bidirectional graph building */
  self->children_fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->child_discovery_iteration = 0;
  self->thread_graph = NULL;
  /* nostrc-hl6: Track NIP-65 relay list fetches per author */
  self->nip65_pubkeys_fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Uses shared query pool from gnostr_get_shared_query_pool() */

  /* nostrc-evz1: Set up GListStore and factory for GtkListView */
  self->thread_model = g_list_store_new(GN_TYPE_NOSTR_EVENT_ITEM);
  self->thread_selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(self->thread_model)));

  if (self->thread_list_view) {
    /* Create and configure factory */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(thread_factory_setup_cb), self);
    g_signal_connect(factory, "bind", G_CALLBACK(thread_factory_bind_cb), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(thread_factory_unbind_cb), self);

    gtk_list_view_set_factory(GTK_LIST_VIEW(self->thread_list_view), factory);
    gtk_list_view_set_model(GTK_LIST_VIEW(self->thread_list_view), self->thread_selection);
    g_object_unref(factory);
  }

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
  gnostr_thread_view_set_focus_event_with_json(self, event_id_hex, NULL);
}

void gnostr_thread_view_set_focus_event_with_json(GnostrThreadView *self,
                                                   const char *event_id_hex,
                                                   const char *event_json) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!event_id_hex || strlen(event_id_hex) != 64) {
    g_warning("[THREAD_VIEW] Invalid event ID");
    return;
  }

  /* Store focus event */
  g_free(self->focus_event_id);
  self->focus_event_id = g_strdup(event_id_hex);

  /* nostrc-a2zd: Pre-populate focus event from JSON if provided.
   * This avoids the nostrdb async ingestion race condition where
   * events may not be queryable immediately after relay receipt. */
  if (event_json && *event_json) {
    ThreadEventItem *item = add_event_from_json(self, event_json);
    if (item) {
      g_message("[THREAD_VIEW] Pre-populated focus event from JSON: %.16s...", event_id_hex);
      /* Also ingest into nostrdb in background for future queries */
      {
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(b, g_strdup(event_json));
        storage_ndb_ingest_events_async(b);
      }
    }
  }

  /* Load the thread */
  load_thread(self);
}

void gnostr_thread_view_set_thread_root(GnostrThreadView *self, const char *root_event_id_hex) {
  gnostr_thread_view_set_thread_root_with_json(self, root_event_id_hex, NULL);
}

void gnostr_thread_view_set_thread_root_with_json(GnostrThreadView *self,
                                                   const char *root_event_id_hex,
                                                   const char *event_json) {
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

  /* nostrc-a2zd: Pre-populate root event from JSON if provided.
   * This avoids the nostrdb async ingestion race condition. */
  if (event_json && *event_json) {
    ThreadEventItem *item = add_event_from_json(self, event_json);
    if (item) {
      g_message("[THREAD_VIEW] Pre-populated root event from JSON: %.16s...", root_event_id_hex);
      /* Also ingest into nostrdb in background for future queries */
      {
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(b, g_strdup(event_json));
        storage_ndb_ingest_events_async(b);
      }
    }
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

  /* Clear child tracking for bidirectional fetching */
  if (self->children_fetched) {
    g_hash_table_remove_all(self->children_fetched);
  }
  self->child_discovery_iteration = 0;

  /* Clear thread graph */
  if (self->thread_graph) {
    thread_graph_free(self->thread_graph);
    self->thread_graph = NULL;
  }

  /* Clear UI - use GListStore for GtkListView */
  if (self->thread_model) {
    g_list_store_remove_all(self->thread_model);
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

/* Internal: update missing events banner state
 * When fetching=TRUE, shows spinner with "Fetching missing messages..."
 * When fetching=FALSE, checks for missing events and shows warning or hides banner */
static void update_missing_events_banner(GnostrThreadView *self, gboolean fetching) {
  if (!self->missing_events_banner) return;

  self->is_fetching_missing = fetching;

  if (fetching) {
    /* Show banner with spinner */
    gtk_widget_set_visible(self->missing_events_banner, TRUE);
    if (self->missing_events_stack && self->missing_events_spinner) {
      gtk_stack_set_visible_child(GTK_STACK(self->missing_events_stack),
                                  self->missing_events_spinner);
      gtk_spinner_start(GTK_SPINNER(self->missing_events_spinner));
    }
    if (self->missing_events_label) {
      gtk_label_set_text(GTK_LABEL(self->missing_events_label),
                         "Fetching missing messages...");
    }
    /* Remove warning style, add info style for fetching state */
    gtk_widget_remove_css_class(self->missing_events_banner, "warning");
    return;
  }

  /* Fetching complete - stop spinner */
  if (self->missing_events_spinner) {
    gtk_spinner_stop(GTK_SPINNER(self->missing_events_spinner));
  }

  /* Check if we have missing ancestors */
  gboolean has_missing = FALSE;
  guint missing_count = 0;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    /* Check for missing parent */
    if (item->parent_id && strlen(item->parent_id) == 64 &&
        !g_hash_table_contains(self->events_by_id, item->parent_id)) {
      has_missing = TRUE;
      missing_count++;
    }
    /* Check for missing root (if different from parent) */
    if (item->root_id && strlen(item->root_id) == 64 &&
        g_strcmp0(item->root_id, item->parent_id) != 0 &&
        !g_hash_table_contains(self->events_by_id, item->root_id)) {
      has_missing = TRUE;
      missing_count++;
    }
  }

  if (has_missing) {
    /* Show banner with warning icon */
    gtk_widget_set_visible(self->missing_events_banner, TRUE);
    if (self->missing_events_stack && self->missing_events_icon) {
      gtk_stack_set_visible_child(GTK_STACK(self->missing_events_stack),
                                  self->missing_events_icon);
    }
    if (self->missing_events_label) {
      char *msg = g_strdup_printf(
        "Some messages in this thread could not be found (%u missing)",
        missing_count);
      gtk_label_set_text(GTK_LABEL(self->missing_events_label), msg);
      g_free(msg);
    }
    /* Add warning style */
    gtk_widget_add_css_class(self->missing_events_banner, "warning");
  } else {
    /* No missing events - hide banner */
    gtk_widget_set_visible(self->missing_events_banner, FALSE);
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

  GNostrEvent *evt = gnostr_event_new_from_json(json_str, NULL);
  if (!evt) return NULL;

  const gchar *id = gnostr_event_get_id(evt);
  if (!id || strlen(id) != 64) {
    g_object_unref(evt);
    return NULL;
  }

  /* Check if already exists */
  if (g_hash_table_contains(self->events_by_id, id)) {
    g_object_unref(evt);
    return g_hash_table_lookup(self->events_by_id, id);
  }

  /* Create new item */
  ThreadEventItem *item = g_new0(ThreadEventItem, 1);
  item->id_hex = g_strdup(id);
  item->pubkey_hex = g_strdup(gnostr_event_get_pubkey(evt));
  item->content = g_strdup(gnostr_event_get_content(evt));
  item->created_at = gnostr_event_get_created_at(evt);

  /* Parse NIP-10 tags with relay hints */
  parse_nip10_from_json_full(json_str, &item->root_id, &item->parent_id,
                              &item->root_relay_hint, &item->parent_relay_hint);

  /* nostrc-hb7c: Extract p-tags for NIP-65 relay lookup of missing authors */
  item->mentioned_pubkeys = extract_ptags_from_json(json_str);

  g_object_unref(evt);

  /* Debug: log what we parsed */
  g_message("[THREAD_VIEW] Added event %.16s... root=%.16s%s parent=%.16s%s",
            item->id_hex,
            item->root_id ? item->root_id : "(none)", item->root_id ? "..." : "",
            item->parent_id ? item->parent_id : "(none)", item->parent_id ? "..." : "");

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

/* Internal: count all descendants of an event recursively and set child_count.
 * This is a post-order traversal that sets child_count on each visited node,
 * so it only needs to be called once per root (O(n) total instead of O(n²)). */
static guint count_descendants(GnostrThreadView *self, const char *event_id) {
  if (!self->thread_graph || !event_id) return 0;

  ThreadNode *node = g_hash_table_lookup(self->thread_graph->nodes, event_id);
  if (!node || !node->child_ids) return 0;

  guint count = node->child_ids->len;
  for (guint i = 0; i < node->child_ids->len; i++) {
    const char *child_id = g_ptr_array_index(node->child_ids, i);
    count += count_descendants(self, child_id);
  }
  /* Set child_count on this node during traversal (post-order) */
  node->child_count = count;
  return count;
}

/* Compare ThreadNodes by their event's created_at */
static gint compare_nodes_by_time(gconstpointer a, gconstpointer b) {
  const ThreadNode *node_a = *(const ThreadNode **)a;
  const ThreadNode *node_b = *(const ThreadNode **)b;

  if (!node_a->event || !node_b->event) return 0;

  if (node_a->event->created_at < node_b->event->created_at) return -1;
  if (node_a->event->created_at > node_b->event->created_at) return 1;
  return 0;
}

/* Internal: mark events on the path from focus to root */
static void mark_focus_path(GnostrThreadView *self) {
  if (!self->thread_graph || !self->focus_event_id) return;

  /* Walk from focus event up to root, marking each node */
  const char *current_id = self->focus_event_id;
  while (current_id) {
    ThreadNode *node = g_hash_table_lookup(self->thread_graph->nodes, current_id);
    if (!node) break;

    node->is_focus_path = TRUE;

    /* Move to parent */
    current_id = node->parent_id;
  }
}

/* Internal: recursive helper to build render order (DFS tree traversal) */
static void add_subtree_to_render_order(GnostrThreadView *self, const char *event_id) {
  if (!self->thread_graph || !event_id) return;

  ThreadNode *node = g_hash_table_lookup(self->thread_graph->nodes, event_id);
  if (!node) return;

  /* Add this node to render order */
  g_ptr_array_add(self->thread_graph->render_order, node);

  /* If collapsed and not on focus path, skip children (they'll be hidden) */
  if (node->is_collapsed && !node->is_focus_path) {
    return;
  }

  /* Sort children by created_at for consistent ordering */
  if (node->child_ids && node->child_ids->len > 0) {
    /* Create array of child nodes for sorting */
    GPtrArray *child_nodes = g_ptr_array_new();
    for (guint i = 0; i < node->child_ids->len; i++) {
      const char *child_id = g_ptr_array_index(node->child_ids, i);
      ThreadNode *child_node = g_hash_table_lookup(self->thread_graph->nodes, child_id);
      if (child_node && child_node->event) {
        g_ptr_array_add(child_nodes, child_node);
      }
    }

    /* Sort by created_at */
    g_ptr_array_sort(child_nodes, compare_nodes_by_time);

    /* Recursively add children */
    for (guint i = 0; i < child_nodes->len; i++) {
      ThreadNode *child_node = g_ptr_array_index(child_nodes, i);
      add_subtree_to_render_order(self, child_node->event->id_hex);
    }

    g_ptr_array_unref(child_nodes);
  }
}

/* Internal: build thread graph from flat event list */
static void build_thread_graph(GnostrThreadView *self) {
  if (g_hash_table_size(self->events_by_id) == 0) return;

  /* Free existing graph */
  if (self->thread_graph) {
    thread_graph_free(self->thread_graph);
  }
  self->thread_graph = thread_graph_new();

  /* Copy focus/root IDs */
  self->thread_graph->focus_id = self->focus_event_id ? g_strdup(self->focus_event_id) : NULL;
  self->thread_graph->root_id = self->thread_root_id ? g_strdup(self->thread_root_id) : NULL;

  /* Step 1: Create nodes for all events */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    ThreadNode *node = thread_node_new(item);
    g_hash_table_insert(self->thread_graph->nodes, item->id_hex, node);
  }

  /* Step 2: Build parent->children relationships */
  g_hash_table_iter_init(&iter, self->thread_graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->event) continue;

    /* Find parent node and add this as a child */
    const char *parent_id = node->event->parent_id;
    if (parent_id && strlen(parent_id) == 64) {
      ThreadNode *parent_node = g_hash_table_lookup(self->thread_graph->nodes, parent_id);
      if (parent_node) {
        /* Add this event as a child of the parent */
        g_ptr_array_add(parent_node->child_ids, g_strdup(node->event->id_hex));
      }
    }
  }

  /* Step 3: Find root node (no parent in our set) */
  const char *discovered_root = NULL;
  g_hash_table_iter_init(&iter, self->thread_graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->event) continue;

    const char *parent_id = node->event->parent_id;

    /* Node is a root if it has no parent, or parent is not in our set */
    if (!parent_id || !g_hash_table_contains(self->thread_graph->nodes, parent_id)) {
      /* Prefer the explicitly set root ID if available */
      if (self->thread_graph->root_id &&
          g_strcmp0(node->event->id_hex, self->thread_graph->root_id) == 0) {
        discovered_root = node->event->id_hex;
        break;
      }
      /* Otherwise take the earliest event as root */
      if (!discovered_root) {
        discovered_root = node->event->id_hex;
      } else {
        ThreadNode *current_root = g_hash_table_lookup(self->thread_graph->nodes, discovered_root);
        if (current_root && current_root->event &&
            node->event->created_at < current_root->event->created_at) {
          discovered_root = node->event->id_hex;
        }
      }
    }
  }

  if (discovered_root && !self->thread_graph->root_id) {
    self->thread_graph->root_id = g_strdup(discovered_root);
  }

  /* Step 4: Calculate depths using BFS from root */
  if (self->thread_graph->root_id) {
    GQueue *queue = g_queue_new();
    ThreadNode *root_node = g_hash_table_lookup(self->thread_graph->nodes, self->thread_graph->root_id);
    if (root_node) {
      root_node->depth = 0;
      g_queue_push_tail(queue, root_node);

      while (!g_queue_is_empty(queue)) {
        ThreadNode *node = g_queue_pop_head(queue);
        if (!node->child_ids) continue;

        for (guint i = 0; i < node->child_ids->len; i++) {
          const char *child_id = g_ptr_array_index(node->child_ids, i);
          ThreadNode *child_node = g_hash_table_lookup(self->thread_graph->nodes, child_id);
          if (child_node) {
            child_node->depth = node->depth + 1;
            if (child_node->depth > MAX_THREAD_DEPTH) {
              child_node->depth = MAX_THREAD_DEPTH;
            }
            g_queue_push_tail(queue, child_node);
          }
        }
      }
      g_queue_free(queue);
    }
  }

  /* Step 5: Mark focus path */
  mark_focus_path(self);

  /* Step 6: Collect all root nodes and calculate child counts efficiently.
   * We call count_descendants once per root, which traverses the tree and
   * sets child_count on every node it visits. This is O(n) instead of the
   * previous O(n²) approach of calling count_descendants for each node. */
  g_ptr_array_set_size(self->thread_graph->render_order, 0);

  /* Collect all root nodes (nodes without a parent in the graph) */
  GPtrArray *root_ids = g_ptr_array_new();
  g_hash_table_iter_init(&iter, self->thread_graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->parent_id || !g_hash_table_contains(self->thread_graph->nodes, node->parent_id)) {
      g_ptr_array_add(root_ids, (gpointer)key);
    }
  }

  /* Calculate child counts by traversing from each root (O(n) total) */
  for (guint i = 0; i < root_ids->len; i++) {
    const char *rid = g_ptr_array_index(root_ids, i);
    count_descendants(self, rid);
  }

  /* Log what we found */
  g_message("[THREAD_VIEW] build_thread_graph: %u nodes, %u roots, graph_root=%.16s%s",
            g_hash_table_size(self->thread_graph->nodes),
            root_ids->len,
            self->thread_graph->root_id ? self->thread_graph->root_id : "(none)",
            self->thread_graph->root_id ? "..." : "");

  for (guint i = 0; i < root_ids->len; i++) {
    const char *rid = g_ptr_array_index(root_ids, i);
    g_message("[THREAD_VIEW]   Root %u: %.16s...", i, rid);
  }

  /* nostrc-kry: Sort roots to ensure correct ordering:
   * 1. thread_root_id first (the known root of the thread)
   * 2. focus_id second (the event user clicked on)
   * 3. Then by timestamp (oldest first) for any orphan roots
   * This ensures the root event always displays at the top. */
  if (root_ids->len > 1) {
    /* Sort using a custom comparison that considers priority and timestamp */
    for (guint i = 0; i < root_ids->len - 1; i++) {
      for (guint j = i + 1; j < root_ids->len; j++) {
        const char *id_a = g_ptr_array_index(root_ids, i);
        const char *id_b = g_ptr_array_index(root_ids, j);

        gboolean a_is_root = self->thread_graph->root_id &&
                             g_strcmp0(id_a, self->thread_graph->root_id) == 0;
        gboolean b_is_root = self->thread_graph->root_id &&
                             g_strcmp0(id_b, self->thread_graph->root_id) == 0;
        gboolean a_is_focus = self->thread_graph->focus_id &&
                              g_strcmp0(id_a, self->thread_graph->focus_id) == 0;
        gboolean b_is_focus = self->thread_graph->focus_id &&
                              g_strcmp0(id_b, self->thread_graph->focus_id) == 0;

        gboolean swap = FALSE;

        /* Priority: root > focus > older timestamp */
        if (b_is_root && !a_is_root) {
          swap = TRUE;
        } else if (!a_is_root && !b_is_root) {
          if (b_is_focus && !a_is_focus) {
            swap = TRUE;
          } else if (!a_is_focus && !b_is_focus) {
            /* Neither is root or focus - sort by timestamp (oldest first) */
            ThreadNode *node_a = g_hash_table_lookup(self->thread_graph->nodes, id_a);
            ThreadNode *node_b = g_hash_table_lookup(self->thread_graph->nodes, id_b);
            if (node_a && node_b && node_a->event && node_b->event) {
              if (node_b->event->created_at < node_a->event->created_at) {
                swap = TRUE;
              }
            }
          }
        }

        if (swap) {
          /* Swap pointers in the array */
          gpointer tmp = root_ids->pdata[i];
          root_ids->pdata[i] = root_ids->pdata[j];
          root_ids->pdata[j] = tmp;
        }
      }
    }

    g_message("[THREAD_VIEW] Sorted roots order:");
    for (guint i = 0; i < root_ids->len; i++) {
      const char *rid = g_ptr_array_index(root_ids, i);
      g_message("[THREAD_VIEW]   Sorted root %u: %.16s...", i, rid);
    }
  }

  /* Render from each root in sorted order */
  for (guint i = 0; i < root_ids->len; i++) {
    const char *root_id = g_ptr_array_index(root_ids, i);
    add_subtree_to_render_order(self, root_id);
  }

  g_message("[THREAD_VIEW] build_thread_graph: render_order has %u nodes",
            self->thread_graph->render_order->len);

  g_ptr_array_unref(root_ids);
}

/* Internal: create collapse indicator showing hidden reply count */
static GtkWidget *create_collapse_indicator(GnostrThreadView *self, ThreadNode *node) {
  GtkWidget *btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
  gtk_widget_add_css_class(btn, "thread-collapsed-indicator");
  gtk_widget_add_css_class(btn, "flat");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_button_set_child(GTK_BUTTON(btn), box);

  GtkWidget *icon = gtk_image_new_from_icon_name("go-down-symbolic");
  gtk_box_append(GTK_BOX(box), icon);

  char *label_text = g_strdup_printf("%u more replies", node->child_count);
  GtkWidget *label = gtk_label_new(label_text);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_box_append(GTK_BOX(box), label);
  g_free(label_text);

  /* Set margin based on depth */
  gtk_widget_set_margin_start(btn, 24 * (node->depth + 1));

  /* Store event ID for click handler */
  g_object_set_data_full(G_OBJECT(btn), "event-id", g_strdup(node->event->id_hex), g_free);

  return btn;
}

/* Callback for collapse indicator click */
static void on_collapse_indicator_clicked(GtkButton *btn, gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  const char *event_id = g_object_get_data(G_OBJECT(btn), "event-id");

  if (event_id) {
    gnostr_thread_view_toggle_branch(self, event_id);
  }
}

/* nostrc-evz1: Factory setup callback - creates GnostrNoteCardRow widgets */
static void thread_factory_setup_cb(GtkSignalListItemFactory *factory,
                                     GtkListItem *item,
                                     gpointer user_data) {
  (void)factory;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  (void)self;

  GtkWidget *row = GTK_WIDGET(gnostr_note_card_row_new());

  /* Connect signals - these relay through GnostrThreadView */
  g_signal_connect(row, "open-profile", G_CALLBACK(on_note_open_profile), user_data);
  g_signal_connect(row, "view-thread-requested", G_CALLBACK(on_note_view_thread), user_data);
  g_signal_connect(row, "report-note-requested", G_CALLBACK(on_note_report_requested), user_data);

  gtk_list_item_set_child(item, row);
}

/* nostrc-evz1: Factory bind callback - binds GnNostrEventItem to GnostrNoteCardRow */
static void thread_factory_bind_cb(GtkSignalListItemFactory *factory,
                                    GtkListItem *item,
                                    gpointer user_data) {
  (void)factory;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  GObject *obj = gtk_list_item_get_item(item);
  GtkWidget *row = gtk_list_item_get_child(item);

  if (!obj || !GN_IS_NOSTR_EVENT_ITEM(obj) || !GNOSTR_IS_NOTE_CARD_ROW(row)) {
    return;
  }

  GnNostrEventItem *event_item = GN_NOSTR_EVENT_ITEM(obj);
  GnostrNoteCardRow *card = GNOSTR_NOTE_CARD_ROW(row);

  /* nostrc-7t5x: CRITICAL - Prepare row for binding. Sets binding_id which gates
   * all setter functions. Without this, set_content/set_author/etc return early
   * and the card displays no content. Matches timeline-view pattern (nostrc-o7pp). */
  gnostr_note_card_row_prepare_for_bind(card);

  /* Get event data */
  const char *event_id = gn_nostr_event_item_get_event_id(event_item);
  const char *pubkey = gn_nostr_event_item_get_pubkey(event_item);
  const char *content = gn_nostr_event_item_get_content(event_item);
  gint64 created_at = gn_nostr_event_item_get_created_at(event_item);
  guint depth = gn_nostr_event_item_get_reply_depth(event_item);
  const char *root_id = gn_nostr_event_item_get_thread_root_id(event_item);
  const char *parent_id = gn_nostr_event_item_get_parent_id(event_item);

  /* Get profile info - g_object_get returns newly allocated strings */
  GnNostrProfile *profile = gn_nostr_event_item_get_profile(event_item);
  gchar *display_name = NULL;
  gchar *handle = NULL;
  gchar *avatar_url = NULL;
  gchar *nip05 = NULL;

  if (profile) {
    g_object_get(profile,
                 "display-name", &display_name,
                 "name", &handle,
                 "picture-url", &avatar_url,
                 "nip05", &nip05,
                 NULL);
  }

  /* Set author info with fallback */
  if (!display_name && !handle && pubkey) {
    char fallback[20];
    snprintf(fallback, sizeof(fallback), "%.8s...", pubkey);
    gnostr_note_card_row_set_author(card, fallback, NULL, avatar_url);
  } else {
    gnostr_note_card_row_set_author(card, display_name, handle, avatar_url);
  }

  /* Set content and metadata */
  gnostr_note_card_row_set_timestamp(card, created_at, NULL);
  gnostr_note_card_row_set_content(card, content);
  gnostr_note_card_row_set_depth(card, depth);
  gnostr_note_card_row_set_ids(card, event_id, root_id, pubkey);

  gboolean is_reply = (parent_id != NULL);
  gnostr_note_card_row_set_thread_info(card, root_id, parent_id, NULL, is_reply);

  if (nip05 && pubkey) {
    gnostr_note_card_row_set_nip05(card, nip05, pubkey);
  }

  gnostr_note_card_row_set_logged_in(card, is_user_logged_in());

  /* Free profile strings from g_object_get */
  g_free(display_name);
  g_free(handle);
  g_free(avatar_url);
  g_free(nip05);

  /* Apply depth-based CSS class */
  char depth_class[16];
  snprintf(depth_class, sizeof(depth_class), "depth-%u", depth);
  gtk_widget_add_css_class(row, depth_class);

  /* Look up ThreadNode for focus path and other styling */
  if (self->thread_graph && event_id) {
    ThreadNode *node = g_hash_table_lookup(self->thread_graph->nodes, event_id);
    if (node) {
      /* Highlight focus event */
      if (self->focus_event_id && g_strcmp0(event_id, self->focus_event_id) == 0) {
        gtk_widget_add_css_class(row, "thread-focus-note");
      }

      /* Focus path styling */
      if (node->is_focus_path) {
        gtk_widget_add_css_class(row, "thread-focus-path");
      }

      /* Root note styling */
      if (self->thread_graph->root_id &&
          g_strcmp0(event_id, self->thread_graph->root_id) == 0) {
        gtk_widget_add_css_class(row, "thread-root-note");
      }
    }
  }
}

/* nostrc-evz1: Factory unbind callback - cleans up CSS classes */
static void thread_factory_unbind_cb(GtkSignalListItemFactory *factory,
                                      GtkListItem *item,
                                      gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *row = gtk_list_item_get_child(item);

  if (!GTK_IS_WIDGET(row)) return;

  /* nostrc-7t5x: CRITICAL - Prepare row for unbinding. Cancels async operations
   * and clears binding_id to prevent stale callbacks from corrupting widget state.
   * Must be called BEFORE CSS class cleanup. Matches timeline-view pattern. */
  if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
    gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(row));
  }

  /* Remove dynamic CSS classes */
  gtk_widget_remove_css_class(row, "thread-focus-note");
  gtk_widget_remove_css_class(row, "thread-focus-path");
  gtk_widget_remove_css_class(row, "thread-root-note");

  /* Remove depth classes */
  for (guint d = 0; d <= MAX_THREAD_DEPTH; d++) {
    char depth_class[16];
    snprintf(depth_class, sizeof(depth_class), "depth-%u", d);
    gtk_widget_remove_css_class(row, depth_class);
  }
}

/* Internal: rebuild UI from sorted events */
static void rebuild_thread_ui(GnostrThreadView *self) {
  if (!self->thread_model) return;

  /* Clear existing model items */
  g_list_store_remove_all(self->thread_model);

  /* Build thread graph for tree-structured rendering */
  build_thread_graph(self);

  if (!self->thread_graph || g_hash_table_size(self->thread_graph->nodes) == 0) {
    show_empty_state(self, "No messages in this thread");
    return;
  }

  /* Update title - show visible/total if some are collapsed */
  guint total_notes = g_hash_table_size(self->thread_graph->nodes);
  guint visible_notes = self->thread_graph->render_order->len;
  if (self->title_label) {
    char title[64];
    if (visible_notes < total_notes) {
      snprintf(title, sizeof(title), "Thread (%u of %u notes)", visible_notes, total_notes);
    } else {
      snprintf(title, sizeof(title), "Thread (%u notes)", total_notes);
    }
    gtk_label_set_text(GTK_LABEL(self->title_label), title);
  }

  /* Add GnNostrEventItem objects to the model in tree order */
  for (guint i = 0; i < self->thread_graph->render_order->len; i++) {
    ThreadNode *node = g_ptr_array_index(self->thread_graph->render_order, i);
    if (!node || !node->event) continue;

    ThreadEventItem *item = node->event;

    /* Update event depth from graph */
    item->depth = node->depth;

    /* Fetch profile if not already done */
    fetch_profile_for_event(self, item);

    /* Create GnNostrEventItem from ThreadEventItem data */
    GnNostrEventItem *event_item = gn_nostr_event_item_new(item->id_hex);

    /* Update with event data */
    gn_nostr_event_item_update_from_event(event_item,
                                           item->pubkey_hex,
                                           item->created_at,
                                           item->content,
                                           1); /* kind 1 = text note */

    /* Set thread info including depth */
    gn_nostr_event_item_set_thread_info(event_item,
                                         item->root_id,
                                         item->parent_id,
                                         item->depth);

    /* Create and set profile if we have profile data */
    if (item->display_name || item->handle || item->avatar_url || item->nip05) {
      GnNostrProfile *profile = gn_nostr_profile_new(item->pubkey_hex);
      if (item->display_name) {
        gn_nostr_profile_set_display_name(profile, item->display_name);
      }
      if (item->handle) {
        gn_nostr_profile_set_name(profile, item->handle);
      }
      if (item->avatar_url) {
        gn_nostr_profile_set_picture_url(profile, item->avatar_url);
      }
      if (item->nip05) {
        gn_nostr_profile_set_nip05(profile, item->nip05);
      }
      gn_nostr_event_item_set_profile(event_item, profile);
      g_object_unref(profile);
    }

    /* Add to model */
    g_list_store_append(self->thread_model, event_item);
    g_object_unref(event_item);
  }

  /* Show the scroll window */
  set_loading_state(self, FALSE);
  if (self->scroll_window) {
    gtk_widget_set_visible(self->scroll_window, TRUE);
  }

  /* nostrc-x3b: Don't update banner here during rebuild - the fetch flow
   * manages the banner state. Only update if not actively fetching. */
  if (!self->is_fetching_missing) {
    update_missing_events_banner(self, FALSE);
  }

  /* Scroll to focus event if set */
  if (self->focus_event_id) {
    /* Find the focus event in render order */
    for (guint i = 0; i < self->thread_graph->render_order->len; i++) {
      ThreadNode *node = g_ptr_array_index(self->thread_graph->render_order, i);
      if (node && node->event && g_strcmp0(node->event->id_hex, self->focus_event_id) == 0) {
        /* Scroll to this position after layout */
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll_window));
        if (vadj && self->thread_graph->render_order->len > 0) {
          /* Approximate position based on index */
          double fraction = (double)i / (double)self->thread_graph->render_order->len;
          double range = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_lower(vadj);
          gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj) + fraction * range);
        }
        break;
      }
    }
  }
}

/* Forward declarations */
static void fetch_missing_ancestors(GnostrThreadView *self);
static void fetch_nip65_for_missing_authors(GnostrThreadView *self);

/* nostrc-hl6: Context for NIP-65 relay fetch callback */
typedef struct {
  GnostrThreadView *self;
  gchar *pubkey_hex;
} Nip65FetchContext;

static void nip65_fetch_ctx_free(Nip65FetchContext *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

/* nostrc-hl6: Callback when NIP-65 relay list is fetched */
static void on_nip65_relays_fetched(GPtrArray *relays, gpointer user_data) {
  Nip65FetchContext *ctx = (Nip65FetchContext *)user_data;
  if (!ctx || !GNOSTR_IS_THREAD_VIEW(ctx->self)) {
    nip65_fetch_ctx_free(ctx);
    return;
  }

  GnostrThreadView *self = ctx->self;
  /* nostrc-59nk: Check disposal flag to prevent modifying disposed widgets */
  if (self->disposed) {
    nip65_fetch_ctx_free(ctx);
    return;
  }

  if (!relays || relays->len == 0) {
    g_debug("[THREAD_VIEW] NIP-65: No relays found for author %.16s...", ctx->pubkey_hex);
    nip65_fetch_ctx_free(ctx);
    return;
  }

  /* Get write relays - these are where the author publishes their posts */
  GPtrArray *write_relays = gnostr_nip65_get_write_relays(relays);
  g_message("[THREAD_VIEW] NIP-65: Author %.16s... has %u write relays",
            ctx->pubkey_hex, write_relays ? write_relays->len : 0);

  if (write_relays && write_relays->len > 0) {
    for (guint i = 0; i < write_relays->len; i++) {
      g_message("[THREAD_VIEW]   Write relay: %s", (const char *)g_ptr_array_index(write_relays, i));
    }

    /* Re-trigger ancestor fetch - the new relays will be picked up */
    /* Reset depth to allow another traversal attempt with new relays */
    self->ancestor_fetch_depth = 0;
    fetch_missing_ancestors(self);
  }

  if (write_relays) g_ptr_array_unref(write_relays);
  /* relays array is owned by caller (gnostr_nip65_fetch_relays_async) */
  nip65_fetch_ctx_free(ctx);
}

/* nostrc-hl6, nostrc-hb7c: Fetch NIP-65 relay lists for authors of missing events.
 * When root/parent events are not found, we extract p-tags from reply events
 * to find the pubkeys of authors we're replying to, then fetch their NIP-65
 * relay lists (kind 10002) and query their write relays for missing events. */
static void fetch_nip65_for_missing_authors(GnostrThreadView *self) {
  if (!self || !self->events_by_id) return;

  /* Collect pubkeys of authors we should query for NIP-65 relay lists.
   * nostrc-hb7c: Use p-tags from events that reference missing parents/roots.
   * The p-tags typically contain the pubkey of the author being replied to. */
  GPtrArray *pubkeys_to_fetch = g_ptr_array_new_with_free_func(g_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;

    /* Check if this event references missing parent or root */
    gboolean has_missing_parent = item->parent_id &&
        !g_hash_table_contains(self->events_by_id, item->parent_id);
    gboolean has_missing_root = item->root_id &&
        g_strcmp0(item->root_id, item->parent_id) != 0 &&
        !g_hash_table_contains(self->events_by_id, item->root_id);

    if (!has_missing_parent && !has_missing_root) continue;

    /* nostrc-hb7c: Extract pubkeys from p-tags - these are the authors
     * of events we're replying to (likely the missing parent/root authors) */
    if (item->mentioned_pubkeys) {
      for (guint i = 0; i < item->mentioned_pubkeys->len; i++) {
        const gchar *pubkey = g_ptr_array_index(item->mentioned_pubkeys, i);
        if (!pubkey || strlen(pubkey) != 64) continue;

        /* Skip if already fetched or queued */
        if (g_hash_table_contains(self->nip65_pubkeys_fetched, pubkey)) continue;

        gboolean found = FALSE;
        for (guint j = 0; j < pubkeys_to_fetch->len; j++) {
          if (g_strcmp0(g_ptr_array_index(pubkeys_to_fetch, j), pubkey) == 0) {
            found = TRUE;
            break;
          }
        }
        if (!found) {
          g_ptr_array_add(pubkeys_to_fetch, g_strdup(pubkey));
          g_message("[THREAD_VIEW] NIP-65: Will fetch relay list for %.16s... (p-tag from %.16s...)",
                    pubkey, item->id_hex);
        }
      }
    }
  }

  if (pubkeys_to_fetch->len == 0) {
    g_debug("[THREAD_VIEW] NIP-65: No authors to fetch relay lists for");
    g_ptr_array_unref(pubkeys_to_fetch);
    return;
  }

  g_message("[THREAD_VIEW] NIP-65: Fetching relay lists for %u authors", pubkeys_to_fetch->len);

  /* Fetch NIP-65 for each pubkey (mark as fetched first to prevent duplicates) */
  for (guint i = 0; i < pubkeys_to_fetch->len; i++) {
    const gchar *pubkey = g_ptr_array_index(pubkeys_to_fetch, i);

    /* Mark as fetched */
    g_hash_table_insert(self->nip65_pubkeys_fetched, g_strdup(pubkey), GINT_TO_POINTER(1));

    /* Create context for callback */
    Nip65FetchContext *ctx = g_new0(Nip65FetchContext, 1);
    ctx->self = self;
    ctx->pubkey_hex = g_strdup(pubkey);

    g_message("[THREAD_VIEW] NIP-65: Fetching relay list for %.16s...", pubkey);
    gnostr_nip65_fetch_relays_async(pubkey, self->fetch_cancellable,
                                     on_nip65_relays_fetched, ctx);
  }

  g_ptr_array_unref(pubkeys_to_fetch);
}

/* Callback for relay query completion */
static void on_thread_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  /* nostrc-xr65: Check result BEFORE accessing user_data (may be dangling if cancelled) */
  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("[THREAD_VIEW] Query failed: %s", error->message);
    }
    g_error_free(error);
    if (results) g_ptr_array_unref(results);
    return;
  }

  /* Now safe to access user_data */
  if (!user_data) return;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self) || self->disposed) return;

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

  /* Add events to our collection; defer NDB ingestion to background (nostrc-mzab) */
  GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < results->len; i++) {
    const char *json = g_ptr_array_index(results, i);
    if (json) {
      g_ptr_array_add(to_ingest, g_strdup(json));
      add_event_from_json(self, json);
    }
  }

  g_ptr_array_unref(results);
  storage_ndb_ingest_events_async(to_ingest); /* takes ownership */

  /* Rebuild UI */
  rebuild_thread_ui(self);

  /* Check if new events reference ancestors we don't have yet */
  fetch_missing_ancestors(self);

  /* Fetch children of newly discovered events for complete graph */
  fetch_children_from_relays(self);
}

/* Callback for root event fetch completion */
static void on_root_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  /* nostrc-xr65: Check result BEFORE accessing user_data (may be dangling if cancelled) */
  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_message("[THREAD_VIEW] Root fetch failed: %s", error->message);
    }
    g_error_free(error);
    if (results) g_ptr_array_unref(results);
    return;
  }

  /* Now safe to access user_data */
  if (!user_data) return;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self) || self->disposed) return;

  g_message("[THREAD_VIEW] on_root_fetch_done: callback fired");

  if (results && results->len > 0) {
    g_message("[THREAD_VIEW] Received %u root/ancestor events from relays", results->len);

    /* Add events to our collection; defer NDB ingestion to background (nostrc-mzab) */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (json) {
        g_ptr_array_add(to_ingest, g_strdup(json));
        add_event_from_json(self, json);
      }
    }
    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */

    /* Rebuild UI with new events */
    rebuild_thread_ui(self);
  } else {
    g_message("[THREAD_VIEW] on_root_fetch_done: NO RESULTS from relay query");
  }

  if (results) g_ptr_array_unref(results);

  /* nostrc-uxz: Always check for missing ancestors, even if this query returned
   * no results. The focus event may have been loaded from nostrdb before the
   * relay query, and its ancestors need to be fetched. */
  if (g_hash_table_size(self->events_by_id) > 0) {
    fetch_missing_ancestors(self);
  }
}

/* Callback for missing ancestor fetch completion.
 * nostrc-46g: Improved to continue chain traversal until root is reached. */
static void on_missing_ancestors_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  /* nostrc-xr65: Check result BEFORE accessing user_data (may be dangling if cancelled) */
  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("[THREAD_VIEW] Missing ancestors fetch failed: %s", error->message);
    }
    g_error_free(error);
    if (results) g_ptr_array_unref(results);
    return;
  }

  /* Now safe to access user_data */
  if (!user_data) return;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self) || self->disposed) return;

  gboolean found_new_events = FALSE;

  if (results && results->len > 0) {
    g_message("[THREAD_VIEW] Received %u ancestor events from relays", results->len);

    /* Defer NDB ingestion to background (nostrc-mzab) */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (json) {
        g_ptr_array_add(to_ingest, g_strdup(json));
        ThreadEventItem *item = add_event_from_json(self, json);
        if (item) {
          g_message("[THREAD_VIEW]   Added ancestor: %.16s...", item->id_hex);
          found_new_events = TRUE;
        }
      }
    }
    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */

    /* Rebuild UI with new events */
    rebuild_thread_ui(self);
  } else {
    g_message("[THREAD_VIEW] No ancestor events returned from relay query");
  }

  if (results) g_ptr_array_unref(results);

  /* nostrc-46g: Continue chain traversal if we found new events.
   * New events may reference additional ancestors we need to fetch. */
  if (found_new_events) {
    fetch_missing_ancestors(self);
    /* Also fetch children of the new ancestors for complete graph */
    fetch_children_from_relays(self);
  } else {
    g_debug("[THREAD_VIEW] No new ancestor events found from relay query");
    /* nostrc-hl6: Try fetching NIP-65 relay lists for missing authors.
     * This may find relays where the root/parent events are published. */
    fetch_nip65_for_missing_authors(self);
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
    /* Fetching complete (hit depth limit) - update banner to show final state */
    update_missing_events_banner(self, FALSE);
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
    /* nostrc-uxz: Add detailed logging to diagnose why no missing ancestors found */
    guint total_events = g_hash_table_size(self->events_by_id);
    guint already_fetched = g_hash_table_size(self->ancestors_fetched);
    g_message("[THREAD_VIEW] No missing ancestors to fetch (events=%u, already_fetched=%u)",
              total_events, already_fetched);

    /* Log the events we have and their parent/root references */
    GHashTableIter dbg_iter;
    gpointer dbg_key, dbg_value;
    g_hash_table_iter_init(&dbg_iter, self->events_by_id);
    while (g_hash_table_iter_next(&dbg_iter, &dbg_key, &dbg_value)) {
      ThreadEventItem *item = (ThreadEventItem *)dbg_value;
      gboolean parent_present = item->parent_id ? g_hash_table_contains(self->events_by_id, item->parent_id) : TRUE;
      gboolean root_present = item->root_id ? g_hash_table_contains(self->events_by_id, item->root_id) : TRUE;
      gboolean parent_fetched = item->parent_id ? g_hash_table_contains(self->ancestors_fetched, item->parent_id) : FALSE;
      gboolean root_fetched = item->root_id ? g_hash_table_contains(self->ancestors_fetched, item->root_id) : FALSE;
      g_message("[THREAD_VIEW]   Event %.16s... parent=%.16s%s (%s) root=%.16s%s (%s)",
                item->id_hex,
                item->parent_id ? item->parent_id : "(none)", item->parent_id ? "..." : "",
                parent_present ? "present" : (parent_fetched ? "fetched" : "MISSING"),
                item->root_id ? item->root_id : "(none)", item->root_id ? "..." : "",
                root_present ? "present" : (root_fetched ? "fetched" : "MISSING"));
    }

    g_ptr_array_unref(missing_ids);
    g_ptr_array_unref(relay_hints);
    /* Fetching complete - update banner to show final state */
    update_missing_events_banner(self, FALSE);
    return;
  }

  /* Show spinner banner while fetching */
  update_missing_events_banner(self, TRUE);

  /* nostrc-46g: Increment depth counter for chain traversal tracking */
  self->ancestor_fetch_depth++;

  /* nostrc-7r5: Log relay hints being used */
  if (relay_hints->len > 0) {
    g_message("[THREAD_VIEW] Fetching %u missing ancestor events (depth %u) with %u relay hints",
              missing_ids->len, self->ancestor_fetch_depth, relay_hints->len);
    for (guint i = 0; i < relay_hints->len; i++) {
      g_message("[THREAD_VIEW]   Hint: %s", (const char *)g_ptr_array_index(relay_hints, i));
    }
  } else {
    g_message("[THREAD_VIEW] Fetching %u missing ancestor events (depth %u), no relay hints",
              missing_ids->len, self->ancestor_fetch_depth);
  }
  /* Log the missing IDs we're looking for */
  for (guint i = 0; i < missing_ids->len; i++) {
    g_message("[THREAD_VIEW]   Missing: %.16s...", (const char *)g_ptr_array_index(missing_ids, i));
  }

  /* Build filter with missing IDs */
  GNostrFilter *gf = gnostr_filter_new();
  gint kinds[2] = { 1, 1111 };
  gnostr_filter_set_kinds(gf, kinds, 2);

  for (guint i = 0; i < missing_ids->len; i++) {
    gnostr_filter_add_id(gf, g_ptr_array_index(missing_ids, i));
  }
  gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
  NostrFilter *filter = gnostr_filter_build(gf);
  g_object_unref(gf);

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

  {
    GNostrPool *pool = gnostr_get_shared_query_pool();
    gnostr_pool_sync_relays(pool, (const gchar **)urls, all_relays->len);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_missing_ancestors_done, self);
  }

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

  /* Build relay URL list: configured relays + hints from loaded events */
  GPtrArray *all_relays = g_ptr_array_new_with_free_func(g_free);

  /* Add configured read relays first */
  GPtrArray *config_relays = gnostr_get_read_relay_urls();
  for (guint i = 0; i < config_relays->len; i++) {
    g_ptr_array_add(all_relays, g_strdup(g_ptr_array_index(config_relays, i)));
  }
  g_ptr_array_unref(config_relays);

  /* Add relay hints from loaded events */
  GHashTableIter hint_iter;
  gpointer hkey, hvalue;
  g_hash_table_iter_init(&hint_iter, self->events_by_id);
  while (g_hash_table_iter_next(&hint_iter, &hkey, &hvalue)) {
    ThreadEventItem *item = (ThreadEventItem *)hvalue;
    if (item->root_relay_hint) {
      add_relay_hint_if_unique(all_relays, item->root_relay_hint);
    }
    if (item->parent_relay_hint) {
      add_relay_hint_if_unique(all_relays, item->parent_relay_hint);
    }
  }

  g_message("[THREAD_VIEW] fetch_thread_from_relays: got %u relay URLs (config + hints)", all_relays->len);
  for (guint i = 0; i < all_relays->len; i++) {
    g_message("[THREAD_VIEW]   Relay %u: %s", i, (const char *)g_ptr_array_index(all_relays, i));
  }

  const char **urls = g_new0(const char*, all_relays->len);
  for (guint i = 0; i < all_relays->len; i++) {
    urls[i] = g_ptr_array_index(all_relays, i);
  }

  g_message("[THREAD_VIEW] ====== STARTING RELAY QUERIES ======");
  g_message("[THREAD_VIEW] root=%.16s... focus=%.16s...", root, focus ? focus : "(same)");
  g_message("[THREAD_VIEW] Querying %u relays", all_relays->len);

  /* Query 1: Fetch all replies and comments (events with #e tag referencing root)
   * NIP-22: kind 1111 is for comments, which use E tag (uppercase) for root reference */
  {
    GNostrFilter *gf = gnostr_filter_new();
    gint kinds[2] = { 1, 1111 }; /* Text notes and NIP-22 comments */
    gnostr_filter_set_kinds(gf, kinds, 2);
    gnostr_filter_tags_append(gf, "e", root);
    gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
    NostrFilter *filter_replies = gnostr_filter_build(gf);
    g_object_unref(gf);

    {
      GNostrPool *pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(pool, (const gchar **)urls, all_relays->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter_replies);
      gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_thread_query_done, self);
    }

    nostr_filter_free(filter_replies);
  }

  /* Query 2: Fetch root event and focus event by ID (they may not reference themselves) */
  g_message("[THREAD_VIEW] Query 2: fetching root=%.16s... focus=%.16s...",
            root, focus ? focus : "(same)");

  {
    GNostrFilter *gf = gnostr_filter_new();
    gint kinds2[2] = { 1, 1111 };
    gnostr_filter_set_kinds(gf, kinds2, 2);  /* Include both kind 1 and 1111 */

    /* Add root ID */
    gnostr_filter_add_id(gf, root);

    /* Add focus ID if different from root */
    if (focus && g_strcmp0(focus, root) != 0) {
      gnostr_filter_add_id(gf, focus);
      g_message("[THREAD_VIEW] Query 2: also fetching focus (different from root)");
    }

    /* Also fetch any parent IDs we know about from loaded events */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->events_by_id);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      ThreadEventItem *item = (ThreadEventItem *)value;
      if (item->parent_id && !g_hash_table_contains(self->events_by_id, item->parent_id)) {
        gnostr_filter_add_id(gf, item->parent_id);
      }
      if (item->root_id && !g_hash_table_contains(self->events_by_id, item->root_id)) {
        gnostr_filter_add_id(gf, item->root_id);
      }
    }

    gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
    NostrFilter *filter_ids = gnostr_filter_build(gf);
    g_object_unref(gf);

    {
      GNostrPool *pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(pool, (const gchar **)urls, all_relays->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter_ids);
      gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_root_fetch_done, self);
    }

    nostr_filter_free(filter_ids);
  }

  /* Query 3: NIP-22 comments use uppercase E tag for root event reference */
  {
    GNostrFilter *gf = gnostr_filter_new();
    gint comment_kind[1] = { 1111 };
    gnostr_filter_set_kinds(gf, comment_kind, 1);
    gnostr_filter_tags_append(gf, "E", root);
    gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
    NostrFilter *filter_nip22 = gnostr_filter_build(gf);
    g_object_unref(gf);

    {
      GNostrPool *pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(pool, (const gchar **)urls, all_relays->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter_nip22);
      gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_thread_query_done, self);
    }

    nostr_filter_free(filter_nip22);
  }
  g_free(urls);
  g_ptr_array_unref(all_relays);

  /* Query 4: Fetch replies to the focus event specifically (children)
   * This enables bidirectional traversal - we want to see replies TO the focus event */
  if (focus && g_strcmp0(focus, root) != 0) {
    GNostrFilter *gf = gnostr_filter_new();
    gint kinds4[2] = { 1, 1111 };
    gnostr_filter_set_kinds(gf, kinds4, 2);
    gnostr_filter_tags_append(gf, "e", focus);
    gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
    NostrFilter *filter_focus_replies = gnostr_filter_build(gf);
    g_object_unref(gf);

    /* Mark focus as fetched for children */
    if (!g_hash_table_contains(self->children_fetched, focus)) {
      g_hash_table_insert(self->children_fetched, g_strdup(focus), GINT_TO_POINTER(1));
    }

    GPtrArray *relay_arr2 = gnostr_get_read_relay_urls();
    const char **urls2 = g_new0(const char*, relay_arr2->len);
    for (guint i = 0; i < relay_arr2->len; i++) {
      urls2[i] = g_ptr_array_index(relay_arr2, i);
    }

    {
      GNostrPool *pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(pool, (const gchar **)urls2, relay_arr2->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter_focus_replies);
      gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_thread_query_done, self);
    }

    nostr_filter_free(filter_focus_replies);
    g_free(urls2);
    g_ptr_array_unref(relay_arr2);
  }

  /* Mark root as fetched for children */
  if (!g_hash_table_contains(self->children_fetched, root)) {
    g_hash_table_insert(self->children_fetched, g_strdup(root), GINT_TO_POINTER(1));
  }
}

/* Callback for child discovery query completion */
static void on_children_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  /* nostrc-xr65: Check result BEFORE accessing user_data (may be dangling if cancelled) */
  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("[THREAD_VIEW] Children query failed: %s", error->message);
    }
    g_error_free(error);
    if (results) g_ptr_array_unref(results);
    return;
  }

  /* Now safe to access user_data */
  if (!user_data) return;
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self) || self->disposed) return;

  gboolean found_new = FALSE;

  if (results && results->len > 0) {
    g_debug("[THREAD_VIEW] Received %u child events from relays", results->len);

    /* Defer NDB ingestion to background (nostrc-mzab) */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (json) {
        g_ptr_array_add(to_ingest, g_strdup(json));
        ThreadEventItem *item = add_event_from_json(self, json);
        if (item) {
          found_new = TRUE;
        }
      }
    }
    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */
  }

  if (results) g_ptr_array_unref(results);

  if (found_new) {
    /* Rebuild UI with new events */
    rebuild_thread_ui(self);

    /* Fetch any missing ancestors of the new children */
    fetch_missing_ancestors(self);

    /* Continue iterative child discovery if we haven't reached the limit */
    fetch_children_from_relays(self);
  }
}

/* Internal: fetch children (replies) of events we have, but haven't queried yet.
 * This implements iterative child discovery for complete graph building. */
static void fetch_children_from_relays(GnostrThreadView *self) {
  if (!self || g_hash_table_size(self->events_by_id) == 0) return;

  /* Check iteration limit to prevent infinite loops */
  if (self->child_discovery_iteration >= MAX_CHILD_DISCOVERY_ITERATIONS) {
    g_debug("[THREAD_VIEW] Reached max child discovery iterations (%d), stopping",
            MAX_CHILD_DISCOVERY_ITERATIONS);
    return;
  }

  /* Collect event IDs that we haven't queried for children yet */
  GPtrArray *unfetched_ids = g_ptr_array_new_with_free_func(g_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *event_id = (const char *)key;

    /* Skip if we've already queried for this event's children */
    if (g_hash_table_contains(self->children_fetched, event_id)) {
      continue;
    }

    /* Add to list and mark as fetched */
    g_ptr_array_add(unfetched_ids, g_strdup(event_id));
    g_hash_table_insert(self->children_fetched, g_strdup(event_id), GINT_TO_POINTER(1));
  }

  if (unfetched_ids->len == 0) {
    g_ptr_array_unref(unfetched_ids);
    g_debug("[THREAD_VIEW] No more events to query for children, discovery complete");
    return;
  }

  self->child_discovery_iteration++;
  g_debug("[THREAD_VIEW] Fetching children for %u events (iteration %u)",
          unfetched_ids->len, self->child_discovery_iteration);

  /* Build filter with #e tags for all unfetched event IDs */
  GNostrFilter *gf = gnostr_filter_new();
  gint kinds[2] = { 1, 1111 };
  gnostr_filter_set_kinds(gf, kinds, 2);

  /* Add all event IDs as #e tag values (replies reference parent via #e) */
  for (guint i = 0; i < unfetched_ids->len; i++) {
    gnostr_filter_tags_append(gf, "e", g_ptr_array_index(unfetched_ids, i));
  }
  gnostr_filter_set_limit(gf, MAX_THREAD_EVENTS);
  NostrFilter *filter = gnostr_filter_build(gf);
  g_object_unref(gf);

  g_ptr_array_unref(unfetched_ids);

  /* Get relay URLs */
  GPtrArray *relay_arr = gnostr_get_read_relay_urls();
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Query relays */
  if (!self->fetch_cancellable) {
    self->fetch_cancellable = g_cancellable_new();
  }

  {
    GNostrPool *pool = gnostr_get_shared_query_pool();
    gnostr_pool_sync_relays(pool, (const gchar **)urls, relay_arr->len);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_children_query_done, self);
  }

  nostr_filter_free(filter);
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

  g_message("[THREAD_VIEW] load_thread: focus=%.16s%s root=%.16s%s",
            focus_id ? focus_id : "(none)", focus_id ? "..." : "",
            root_id ? root_id : "(none)", root_id ? "..." : "");

  if (!focus_id && !root_id) {
    show_empty_state(self, "No thread selected");
    return;
  }

  /* nostrc-46g: Reset ancestor tracking for new thread load */
  if (self->ancestors_fetched) {
    g_hash_table_remove_all(self->ancestors_fetched);
  }
  self->ancestor_fetch_depth = 0;

  /* Reset child tracking for bidirectional fetching */
  if (self->children_fetched) {
    g_hash_table_remove_all(self->children_fetched);
  }
  self->child_discovery_iteration = 0;

  /* Clear existing thread graph */
  if (self->thread_graph) {
    thread_graph_free(self->thread_graph);
    self->thread_graph = NULL;
  }

  set_loading_state(self, TRUE);

  /* First, try to load focus event from nostrdb */
  ThreadEventItem *focus_item = NULL;
  if (focus_id) {
    focus_item = load_event_by_id(self, focus_id);
    g_message("[THREAD_VIEW] nostrdb lookup for focus %.16s: %s",
              focus_id, focus_item ? "FOUND" : "NOT FOUND");

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
    int query_rc = storage_ndb_query(txn, filter_json, &results, &count);
    g_message("[THREAD_VIEW] nostrdb query for root %.16s: rc=%d count=%d",
              query_root, query_rc, count);
    if (query_rc == 0 && results) {
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

  /* Log total events loaded from nostrdb */
  guint ndb_count = g_hash_table_size(self->events_by_id);
  g_message("[THREAD_VIEW] Total events loaded from nostrdb: %u", ndb_count);

  /* Show what we have from local DB */
  if (ndb_count > 0) {
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

  if (!self->thread_model || !self->sorted_events) return;

  guint model_len = g_list_model_get_n_items(G_LIST_MODEL(self->thread_model));
  if (model_len == 0) return;

  /* nostrc-sk8o: Update profile data directly in model items instead of rebuilding.
   * This avoids O(N) profile fetches that rebuild_thread_ui() would trigger. */
  for (guint i = 0; i < model_len; i++) {
    GnNostrEventItem *event_item = g_list_model_get_item(G_LIST_MODEL(self->thread_model), i);
    if (!event_item) continue;

    const char *event_id = gn_nostr_event_item_get_event_id(event_item);
    if (!event_id) {
      g_object_unref(event_item);
      continue;
    }

    /* Find matching ThreadEventItem by event ID */
    ThreadEventItem *item = g_hash_table_lookup(self->events_by_id, event_id);
    if (item) {
      /* Update profile from cache */
      update_item_profile_from_cache(item);

      /* Update the GnNostrEventItem's profile */
      if (item->display_name || item->handle || item->avatar_url || item->nip05) {
        GnNostrProfile *profile = gn_nostr_profile_new(item->pubkey_hex);
        if (item->display_name) gn_nostr_profile_set_display_name(profile, item->display_name);
        if (item->handle) gn_nostr_profile_set_name(profile, item->handle);
        if (item->avatar_url) gn_nostr_profile_set_picture_url(profile, item->avatar_url);
        if (item->nip05) gn_nostr_profile_set_nip05(profile, item->nip05);
        gn_nostr_event_item_set_profile(event_item, profile);
        g_object_unref(profile);
      }
    }

    g_object_unref(event_item);
  }

  /* Signal that items have changed so factory rebinds with new data */
  g_list_model_items_changed(G_LIST_MODEL(self->thread_model), 0, model_len, model_len);
}

/* ========== nostrc-50t: nostrdb subscription for live thread updates ========== */

/* Debounce interval for UI rebuild after receiving new events (ms) */
#define THREAD_REBUILD_DEBOUNCE_MS 150

/* Timeout callback to rebuild the UI after receiving new events */
static gboolean on_rebuild_debounce_timeout(gpointer user_data) {
  GnostrThreadView *self = GNOSTR_THREAD_VIEW(user_data);
  if (!GNOSTR_IS_THREAD_VIEW(self)) return G_SOURCE_REMOVE;
  /* nostrc-59nk: Check disposal flag to prevent modifying disposed widgets */
  if (self->disposed) return G_SOURCE_REMOVE;

  self->rebuild_pending_id = 0;

  /* Rebuild UI with newly arrived events */
  rebuild_thread_ui(self);

  /* Check if new events reference ancestors we don't have yet */
  fetch_missing_ancestors(self);

  return G_SOURCE_REMOVE;
}

/* Schedule a debounced UI rebuild */
/* LEGITIMATE TIMEOUT - Debounce thread rebuild to batch rapid updates.
 * nostrc-b0h: Audited - debouncing expensive UI rebuilds is appropriate. */
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
  /* nostrc-59nk: Check disposal flag to prevent modifying disposed widgets */
  if (self->disposed) return;

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

/* ========== Public API for branch collapse/expand ========== */

void gnostr_thread_view_toggle_branch(GnostrThreadView *self, const char *event_id_hex) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));
  g_return_if_fail(event_id_hex != NULL);

  if (!self->thread_graph) return;

  ThreadNode *node = g_hash_table_lookup(self->thread_graph->nodes, event_id_hex);
  if (!node) return;

  /* Toggle collapsed state */
  node->is_collapsed = !node->is_collapsed;

  /* Rebuild UI to reflect change */
  rebuild_thread_ui(self);
}

void gnostr_thread_view_expand_all(GnostrThreadView *self) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!self->thread_graph) return;

  /* Expand all nodes */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->thread_graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    node->is_collapsed = FALSE;
  }

  /* Rebuild UI to reflect change */
  rebuild_thread_ui(self);
}

void gnostr_thread_view_collapse_non_focus(GnostrThreadView *self) {
  g_return_if_fail(GNOSTR_IS_THREAD_VIEW(self));

  if (!self->thread_graph) return;

  /* Collapse all nodes not on focus path that have children */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->thread_graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->is_focus_path && node->child_ids && node->child_ids->len > 0) {
      node->is_collapsed = TRUE;
    }
  }

  /* Rebuild UI to reflect change */
  rebuild_thread_ui(self);
}
