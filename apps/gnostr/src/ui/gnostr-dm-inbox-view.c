/**
 * GnostrDmInboxView - List of DM conversations
 *
 * Displays a scrollable list of conversation summaries.
 */

#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-row.h"

struct _GnostrDmInboxView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListBox *list_box;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkButton *btn_compose;

    /* Data */
    char *user_pubkey;
    GHashTable *conversations;  /* peer_pubkey -> GnostrDmRow */
};

G_DEFINE_TYPE(GnostrDmInboxView, gnostr_dm_inbox_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_CONVERSATION,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_COMPOSE_DM,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_dm_inbox_view_dispose(GObject *object)
{
    GnostrDmInboxView *self = GNOSTR_DM_INBOX_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_dm_inbox_view_parent_class)->dispose(object);
}

static void
gnostr_dm_inbox_view_finalize(GObject *object)
{
    GnostrDmInboxView *self = GNOSTR_DM_INBOX_VIEW(object);

    g_free(self->user_pubkey);
    g_hash_table_destroy(self->conversations);

    G_OBJECT_CLASS(gnostr_dm_inbox_view_parent_class)->finalize(object);
}

static void
on_row_open_conversation(GnostrDmRow *row, const char *peer_pubkey, GnostrDmInboxView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_CONVERSATION], 0, peer_pubkey);
}

static void
on_row_open_profile(GnostrDmRow *row, const char *pubkey, GnostrDmInboxView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_compose_clicked(GtkButton *button, GnostrDmInboxView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_COMPOSE_DM], 0);
}

static void
gnostr_dm_inbox_view_class_init(GnostrDmInboxViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_dm_inbox_view_dispose;
    object_class->finalize = gnostr_dm_inbox_view_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-dm-inbox-view.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, list_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmInboxView, btn_compose);

    /* Signals */
    signals[SIGNAL_OPEN_CONVERSATION] = g_signal_new(
        "open-conversation",
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

    signals[SIGNAL_COMPOSE_DM] = g_signal_new(
        "compose-dm",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "dm-inbox");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_dm_inbox_view_init(GnostrDmInboxView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->user_pubkey = NULL;
    self->conversations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Connect compose button */
    g_signal_connect(self->btn_compose, "clicked", G_CALLBACK(on_compose_clicked), self);

    /* Configure list box */
    gtk_list_box_set_selection_mode(self->list_box, GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(self->list_box, FALSE);
}

GnostrDmInboxView *
gnostr_dm_inbox_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_INBOX_VIEW, NULL);
}

void
gnostr_dm_inbox_view_upsert_conversation(GnostrDmInboxView *self,
                                          const GnostrDmConversation *conv)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));
    g_return_if_fail(conv != NULL);
    g_return_if_fail(conv->peer_pubkey != NULL);

    /* Check if conversation already exists */
    GnostrDmRow *row = g_hash_table_lookup(self->conversations, conv->peer_pubkey);

    if (!row) {
        /* Create new row */
        row = gnostr_dm_row_new();

        /* Connect signals */
        g_signal_connect(row, "open-conversation",
                         G_CALLBACK(on_row_open_conversation), self);
        g_signal_connect(row, "open-profile",
                         G_CALLBACK(on_row_open_profile), self);

        /* Add to list */
        gtk_list_box_prepend(self->list_box, GTK_WIDGET(row));
        g_hash_table_insert(self->conversations, g_strdup(conv->peer_pubkey), row);
    }

    /* Update row data */
    gnostr_dm_row_set_peer(row, conv->peer_pubkey, conv->display_name,
                           conv->handle, conv->avatar_url);
    gnostr_dm_row_set_preview(row, conv->last_message, conv->is_outgoing);
    gnostr_dm_row_set_timestamp(row, conv->last_timestamp, NULL);
    gnostr_dm_row_set_unread(row, conv->unread_count);

    /* Show list, hide empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "list");
}

void
gnostr_dm_inbox_view_remove_conversation(GnostrDmInboxView *self,
                                          const char *peer_pubkey)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));
    g_return_if_fail(peer_pubkey != NULL);

    GnostrDmRow *row = g_hash_table_lookup(self->conversations, peer_pubkey);
    if (row) {
        gtk_list_box_remove(self->list_box, GTK_WIDGET(row));
        g_hash_table_remove(self->conversations, peer_pubkey);
    }

    /* Check if empty */
    if (g_hash_table_size(self->conversations) == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_dm_inbox_view_clear(GnostrDmInboxView *self)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));

    /* Remove all children from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    g_hash_table_remove_all(self->conversations);
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

void
gnostr_dm_inbox_view_mark_read(GnostrDmInboxView *self,
                                const char *peer_pubkey)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));
    g_return_if_fail(peer_pubkey != NULL);

    GnostrDmRow *row = g_hash_table_lookup(self->conversations, peer_pubkey);
    if (row) {
        gnostr_dm_row_set_unread(row, 0);
    }
}

void
gnostr_dm_inbox_view_set_user_pubkey(GnostrDmInboxView *self,
                                      const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}

void
gnostr_dm_inbox_view_set_empty(GnostrDmInboxView *self, gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));

    if (is_empty) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    }
}

void
gnostr_dm_inbox_view_set_loading(GnostrDmInboxView *self, gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_DM_INBOX_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_spinner_start(self->loading_spinner);
    } else {
        gtk_spinner_stop(self->loading_spinner);
        /* Switch to list or empty based on content */
        if (g_hash_table_size(self->conversations) > 0) {
            gtk_stack_set_visible_child_name(self->content_stack, "list");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        }
    }
}

void
gnostr_dm_conversation_free(GnostrDmConversation *conv)
{
    if (!conv) return;

    g_free(conv->peer_pubkey);
    g_free(conv->display_name);
    g_free(conv->handle);
    g_free(conv->avatar_url);
    g_free(conv->last_message);
    g_free(conv);
}
