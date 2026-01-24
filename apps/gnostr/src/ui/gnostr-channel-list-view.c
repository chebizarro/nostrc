/**
 * GnostrChannelListView - NIP-28 Public Chat Channel Browser
 *
 * Displays a scrollable list of public chat channels.
 */

#include "gnostr-channel-list-view.h"
#include "gnostr-channel-row.h"

struct _GnostrChannelListView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListBox *list_box;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkButton *btn_create;
    GtkSearchEntry *search_entry;

    /* Data */
    char *user_pubkey;
    GHashTable *channels;  /* channel_id -> GnostrChannelRow */
};

G_DEFINE_TYPE(GnostrChannelListView, gnostr_channel_list_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_CHANNEL_SELECTED,
    SIGNAL_CREATE_CHANNEL,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_channel_list_view_dispose(GObject *object)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_channel_list_view_parent_class)->dispose(object);
}

static void
gnostr_channel_list_view_finalize(GObject *object)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(object);

    g_free(self->user_pubkey);
    g_hash_table_destroy(self->channels);

    G_OBJECT_CLASS(gnostr_channel_list_view_parent_class)->finalize(object);
}

static void
on_row_channel_selected(GnostrChannelRow *row, const char *channel_id, GnostrChannelListView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_CHANNEL_SELECTED], 0, channel_id);
}

static void
on_row_open_profile(GnostrChannelRow *row, const char *pubkey, GnostrChannelListView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_create_clicked(GtkButton *button, GnostrChannelListView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_CREATE_CHANNEL], 0);
}

static gboolean
filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GnostrChannelListView *self = GNOSTR_CHANNEL_LIST_VIEW(user_data);
    if (!self->search_entry)
        return TRUE;

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    if (!search_text || !*search_text)
        return TRUE;

    /* Get the channel row and check if name/about contains search text */
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child || !GNOSTR_IS_CHANNEL_ROW(child))
        return TRUE;

    GnostrChannelRow *channel_row = GNOSTR_CHANNEL_ROW(child);
    const char *name = gnostr_channel_row_get_name(channel_row);
    const char *about = gnostr_channel_row_get_about(channel_row);

    gchar *search_lower = g_utf8_strdown(search_text, -1);
    gboolean visible = FALSE;

    if (name) {
        gchar *name_lower = g_utf8_strdown(name, -1);
        if (strstr(name_lower, search_lower))
            visible = TRUE;
        g_free(name_lower);
    }

    if (!visible && about) {
        gchar *about_lower = g_utf8_strdown(about, -1);
        if (strstr(about_lower, search_lower))
            visible = TRUE;
        g_free(about_lower);
    }

    g_free(search_lower);
    return visible;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrChannelListView *self)
{
    (void)entry;
    gtk_list_box_invalidate_filter(self->list_box);
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
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelListView, list_box);
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
    self->channels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Connect create button */
    g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create_clicked), self);

    /* Connect search */
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

    /* Configure list box */
    gtk_list_box_set_selection_mode(self->list_box, GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(self->list_box, FALSE);
    gtk_list_box_set_filter_func(self->list_box, filter_func, self, NULL);
}

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

    /* Check if channel already exists */
    GnostrChannelRow *row = g_hash_table_lookup(self->channels, channel->channel_id);

    if (!row) {
        /* Create new row */
        row = gnostr_channel_row_new();

        /* Connect signals */
        g_signal_connect(row, "channel-selected",
                         G_CALLBACK(on_row_channel_selected), self);
        g_signal_connect(row, "open-profile",
                         G_CALLBACK(on_row_open_profile), self);

        /* Add to list */
        gtk_list_box_prepend(self->list_box, GTK_WIDGET(row));
        g_hash_table_insert(self->channels, g_strdup(channel->channel_id), row);
    }

    /* Update row data */
    gnostr_channel_row_set_channel(row, channel);

    /* Show list, hide empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "list");
}

void
gnostr_channel_list_view_remove_channel(GnostrChannelListView *self,
                                         const char *channel_id)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));
    g_return_if_fail(channel_id != NULL);

    GnostrChannelRow *row = g_hash_table_lookup(self->channels, channel_id);
    if (row) {
        GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(row));
        if (parent && GTK_IS_LIST_BOX_ROW(parent)) {
            gtk_list_box_remove(self->list_box, parent);
        }
        g_hash_table_remove(self->channels, channel_id);
    }

    /* Check if empty */
    if (g_hash_table_size(self->channels) == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_channel_list_view_clear(GnostrChannelListView *self)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    /* Remove all children from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    g_hash_table_remove_all(self->channels);
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
        /* Switch to list or empty based on content */
        if (g_hash_table_size(self->channels) > 0) {
            gtk_stack_set_visible_child_name(self->content_stack, "list");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        }
    }
}

void
gnostr_channel_list_view_set_empty(GnostrChannelListView *self,
                                    gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    if (is_empty) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    }
}

const char *
gnostr_channel_list_view_get_selected_id(GnostrChannelListView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self), NULL);

    GtkListBoxRow *selected = gtk_list_box_get_selected_row(self->list_box);
    if (!selected) return NULL;

    GtkWidget *child = gtk_list_box_row_get_child(selected);
    if (!child || !GNOSTR_IS_CHANNEL_ROW(child))
        return NULL;

    return gnostr_channel_row_get_channel_id(GNOSTR_CHANNEL_ROW(child));
}

void
gnostr_channel_list_view_set_user_pubkey(GnostrChannelListView *self,
                                          const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_LIST_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}
