/**
 * GnostrNotificationsView - List of notifications
 *
 * Displays a scrollable list of notifications including mentions, replies,
 * reposts, reactions, zaps, and new followers.
 */

#include "gnostr-notifications-view.h"
#include "gnostr-notification-row.h"
#include "../util/utils.h"

/* Maximum notifications to keep in memory to prevent unbounded growth */
#define NOTIFICATIONS_MAX 500

struct _GnostrNotificationsView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkScrolledWindow *scroller;
    GtkListBox *list_box;
    GtkStack *content_stack;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkButton *btn_mark_all_read;

    /* Data */
    char *user_pubkey;
    GHashTable *notifications;  /* notification_id -> GnostrNotificationRow */
    guint unread_count;
    gint64 last_checked;
};

G_DEFINE_TYPE(GnostrNotificationsView, gnostr_notifications_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_NOTE,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_notifications_view_dispose(GObject *object)
{
    GnostrNotificationsView *self = GNOSTR_NOTIFICATIONS_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_notifications_view_parent_class)->dispose(object);
}

static void
gnostr_notifications_view_finalize(GObject *object)
{
    GnostrNotificationsView *self = GNOSTR_NOTIFICATIONS_VIEW(object);

    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_pointer(&self->notifications, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_notifications_view_parent_class)->finalize(object);
}

static void
on_row_open_note(GnostrNotificationRow *row, const char *note_id, GnostrNotificationsView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_NOTE], 0, note_id);
}

static void
on_row_open_profile(GnostrNotificationRow *row, const char *pubkey, GnostrNotificationsView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_row_mark_read(GnostrNotificationRow *row, const char *notification_id, GnostrNotificationsView *self)
{
    (void)row;
    gnostr_notifications_view_mark_read(self, notification_id);
}

static void
on_mark_all_read_clicked(GtkButton *button, GnostrNotificationsView *self)
{
    (void)button;
    gnostr_notifications_view_mark_all_read(self);
}

static gint
compare_rows_by_timestamp(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data)
{
    (void)user_data;

    /* Get the actual notification row widgets */
    GtkWidget *child1 = gtk_list_box_row_get_child(row1);
    GtkWidget *child2 = gtk_list_box_row_get_child(row2);

    if (!GNOSTR_IS_NOTIFICATION_ROW(child1) || !GNOSTR_IS_NOTIFICATION_ROW(child2))
        return 0;

    /* Newer notifications should appear first (descending order) */
    /* For now, just maintain insertion order; proper timestamp sorting
       would require storing timestamp in the row */
    return 0;
}

static void
gnostr_notifications_view_class_init(GnostrNotificationsViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_notifications_view_dispose;
    object_class->finalize = gnostr_notifications_view_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-notifications-view.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, list_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, empty_state);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationsView, btn_mark_all_read);

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

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "notifications-view");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_notifications_view_init(GnostrNotificationsView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->user_pubkey = NULL;
    self->notifications = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->unread_count = 0;
    self->last_checked = 0;

    /* Connect mark all read button */
    g_signal_connect(self->btn_mark_all_read, "clicked",
                     G_CALLBACK(on_mark_all_read_clicked), self);

    /* Configure list box */
    gtk_list_box_set_selection_mode(self->list_box, GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(self->list_box, FALSE);
    gtk_list_box_set_sort_func(self->list_box, compare_rows_by_timestamp, NULL, NULL);
}

GnostrNotificationsView *
gnostr_notifications_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_NOTIFICATIONS_VIEW, NULL);
}

void
gnostr_notifications_view_add_notification(GnostrNotificationsView *self,
                                            const GnostrNotification *notif)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));
    g_return_if_fail(notif != NULL);
    g_return_if_fail(notif->id != NULL);

    /* Check if notification already exists */
    if (g_hash_table_contains(self->notifications, notif->id)) {
        /* Update existing notification */
        gnostr_notifications_view_update_notification(self, notif);
        return;
    }

    /* Create new row */
    GnostrNotificationRow *row = gnostr_notification_row_new();

    /* Connect signals */
    g_signal_connect(row, "open-note",
                     G_CALLBACK(on_row_open_note), self);
    g_signal_connect(row, "open-profile",
                     G_CALLBACK(on_row_open_profile), self);
    g_signal_connect(row, "mark-read",
                     G_CALLBACK(on_row_mark_read), self);

    /* Set notification data */
    gnostr_notification_row_set_notification(row, notif);

    /* Add to list (prepend for newest first) */
    gtk_list_box_prepend(self->list_box, GTK_WIDGET(row));
    g_hash_table_insert(self->notifications, g_strdup(notif->id), row);

    /* Prune oldest notifications if exceeding limit to prevent unbounded memory growth */
    while (g_hash_table_size(self->notifications) > NOTIFICATIONS_MAX) {
        /* Remove last (oldest) row from list box */
        GtkWidget *last_child = NULL;
        for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box));
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            last_child = child;
        }
        if (last_child && GNOSTR_IS_NOTIFICATION_ROW(last_child)) {
            GnostrNotificationRow *old_row = GNOSTR_NOTIFICATION_ROW(last_child);
            const char *old_id = gnostr_notification_row_get_id(old_row);
            if (old_id) {
                g_hash_table_remove(self->notifications, old_id);
            }
            gtk_list_box_remove(self->list_box, last_child);
        } else {
            break;  /* Safety: avoid infinite loop */
        }
    }

    /* Update unread count */
    if (!notif->is_read) {
        self->unread_count++;
    }

    /* Show list, hide empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "list");

    /* Show/hide mark all read button based on unread count */
    gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), self->unread_count > 0);
}

void
gnostr_notifications_view_update_notification(GnostrNotificationsView *self,
                                                const GnostrNotification *notif)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));
    g_return_if_fail(notif != NULL);
    g_return_if_fail(notif->id != NULL);

    GnostrNotificationRow *row = g_hash_table_lookup(self->notifications, notif->id);
    if (row) {
        gboolean was_unread = !gnostr_notification_row_is_read(row);
        gnostr_notification_row_set_notification(row, notif);

        /* Update unread count if read state changed */
        if (was_unread && notif->is_read) {
            if (self->unread_count > 0)
                self->unread_count--;
        } else if (!was_unread && !notif->is_read) {
            self->unread_count++;
        }

        gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), self->unread_count > 0);
    }
}

void
gnostr_notifications_view_remove_notification(GnostrNotificationsView *self,
                                                const char *notification_id)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));
    g_return_if_fail(notification_id != NULL);

    GnostrNotificationRow *row = g_hash_table_lookup(self->notifications, notification_id);
    if (row) {
        /* Update unread count */
        if (!gnostr_notification_row_is_read(row)) {
            if (self->unread_count > 0)
                self->unread_count--;
        }

        /* Find parent GtkListBoxRow and remove */
        GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(row));
        if (parent && GTK_IS_LIST_BOX_ROW(parent)) {
            gtk_list_box_remove(self->list_box, parent);
        }
        g_hash_table_remove(self->notifications, notification_id);
    }

    /* Check if empty */
    if (g_hash_table_size(self->notifications) == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }

    gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), self->unread_count > 0);
}

void
gnostr_notifications_view_clear(GnostrNotificationsView *self)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));

    /* Remove all children from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    g_hash_table_remove_all(self->notifications);
    self->unread_count = 0;
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
    gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), FALSE);
}

void
gnostr_notifications_view_mark_read(GnostrNotificationsView *self,
                                     const char *notification_id)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));
    g_return_if_fail(notification_id != NULL);

    GnostrNotificationRow *row = g_hash_table_lookup(self->notifications, notification_id);
    if (row && !gnostr_notification_row_is_read(row)) {
        gnostr_notification_row_set_read(row, TRUE);
        if (self->unread_count > 0)
            self->unread_count--;
        gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), self->unread_count > 0);
    }
}

void
gnostr_notifications_view_mark_all_read(GnostrNotificationsView *self)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, self->notifications);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnostrNotificationRow *row = GNOSTR_NOTIFICATION_ROW(value);
        if (!gnostr_notification_row_is_read(row)) {
            gnostr_notification_row_set_read(row, TRUE);
        }
    }

    self->unread_count = 0;
    self->last_checked = (gint64)time(NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_mark_all_read), FALSE);
}

guint
gnostr_notifications_view_get_unread_count(GnostrNotificationsView *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self), 0);
    return self->unread_count;
}

void
gnostr_notifications_view_set_user_pubkey(GnostrNotificationsView *self,
                                           const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));

    /* nostrc-akyz: defensively normalize npub/nprofile to hex */
    g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    g_free(self->user_pubkey);
    self->user_pubkey = hex ? g_strdup(hex) : NULL;
}

void
gnostr_notifications_view_set_empty(GnostrNotificationsView *self, gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));

    if (is_empty) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "list");
    }
}

void
gnostr_notifications_view_set_loading(GnostrNotificationsView *self, gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_spinner_start(self->loading_spinner);
    } else {
        gtk_spinner_stop(self->loading_spinner);
        /* Switch to list or empty based on content */
        if (g_hash_table_size(self->notifications) > 0) {
            gtk_stack_set_visible_child_name(self->content_stack, "list");
        } else {
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
        }
    }
}

void
gnostr_notifications_view_set_last_checked(GnostrNotificationsView *self, gint64 timestamp)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self));
    self->last_checked = timestamp;
}

gint64
gnostr_notifications_view_get_last_checked(GnostrNotificationsView *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATIONS_VIEW(self), 0);
    return self->last_checked;
}

void
gnostr_notification_free(GnostrNotification *notif)
{
    if (!notif) return;

    g_free(notif->id);
    g_free(notif->actor_pubkey);
    g_free(notif->actor_name);
    g_free(notif->actor_handle);
    g_free(notif->actor_avatar_url);
    g_free(notif->target_note_id);
    g_free(notif->content_preview);
    g_free(notif);
}

const char *
gnostr_notification_type_name(GnostrNotificationType type)
{
    switch (type) {
        case GNOSTR_NOTIFICATION_TYPE_MENTION:
            return "mention";
        case GNOSTR_NOTIFICATION_TYPE_REPLY:
            return "reply";
        case GNOSTR_NOTIFICATION_TYPE_REPOST:
            return "repost";
        case GNOSTR_NOTIFICATION_TYPE_REACTION:
            return "reaction";
        case GNOSTR_NOTIFICATION_TYPE_ZAP:
            return "zap";
        case GNOSTR_NOTIFICATION_TYPE_FOLLOW:
            return "follow";
        case GNOSTR_NOTIFICATION_TYPE_LIST:
            return "list";
        default:
            return "unknown";
    }
}
