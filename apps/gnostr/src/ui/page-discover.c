/**
 * GnostrPageDiscover - Discover page for browsing and searching profiles
 *
 * Two modes:
 * 1. Local: Browse all cached profiles from nostrdb with sorting/filtering
 * 2. Network: NIP-50 search to index relays
 *
 * Features:
 * - Virtualized GtkListView for performance with large profile counts
 * - Sort by: recently seen, alphabetical, following first
 * - Filter by search text (name, NIP-05, bio)
 * - Empty state when no profiles cached
 */

#define G_LOG_DOMAIN "gnostr-discover"

#include "page-discover.h"
#include "gnostr-profile-row.h"
#include "gnostr-live-card.h"
#include "gnostr-articles-view.h"
#include "../model/gn-profile-list-model.h"
#include "../model/gn-nostr-profile.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "../util/discover-search.h"
#include "../util/nip53_live.h"
#include "../storage_ndb.h"

#include <string.h>
#include "../util/debounce.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/page-discover.ui"

/* Debounce delay for search-as-you-type (milliseconds) */
#define SEARCH_DEBOUNCE_MS 300

/* Maximum network search results */
#define MAX_NETWORK_SEARCH_RESULTS 50

/* NIP-53 Live Activity kind */
#define KIND_LIVE_ACTIVITY 30311

struct _GnostrPageDiscover {
    GtkWidget parent_instance;

    /* Template widgets - Profile search */
    GtkSearchEntry *search_entry;
    GtkToggleButton *btn_local;
    GtkToggleButton *btn_network;
    GtkDropDown *sort_dropdown;
    GtkLabel *lbl_profile_count;
    GtkStack *content_stack;
    GtkListView *results_list;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkScrolledWindow *scroller;
    GtkButton *btn_communities;

    /* Template widgets - Mode toggle */
    GtkToggleButton *btn_mode_people;
    GtkToggleButton *btn_mode_live;
    GtkToggleButton *btn_mode_articles;
    GtkBox *filter_row;  /* Profile filter row (Local/Network) */

    /* Template widgets - Articles view (NIP-54 Wiki + NIP-23 Long-form) */
    GnostrArticlesView *articles_view;

    /* Template widgets - Live Activities */
    GtkFlowBox *live_flow_box;
    GtkFlowBox *scheduled_flow_box;
    GtkBox *live_now_section;
    GtkBox *scheduled_section;
    GtkSpinner *live_loading_spinner;
    GtkButton *btn_refresh_live;
    GtkButton *btn_refresh_live_empty;

    /* Local profile browser (mode: local) */
    GnProfileListModel *profile_model;
    GtkSingleSelection *local_selection;
    GtkListItemFactory *local_factory;

    /* Network search results (mode: network) */
    GListStore *network_results_model;
    GtkSingleSelection *network_selection;
    GtkListItemFactory *network_factory;

    /* Live activities data */
    GPtrArray *live_activities;      /* GnostrLiveActivity* array */
    GPtrArray *scheduled_activities; /* GnostrLiveActivity* array */
    GCancellable *live_cancellable;
    gboolean live_loaded;
    uint64_t live_sub_id;            /* nostrdb subscription ID for live events */

    /* State */
    GnostrDebounce *search_debounce;
    gboolean profiles_loaded;
    gboolean is_local_mode;
    gboolean is_live_mode;     /* TRUE for Live mode, FALSE for People mode */
    gboolean is_articles_mode; /* TRUE for Articles mode */
    gboolean articles_loaded;
    GCancellable *search_cancellable;
};

G_DEFINE_TYPE(GnostrPageDiscover, gnostr_page_discover, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_PROFILE,
    SIGNAL_FOLLOW_REQUESTED,
    SIGNAL_UNFOLLOW_REQUESTED,
    SIGNAL_MUTE_REQUESTED,
    SIGNAL_COPY_NPUB_REQUESTED,
    SIGNAL_OPEN_COMMUNITIES,
    SIGNAL_WATCH_LIVE,
    SIGNAL_OPEN_ARTICLE,
    SIGNAL_ZAP_ARTICLE_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Network Search Result Item --- */

#define GNOSTR_TYPE_NETWORK_RESULT_ITEM (gnostr_network_result_item_get_type())
G_DECLARE_FINAL_TYPE(GnostrNetworkResultItem, gnostr_network_result_item, GNOSTR, NETWORK_RESULT_ITEM, GObject)

struct _GnostrNetworkResultItem {
    GObject parent_instance;
    char *pubkey_hex;
    char *display_name;
    char *name;
    char *nip05;
    char *picture;
    char *about;
};

G_DEFINE_TYPE(GnostrNetworkResultItem, gnostr_network_result_item, G_TYPE_OBJECT)

static void
gnostr_network_result_item_finalize(GObject *object)
{
    GnostrNetworkResultItem *self = GNOSTR_NETWORK_RESULT_ITEM(object);
    g_free(self->pubkey_hex);
    g_free(self->display_name);
    g_free(self->name);
    g_free(self->nip05);
    g_free(self->picture);
    g_free(self->about);
    G_OBJECT_CLASS(gnostr_network_result_item_parent_class)->finalize(object);
}

static void
gnostr_network_result_item_class_init(GnostrNetworkResultItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = gnostr_network_result_item_finalize;
}

static void
gnostr_network_result_item_init(GnostrNetworkResultItem *self)
{
    (void)self;
}

static GnostrNetworkResultItem *
gnostr_network_result_item_new_from_search_result(GnostrSearchResult *result)
{
    GnostrNetworkResultItem *item = g_object_new(GNOSTR_TYPE_NETWORK_RESULT_ITEM, NULL);
    item->pubkey_hex = g_strdup(result->pubkey_hex);
    item->display_name = g_strdup(result->display_name);
    item->name = g_strdup(result->name);
    item->nip05 = g_strdup(result->nip05);
    item->picture = g_strdup(result->picture);
    item->about = g_strdup(result->about);
    return item;
}

/* --- Forward Declarations --- */

static void update_content_state(GnostrPageDiscover *self);
static void update_profile_count(GnostrPageDiscover *self);
static void switch_to_local_model(GnostrPageDiscover *self);
static void switch_to_network_model(GnostrPageDiscover *self);
static void switch_to_people_mode(GnostrPageDiscover *self);
static void switch_to_live_mode(GnostrPageDiscover *self);
static void update_live_content_state(GnostrPageDiscover *self);
static void populate_live_activities(GnostrPageDiscover *self);
static void clear_live_activities(GnostrPageDiscover *self);

/* --- Local Profile Row Factory --- */

static void
on_local_row_open_profile(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_row_follow_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_FOLLOW_REQUESTED], 0, pubkey);
}

static void
on_row_unfollow_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_UNFOLLOW_REQUESTED], 0, pubkey);
}

static void
on_row_mute_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_MUTE_REQUESTED], 0, pubkey);
}

static void
on_row_copy_npub_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_COPY_NPUB_REQUESTED], 0, pubkey);
}

static void
setup_local_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GnostrProfileRow *row = gnostr_profile_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
bind_local_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    GnostrProfileRow *row = GNOSTR_PROFILE_ROW(gtk_list_item_get_child(list_item));
    GnNostrProfile *profile = gtk_list_item_get_item(list_item);

    if (!profile)
        return;

    const char *pubkey = gn_nostr_profile_get_pubkey(profile);
    const char *display_name = gn_nostr_profile_get_display_name(profile);
    const char *name = gn_nostr_profile_get_name(profile);
    const char *nip05 = gn_nostr_profile_get_nip05(profile);
    const char *about = gn_nostr_profile_get_about(profile);
    const char *picture = gn_nostr_profile_get_picture_url(profile);

    gnostr_profile_row_set_profile(row, pubkey, display_name, name, nip05, about, picture);

    /* Set follow and muted status */
    gboolean is_muted = gn_profile_list_model_is_pubkey_muted(self->profile_model, pubkey);
    gnostr_profile_row_set_muted(row, is_muted);

    /* Disconnect all previous signal handlers */
    g_signal_handlers_disconnect_by_func(row, on_local_row_open_profile, self);
    g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);

    /* Connect all signals */
    g_signal_connect(row, "open-profile", G_CALLBACK(on_local_row_open_profile), self);
    g_signal_connect(row, "follow-requested", G_CALLBACK(on_row_follow_requested), self);
    g_signal_connect(row, "unfollow-requested", G_CALLBACK(on_row_unfollow_requested), self);
    g_signal_connect(row, "mute-requested", G_CALLBACK(on_row_mute_requested), self);
    g_signal_connect(row, "copy-npub-requested", G_CALLBACK(on_row_copy_npub_requested), self);
}

static void
unbind_local_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    GnostrProfileRow *row = GNOSTR_PROFILE_ROW(gtk_list_item_get_child(list_item));
    if (row) {
        g_signal_handlers_disconnect_by_func(row, on_local_row_open_profile, self);
        g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);
    }
}

/* --- Network Search Row Factory --- */

static void
setup_network_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GnostrProfileRow *row = gnostr_profile_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_network_row_open_profile(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
bind_network_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    GnostrProfileRow *row = GNOSTR_PROFILE_ROW(gtk_list_item_get_child(list_item));
    GnostrNetworkResultItem *item = gtk_list_item_get_item(list_item);

    if (!item)
        return;

    gnostr_profile_row_set_profile(row,
                                   item->pubkey_hex,
                                   item->display_name,
                                   item->name,
                                   item->nip05,
                                   item->about,
                                   item->picture);

    /* Disconnect all previous signal handlers */
    g_signal_handlers_disconnect_by_func(row, on_network_row_open_profile, self);
    g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);

    /* Connect all signals */
    g_signal_connect(row, "open-profile", G_CALLBACK(on_network_row_open_profile), self);
    g_signal_connect(row, "follow-requested", G_CALLBACK(on_row_follow_requested), self);
    g_signal_connect(row, "unfollow-requested", G_CALLBACK(on_row_unfollow_requested), self);
    g_signal_connect(row, "mute-requested", G_CALLBACK(on_row_mute_requested), self);
    g_signal_connect(row, "copy-npub-requested", G_CALLBACK(on_row_copy_npub_requested), self);
}

static void
unbind_network_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    GnostrProfileRow *row = GNOSTR_PROFILE_ROW(gtk_list_item_get_child(list_item));
    if (row) {
        g_signal_handlers_disconnect_by_func(row, on_network_row_open_profile, self);
        g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
        g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);
    }
}

/* --- Mode Switching --- */

static void
switch_to_local_model(GnostrPageDiscover *self)
{
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->local_selection));
    gtk_list_view_set_factory(self->results_list, self->local_factory);
    self->is_local_mode = TRUE;

    /* Load profiles if not already loaded */
    if (!self->profiles_loaded) {
        self->profiles_loaded = TRUE;
        gn_profile_list_model_load_profiles(self->profile_model);
    }

    /* Show sort dropdown in local mode */
    gtk_widget_set_visible(GTK_WIDGET(self->sort_dropdown), TRUE);

    update_content_state(self);
}

static void
switch_to_network_model(GnostrPageDiscover *self)
{
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->network_selection));
    gtk_list_view_set_factory(self->results_list, self->network_factory);
    self->is_local_mode = FALSE;

    /* Hide sort dropdown in network mode (search relevance determines order) */
    gtk_widget_set_visible(GTK_WIDGET(self->sort_dropdown), FALSE);

    update_content_state(self);
}

/* --- State Updates --- */

static void
update_profile_count(GnostrPageDiscover *self)
{
    if (self->is_local_mode) {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->profile_model));
        guint total = gn_profile_list_model_get_total_count(self->profile_model);

        char *text;
        if (count == total) {
            text = g_strdup_printf("%u profiles", total);
        } else {
            text = g_strdup_printf("%u of %u profiles", count, total);
        }
        gtk_label_set_text(self->lbl_profile_count, text);
        g_free(text);
    } else {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));
        char *text = g_strdup_printf("%u results", count);
        gtk_label_set_text(self->lbl_profile_count, text);
        g_free(text);
    }
}

static void
update_content_state(GnostrPageDiscover *self)
{
    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    gboolean has_search = search_text && *search_text;

    g_message("discover: update_content_state - is_local=%d, is_live=%d, is_articles=%d",
            self->is_local_mode, self->is_live_mode, self->is_articles_mode);

    if (self->is_local_mode) {
        /* Local mode */
        gboolean is_loading = gn_profile_list_model_is_loading(self->profile_model);
        g_message("discover: local mode - is_loading=%d", is_loading);

        if (is_loading) {
            gtk_spinner_start(self->loading_spinner);
            gtk_stack_set_visible_child_name(self->content_stack, "loading");
            g_message("discover: showing 'loading' state");
            return;
        }

        gtk_spinner_stop(self->loading_spinner);

        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->profile_model));
        guint total = gn_profile_list_model_get_total_count(self->profile_model);
        g_message("discover: count=%u, total=%u", count, total);

        if (total == 0) {
            /* No profiles in database at all */
            g_message("discover: showing 'empty' state (total=0)");
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        } else if (count == 0) {
            /* No visible profiles - either filtered by search or all blocked */
            if (has_search) {
                g_message("discover: showing 'no-results' state");
                gtk_stack_set_visible_child_name(self->content_stack, "no-results");
            } else {
                /* All profiles are blocked/filtered, show empty state */
                g_message("discover: showing 'empty' state (count=0)");
                gtk_stack_set_visible_child_name(self->content_stack, "empty");
            }
        } else {
            /* Has results - show the results list */
            g_message("discover: showing 'results' state with %u profiles", count);
            gtk_stack_set_visible_child_name(self->content_stack, "results");
        }
    } else {
        /* Network mode */
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));

        if (count == 0 && !has_search) {
            /* Network mode with no search - show empty state */
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        } else if (count == 0 && has_search) {
            gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "results");
        }
    }

    update_profile_count(self);
}

/* --- Search Handling --- */

static void
on_network_search_complete(GPtrArray *results, GError *error, gpointer user_data)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    gtk_spinner_stop(self->loading_spinner);

    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_message("discover: search cancelled");
        return;
    }

    if (error) {
        g_warning("discover: search error: %s", error->message);
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
        if (results) g_ptr_array_unref(results);
        return;
    }

    /* Clear existing results */
    g_list_store_remove_all(self->network_results_model);

    /* Populate results */
    if (results && results->len > 0) {
        g_message("discover: got %u network results", results->len);
        for (guint i = 0; i < results->len; i++) {
            GnostrSearchResult *result = g_ptr_array_index(results, i);
            GnostrNetworkResultItem *item = gnostr_network_result_item_new_from_search_result(result);
            g_list_store_append(self->network_results_model, item);
            g_object_unref(item);
        }
    }

    if (results) g_ptr_array_unref(results);

    update_content_state(self);
}

static void
do_network_search(GnostrPageDiscover *self, const char *text)
{
    /* Cancel pending search */
    if (self->search_cancellable) {
        g_cancellable_cancel(self->search_cancellable);
        g_clear_object(&self->search_cancellable);
    }

    if (!text || !*text) {
        g_list_store_remove_all(self->network_results_model);
        update_content_state(self);
        return;
    }

    GnostrSearchQuery *query = gnostr_search_parse_query(text);
    if (!query) {
        g_warning("discover: failed to parse query '%s'", text);
        return;
    }

    gtk_spinner_start(self->loading_spinner);
    gtk_stack_set_visible_child_name(self->content_stack, "loading");

    self->search_cancellable = g_cancellable_new();

    gnostr_discover_search_async(
        query,
        TRUE,  /* search_network */
        TRUE,  /* search_local - include local results too */
        MAX_NETWORK_SEARCH_RESULTS,
        self->search_cancellable,
        on_network_search_complete,
        self
    );

    gnostr_search_query_free(query);
}

static gboolean
search_debounce_cb(gpointer user_data)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

    if (self->is_local_mode) {
        /* Filter local profiles */
        gn_profile_list_model_filter(self->profile_model, text);
        update_content_state(self);
    } else {
        /* Network search */
        do_network_search(self, text);
    }

    return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrPageDiscover *self)
{
    (void)entry;

    gnostr_debounce_trigger(self->search_debounce);
}

static void
on_search_activate(GtkSearchEntry *entry, GnostrPageDiscover *self)
{
    (void)entry;

    gnostr_debounce_cancel(self->search_debounce);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

    if (self->is_local_mode) {
        gn_profile_list_model_filter(self->profile_model, text);
        update_content_state(self);
    } else {
        do_network_search(self, text);
    }
}

/* --- Communities Button Handler --- */

static void
on_communities_clicked(GtkButton *button, GnostrPageDiscover *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_OPEN_COMMUNITIES], 0);
}

/* --- Articles View Signal Handlers --- */

static void
on_articles_open_article(GnostrArticlesView *view, const char *event_id, gint kind, GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, event_id, kind);
}

static void
on_articles_open_profile(GnostrArticlesView *view, const char *pubkey_hex, GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void
on_articles_zap_requested(GnostrArticlesView *view, const char *event_id,
                           const char *pubkey_hex, const char *lud16,
                           GnostrPageDiscover *self)
{
    (void)view;
    g_signal_emit(self, signals[SIGNAL_ZAP_ARTICLE_REQUESTED], 0, event_id, pubkey_hex, lud16);
}

/* --- Live Activity Handling --- */

static void
on_live_card_watch_live(GnostrLiveCard *card, GnostrPageDiscover *self)
{
    const GnostrLiveActivity *activity = gnostr_live_card_get_activity(card);
    if (activity && activity->event_id) {
        g_signal_emit(self, signals[SIGNAL_WATCH_LIVE], 0, activity->event_id);
    }
}

static void
on_live_card_profile_clicked(GnostrLiveCard *card, const char *pubkey_hex, GnostrPageDiscover *self)
{
    (void)card;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void
clear_live_flow_boxes(GnostrPageDiscover *self)
{
    /* Clear live flow box widgets */
    if (self->live_flow_box) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->live_flow_box))) != NULL) {
            gtk_flow_box_remove(self->live_flow_box, child);
        }
    }

    /* Clear scheduled flow box widgets */
    if (self->scheduled_flow_box) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->scheduled_flow_box))) != NULL) {
            gtk_flow_box_remove(self->scheduled_flow_box, child);
        }
    }
}

static void
clear_live_activities(GnostrPageDiscover *self)
{
    /* Clear UI widgets */
    clear_live_flow_boxes(self);

    /* Free activity arrays */
    if (self->live_activities) {
        g_ptr_array_unref(self->live_activities);
        self->live_activities = NULL;
    }
    if (self->scheduled_activities) {
        g_ptr_array_unref(self->scheduled_activities);
        self->scheduled_activities = NULL;
    }
}

static void
populate_live_activities(GnostrPageDiscover *self)
{
    if (!self->live_flow_box || !self->scheduled_flow_box)
        return;

    /* Clear existing cards from UI (but keep arrays) */
    clear_live_flow_boxes(self);

    /* Add live activities */
    if (self->live_activities && self->live_activities->len > 0) {
        for (guint i = 0; i < self->live_activities->len; i++) {
            GnostrLiveActivity *activity = g_ptr_array_index(self->live_activities, i);
            GnostrLiveCard *card = gnostr_live_card_new();
            gnostr_live_card_set_activity(card, activity);
            gnostr_live_card_set_compact(card, FALSE);

            g_signal_connect(card, "watch-live", G_CALLBACK(on_live_card_watch_live), self);
            g_signal_connect(card, "profile-clicked", G_CALLBACK(on_live_card_profile_clicked), self);

            gtk_flow_box_insert(self->live_flow_box, GTK_WIDGET(card), -1);
        }
        gtk_widget_set_visible(GTK_WIDGET(self->live_now_section), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->live_now_section), FALSE);
    }

    /* Add scheduled activities */
    if (self->scheduled_activities && self->scheduled_activities->len > 0) {
        for (guint i = 0; i < self->scheduled_activities->len; i++) {
            GnostrLiveActivity *activity = g_ptr_array_index(self->scheduled_activities, i);
            GnostrLiveCard *card = gnostr_live_card_new();
            gnostr_live_card_set_activity(card, activity);
            gnostr_live_card_set_compact(card, TRUE);

            g_signal_connect(card, "watch-live", G_CALLBACK(on_live_card_watch_live), self);
            g_signal_connect(card, "profile-clicked", G_CALLBACK(on_live_card_profile_clicked), self);

            gtk_flow_box_insert(self->scheduled_flow_box, GTK_WIDGET(card), -1);
        }
        gtk_widget_set_visible(GTK_WIDGET(self->scheduled_section), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->scheduled_section), FALSE);
    }
}

static void
update_live_content_state(GnostrPageDiscover *self)
{
    gboolean has_live = self->live_activities && self->live_activities->len > 0;
    gboolean has_scheduled = self->scheduled_activities && self->scheduled_activities->len > 0;

    if (has_live || has_scheduled) {
        gtk_stack_set_visible_child_name(self->content_stack, "live");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "live-empty");
    }
}

static void
switch_to_people_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = FALSE;
    self->is_articles_mode = FALSE;

    /* Show profile search UI */
    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), TRUE);

    /* Show the appropriate content */
    update_content_state(self);
}

static void
switch_to_live_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = TRUE;
    self->is_articles_mode = FALSE;

    /* Hide profile search UI */
    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), FALSE);

    /* Load live activities if not already loaded */
    if (!self->live_loaded) {
        gnostr_page_discover_load_live_activities(self);
    } else {
        update_live_content_state(self);
    }
}

static void
switch_to_articles_mode(GnostrPageDiscover *self)
{
    self->is_live_mode = FALSE;
    self->is_articles_mode = TRUE;

    /* Hide profile search UI (articles view has its own) */
    gtk_widget_set_visible(GTK_WIDGET(self->search_entry), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->filter_row), FALSE);

    /* Show articles view in stack */
    gtk_stack_set_visible_child_name(self->content_stack, "articles");

    /* Load articles if not already loaded */
    if (!self->articles_loaded && self->articles_view) {
        self->articles_loaded = TRUE;
        gnostr_articles_view_load_articles(self->articles_view);
    }
}

static void
on_mode_toggled(GtkToggleButton *button, GnostrPageDiscover *self)
{
    /* Ensure mutual exclusivity */
    if (gtk_toggle_button_get_active(button)) {
        if (button == self->btn_mode_people) {
            gtk_toggle_button_set_active(self->btn_mode_live, FALSE);
            if (self->btn_mode_articles)
                gtk_toggle_button_set_active(self->btn_mode_articles, FALSE);
            switch_to_people_mode(self);
        } else if (button == self->btn_mode_live) {
            gtk_toggle_button_set_active(self->btn_mode_people, FALSE);
            if (self->btn_mode_articles)
                gtk_toggle_button_set_active(self->btn_mode_articles, FALSE);
            switch_to_live_mode(self);
        } else if (button == self->btn_mode_articles) {
            gtk_toggle_button_set_active(self->btn_mode_people, FALSE);
            gtk_toggle_button_set_active(self->btn_mode_live, FALSE);
            switch_to_articles_mode(self);
        }
    } else {
        /* Don't allow all to be inactive */
        gboolean any_active = gtk_toggle_button_get_active(self->btn_mode_people) ||
                              gtk_toggle_button_get_active(self->btn_mode_live) ||
                              (self->btn_mode_articles && gtk_toggle_button_get_active(self->btn_mode_articles));
        if (!any_active) {
            gtk_toggle_button_set_active(button, TRUE);
        }
    }
}

static void
on_refresh_live_clicked(GtkButton *button, GnostrPageDiscover *self)
{
    (void)button;
    self->live_loaded = FALSE;
    gnostr_page_discover_load_live_activities(self);
}

/* --- Filter & Sort Handling --- */

static void
on_filter_toggled(GtkToggleButton *button, GnostrPageDiscover *self)
{
    /* Ensure mutual exclusivity */
    if (gtk_toggle_button_get_active(button)) {
        if (button == self->btn_local) {
            gtk_toggle_button_set_active(self->btn_network, FALSE);
            switch_to_local_model(self);
        } else if (button == self->btn_network) {
            gtk_toggle_button_set_active(self->btn_local, FALSE);
            switch_to_network_model(self);
            /* Re-trigger search if there's text */
            const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
            if (text && *text) {
                do_network_search(self, text);
            }
        }
    } else {
        /* Don't allow both to be inactive */
        if (!gtk_toggle_button_get_active(self->btn_local) &&
            !gtk_toggle_button_get_active(self->btn_network)) {
            gtk_toggle_button_set_active(button, TRUE);
        }
    }
}

static void
on_sort_changed(GtkDropDown *dropdown, GParamSpec *pspec, GnostrPageDiscover *self)
{
    (void)pspec;

    guint selected = gtk_drop_down_get_selected(dropdown);
    GnProfileSortMode mode;

    switch (selected) {
        case 0:
            mode = GN_PROFILE_SORT_RECENT;
            break;
        case 1:
            mode = GN_PROFILE_SORT_ALPHABETICAL;
            break;
        case 2:
            mode = GN_PROFILE_SORT_FOLLOWING;
            break;
        default:
            mode = GN_PROFILE_SORT_RECENT;
    }

    gn_profile_list_model_set_sort_mode(self->profile_model, mode);
}

/* --- Model Signal Handlers --- */

static void
on_model_loading_changed(GnProfileListModel *model, GParamSpec *pspec, GnostrPageDiscover *self)
{
    (void)model;
    (void)pspec;
    /* Only update content state if we're in People mode */
    if (!self->is_live_mode && !self->is_articles_mode) {
        update_content_state(self);
    }
}

static void
on_model_items_changed(GListModel *model, guint position, guint removed, guint added, GnostrPageDiscover *self)
{
    (void)model;
    g_message("discover: on_model_items_changed - pos=%u, removed=%u, added=%u", position, removed, added);
    /* Only update content state if we're in People mode */
    if (!self->is_live_mode && !self->is_articles_mode) {
        update_content_state(self);
    }
}

/* --- GObject Implementation --- */

static void
gnostr_page_discover_dispose(GObject *object)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(object);

    gnostr_debounce_free(self->search_debounce);

    if (self->search_cancellable) {
        g_cancellable_cancel(self->search_cancellable);
        g_clear_object(&self->search_cancellable);
    }

    if (self->live_cancellable) {
        g_cancellable_cancel(self->live_cancellable);
        g_clear_object(&self->live_cancellable);
    }

    /* Unsubscribe from live activities subscription */
    if (self->live_sub_id) {
        gn_ndb_unsubscribe(self->live_sub_id);
        self->live_sub_id = 0;
    }

    if (self->live_activities) {
        g_ptr_array_unref(self->live_activities);
        self->live_activities = NULL;
    }
    if (self->scheduled_activities) {
        g_ptr_array_unref(self->scheduled_activities);
        self->scheduled_activities = NULL;
    }

    g_clear_object(&self->profile_model);
    g_clear_object(&self->local_selection);
    g_clear_object(&self->local_factory);
    g_clear_object(&self->network_results_model);
    g_clear_object(&self->network_selection);
    g_clear_object(&self->network_factory);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_page_discover_parent_class)->dispose(object);
}

static void
gnostr_page_discover_finalize(GObject *object)
{
    G_OBJECT_CLASS(gnostr_page_discover_parent_class)->finalize(object);
}

static void
gnostr_page_discover_class_init(GnostrPageDiscoverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_page_discover_dispose;
    object_class->finalize = gnostr_page_discover_finalize;

    /* Ensure child widget types are registered before loading template */
    g_type_ensure(GNOSTR_TYPE_ARTICLES_VIEW);

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

    /* Bind template children - Profile search */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, search_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_local);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_network);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, sort_dropdown);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, lbl_profile_count);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, results_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_communities);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, filter_row);

    /* Bind template children - Mode toggle */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_people);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_live);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_mode_articles);

    /* Bind template children - Articles view */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, articles_view);

    /* Bind template children - Live Activities */
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_flow_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scheduled_flow_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_now_section);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, scheduled_section);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, live_loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_refresh_live);
    gtk_widget_class_bind_template_child(widget_class, GnostrPageDiscover, btn_refresh_live_empty);

    /* Signals */
    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_FOLLOW_REQUESTED] = g_signal_new(
        "follow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_UNFOLLOW_REQUESTED] = g_signal_new(
        "unfollow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_MUTE_REQUESTED] = g_signal_new(
        "mute-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_COPY_NPUB_REQUESTED] = g_signal_new(
        "copy-npub-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_COMMUNITIES] = g_signal_new(
        "open-communities",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_WATCH_LIVE] = g_signal_new(
        "watch-live",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_ARTICLE] = g_signal_new(
        "open-article",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    signals[SIGNAL_ZAP_ARTICLE_REQUESTED] = g_signal_new(
        "zap-article-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "page-discover");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_page_discover_init(GnostrPageDiscover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->search_debounce = gnostr_debounce_new(SEARCH_DEBOUNCE_MS, search_debounce_cb, self);
    self->profiles_loaded = FALSE;
    self->is_local_mode = TRUE;
    self->is_live_mode = FALSE;
    self->is_articles_mode = FALSE;
    self->live_loaded = FALSE;
    self->articles_loaded = FALSE;
    self->live_activities = NULL;
    self->scheduled_activities = NULL;
    self->live_cancellable = NULL;
    self->live_sub_id = 0;

    /* Create local profile browser model */
    self->profile_model = gn_profile_list_model_new();
    self->local_selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->profile_model)));
    gtk_single_selection_set_autoselect(self->local_selection, FALSE);
    gtk_single_selection_set_can_unselect(self->local_selection, TRUE);

    /* Create local row factory */
    self->local_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(self->local_factory, "setup", G_CALLBACK(setup_local_row), self);
    g_signal_connect(self->local_factory, "bind", G_CALLBACK(bind_local_row), self);
    g_signal_connect(self->local_factory, "unbind", G_CALLBACK(unbind_local_row), self);

    /* Create network search model */
    self->network_results_model = g_list_store_new(GNOSTR_TYPE_NETWORK_RESULT_ITEM);
    self->network_selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->network_results_model)));
    gtk_single_selection_set_autoselect(self->network_selection, FALSE);
    gtk_single_selection_set_can_unselect(self->network_selection, TRUE);

    /* Create network row factory */
    self->network_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(self->network_factory, "setup", G_CALLBACK(setup_network_row), self);
    g_signal_connect(self->network_factory, "bind", G_CALLBACK(bind_network_row), self);
    g_signal_connect(self->network_factory, "unbind", G_CALLBACK(unbind_network_row), self);

    /* Start with local model - set is_local_mode before connecting signals */
    self->is_local_mode = TRUE;
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->local_selection));
    gtk_list_view_set_factory(self->results_list, self->local_factory);

    /* Connect search signals */
    g_signal_connect(self->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->search_entry, "activate",
                     G_CALLBACK(on_search_activate), self);

    /* Connect filter toggle signals */
    g_signal_connect(self->btn_local, "toggled",
                     G_CALLBACK(on_filter_toggled), self);
    g_signal_connect(self->btn_network, "toggled",
                     G_CALLBACK(on_filter_toggled), self);

    /* Connect sort dropdown */
    g_signal_connect(self->sort_dropdown, "notify::selected",
                     G_CALLBACK(on_sort_changed), self);

    /* Connect model signals */
    g_signal_connect(self->profile_model, "notify::is-loading",
                     G_CALLBACK(on_model_loading_changed), self);
    g_signal_connect(self->profile_model, "items-changed",
                     G_CALLBACK(on_model_items_changed), self);

    /* Default to local filter active */
    gtk_toggle_button_set_active(self->btn_local, TRUE);

    /* Connect communities button */
    if (self->btn_communities) {
        g_signal_connect(self->btn_communities, "clicked",
                         G_CALLBACK(on_communities_clicked), self);
    }

    /* Connect mode toggle signals */
    if (self->btn_mode_people) {
        g_signal_connect(self->btn_mode_people, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }
    if (self->btn_mode_live) {
        g_signal_connect(self->btn_mode_live, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }
    if (self->btn_mode_articles) {
        g_signal_connect(self->btn_mode_articles, "toggled",
                         G_CALLBACK(on_mode_toggled), self);
    }

    /* Connect articles view signals */
    if (self->articles_view) {
        g_signal_connect(self->articles_view, "open-article",
                         G_CALLBACK(on_articles_open_article), self);
        g_signal_connect(self->articles_view, "open-profile",
                         G_CALLBACK(on_articles_open_profile), self);
        g_signal_connect(self->articles_view, "zap-requested",
                         G_CALLBACK(on_articles_zap_requested), self);
    }

    /* Connect live refresh buttons */
    if (self->btn_refresh_live) {
        g_signal_connect(self->btn_refresh_live, "clicked",
                         G_CALLBACK(on_refresh_live_clicked), self);
    }
    if (self->btn_refresh_live_empty) {
        g_signal_connect(self->btn_refresh_live_empty, "clicked",
                         G_CALLBACK(on_refresh_live_clicked), self);
    }

    /* Default to People mode - call switch function directly since
     * btn_mode_people may already be active from UI template, which
     * means gtk_toggle_button_set_active won't emit the toggled signal */
    if (self->btn_mode_people) {
        gtk_toggle_button_set_active(self->btn_mode_people, TRUE);
    }
    switch_to_people_mode(self);

    /* Load profiles - must be done explicitly since btn_local may already
     * be active in the UI template (so toggled signal won't fire) and
     * switch_to_people_mode doesn't call switch_to_local_model */
    if (!self->profiles_loaded) {
        self->profiles_loaded = TRUE;
        g_message("discover: loading profiles in init");
        gn_profile_list_model_load_profiles(self->profile_model);
    }

    /* Ensure loading state is shown */
    if (gn_profile_list_model_is_loading(self->profile_model)) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
    }
}

GnostrPageDiscover *
gnostr_page_discover_new(void)
{
    return g_object_new(GNOSTR_TYPE_PAGE_DISCOVER, NULL);
}

void
gnostr_page_discover_load_profiles(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (!self->profiles_loaded) {
        self->profiles_loaded = TRUE;
        gn_profile_list_model_load_profiles(self->profile_model);
    }
}

void
gnostr_page_discover_set_loading(GnostrPageDiscover *self, gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (is_loading) {
        gtk_spinner_start(self->loading_spinner);
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
    } else {
        gtk_spinner_stop(self->loading_spinner);
        update_content_state(self);
    }
}

void
gnostr_page_discover_clear_results(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gn_profile_list_model_filter(self->profile_model, NULL);
    g_list_store_remove_all(self->network_results_model);
    update_content_state(self);
}

const char *
gnostr_page_discover_get_search_text(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), NULL);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    return (text && *text) ? text : NULL;
}

void
gnostr_page_discover_set_following(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_following_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_set_muted(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_muted_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_set_blocked(GnostrPageDiscover *self, const char **pubkeys)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    gn_profile_list_model_set_blocked_set(self->profile_model, pubkeys);
}

void
gnostr_page_discover_refresh(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    /* Force reload from database */
    self->profiles_loaded = FALSE;
    gnostr_page_discover_load_profiles(self);
}

gboolean
gnostr_page_discover_is_network_search_enabled(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return gtk_toggle_button_get_active(self->btn_network);
}

gboolean
gnostr_page_discover_is_local_search_enabled(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), TRUE);
    return gtk_toggle_button_get_active(self->btn_local);
}

guint
gnostr_page_discover_get_result_count(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), 0);

    if (self->is_local_mode) {
        return g_list_model_get_n_items(G_LIST_MODEL(self->profile_model));
    } else {
        return g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));
    }
}

/* Compare function for sorting live activities by start time (ascending) */
static gint
compare_activities_by_start_time(gconstpointer a, gconstpointer b)
{
    const GnostrLiveActivity *act_a = *(const GnostrLiveActivity **)a;
    const GnostrLiveActivity *act_b = *(const GnostrLiveActivity **)b;

    gint64 time_a = act_a->starts_at > 0 ? act_a->starts_at : act_a->created_at;
    gint64 time_b = act_b->starts_at > 0 ? act_b->starts_at : act_b->created_at;

    if (time_a < time_b) return -1;
    if (time_a > time_b) return 1;
    return 0;
}

/* Compare function for sorting live activities by created_at (descending for recency) */
static gint
compare_activities_by_recency(gconstpointer a, gconstpointer b)
{
    const GnostrLiveActivity *act_a = *(const GnostrLiveActivity **)a;
    const GnostrLiveActivity *act_b = *(const GnostrLiveActivity **)b;

    /* More recent first */
    if (act_a->created_at > act_b->created_at) return -1;
    if (act_a->created_at < act_b->created_at) return 1;
    return 0;
}

/* Check if an activity with the same d_tag and pubkey already exists */
static gboolean
activity_exists_in_array(GPtrArray *array, const char *pubkey, const char *d_tag)
{
    if (!array || !pubkey || !d_tag)
        return FALSE;

    for (guint i = 0; i < array->len; i++) {
        GnostrLiveActivity *existing = g_ptr_array_index(array, i);
        if (existing->pubkey && existing->d_tag &&
            strcmp(existing->pubkey, pubkey) == 0 &&
            strcmp(existing->d_tag, d_tag) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Process a single live activity event and add to appropriate array */
static void
process_live_activity_event(GnostrPageDiscover *self, storage_ndb_note *note)
{
    if (!note)
        return;

    /* Get note tags as JSON for parsing */
    char *tags_json = storage_ndb_note_tags_json(note);
    if (!tags_json)
        return;

    /* Get event metadata */
    const unsigned char *id_bin = storage_ndb_note_id(note);
    const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
    gint64 created_at = (gint64)storage_ndb_note_created_at(note);

    char event_id_hex[65], pubkey_hex[65];
    storage_ndb_hex_encode(id_bin, event_id_hex);
    storage_ndb_hex_encode(pubkey_bin, pubkey_hex);

    /* Parse the live activity */
    GnostrLiveActivity *activity = gnostr_live_activity_parse_tags(
        tags_json, pubkey_hex, event_id_hex, created_at);

    g_free(tags_json);

    if (!activity) {
        g_message("discover: failed to parse live activity event");
        return;
    }

    /* Skip ended activities (we only show live and planned) */
    if (activity->status == GNOSTR_LIVE_STATUS_ENDED) {
        gnostr_live_activity_free(activity);
        return;
    }

    /* Check for duplicate by pubkey+d_tag (replaceable events) */
    GPtrArray *target_array = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
        ? self->live_activities
        : self->scheduled_activities;

    if (activity_exists_in_array(target_array, activity->pubkey, activity->d_tag)) {
        /* We already have this activity, skip (assuming we got newest first) */
        gnostr_live_activity_free(activity);
        return;
    }

    /* Also check the other array */
    GPtrArray *other_array = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
        ? self->scheduled_activities
        : self->live_activities;

    if (activity_exists_in_array(other_array, activity->pubkey, activity->d_tag)) {
        gnostr_live_activity_free(activity);
        return;
    }

    /* Add to appropriate array based on status */
    if (activity->status == GNOSTR_LIVE_STATUS_LIVE) {
        g_ptr_array_add(self->live_activities, activity);
        g_message("discover: added live activity '%s' (live)", activity->title ? activity->title : "(untitled)");
    } else if (activity->status == GNOSTR_LIVE_STATUS_PLANNED) {
        g_ptr_array_add(self->scheduled_activities, activity);
        g_message("discover: added live activity '%s' (planned)", activity->title ? activity->title : "(untitled)");
    } else {
        /* Unknown status - treat as planned if it has a future start time */
        if (activity->starts_at > (g_get_real_time() / G_USEC_PER_SEC)) {
            g_ptr_array_add(self->scheduled_activities, activity);
        } else {
            gnostr_live_activity_free(activity);
        }
    }
}

/* Callback for NIP-53 live activity subscription */
static void
on_live_activities_received(uint64_t subid,
                            const uint64_t *note_keys,
                            guint n_keys,
                            gpointer user_data)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    (void)subid;

    if (!self || n_keys == 0)
        return;

    g_message("discover: received %u live activity events", n_keys);

    /* Get transaction to access notes */
    void *txn = NULL;
    if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
        g_warning("discover: failed to begin query for live activities");
        return;
    }

    /* Ensure arrays exist */
    if (!self->live_activities) {
        self->live_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    }
    if (!self->scheduled_activities) {
        self->scheduled_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    }

    /* Process each event */
    for (guint i = 0; i < n_keys; i++) {
        storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
        if (!note)
            continue;

        /* Verify this is a kind 30311 event */
        uint32_t kind = storage_ndb_note_kind(note);
        if (kind != KIND_LIVE_ACTIVITY)
            continue;

        process_live_activity_event(self, note);
    }

    storage_ndb_end_query(txn);

    /* Sort arrays */
    if (self->live_activities && self->live_activities->len > 0) {
        g_ptr_array_sort(self->live_activities, compare_activities_by_recency);
    }
    if (self->scheduled_activities && self->scheduled_activities->len > 0) {
        g_ptr_array_sort(self->scheduled_activities, compare_activities_by_start_time);
    }

    /* Update UI */
    self->live_loaded = TRUE;

    /* Stop loading spinner */
    if (self->live_loading_spinner) {
        gtk_spinner_stop(self->live_loading_spinner);
    }

    /* Populate UI with activities */
    populate_live_activities(self);
    update_live_content_state(self);
}

void
gnostr_page_discover_load_live_activities(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    /* Cancel any pending load */
    if (self->live_cancellable) {
        g_cancellable_cancel(self->live_cancellable);
        g_clear_object(&self->live_cancellable);
    }

    /* Unsubscribe from existing subscription */
    if (self->live_sub_id) {
        gn_ndb_unsubscribe(self->live_sub_id);
        self->live_sub_id = 0;
    }

    /* Show loading state */
    if (self->live_loading_spinner) {
        gtk_spinner_start(self->live_loading_spinner);
    }
    gtk_stack_set_visible_child_name(self->content_stack, "live-loading");

    self->live_cancellable = g_cancellable_new();

    /* Clear existing activities */
    clear_live_activities(self);

    /* Initialize arrays for activities */
    self->live_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    self->scheduled_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);

    /*
     * Subscribe to NIP-53 live activity events (kind 30311).
     *
     * The filter queries for:
     * - kind 30311 (live activity)
     * - limit to recent events (activities are usually short-lived)
     *
     * Note: We can't filter by status tag directly in nostrdb, so we
     * filter in the callback to only show live and planned events.
     */
    const char *filter_json = "[{\"kinds\":[30311],\"limit\":100}]";

    self->live_sub_id = gn_ndb_subscribe(
        filter_json,
        on_live_activities_received,
        self,
        NULL  /* no destroy notify - self manages its own lifecycle */
    );

    if (self->live_sub_id == 0) {
        g_warning("discover: failed to subscribe to live activities");

        /* Stop loading and show empty state */
        if (self->live_loading_spinner) {
            gtk_spinner_stop(self->live_loading_spinner);
        }
        self->live_loaded = TRUE;
        update_live_content_state(self);
        return;
    }

    g_message("discover: subscribed to live activities (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->live_sub_id);

    /*
     * The subscription callback will be called when matching events arrive.
     * For existing data in nostrdb, the dispatcher will notify us.
     *
     * Also do an initial query to load any existing events in the database.
     * This handles the case where events are already cached.
     */
    void *txn = NULL;
    if (storage_ndb_begin_query_retry(&txn, 3, 10) == 0 && txn) {
        char **results = NULL;
        int count = 0;

        /* Query for existing live activity events */
        if (storage_ndb_query(txn, filter_json, &results, &count) == 0 && results && count > 0) {
            g_message("discover: found %d existing live activity events", count);

            for (int i = 0; i < count; i++) {
                if (results[i]) {
                    /* Parse the JSON event directly */
                    GnostrLiveActivity *activity = gnostr_live_activity_parse(results[i]);
                    if (activity) {
                        /* Skip ended activities */
                        if (activity->status == GNOSTR_LIVE_STATUS_ENDED) {
                            gnostr_live_activity_free(activity);
                            continue;
                        }

                        /* Check for duplicates */
                        GPtrArray *target = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
                            ? self->live_activities
                            : self->scheduled_activities;

                        if (!activity_exists_in_array(target, activity->pubkey, activity->d_tag) &&
                            !activity_exists_in_array(
                                (target == self->live_activities) ? self->scheduled_activities : self->live_activities,
                                activity->pubkey, activity->d_tag)) {

                            if (activity->status == GNOSTR_LIVE_STATUS_LIVE) {
                                g_ptr_array_add(self->live_activities, activity);
                            } else if (activity->status == GNOSTR_LIVE_STATUS_PLANNED ||
                                       activity->starts_at > (g_get_real_time() / G_USEC_PER_SEC)) {
                                g_ptr_array_add(self->scheduled_activities, activity);
                            } else {
                                gnostr_live_activity_free(activity);
                            }
                        } else {
                            gnostr_live_activity_free(activity);
                        }
                    }
                }
            }

            storage_ndb_free_results(results, count);
        }

        storage_ndb_end_query(txn);

        /* Sort arrays */
        if (self->live_activities && self->live_activities->len > 0) {
            g_ptr_array_sort(self->live_activities, compare_activities_by_recency);
        }
        if (self->scheduled_activities && self->scheduled_activities->len > 0) {
            g_ptr_array_sort(self->scheduled_activities, compare_activities_by_start_time);
        }

        /* Update UI if we found any events */
        if ((self->live_activities && self->live_activities->len > 0) ||
            (self->scheduled_activities && self->scheduled_activities->len > 0)) {

            self->live_loaded = TRUE;
            if (self->live_loading_spinner) {
                gtk_spinner_stop(self->live_loading_spinner);
            }
            populate_live_activities(self);
            update_live_content_state(self);
            return;
        }
    }

    /* No events found in initial query, show empty state */
    self->live_loaded = TRUE;
    if (self->live_loading_spinner) {
        gtk_spinner_stop(self->live_loading_spinner);
    }
    update_live_content_state(self);
}

gboolean
gnostr_page_discover_is_live_mode(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return self->is_live_mode;
}

gboolean
gnostr_page_discover_is_articles_mode(GnostrPageDiscover *self)
{
    g_return_val_if_fail(GNOSTR_IS_PAGE_DISCOVER(self), FALSE);
    return self->is_articles_mode;
}
