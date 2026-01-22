/**
 * GnostrNotificationRow - A notification row for the notifications view
 *
 * Displays actor avatar, action description, content preview, and timestamp.
 */

#include "gnostr-notification-row.h"
#include "gnostr-avatar-cache.h"
#include <time.h>

struct _GnostrNotificationRow {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkButton *btn_avatar;
    GtkOverlay *avatar_box;
    GtkPicture *avatar_image;
    GtkLabel *avatar_initials;
    GtkImage *type_icon;
    GtkLabel *lbl_actor;
    GtkLabel *lbl_action;
    GtkLabel *lbl_content;
    GtkLabel *lbl_timestamp;
    GtkBox *unread_indicator;

    /* Data */
    char *notification_id;
    char *actor_pubkey;
    char *target_note_id;
    char *avatar_url;
    GnostrNotificationType type;
    gboolean is_read;
};

G_DEFINE_TYPE(GnostrNotificationRow, gnostr_notification_row, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_NOTE,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_MARK_READ,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_notification_row_dispose(GObject *object)
{
    GnostrNotificationRow *self = GNOSTR_NOTIFICATION_ROW(object);

    /* Unparent template child */
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_notification_row_parent_class)->dispose(object);
}

static void
gnostr_notification_row_finalize(GObject *object)
{
    GnostrNotificationRow *self = GNOSTR_NOTIFICATION_ROW(object);

    g_free(self->notification_id);
    g_free(self->actor_pubkey);
    g_free(self->target_note_id);
    g_free(self->avatar_url);

    G_OBJECT_CLASS(gnostr_notification_row_parent_class)->finalize(object);
}

static void
on_avatar_clicked(GtkButton *button, GnostrNotificationRow *self)
{
    (void)button;
    if (self->actor_pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->actor_pubkey);
    }
}

static void
on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, GnostrNotificationRow *self)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    /* Mark as read on click */
    if (!self->is_read && self->notification_id) {
        g_signal_emit(self, signals[SIGNAL_MARK_READ], 0, self->notification_id);
    }

    /* Navigate based on notification type */
    if (self->target_note_id) {
        g_signal_emit(self, signals[SIGNAL_OPEN_NOTE], 0, self->target_note_id);
    } else if (self->actor_pubkey) {
        /* For follow notifications, open profile */
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->actor_pubkey);
    }
}

static void
gnostr_notification_row_class_init(GnostrNotificationRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_notification_row_dispose;
    object_class->finalize = gnostr_notification_row_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-notification-row.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, btn_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, avatar_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, type_icon);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, lbl_actor);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, lbl_action);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, lbl_content);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, lbl_timestamp);
    gtk_widget_class_bind_template_child(widget_class, GnostrNotificationRow, unread_indicator);

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

    signals[SIGNAL_MARK_READ] = g_signal_new(
        "mark-read",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "notification-row");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_notification_row_init(GnostrNotificationRow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->notification_id = NULL;
    self->actor_pubkey = NULL;
    self->target_note_id = NULL;
    self->avatar_url = NULL;
    self->is_read = FALSE;

    /* Connect signals */
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);

    /* Click gesture for the whole row */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GnostrNotificationRow *
gnostr_notification_row_new(void)
{
    return g_object_new(GNOSTR_TYPE_NOTIFICATION_ROW, NULL);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("?");

    /* Get first char, handle UTF-8 */
    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

static const char *
get_type_icon_name(GnostrNotificationType type)
{
    switch (type) {
        case GNOSTR_NOTIFICATION_TYPE_MENTION:
            return "user-available-symbolic";
        case GNOSTR_NOTIFICATION_TYPE_REPLY:
            return "mail-reply-sender-symbolic";
        case GNOSTR_NOTIFICATION_TYPE_REPOST:
            return "emblem-shared-symbolic";
        case GNOSTR_NOTIFICATION_TYPE_REACTION:
            return "emblem-favorite-symbolic";
        case GNOSTR_NOTIFICATION_TYPE_ZAP:
            return "weather-storm-symbolic";
        case GNOSTR_NOTIFICATION_TYPE_FOLLOW:
            return "contact-new-symbolic";
        default:
            return "dialog-information-symbolic";
    }
}

static const char *
get_action_text(GnostrNotificationType type, guint64 zap_amount_msats)
{
    switch (type) {
        case GNOSTR_NOTIFICATION_TYPE_MENTION:
            return "mentioned you";
        case GNOSTR_NOTIFICATION_TYPE_REPLY:
            return "replied to your note";
        case GNOSTR_NOTIFICATION_TYPE_REPOST:
            return "reposted your note";
        case GNOSTR_NOTIFICATION_TYPE_REACTION:
            return "reacted to your note";
        case GNOSTR_NOTIFICATION_TYPE_ZAP:
            if (zap_amount_msats > 0) {
                static char zap_buf[64];
                guint64 sats = zap_amount_msats / 1000;
                g_snprintf(zap_buf, sizeof(zap_buf), "zapped you %" G_GUINT64_FORMAT " sats", sats);
                return zap_buf;
            }
            return "zapped your note";
        case GNOSTR_NOTIFICATION_TYPE_FOLLOW:
            return "started following you";
        default:
            return "interacted with you";
    }
}

static char *
format_relative_time(gint64 timestamp)
{
    gint64 now = (gint64)time(NULL);
    gint64 diff = now - timestamp;

    if (diff < 0)
        return g_strdup("now");
    if (diff < 60)
        return g_strdup("now");
    if (diff < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m", diff / 60);
    if (diff < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h", diff / 3600);
    if (diff < 604800)
        return g_strdup_printf("%" G_GINT64_FORMAT "d", diff / 86400);

    /* Older than a week - show date */
    time_t t = (time_t)timestamp;
    struct tm *tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%b %d", tm);
    return g_strdup(buf);
}

void
gnostr_notification_row_set_notification(GnostrNotificationRow *self,
                                          const GnostrNotification *notif)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self));
    g_return_if_fail(notif != NULL);

    /* Store IDs */
    g_free(self->notification_id);
    self->notification_id = g_strdup(notif->id);

    g_free(self->actor_pubkey);
    self->actor_pubkey = g_strdup(notif->actor_pubkey);

    g_free(self->target_note_id);
    self->target_note_id = g_strdup(notif->target_note_id);

    g_free(self->avatar_url);
    self->avatar_url = g_strdup(notif->actor_avatar_url);

    self->type = notif->type;
    self->is_read = notif->is_read;

    /* Set actor name */
    const char *name = notif->actor_name;
    if (!name || !*name) {
        /* Fallback to truncated pubkey */
        if (notif->actor_pubkey && strlen(notif->actor_pubkey) >= 8) {
            char *truncated = g_strdup_printf("%.8s...", notif->actor_pubkey);
            gtk_label_set_text(self->lbl_actor, truncated);
            g_free(truncated);
        } else {
            gtk_label_set_text(self->lbl_actor, "Unknown");
        }
        name = notif->actor_pubkey;
    } else {
        gtk_label_set_text(self->lbl_actor, notif->actor_name);
    }

    /* Set type icon */
    gtk_image_set_from_icon_name(self->type_icon, get_type_icon_name(notif->type));

    /* Set action text */
    gtk_label_set_text(self->lbl_action, get_action_text(notif->type, notif->zap_amount_msats));

    /* Set content preview */
    if (notif->content_preview && *notif->content_preview) {
        gtk_label_set_text(self->lbl_content, notif->content_preview);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_content), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_content), FALSE);
    }

    /* Set timestamp */
    char *ts = format_relative_time(notif->created_at);
    gtk_label_set_text(self->lbl_timestamp, ts);
    g_free(ts);

    /* Set avatar initials */
    char *initials = get_initials(name);
    gtk_label_set_text(self->avatar_initials, initials);
    g_free(initials);

    /* Load avatar image if URL provided */
    if (notif->actor_avatar_url && *notif->actor_avatar_url) {
        gnostr_avatar_download_async(notif->actor_avatar_url,
                                      GTK_WIDGET(self->avatar_image),
                                      GTK_WIDGET(self->avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_image), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_initials), TRUE);
    }

    /* Update read state */
    gnostr_notification_row_set_read(self, notif->is_read);
}

const char *
gnostr_notification_row_get_id(GnostrNotificationRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self), NULL);
    return self->notification_id;
}

const char *
gnostr_notification_row_get_target_note_id(GnostrNotificationRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self), NULL);
    return self->target_note_id;
}

const char *
gnostr_notification_row_get_actor_pubkey(GnostrNotificationRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self), NULL);
    return self->actor_pubkey;
}

void
gnostr_notification_row_set_read(GnostrNotificationRow *self, gboolean is_read)
{
    g_return_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self));

    self->is_read = is_read;

    if (is_read) {
        gtk_widget_set_visible(GTK_WIDGET(self->unread_indicator), FALSE);
        gtk_widget_remove_css_class(GTK_WIDGET(self), "unread");
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->unread_indicator), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(self), "unread");
    }
}

gboolean
gnostr_notification_row_is_read(GnostrNotificationRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_NOTIFICATION_ROW(self), TRUE);
    return self->is_read;
}
