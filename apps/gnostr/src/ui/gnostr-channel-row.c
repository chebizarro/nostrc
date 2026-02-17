/**
 * GnostrChannelRow - A row widget for displaying a NIP-28 channel
 *
 * Shows channel avatar, name, description, and statistics.
 */

#include "gnostr-channel-row.h"
#include "gnostr-avatar-cache.h"
#include <time.h>

struct _GnostrChannelRow {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkButton *btn_avatar;
    GtkOverlay *avatar_box;
    GtkPicture *avatar_image;
    GtkLabel *avatar_initials;
    GtkLabel *lbl_name;
    GtkLabel *lbl_about;
    GtkLabel *lbl_stats;
    GtkLabel *lbl_created;

    /* Data */
    char *channel_id;
    char *creator_pubkey;
    char *name;
    char *about;
    char *picture;
};

G_DEFINE_TYPE(GnostrChannelRow, gnostr_channel_row, GTK_TYPE_WIDGET)

enum {
    SIGNAL_CHANNEL_SELECTED,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_channel_row_dispose(GObject *object)
{
    GnostrChannelRow *self = GNOSTR_CHANNEL_ROW(object);

    /* Unparent template child */
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_channel_row_parent_class)->dispose(object);
}

static void
gnostr_channel_row_finalize(GObject *object)
{
    GnostrChannelRow *self = GNOSTR_CHANNEL_ROW(object);

    g_free(self->channel_id);
    g_free(self->creator_pubkey);
    g_free(self->name);
    g_free(self->about);
    g_free(self->picture);

    G_OBJECT_CLASS(gnostr_channel_row_parent_class)->finalize(object);
}

static void
on_avatar_clicked(GtkButton *button, GnostrChannelRow *self)
{
    (void)button;
    if (self->creator_pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->creator_pubkey);
    }
}

static void
on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, GnostrChannelRow *self)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    if (self->channel_id) {
        g_signal_emit(self, signals[SIGNAL_CHANNEL_SELECTED], 0, self->channel_id);
    }
}

static void
gnostr_channel_row_class_init(GnostrChannelRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_channel_row_dispose;
    object_class->finalize = gnostr_channel_row_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-channel-row.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, btn_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, avatar_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, lbl_name);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, lbl_about);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, lbl_stats);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelRow, lbl_created);

    /* Signals */
    signals[SIGNAL_CHANNEL_SELECTED] = g_signal_new(
        "channel-selected",
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
    gtk_widget_class_set_css_name(widget_class, "channel-row");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_channel_row_init(GnostrChannelRow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->channel_id = NULL;
    self->creator_pubkey = NULL;
    self->name = NULL;
    self->about = NULL;
    self->picture = NULL;

    /* Connect signals */
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);

    /* Click gesture for the whole row */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "released", G_CALLBACK(on_row_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GnostrChannelRow *
gnostr_channel_row_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHANNEL_ROW, NULL);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("#");

    /* Get first char, handle UTF-8 */
    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

static char *
format_relative_time(gint64 timestamp)
{
    gint64 now = (gint64)time(NULL);
    gint64 diff = now - timestamp;

    if (diff < 0)
        return g_strdup("just now");
    if (diff < 60)
        return g_strdup("just now");
    if (diff < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", diff / 60);
    if (diff < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", diff / 3600);
    if (diff < 604800)
        return g_strdup_printf("%" G_GINT64_FORMAT "d ago", diff / 86400);
    if (diff < 2592000)
        return g_strdup_printf("%" G_GINT64_FORMAT "w ago", diff / 604800);

    /* Older than a month - show date */
    time_t t = (time_t)timestamp;
    struct tm *tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%b %d, %Y", tm);
    return g_strdup(buf);
}

void
gnostr_channel_row_set_channel(GnostrChannelRow *self,
                                const GnostrChannel *channel)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_ROW(self));
    g_return_if_fail(channel != NULL);

    /* Store data */
    g_free(self->channel_id);
    self->channel_id = g_strdup(channel->channel_id);

    g_free(self->creator_pubkey);
    self->creator_pubkey = g_strdup(channel->creator_pubkey);

    g_free(self->name);
    self->name = g_strdup(channel->name);

    g_free(self->about);
    self->about = g_strdup(channel->about);

    g_free(self->picture);
    self->picture = g_strdup(channel->picture);

    /* Update UI */
    const char *display_name = channel->name;
    if (!display_name || !*display_name) {
        /* Fallback to truncated channel ID */
        if (channel->channel_id && strlen(channel->channel_id) >= 8) {
            char *truncated = g_strdup_printf("#%.8s...", channel->channel_id);
            gtk_label_set_text(self->lbl_name, truncated);
            g_free(truncated);
        } else {
            gtk_label_set_text(self->lbl_name, "Unnamed Channel");
        }
        display_name = channel->channel_id;
    } else {
        gtk_label_set_text(self->lbl_name, channel->name);
    }

    /* Set about/description */
    if (channel->about && *channel->about) {
        gtk_label_set_text(self->lbl_about, channel->about);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_about), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_about), FALSE);
    }

    /* Set statistics */
    char *stats = g_strdup_printf("%u members, %u messages",
                                   channel->member_count,
                                   channel->message_count);
    gtk_label_set_text(self->lbl_stats, stats);
    g_free(stats);

    /* Set created time */
    if (channel->created_at > 0) {
        char *created = format_relative_time(channel->created_at);
        char *created_text = g_strdup_printf("Created %s", created);
        gtk_label_set_text(self->lbl_created, created_text);
        g_free(created);
        g_free(created_text);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_created), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_created), FALSE);
    }

    /* Set avatar initials */
    char *initials = get_initials(display_name);
    gtk_label_set_text(self->avatar_initials, initials);
    g_free(initials);

    /* Load avatar image if URL provided */
    if (channel->picture && *channel->picture) {
        gnostr_avatar_download_async(channel->picture,
                                      GTK_WIDGET(self->avatar_image),
                                      GTK_WIDGET(self->avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_image), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_initials), TRUE);
    }
}

const char *
gnostr_channel_row_get_channel_id(GnostrChannelRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_ROW(self), NULL);
    return self->channel_id;
}

const char *
gnostr_channel_row_get_name(GnostrChannelRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_ROW(self), NULL);
    return self->name;
}

const char *
gnostr_channel_row_get_about(GnostrChannelRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_ROW(self), NULL);
    return self->about;
}

const char *
gnostr_channel_row_get_creator_pubkey(GnostrChannelRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_ROW(self), NULL);
    return self->creator_pubkey;
}
