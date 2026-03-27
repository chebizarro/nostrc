#define G_LOG_DOMAIN "gnostr-discover-people"

#include "page-discover-private.h"
#include "gnostr-profile-row.h"
#include "../model/gn-profile-list-model.h"
#include <nostr-gobject-1.0/gn-nostr-profile.h>
#include "../util/discover-search.h"
#include "../util/debounce.h"

#define SEARCH_DEBOUNCE_MS 300
#define MAX_NETWORK_SEARCH_RESULTS 50

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

static void update_content_state(GnostrPageDiscover *self);
static void update_profile_count(GnostrPageDiscover *self);
static void switch_to_local_model(GnostrPageDiscover *self);
static void switch_to_network_model(GnostrPageDiscover *self);
static gboolean search_debounce_cb(gpointer user_data);

static void
on_local_row_open_profile(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    gnostr_page_discover_emit_open_profile_internal(self, pubkey);
}

static void
on_row_follow_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    gnostr_page_discover_emit_follow_requested_internal(self, pubkey);
}

static void
on_row_unfollow_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    gnostr_page_discover_emit_unfollow_requested_internal(self, pubkey);
}

static void
on_row_mute_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    gnostr_page_discover_emit_mute_requested_internal(self, pubkey);
}

static void
on_row_copy_npub_requested(GnostrProfileRow *row, const char *pubkey, GnostrPageDiscover *self)
{
    (void)row;
    gnostr_page_discover_emit_copy_npub_requested_internal(self, pubkey);
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
    GNostrProfile *profile = gtk_list_item_get_item(list_item);

    if (!profile)
        return;

    const char *pubkey = gnostr_profile_get_pubkey(profile);
    const char *display_name = gnostr_profile_get_display_name(profile);
    const char *name = gnostr_profile_get_name(profile);
    const char *nip05 = gnostr_profile_get_nip05(profile);
    const char *about = gnostr_profile_get_about(profile);
    const char *picture = gnostr_profile_get_picture_url(profile);

    gnostr_profile_row_set_profile(row, pubkey, display_name, name, nip05, about, picture);

    gboolean is_muted = gn_profile_list_model_is_pubkey_muted(self->profile_model, pubkey);
    gnostr_profile_row_set_muted(row, is_muted);

    g_signal_handlers_disconnect_by_func(row, on_local_row_open_profile, self);
    g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);

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
    gnostr_page_discover_emit_open_profile_internal(self, pubkey);
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

    g_signal_handlers_disconnect_by_func(row, on_network_row_open_profile, self);
    g_signal_handlers_disconnect_by_func(row, on_row_follow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_unfollow_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_mute_requested, self);
    g_signal_handlers_disconnect_by_func(row, on_row_copy_npub_requested, self);

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

static void
switch_to_local_model(GnostrPageDiscover *self)
{
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->local_selection));
    gtk_list_view_set_factory(self->results_list, self->local_factory);
    self->is_local_mode = TRUE;

    if (!self->profiles_loaded) {
        self->profiles_loaded = TRUE;
        gn_profile_list_model_load_profiles(self->profile_model);
    }

    gtk_widget_set_visible(GTK_WIDGET(self->sort_dropdown), TRUE);

    update_content_state(self);
}

static void
switch_to_network_model(GnostrPageDiscover *self)
{
    gtk_list_view_set_model(self->results_list, GTK_SELECTION_MODEL(self->network_selection));
    gtk_list_view_set_factory(self->results_list, self->network_factory);
    self->is_local_mode = FALSE;

    gtk_widget_set_visible(GTK_WIDGET(self->sort_dropdown), FALSE);

    update_content_state(self);
}

static void
update_profile_count(GnostrPageDiscover *self)
{
    if (self->is_local_mode) {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->profile_model));
        guint total = gn_profile_list_model_get_total_count(self->profile_model);

        g_autofree char *text = NULL;
        if (count == total) {
            text = g_strdup_printf("%u profiles", total);
        } else {
            text = g_strdup_printf("%u of %u profiles", count, total);
        }
        gtk_label_set_text(self->lbl_profile_count, text);
    } else {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));
        g_autofree char *text = g_strdup_printf("%u results", count);
        gtk_label_set_text(self->lbl_profile_count, text);
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
            g_message("discover: showing 'empty' state (total=0)");
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        } else if (count == 0) {
            if (has_search) {
                g_message("discover: showing 'no-results' state");
                gtk_stack_set_visible_child_name(self->content_stack, "no-results");
            } else {
                g_message("discover: showing 'empty' state (count=0)");
                gtk_stack_set_visible_child_name(self->content_stack, "empty");
            }
        } else {
            g_message("discover: showing 'results' state with %u profiles", count);
            gtk_stack_set_visible_child_name(self->content_stack, "results");
        }
    } else {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(self->network_results_model));

        if (count == 0 && !has_search) {
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        } else if (count == 0 && has_search) {
            gtk_stack_set_visible_child_name(self->content_stack, "no-results");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "results");
        }
    }

    update_profile_count(self);
}

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

    g_list_store_remove_all(self->network_results_model);

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
        TRUE,
        TRUE,
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
        gn_profile_list_model_filter(self->profile_model, text);
        update_content_state(self);
    } else {
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

static void
on_filter_toggled(GtkToggleButton *button, GnostrPageDiscover *self)
{
    if (gtk_toggle_button_get_active(button)) {
        if (button == self->btn_local) {
            gtk_toggle_button_set_active(self->btn_network, FALSE);
            switch_to_local_model(self);
        } else if (button == self->btn_network) {
            gtk_toggle_button_set_active(self->btn_local, FALSE);
            switch_to_network_model(self);
            const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
            if (text && *text) {
                do_network_search(self, text);
            }
        }
    } else {
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

static void
on_model_loading_changed(GnProfileListModel *model, GParamSpec *pspec, GnostrPageDiscover *self)
{
    (void)model;
    (void)pspec;
    if (!self->is_live_mode && !self->is_articles_mode) {
        update_content_state(self);
    }
}

static void
on_model_items_changed(GListModel *model, guint position, guint removed, guint added, GnostrPageDiscover *self)
{
    (void)model;
    g_message("discover: on_model_items_changed - pos=%u, removed=%u, added=%u", position, removed, added);
    if (!self->is_live_mode && !self->is_articles_mode) {
        update_content_state(self);
    }
}

void
gnostr_page_discover_people_init(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    self->search_debounce = gnostr_debounce_new(SEARCH_DEBOUNCE_MS, search_debounce_cb, self);
    self->profiles_loaded = FALSE;
    self->is_local_mode = TRUE;

    self->profile_model = gn_profile_list_model_new();
    self->local_selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->profile_model)));
    gtk_single_selection_set_autoselect(self->local_selection, FALSE);
    gtk_single_selection_set_can_unselect(self->local_selection, TRUE);

    self->local_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(self->local_factory, "setup", G_CALLBACK(setup_local_row), self);
    g_signal_connect(self->local_factory, "bind", G_CALLBACK(bind_local_row), self);
    g_signal_connect(self->local_factory, "unbind", G_CALLBACK(unbind_local_row), self);

    self->network_results_model = g_list_store_new(GNOSTR_TYPE_NETWORK_RESULT_ITEM);
    self->network_selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->network_results_model)));
    gtk_single_selection_set_autoselect(self->network_selection, FALSE);
    gtk_single_selection_set_can_unselect(self->network_selection, TRUE);

    self->network_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(self->network_factory, "setup", G_CALLBACK(setup_network_row), self);
    g_signal_connect(self->network_factory, "bind", G_CALLBACK(bind_network_row), self);
    g_signal_connect(self->network_factory, "unbind", G_CALLBACK(unbind_network_row), self);

    g_signal_connect(self->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->search_entry, "activate",
                     G_CALLBACK(on_search_activate), self);

    g_signal_connect(self->btn_local, "toggled",
                     G_CALLBACK(on_filter_toggled), self);
    g_signal_connect(self->btn_network, "toggled",
                     G_CALLBACK(on_filter_toggled), self);

    g_signal_connect(self->sort_dropdown, "notify::selected",
                     G_CALLBACK(on_sort_changed), self);

    g_signal_connect(self->profile_model, "notify::is-loading",
                     G_CALLBACK(on_model_loading_changed), self);
    g_signal_connect(self->profile_model, "items-changed",
                     G_CALLBACK(on_model_items_changed), self);

    gtk_toggle_button_set_active(self->btn_local, TRUE);
    switch_to_local_model(self);
}

void
gnostr_page_discover_people_dispose(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    gnostr_debounce_free(self->search_debounce);
    self->search_debounce = NULL;

    if (self->search_cancellable) {
        g_cancellable_cancel(self->search_cancellable);
        g_clear_object(&self->search_cancellable);
    }

    if (self->profile_model) {
        g_signal_handlers_disconnect_by_data(self->profile_model, self);
        g_clear_object(&self->profile_model);
    }
    g_clear_object(&self->local_selection);
    g_clear_object(&self->local_factory);
    g_clear_object(&self->network_results_model);
    g_clear_object(&self->network_selection);
    g_clear_object(&self->network_factory);
}

void
gnostr_page_discover_people_present(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));
    update_content_state(self);
}

void
gnostr_page_discover_people_load_profiles_internal(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (!self->profiles_loaded) {
        self->profiles_loaded = TRUE;
        gn_profile_list_model_load_profiles(self->profile_model);
    }
}

void
gnostr_page_discover_people_set_loading_internal(GnostrPageDiscover *self, gboolean is_loading)
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
gnostr_page_discover_people_clear_results_internal(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gn_profile_list_model_filter(self->profile_model, NULL);
    g_list_store_remove_all(self->network_results_model);
    update_content_state(self);
}

void
gnostr_page_discover_people_refresh_internal(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    self->profiles_loaded = FALSE;
    gnostr_page_discover_people_load_profiles_internal(self);
}
