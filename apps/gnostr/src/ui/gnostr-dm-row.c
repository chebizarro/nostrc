/**
 * GnostrDmRow - A conversation row for the DM inbox
 *
 * Displays peer avatar, name, message preview, timestamp, and unread indicator.
 */

#include "gnostr-dm-row.h"
#include "gnostr-avatar-cache.h"
#include <time.h>

struct _GnostrDmRow {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkButton *btn_avatar;
    GtkOverlay *avatar_box;
    GtkPicture *avatar_image;
    GtkLabel *avatar_initials;
    GtkLabel *lbl_display;
    GtkLabel *lbl_handle;
    GtkLabel *lbl_preview;
    GtkLabel *lbl_timestamp;
    GtkLabel *lbl_unread;
    GtkBox *unread_badge;

    /* Data */
    char *peer_pubkey;
    char *avatar_url;
};

G_DEFINE_TYPE(GnostrDmRow, gnostr_dm_row, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_CONVERSATION,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_dm_row_dispose(GObject *object)
{
    GnostrDmRow *self = GNOSTR_DM_ROW(object);

    /* Unparent template child */
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_dm_row_parent_class)->dispose(object);
}

static void
gnostr_dm_row_finalize(GObject *object)
{
    GnostrDmRow *self = GNOSTR_DM_ROW(object);

    g_free(self->peer_pubkey);
    g_free(self->avatar_url);

    G_OBJECT_CLASS(gnostr_dm_row_parent_class)->finalize(object);
}

static void
on_avatar_clicked(GtkButton *button, GnostrDmRow *self)
{
    if (self->peer_pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->peer_pubkey);
    }
}

static void
on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, GnostrDmRow *self)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    if (self->peer_pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_CONVERSATION], 0, self->peer_pubkey);
    }
}

static void
gnostr_dm_row_class_init(GnostrDmRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_dm_row_dispose;
    object_class->finalize = gnostr_dm_row_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-dm-row.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, btn_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, avatar_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, lbl_display);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, lbl_handle);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, lbl_preview);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, lbl_timestamp);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, lbl_unread);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmRow, unread_badge);

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

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "dm-row");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_dm_row_init(GnostrDmRow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->peer_pubkey = NULL;
    self->avatar_url = NULL;

    /* Connect signals */
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);

    /* Click gesture for the whole row */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GnostrDmRow *
gnostr_dm_row_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_ROW, NULL);
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

void
gnostr_dm_row_set_peer(GnostrDmRow *self,
                       const char *pubkey_hex,
                       const char *display_name,
                       const char *handle,
                       const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_DM_ROW(self));

    g_free(self->peer_pubkey);
    self->peer_pubkey = g_strdup(pubkey_hex);

    g_free(self->avatar_url);
    self->avatar_url = g_strdup(avatar_url);

    /* Set display name */
    const char *name = display_name;
    if (!name || !*name) {
        /* Fallback to truncated pubkey */
        if (pubkey_hex && strlen(pubkey_hex) >= 8) {
            char *truncated = g_strdup_printf("%.8s...", pubkey_hex);
            gtk_label_set_text(self->lbl_display, truncated);
            g_free(truncated);
        } else {
            gtk_label_set_text(self->lbl_display, "Unknown");
        }
        name = pubkey_hex;
    } else {
        gtk_label_set_text(self->lbl_display, display_name);
    }

    /* Set handle */
    if (handle && *handle) {
        gtk_label_set_text(self->lbl_handle, handle);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_handle), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_handle), FALSE);
    }

    /* Set avatar initials */
    char *initials = get_initials(name);
    gtk_label_set_text(self->avatar_initials, initials);
    g_free(initials);

    /* Load avatar image if URL provided */
    if (avatar_url && *avatar_url) {
        /* Use avatar cache to load async */
        gnostr_avatar_download_async(avatar_url, GTK_WIDGET(self->avatar_image), GTK_WIDGET(self->avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_image), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_initials), TRUE);
    }
}

void
gnostr_dm_row_set_preview(GnostrDmRow *self,
                          const char *preview,
                          gboolean is_outgoing)
{
    g_return_if_fail(GNOSTR_IS_DM_ROW(self));

    if (preview && *preview) {
        if (is_outgoing) {
            char *with_prefix = g_strdup_printf("You: %s", preview);
            gtk_label_set_text(self->lbl_preview, with_prefix);
            g_free(with_prefix);
        } else {
            gtk_label_set_text(self->lbl_preview, preview);
        }
    } else {
        gtk_label_set_text(self->lbl_preview, "");
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
gnostr_dm_row_set_timestamp(GnostrDmRow *self,
                            gint64 created_at,
                            const char *fallback_ts)
{
    g_return_if_fail(GNOSTR_IS_DM_ROW(self));

    char *ts = NULL;
    if (created_at > 0) {
        ts = format_relative_time(created_at);

        /* Set tooltip with full date/time */
        GDateTime *dt = g_date_time_new_from_unix_local(created_at);
        if (dt) {
            gchar *full_date = g_date_time_format(dt, "%B %d, %Y at %l:%M %p");
            if (full_date) {
                gtk_widget_set_tooltip_text(GTK_WIDGET(self->lbl_timestamp), full_date);
                g_free(full_date);
            }
            g_date_time_unref(dt);
        }
    } else if (fallback_ts) {
        ts = g_strdup(fallback_ts);
    }

    if (ts) {
        gtk_label_set_text(self->lbl_timestamp, ts);
        g_free(ts);
    }
}

void
gnostr_dm_row_set_unread(GnostrDmRow *self, guint unread_count)
{
    g_return_if_fail(GNOSTR_IS_DM_ROW(self));

    if (unread_count > 0) {
        char *text = g_strdup_printf("%u", unread_count);
        gtk_label_set_text(self->lbl_unread, text);
        g_free(text);
        gtk_widget_set_visible(GTK_WIDGET(self->unread_badge), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(self), "unread");
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->unread_badge), FALSE);
        gtk_widget_remove_css_class(GTK_WIDGET(self), "unread");
    }
}

const char *
gnostr_dm_row_get_peer_pubkey(GnostrDmRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_DM_ROW(self), NULL);
    return self->peer_pubkey;
}
