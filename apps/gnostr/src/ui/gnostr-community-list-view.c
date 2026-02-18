/**
 * GnostrCommunityListView - NIP-72 Moderated Community Browser
 *
 * Displays a scrollable list of moderated communities using GnostrCommunityCard.
 */

#include "gnostr-community-list-view.h"
#include "gnostr-community-card.h"

struct _GnostrCommunityListView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListBox *list_box;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkSearchEntry *search_entry;

    /* Data */
    char *user_pubkey;
    GHashTable *communities;  /* a_tag -> GnostrCommunityCard */
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

static void
gnostr_community_list_view_dispose(GObject *object)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_community_list_view_parent_class)->dispose(object);
}

static void
gnostr_community_list_view_finalize(GObject *object)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(object);

    g_free(self->user_pubkey);
    g_clear_pointer(&self->communities, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_community_list_view_parent_class)->finalize(object);
}

static void
on_card_community_selected(GnostrCommunityCard *card, const char *a_tag, GnostrCommunityListView *self)
{
    (void)card;
    g_signal_emit(self, signals[SIGNAL_COMMUNITY_SELECTED], 0, a_tag);
}

static void
on_card_open_profile(GnostrCommunityCard *card, const char *pubkey, GnostrCommunityListView *self)
{
    (void)card;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_card_join_community(GnostrCommunityCard *card, const char *a_tag, GnostrCommunityListView *self)
{
    (void)card;
    g_signal_emit(self, signals[SIGNAL_JOIN_COMMUNITY], 0, a_tag);
}

static void
on_card_leave_community(GnostrCommunityCard *card, const char *a_tag, GnostrCommunityListView *self)
{
    (void)card;
    g_signal_emit(self, signals[SIGNAL_LEAVE_COMMUNITY], 0, a_tag);
}

static gboolean
filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GnostrCommunityListView *self = GNOSTR_COMMUNITY_LIST_VIEW(user_data);
    if (!self->search_entry)
        return TRUE;

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    if (!search_text || !*search_text)
        return TRUE;

    /* Get the community card and check if name/description contains search text */
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child || !GNOSTR_IS_COMMUNITY_CARD(child))
        return TRUE;

    GnostrCommunityCard *card = GNOSTR_COMMUNITY_CARD(child);
    const char *name = gnostr_community_card_get_name(card);
    const char *description = gnostr_community_card_get_description(card);

    gchar *search_lower = g_utf8_strdown(search_text, -1);
    gboolean visible = FALSE;

    if (name) {
        gchar *name_lower = g_utf8_strdown(name, -1);
        if (strstr(name_lower, search_lower))
            visible = TRUE;
        g_free(name_lower);
    }

    if (!visible && description) {
        gchar *desc_lower = g_utf8_strdown(description, -1);
        if (strstr(desc_lower, search_lower))
            visible = TRUE;
        g_free(desc_lower);
    }

    g_free(search_lower);
    return visible;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrCommunityListView *self)
{
    (void)entry;
    gtk_list_box_invalidate_filter(self->list_box);
}

static void
gnostr_community_list_view_class_init(GnostrCommunityListViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_community_list_view_dispose;
    object_class->finalize = gnostr_community_list_view_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-community-list-view.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, list_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrCommunityListView, search_entry);

    /* Signals */
    signals[SIGNAL_COMMUNITY_SELECTED] = g_signal_new(
        "community-selected",
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

    signals[SIGNAL_JOIN_COMMUNITY] = g_signal_new(
        "join-community",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_LEAVE_COMMUNITY] = g_signal_new(
        "leave-community",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "community-list");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_community_list_view_init(GnostrCommunityListView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->user_pubkey = NULL;
    self->communities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Connect search */
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

    /* Configure list box */
    gtk_list_box_set_selection_mode(self->list_box, GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(self->list_box, FALSE);
    gtk_list_box_set_filter_func(self->list_box, filter_func, self, NULL);
}

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

    /* Build the "a" tag for this community */
    g_autofree char *a_tag = gnostr_community_get_a_tag(community);
    g_return_if_fail(a_tag != NULL);

    /* Check if community already exists */
    GnostrCommunityCard *card = g_hash_table_lookup(self->communities, a_tag);

    if (!card) {
        /* Create new card */
        card = gnostr_community_card_new();

        /* Connect signals */
        g_signal_connect(card, "community-selected",
                         G_CALLBACK(on_card_community_selected), self);
        g_signal_connect(card, "open-profile",
                         G_CALLBACK(on_card_open_profile), self);
        g_signal_connect(card, "join-community",
                         G_CALLBACK(on_card_join_community), self);
        g_signal_connect(card, "leave-community",
                         G_CALLBACK(on_card_leave_community), self);

        /* Set logged-in state */
        gnostr_community_card_set_logged_in(card, self->user_pubkey != NULL);

        /* Check if user is a moderator */
        if (self->user_pubkey) {
            gboolean is_mod = gnostr_community_is_moderator(community, self->user_pubkey);
            gnostr_community_card_set_is_moderator(card, is_mod);
        }

        /* Add to list */
        gtk_list_box_prepend(self->list_box, GTK_WIDGET(card));
        g_hash_table_insert(self->communities, g_strdup(a_tag), card);
    }

    /* Update card data */
    gnostr_community_card_set_community(card, community);

    /* Show list, hide empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "list");
}

void
gnostr_community_list_view_remove_community(GnostrCommunityListView *self,
                                              const char *a_tag)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));
    g_return_if_fail(a_tag != NULL);

    GnostrCommunityCard *card = g_hash_table_lookup(self->communities, a_tag);
    if (card) {
        GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(card));
        if (parent && GTK_IS_LIST_BOX_ROW(parent)) {
            gtk_list_box_remove(self->list_box, parent);
        }
        g_hash_table_remove(self->communities, a_tag);
    }

    /* Check if empty */
    if (g_hash_table_size(self->communities) == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_community_list_view_clear(GnostrCommunityListView *self)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    /* Remove all children from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    g_hash_table_remove_all(self->communities);
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
        /* Switch to list or empty based on content */
        if (g_hash_table_size(self->communities) > 0) {
            gtk_stack_set_visible_child_name(self->content_stack, "list");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        }
    }
}

void
gnostr_community_list_view_set_empty(GnostrCommunityListView *self,
                                       gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    if (is_empty) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    }
}

const char *
gnostr_community_list_view_get_selected_a_tag(GnostrCommunityListView *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self), NULL);

    GtkListBoxRow *selected = gtk_list_box_get_selected_row(self->list_box);
    if (!selected) return NULL;

    GtkWidget *child = gtk_list_box_row_get_child(selected);
    if (!child || !GNOSTR_IS_COMMUNITY_CARD(child))
        return NULL;

    return gnostr_community_card_get_a_tag(GNOSTR_COMMUNITY_CARD(child));
}

void
gnostr_community_list_view_set_user_pubkey(GnostrCommunityListView *self,
                                             const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);

    /* Update logged-in state on all cards */
    gboolean logged_in = pubkey_hex != NULL;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->communities);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnostrCommunityCard *card = GNOSTR_COMMUNITY_CARD(value);
        gnostr_community_card_set_logged_in(card, logged_in);
    }
}

void
gnostr_community_list_view_set_joined(GnostrCommunityListView *self,
                                        const char *a_tag,
                                        gboolean is_joined)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_LIST_VIEW(self));
    g_return_if_fail(a_tag != NULL);

    GnostrCommunityCard *card = g_hash_table_lookup(self->communities, a_tag);
    if (card) {
        gnostr_community_card_set_joined(card, is_joined);
    }
}
