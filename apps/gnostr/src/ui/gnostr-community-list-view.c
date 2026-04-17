/**
 * GnostrCommunityListView - NIP-72 Moderated Community Browser
 *
 * Displays a scrollable list of moderated communities using GListStore
 * + GtkFilterListModel + GtkListView with a GtkSignalListItemFactory.
 */

#include "gnostr-community-list-view.h"
#include "gnostr-community-card.h"
#include "../model/gnostr-community-item.h"

struct _GnostrCommunityListView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListView *list_view;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkSearchEntry *search_entry;

    /* Model pipeline */
    GListStore *store;
    GtkFilterListModel *filter_model;
    GtkCustomFilter *custom_filter;

    /* Data */
    char *user_pubkey;
    GHashTable *index;  /* a_tag (char*) -> guint position */
};

G_DEFINE_TYPE(GnostrCommunityListView, gnostr_community_list_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_COMMUNITY_SELECTED,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_JOIN_COMMUNITY,
    SIGNAL_LEAVE_COMMUNITY,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Index rebuild --- */

static void
rebuild_index(GnostrCommunityListView *self)
{
    g_hash_table_remove_all(self->index);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GnostrCommunityItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), i);
        const char *a_tag = gnostr_community_item_get_a_tag(item);
        if (a_tag)
            g_hash_table_insert(self->index, (gpointer)a_tag, GUINT_TO_POINTER(i));
    }
}

/* --- Filter function --- */

static gboolean
community_filter_func(gpointer item, gpointer user_data)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(user_data);
    GnostrCommunityItem *community_item = GNOSTR_COMMUNITY_ITEM(item);

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    if (!search_text || !*search_text)
        return TRUE;

    const char *name = gnostr_community_item_get_name(community_item);
    const char *description = gnostr_community_item_get_description(community_item);

    g_autofree char *search_lower = g_utf8_strdown(search_text, -1);

    if (name) {
        g_autofree char *name_lower = g_utf8_strdown(name, -1);
        if (strstr(name_lower, search_lower))
            return TRUE;
    }

    if (description) {
        g_autofree char *desc_lower = g_utf8_strdown(description, -1);
        if (strstr(desc_lower, search_lower))
            return TRUE;
    }

    return FALSE;
}

/* --- UI state --- */

static void
update_state(GnostrCommunityListView *self)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->filter_model));
    if (n > 0)
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    else
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

/* --- Signal callbacks from factory-bound cards --- */

static void
on_card_community_selected(GnostrCommunityCard *card, const char *a_tag, gpointer user_data)
{
    (void)card;
    g_signal_emit(GNOSTR_COMMUNITY_LIST_VIEW(user_data), signals[SIGNAL_COMMUNITY_SELECTED], 0, a_tag);
}

static void
on_card_open_profile(GnostrCommunityCard *card, const char *pubkey, gpointer user_data)
{
    (void)card;
    g_signal_emit(GNOSTR_COMMUNITY_LIST_VIEW(user_data), signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_card_join_community(GnostrCommunityCard *card, const char *a_tag, gpointer user_data)
{
    (void)card;
    g_signal_emit(GNOSTR_COMMUNITY_LIST_VIEW(user_data), signals[SIGNAL_JOIN_COMMUNITY], 0, a_tag);
}

static void
on_card_leave_community(GnostrCommunityCard *card, const char *a_tag, gpointer user_data)
{
    (void)card;
    g_signal_emit(GNOSTR_COMMUNITY_LIST_VIEW(user_data), signals[SIGNAL_LEAVE_COMMUNITY], 0, a_tag);
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrCommunityListView *self)
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
    GnostrCommunityCard *card = gnostr_community_card_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(card));
}

static void
factory_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
             GtkListItem *list_item,
             gpointer user_data)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(user_data);
    GnostrCommunityItem *item = gtk_list_item_get_item(list_item);
    GnostrCommunityCard *card = GNOSTR_COMMUNITY_CARD(gtk_list_item_get_child(list_item));

    if (!item || !card)
        return;

    const GnostrCommunity *community = gnostr_community_item_get_community(item);
    if (community) {
        gnostr_community_card_set_community(card, community);
        gnostr_community_card_set_logged_in(card, self->user_pubkey != NULL);
        gnostr_community_card_set_joined(card, gnostr_community_item_get_joined(item));

        if (self->user_pubkey) {
            gboolean is_mod = gnostr_community_is_moderator(community, self->user_pubkey);
            gnostr_community_card_set_is_moderator(card, is_mod);
        }
    }

    g_signal_connect(card, "community-selected", G_CALLBACK(on_card_community_selected), self);
    g_signal_connect(card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(card, "join-community", G_CALLBACK(on_card_join_community), self);
    g_signal_connect(card, "leave-community", G_CALLBACK(on_card_leave_community), self);
}

static void
factory_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
               GtkListItem *list_item,
               gpointer user_data)
{
    GnostrCommunityCard *card = GNOSTR_COMMUNITY_CARD(gtk_list_item_get_child(list_item));
    if (card)
        g_signal_handlers_disconnect_by_data(card, user_data);
}

/* --- GObject lifecycle --- */

static void
gnostr_community_list_view_dispose(GObject *object)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    g_clear_object(&self->filter_model);
    g_clear_object(&self->custom_filter);
    g_clear_object(&self->store);

    G_OBJECT_CLASS(gnostr_community_list_view_parent_class)->dispose(object);
}

static void
gnostr_community_list_view_finalize(GObject *object)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(object);

    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_pointer(&self->index, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_community_list_view_parent_class)->finalize(object);
}

static void
gnostr_community_list_view_class_init(GnostrCommunityListViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_community_list_view_dispose;
    object_class->finalize = gnostr_community_list_view_finalize;

    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-community-list-view.ui");

    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, list_view);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, search_entry);

    signals[SIGNAL_COMMUNITY_SELECTED] = g_signal_new(
        "community-selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_JOIN_COMMUNITY] = g_signal_new(
        "join-community", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_LEAVE_COMMUNITY] = g_signal_new(
        "leave-community", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    gtk_widget_class_set_css_name(widget_class, "community-list");
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_community_list_view_init(GnostrCommunityListView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->user_pubkey = NULL;
    self->index = g_hash_table_new(g_str_hash, g_str_equal);

    /* Model pipeline */
    self->store = g_list_store_new(GNOSTR_TYPE_COMMUNITY_ITEM);

    self->custom_filter = gtk_custom_filter_new(community_filter_func, self, NULL);
    self->filter_model = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(self->store)),
        GTK_FILTER(g_object_ref(self->custom_filter)));

    GtkNoSelection *selection = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(self->filter_model)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), self);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);

    gtk_list_view_set_model(self->list_view, GTK_SELECTION_MODEL(selection));
    gtk_list_view_set_factory(self->list_view, factory);
    g_object_unref(selection);
    g_object_unref(factory);

    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
}

/* --- Public API (preserved) --- */

GnostrCommunityListView *
gnostr_community_list_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_COMMUNITY_LIST_VIEW, NULL);
}

void
gnostr_community_list_view_upsert_community(GnostrCommunityListView *self,
                                              const GnostrCommunity *community)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));
    g_return_if_fail(community != NULL);

    g_autofree char *a_tag = gnostr_community_get_a_tag(community);
    g_return_if_fail(a_tag != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, a_tag, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_autoptr(GnostrCommunityItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
        if (item) {
            gnostr_community_item_update(item, community);
            g_list_store_remove(self->store, pos);
            g_list_store_insert(self->store, pos, item);
        }
    } else {
        g_autoptr(GnostrCommunityItem) item = gnostr_community_item_new(community);
        g_list_store_append(self->store, item);
        rebuild_index(self);
    }

    update_state(self);
}

void
gnostr_community_list_view_remove_community(GnostrCommunityListView *self,
                                              const char *a_tag)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));
    g_return_if_fail(a_tag != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, a_tag, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_list_store_remove(self->store, pos);
        rebuild_index(self);
    }

    update_state(self);
}

void
gnostr_community_list_view_clear(GnostrCommunityListView *self)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    g_list_store_remove_all(self->store);
    g_hash_table_remove_all(self->index);
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

void
gnostr_community_list_view_set_loading(GnostrCommunityListView *self,
                                         gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_spinner_start(self->loading_spinner);
    } else {
        gtk_spinner_stop(self->loading_spinner);
        update_state(self);
    }
}

void
gnostr_community_list_view_set_empty(GnostrCommunityListView *self,
                                       gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    if (is_empty)
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    else
        gtk_stack_set_visible_child_name(self->content_stack, "list");
}

const char *
gnostr_community_list_view_get_selected_a_tag(GnostrCommunityListView *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self), NULL);
    return NULL;
}

void
gnostr_community_list_view_set_user_pubkey(GnostrCommunityListView *self,
                                             const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    /* Cards will pick up logged_in state on next bind */
}

void
gnostr_community_list_view_set_joined(GnostrCommunityListView *self,
                                        const char *a_tag,
                                        gboolean is_joined)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));
    g_return_if_fail(a_tag != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, a_tag, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_autoptr(GnostrCommunityItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
        if (item) {
            gnostr_community_item_set_joined(item, is_joined);
            /* Force rebind by remove+reinsert */
            g_list_store_remove(self->store, pos);
            g_list_store_insert(self->store, pos, item);
        }
    }
}
