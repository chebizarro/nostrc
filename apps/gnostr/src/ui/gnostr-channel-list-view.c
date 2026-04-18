/**
 * GnostrChannelListView - NIP-28 Public Chat Channel Browser
 *
 * Displays a scrollable list of public chat channels using GListStore
 * + GtkFilterListModel + GtkListView with a GtkSignalListItemFactory.
 */

#include "gnostr-channel-list-view.h"
#include "gnostr-channel-row.h"
#include "../model/gnostr-channel-item.h"

struct _GnostrChannelListView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListView *list_view;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkButton *btn_create;
    GtkSearchEntry *search_entry;

    /* Model pipeline */
    GListStore *store;                    /* GnostrChannelItem objects */
    GtkFilterListModel *filter_model;
    GtkCustomFilter *custom_filter;

    /* Data */
    char *user_pubkey;
    GHashTable *index;  /* channel_id (char*) -> guint position */
};

G_DEFINE_TYPE(GnostrChannelListView, gnostr_channel_list_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_CHANNEL_SELECTED,
    SIGNAL_CREATE_CHANNEL,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Index rebuild --- */

static void
rebuild_index(GnostrChannelListView *self)
{
    g_hash_table_remove_all(self->index);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GnostrChannelItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), i);
        const char *cid = gnostr_channel_item_get_channel_id(item);
        if (cid)
            g_hash_table_insert(self->index, (gpointer)cid, GUINT_TO_POINTER(i));
    }
}

/* --- Filter function --- */

static gboolean
channel_filter_func(gpointer item, gpointer user_data)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(user_data);
    GnostrChannelItem *channel_item = GNOSTR_CHANNEL_ITEM(item);

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    if (!search_text || !*search_text)
        return TRUE;

    const char *name = gnostr_channel_item_get_name(channel_item);
    const char *about = gnostr_channel_item_get_about(channel_item);

    g_autofree char *search_lower = g_utf8_strdown(search_text, -1);

    if (name) {
        g_autofree char *name_lower = g_utf8_strdown(name, -1);
        if (strstr(name_lower, search_lower))
            return TRUE;
    }

    if (about) {
        g_autofree char *about_lower = g_utf8_strdown(about, -1);
        if (strstr(about_lower, search_lower))
            return TRUE;
    }

    return FALSE;
}

/* --- UI state --- */

static void
update_state(GnostrChannelListView *self)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->filter_model));
    if (n > 0)
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    else
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

/* --- Signal callbacks from factory-bound rows --- */

static void
on_row_channel_selected(GnostrChannelRow *row, const char *channel_id, gpointer user_data)
{
    (void)row;
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(user_data);
    g_signal_emit(self, signals[SIGNAL_CHANNEL_SELECTED], 0, channel_id);
}

static void
on_row_open_profile(GnostrChannelRow *row, const char *pubkey, gpointer user_data)
{
    (void)row;
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(user_data);
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_create_clicked(GtkButton *button, GnostrChannelListView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_CREATE_CHANNEL], 0);
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrChannelListView *self)
{
    (void)entry;
    gtk_filter_changed(GTK_FILTER(self->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_state(self);
}

/* --- Factory callbacks --- */

static void
factory_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
              GtkListItem *list_item,
              gpointer user_data G_GNUC_UNUSED)
{
    GnostrChannelRow *row = gnostr_channel_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
factory_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
             GtkListItem *list_item,
             gpointer user_data)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(user_data);
    GnostrChannelItem *item = gtk_list_item_get_item(list_item);
    GnostrChannelRow *row = GNOSTR_CHANNEL_ROW(gtk_list_item_get_child(list_item));

    if (!item || !row)
        return;

    const GnostrChannel *channel = gnostr_channel_item_get_channel(item);
    if (channel)
        gnostr_channel_row_set_channel(row, channel);

    g_signal_connect(row, "channel-selected", G_CALLBACK(on_row_channel_selected), self);
    g_signal_connect(row, "open-profile", G_CALLBACK(on_row_open_profile), self);
}

static void
factory_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
               GtkListItem *list_item,
               gpointer user_data)
{
    GnostrChannelRow *row = GNOSTR_CHANNEL_ROW(gtk_list_item_get_child(list_item));
    if (row)
        g_signal_handlers_disconnect_by_data(row, user_data);
}

/* --- GObject lifecycle --- */

static void
gnostr_channel_list_view_dispose(GObject *object)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    g_clear_object(&self->filter_model);
    g_clear_object(&self->custom_filter);
    g_clear_object(&self->store);

    G_OBJECT_CLASS(gnostr_channel_list_view_parent_class)->dispose(object);
}

static void
gnostr_channel_list_view_finalize(GObject *object)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(object);

    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_pointer(&self->index, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_channel_list_view_parent_class)->finalize(object);
}

static void
gnostr_channel_list_view_class_init(GnostrChannelListViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_channel_list_view_dispose;
    object_class->finalize = gnostr_channel_list_view_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-channel-list-view.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, list_view);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, btn_create);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, search_entry);

    /* Signals */
    signals[SIGNAL_CHANNEL_SELECTED] = g_signal_new(
        "channel-selected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_CREATE_CHANNEL] = g_signal_new(
        "create-channel",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "channel-list");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_channel_list_view_init(GnostrChannelListView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->user_pubkey = NULL;
    self->index = g_hash_table_new(g_str_hash, g_str_equal);

    /* Model pipeline: GListStore → GtkFilterListModel → GtkNoSelection → ListView */
    self->store = g_list_store_new(GNOSTR_TYPE_CHANNEL_ITEM);

    self->custom_filter = gtk_custom_filter_new(channel_filter_func, self, NULL);
    self->filter_model = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(self->store)),
        GTK_FILTER(g_object_ref(self->custom_filter)));

    GtkNoSelection *selection = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(self->filter_model)));

    /* Factory */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), self);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);

    gtk_list_view_set_model(self->list_view, GTK_SELECTION_MODEL(selection));
    gtk_list_view_set_factory(self->list_view, factory);
    g_object_unref(selection);
    g_object_unref(factory);

    /* Connect UI signals */
    g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create_clicked), self);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
}

/* --- Public API (preserved for callers) --- */

GnostrChannelListView *
gnostr_channel_list_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHANNEL_LIST_VIEW, NULL);
}

void
gnostr_channel_list_view_upsert_channel(GnostrChannelListView *self,
                                         const GnostrChannel *channel)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));
    g_return_if_fail(channel != NULL);
    g_return_if_fail(channel->channel_id != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, channel->channel_id, NULL, &pos_ptr)) {
        /* Update existing item */
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_autoptr(GnostrChannelItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
        if (item) {
            gnostr_channel_item_update(item, channel);
            /* Notify the store of the change by removing and re-inserting */
            g_list_store_remove(self->store, pos);
            g_list_store_insert(self->store, pos, item);
            /* Index stays the same for this position */
        } else {
            /* Stale index entry — rebuild and fall through to append */
            rebuild_index(self);
            g_autoptr(GnostrChannelItem) new_item = gnostr_channel_item_new(channel);
            g_list_store_append(self->store, new_item);
            rebuild_index(self);
        }
    } else {
        /* Append new item */
        g_autoptr(GnostrChannelItem) item = gnostr_channel_item_new(channel);
        g_list_store_append(self->store, item);
        rebuild_index(self);
    }

    update_state(self);
}

void
gnostr_channel_list_view_remove_channel(GnostrChannelListView *self,
                                         const char *channel_id)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));
    g_return_if_fail(channel_id != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, channel_id, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_list_store_remove(self->store, pos);
        rebuild_index(self);
    }

    update_state(self);
}

void
gnostr_channel_list_view_clear(GnostrChannelListView *self)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    g_list_store_remove_all(self->store);
    g_hash_table_remove_all(self->index);
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

void
gnostr_channel_list_view_set_loading(GnostrChannelListView *self,
                                      gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_spinner_start(self->loading_spinner);
    } else {
        gtk_spinner_stop(self->loading_spinner);
        update_state(self);
    }
}

void
gnostr_channel_list_view_set_empty(GnostrChannelListView *self,
                                    gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    if (is_empty)
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    else
        gtk_stack_set_visible_child_name(self->content_stack, "list");
}

const char *
gnostr_channel_list_view_get_selected_id(GnostrChannelListView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self), NULL);
    /* GtkListView with GtkNoSelection has no selected item.
     * Selection is handled via "channel-selected" signal on row click. */
    return NULL;
}

void
gnostr_channel_list_view_set_user_pubkey(GnostrChannelListView *self,
                                          const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}
