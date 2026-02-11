/**
 * GnostrSearchResultsView - Search view for finding notes
 *
 * Displays a search interface with filter chips and note results.
 * Supports:
 * - Local nostrdb text search (fast, works offline)
 * - Relay search via NIP-50 (optional, requires supporting relays)
 * - Search by content text, hashtags, and mentions
 */

#include "gnostr-search-results-view.h"
#include "note_card_row.h"
#include "gnostr-main-window.h"
#include "gnostr-profile-provider.h"
#include "../storage_ndb.h"
#include "../util/relays.h"
#include "nostr_pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include <gio/gio.h>
#include <string.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-search-results-view.ui"

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

/* Maximum results to display */
#define MAX_LOCAL_RESULTS 100
#define MAX_RELAY_RESULTS 50

/* Search result item for the list model */
typedef struct {
    GObject parent_instance;
    char *event_id_hex;
    char *pubkey_hex;
    char *content;
    gint64 created_at;
    char *author_name;
    char *author_handle;
    char *avatar_url;
} SearchResultItem;

typedef struct {
    GObjectClass parent_class;
} SearchResultItemClass;

GType search_result_item_get_type(void);
G_DEFINE_TYPE(SearchResultItem, search_result_item, G_TYPE_OBJECT)

static void search_result_item_finalize(GObject *obj) {
    SearchResultItem *self = (SearchResultItem *)obj;
    g_free(self->event_id_hex);
    g_free(self->pubkey_hex);
    g_free(self->content);
    g_free(self->author_name);
    g_free(self->author_handle);
    g_free(self->avatar_url);
    G_OBJECT_CLASS(search_result_item_parent_class)->finalize(obj);
}

static void search_result_item_class_init(SearchResultItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = search_result_item_finalize;
}

static void search_result_item_init(SearchResultItem *self) {
    (void)self;
}

static SearchResultItem *search_result_item_new(void) {
    return g_object_new(search_result_item_get_type(), NULL);
}

/* Main view structure */
struct _GnostrSearchResultsView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkSearchEntry *search_entry;
    GtkToggleButton *btn_local;
    GtkToggleButton *btn_relay;
    GtkStack *content_stack;
    GtkListView *results_list;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkScrolledWindow *scroller;
    GtkLabel *results_count_label;
    GtkLabel *no_results_hint;

    /* Model */
    GListStore *results_model;
    GtkSingleSelection *selection;

    /* Search state */
    GCancellable *search_cancellable;
    guint search_debounce_id;
    guint search_debounce_ms;
};

G_DEFINE_TYPE(GnostrSearchResultsView, gnostr_search_results_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_NOTE,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_SEARCH_HASHTAG,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void execute_local_search(GnostrSearchResultsView *self, const char *query);
static void execute_relay_search(GnostrSearchResultsView *self, const char *query);
static void populate_result_item(SearchResultItem *item, const char *event_json);

static void
gnostr_search_results_view_dispose(GObject *object)
{
    GnostrSearchResultsView *self = GNOSTR_SEARCH_RESULTS_VIEW(object);

    if (self->search_debounce_id > 0) {
        g_source_remove(self->search_debounce_id);
        self->search_debounce_id = 0;
    }

    if (self->search_cancellable) {
        g_cancellable_cancel(self->search_cancellable);
        g_clear_object(&self->search_cancellable);
    }

    g_clear_object(&self->results_model);
    g_clear_object(&self->selection);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_search_results_view_parent_class)->dispose(object);
}

static void
gnostr_search_results_view_finalize(GObject *object)
{
    G_OBJECT_CLASS(gnostr_search_results_view_parent_class)->finalize(object);
}

static gboolean
search_debounce_cb(gpointer user_data)
{
    GnostrSearchResultsView *self = GNOSTR_SEARCH_RESULTS_VIEW(user_data);
    self->search_debounce_id = 0;
    gnostr_search_results_view_execute_search(self);
    return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrSearchResultsView *self)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (!text || *text == '\0') {
        /* Empty search - show empty state */
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
        gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), FALSE);
        return;
    }

    /* LEGITIMATE TIMEOUT - Search input debounce (configurable, default 300ms).
     * nostrc-b0h: Audited - debouncing user input is appropriate. */
    if (self->search_debounce_id > 0) {
        g_source_remove(self->search_debounce_id);
    }
    self->search_debounce_id = g_timeout_add(
        self->search_debounce_ms ? self->search_debounce_ms : 300,
        search_debounce_cb, self);
}

static void
on_search_activate(GtkSearchEntry *entry, GnostrSearchResultsView *self)
{
    (void)entry;
    /* Search activated (Enter pressed) - execute immediately */
    if (self->search_debounce_id > 0) {
        g_source_remove(self->search_debounce_id);
        self->search_debounce_id = 0;
    }
    gnostr_search_results_view_execute_search(self);
}

static void
on_filter_toggled(GtkToggleButton *button, GnostrSearchResultsView *self)
{
    /* Ensure mutual exclusivity of filter buttons */
    if (gtk_toggle_button_get_active(button)) {
        if (button == self->btn_local) {
            gtk_toggle_button_set_active(self->btn_relay, FALSE);
        } else if (button == self->btn_relay) {
            gtk_toggle_button_set_active(self->btn_local, FALSE);
        }
        /* Re-execute search with new filter */
        const char *text = gnostr_search_results_view_get_search_text(self);
        if (text && *text) {
            gnostr_search_results_view_execute_search(self);
        }
    } else {
        /* Don't allow both to be inactive - keep at least one active */
        if (!gtk_toggle_button_get_active(self->btn_local) &&
            !gtk_toggle_button_get_active(self->btn_relay)) {
            gtk_toggle_button_set_active(button, TRUE);
        }
    }
}

/* Row factory setup callback */
static void
setup_result_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GnostrNoteCardRow *row = gnostr_note_card_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

/* Row factory bind callback */
static void
bind_result_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrSearchResultsView *view = GNOSTR_SEARCH_RESULTS_VIEW(user_data);
    (void)view;

    GnostrNoteCardRow *row = GNOSTR_NOTE_CARD_ROW(gtk_list_item_get_child(list_item));
    SearchResultItem *item = g_object_ref(gtk_list_item_get_item(list_item));

    if (!row || !item) {
        if (item) g_object_unref(item);
        return;
    }

    /* Set author info */
    const char *display = item->author_name && *item->author_name
        ? item->author_name
        : (item->author_handle && *item->author_handle ? item->author_handle : "Unknown");
    const char *handle = item->author_handle && *item->author_handle
        ? item->author_handle
        : (item->pubkey_hex ? item->pubkey_hex : "");
    gnostr_note_card_row_set_author(row, display, handle, item->avatar_url);

    /* Set timestamp */
    gnostr_note_card_row_set_timestamp(row, item->created_at, NULL);

    /* Set content */
    gnostr_note_card_row_set_content(row, item->content);

    /* Set IDs */
    gnostr_note_card_row_set_ids(row, item->event_id_hex, NULL, item->pubkey_hex);

    /* Set login state for authentication-required buttons */
    gnostr_note_card_row_set_logged_in(row, is_user_logged_in());

    g_object_unref(item);
}

/* Row factory unbind callback — nostrc-b3b0: prevent Pango SEGV on model clear.
 * When g_list_store_remove_all is called, GTK unbinds rows before disposal.
 * Without this, NoteCardRow dispose runs with live PangoLayout refs → SEGV. */
static void
unbind_result_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *child = gtk_list_item_get_child(list_item);
    if (child && GNOSTR_IS_NOTE_CARD_ROW(child)) {
        gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(child));
    }
}

/* Row factory teardown callback — nostrc-b3b0: safety net for mass removal.
 * During g_list_store_remove_all, GTK may teardown rows whose unbind already
 * ran (prepare_for_unbind is idempotent via the disposed flag). But if teardown
 * fires without a prior unbind (edge case during rapid model changes), this
 * prevents Pango SEGV from uncleaned PangoLayouts. */
static void
teardown_result_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *child = gtk_list_item_get_child(list_item);
    if (child && GNOSTR_IS_NOTE_CARD_ROW(child)) {
        gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(child));
    }
}

/* Row activation handler */
static void
on_row_activated(GtkListView *list_view, guint position, gpointer user_data)
{
    GnostrSearchResultsView *self = GNOSTR_SEARCH_RESULTS_VIEW(user_data);
    (void)list_view;

    SearchResultItem *item = g_list_model_get_item(G_LIST_MODEL(self->results_model), position);
    if (item && item->event_id_hex) {
        g_signal_emit(self, signals[SIGNAL_OPEN_NOTE], 0, item->event_id_hex);
    }
    if (item) g_object_unref(item);
}

static void
gnostr_search_results_view_class_init(GnostrSearchResultsViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_search_results_view_dispose;
    object_class->finalize = gnostr_search_results_view_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, search_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, btn_local);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, btn_relay);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, results_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, results_count_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrSearchResultsView, no_results_hint);

    /* Signals */
    signals[SIGNAL_OPEN_NOTE] = g_signal_new(
        "open-note",
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

    signals[SIGNAL_SEARCH_HASHTAG] = g_signal_new(
        "search-hashtag",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "search-results-view");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_search_results_view_init(GnostrSearchResultsView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    /* Initialize model */
    self->results_model = g_list_store_new(search_result_item_get_type());
    self->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->results_model)));
    gtk_single_selection_set_autoselect(self->selection, FALSE);

    /* Set up list view with factory */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_result_row), self);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_result_row), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(unbind_result_row), self);
    g_signal_connect(factory, "teardown", G_CALLBACK(teardown_result_row), self);
    gtk_list_view_set_factory(self->results_list, factory);
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->selection));
    g_object_unref(factory);

    /* Connect signals */
    g_signal_connect(self->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->search_entry, "activate",
                     G_CALLBACK(on_search_activate), self);

    g_signal_connect(self->btn_local, "toggled",
                     G_CALLBACK(on_filter_toggled), self);
    g_signal_connect(self->btn_relay, "toggled",
                     G_CALLBACK(on_filter_toggled), self);

    g_signal_connect(self->results_list, "activate",
                     G_CALLBACK(on_row_activated), self);

    /* Default to local filter active */
    gtk_toggle_button_set_active(self->btn_local, TRUE);

    /* Start with empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "empty");

    /* Default debounce */
    self->search_debounce_ms = 300;
}

GnostrSearchResultsView *
gnostr_search_results_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_SEARCH_RESULTS_VIEW, NULL);
}

void
gnostr_search_results_view_set_loading(GnostrSearchResultsView *self, gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self));

    if (is_loading) {
        gtk_spinner_start(self->loading_spinner);
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), FALSE);
    } else {
        gtk_spinner_stop(self->loading_spinner);
    }
}

void
gnostr_search_results_view_clear_results(GnostrSearchResultsView *self)
{
    g_return_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self));

    g_list_store_remove_all(self->results_model);
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
    gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), FALSE);
}

const char *
gnostr_search_results_view_get_search_text(GnostrSearchResultsView *self)
{
    g_return_val_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self), NULL);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    return (text && *text) ? text : NULL;
}

void
gnostr_search_results_view_set_search_text(GnostrSearchResultsView *self, const char *text)
{
    g_return_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self));

    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), text ? text : "");
}

gboolean
gnostr_search_results_view_is_local_search(GnostrSearchResultsView *self)
{
    g_return_val_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self), TRUE);
    return gtk_toggle_button_get_active(self->btn_local);
}

void
gnostr_search_results_view_execute_search(GnostrSearchResultsView *self)
{
    g_return_if_fail(GNOSTR_IS_SEARCH_RESULTS_VIEW(self));

    const char *query = gnostr_search_results_view_get_search_text(self);
    if (!query || !*query) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
        return;
    }

    /* Cancel any pending search */
    if (self->search_cancellable) {
        g_cancellable_cancel(self->search_cancellable);
        g_clear_object(&self->search_cancellable);
    }
    self->search_cancellable = g_cancellable_new();

    /* Show loading state */
    gnostr_search_results_view_set_loading(self, TRUE);

    /* Clear previous results */
    g_list_store_remove_all(self->results_model);

    if (gnostr_search_results_view_is_local_search(self)) {
        execute_local_search(self, query);
    } else {
        execute_relay_search(self, query);
    }
}

/* Populate a SearchResultItem from event JSON */
static void
populate_result_item(SearchResultItem *item, const char *event_json)
{
    if (!item || !event_json) return;

    NostrEvent *evt = nostr_event_new();
    if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
        if (evt) nostr_event_free(evt);
        return;
    }

    /* Extract event data */
    const char *id = nostr_event_get_id(evt);
    const char *pubkey = nostr_event_get_pubkey(evt);
    const char *content = nostr_event_get_content(evt);
    uint32_t created_at = nostr_event_get_created_at(evt);

    if (id) item->event_id_hex = g_strdup(id);
    if (pubkey) item->pubkey_hex = g_strdup(pubkey);
    if (content) item->content = g_strdup(content);
    item->created_at = (gint64)created_at;

    /* Try to get profile info from provider cache */
    if (pubkey) {
        GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey);
        if (meta) {
            if (meta->display_name && *meta->display_name) {
                item->author_name = g_strdup(meta->display_name);
            } else if (meta->name && *meta->name) {
                item->author_name = g_strdup(meta->name);
            }
            if (meta->name && *meta->name) {
                item->author_handle = g_strdup_printf("@%s", meta->name);
            }
            if (meta->picture && *meta->picture) {
                item->avatar_url = g_strdup(meta->picture);
            }
            gnostr_profile_meta_free(meta);
        }
    }

    nostr_event_free(evt);
}

/* Execute local nostrdb text search */
static void
execute_local_search(GnostrSearchResultsView *self, const char *query)
{
    void *txn = NULL;
    int rc = storage_ndb_begin_query(&txn);
    if (rc != 0 || !txn) {
        g_warning("[SEARCH] Failed to begin nostrdb query transaction");
        gnostr_search_results_view_set_loading(self, FALSE);
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint, "Database temporarily unavailable. Please try again.");
        return;
    }

    /* Build config JSON for text search - limit to kind 1 (text notes) */
    char config_json[256];
    snprintf(config_json, sizeof(config_json),
             "{\"limit\":%d,\"kinds\":[1]}", MAX_LOCAL_RESULTS);

    char **results = NULL;
    int count = 0;

    rc = storage_ndb_text_search(txn, query, config_json, &results, &count);
    storage_ndb_end_query(txn);

    if (rc != 0) {
        g_warning("[SEARCH] Text search failed with rc=%d", rc);
        gnostr_search_results_view_set_loading(self, FALSE);
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint, "Search failed. Please try again.");
        return;
    }

    gnostr_search_results_view_set_loading(self, FALSE);

    if (count == 0 || !results) {
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint,
            "Try a different search term or switch to relay search.");
        gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), FALSE);
        return;
    }

    /* Populate results */
    for (int i = 0; i < count && results[i]; i++) {
        SearchResultItem *item = search_result_item_new();
        populate_result_item(item, results[i]);
        g_list_store_append(self->results_model, item);
        g_object_unref(item);
    }

    /* Clean up */
    storage_ndb_free_results(results, count);

    /* Update UI */
    char count_text[64];
    snprintf(count_text, sizeof(count_text), "%d result%s found",
             count, count == 1 ? "" : "s");
    gtk_label_set_text(self->results_count_label, count_text);
    gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), TRUE);
    gtk_stack_set_visible_child_name(self->content_stack, "results");
}

/* Relay search completion callback */
typedef struct {
    GnostrSearchResultsView *view;
    GCancellable *cancellable;
} RelaySearchCtx;

static void
on_relay_search_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RelaySearchCtx *ctx = (RelaySearchCtx *)user_data;
    GnostrSearchResultsView *self = ctx->view;

    if (g_cancellable_is_cancelled(ctx->cancellable)) {
        g_object_unref(ctx->cancellable);
        g_free(ctx);
        return;
    }

    GError *error = NULL;
    GPtrArray *events = gnostr_pool_query_finish(
        GNOSTR_POOL(source), result, &error);

    gnostr_search_results_view_set_loading(self, FALSE);

    if (error) {
        g_warning("[SEARCH] Relay search failed: %s", error->message);
        g_error_free(error);
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint,
            "Relay search failed. The relay may not support NIP-50 search.");
        g_object_unref(ctx->cancellable);
        g_free(ctx);
        return;
    }

    if (!events || events->len == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint,
            "No results from relays. Try a different search term or local search.");
        gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), FALSE);
        if (events) g_ptr_array_unref(events);
        g_object_unref(ctx->cancellable);
        g_free(ctx);
        return;
    }

    /* Populate results and defer NDB ingestion to background (nostrc-mzab) */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < events->len; i++) {
        const char *event_json = g_ptr_array_index(events, i);
        if (!event_json) continue;

        g_ptr_array_add(to_ingest, g_strdup(event_json));

        SearchResultItem *item = search_result_item_new();
        populate_result_item(item, event_json);
        g_list_store_append(self->results_model, item);
        g_object_unref(item);
    }
    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */

    /* Update UI */
    char count_text[64];
    snprintf(count_text, sizeof(count_text), "%u result%s found",
             events->len, events->len == 1 ? "" : "s");
    gtk_label_set_text(self->results_count_label, count_text);
    gtk_widget_set_visible(GTK_WIDGET(self->results_count_label), TRUE);
    gtk_stack_set_visible_child_name(self->content_stack, "results");

    g_ptr_array_unref(events);
    g_object_unref(ctx->cancellable);
    g_free(ctx);
}

/* Execute relay search via NIP-50 */
static void
execute_relay_search(GnostrSearchResultsView *self, const char *query)
{
    /* Get relay URLs that support NIP-50 (or all relays as fallback) */
    GPtrArray *relay_urls = gnostr_get_read_relay_urls();
    if (!relay_urls || relay_urls->len == 0) {
        g_warning("[SEARCH] No relays configured for search");
        gnostr_search_results_view_set_loading(self, FALSE);
        gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        gtk_label_set_text(self->no_results_hint,
            "No relays configured. Add relays in settings.");
        if (relay_urls) g_ptr_array_unref(relay_urls);
        return;
    }

    /* Build URL array */
    const char **urls = g_new0(const char *, relay_urls->len);
    for (guint i = 0; i < relay_urls->len; i++) {
        urls[i] = g_ptr_array_index(relay_urls, i);
    }

    /* Create NIP-50 search filter */
    NostrFilter *filter = nostr_filter_new();
    int kinds[] = {1};  /* Text notes only */
    nostr_filter_set_kinds(filter, kinds, 1);
    nostr_filter_set_limit(filter, MAX_RELAY_RESULTS);
    nostr_filter_set_search(filter, query);

    /* Create pool and execute search */
    static GNostrPool *s_pool = NULL;
    if (!s_pool) s_pool = gnostr_pool_new();

    RelaySearchCtx *ctx = g_new0(RelaySearchCtx, 1);
    ctx->view = self;
    ctx->cancellable = g_object_ref(self->search_cancellable);

    gnostr_pool_sync_relays(s_pool, (const gchar **)urls, relay_urls->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(s_pool, _qf, self->search_cancellable, on_relay_search_done, ctx);
    }

    nostr_filter_free(filter);
    g_free(urls);
    g_ptr_array_unref(relay_urls);
}
