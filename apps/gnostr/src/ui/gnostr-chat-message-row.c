/**
 * GnostrChatMessageRow - A row widget for displaying a chat message
 *
 * Shows author avatar, name, timestamp, message content, and action buttons.
 */

#include "gnostr-chat-message-row.h"
#include "gnostr-avatar-cache.h"
#include <time.h>

struct _GnostrChatMessageRow {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkBox *content_box;
    GtkButton *btn_avatar;
    GtkOverlay *avatar_box;
    GtkPicture *avatar_image;
    GtkLabel *avatar_initials;
    GtkLabel *lbl_author;
    GtkLabel *lbl_timestamp;
    GtkLabel *lbl_content;
    GtkBox *action_box;
    GtkButton *btn_reply;
    GtkButton *btn_hide;
    GtkButton *btn_mute;

    /* Data */
    char *message_id;
    char *author_pubkey;
    char *author_name;
    char *avatar_url;
    gboolean is_own;
    gboolean is_hidden;
};

G_DEFINE_TYPE(GnostrChatMessageRow, gnostr_chat_message_row, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_PROFILE,
    SIGNAL_REPLY,
    SIGNAL_HIDE,
    SIGNAL_MUTE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_chat_message_row_dispose(GObject *object)
{
    GnostrChatMessageRow *self = GNOSTR_CHAT_MESSAGE_ROW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_chat_message_row_parent_class)->dispose(object);
}

static void
gnostr_chat_message_row_finalize(GObject *object)
{
    GnostrChatMessageRow *self = GNOSTR_CHAT_MESSAGE_ROW(object);

    g_free(self->message_id);
    g_free(self->author_pubkey);
    g_free(self->author_name);
    g_free(self->avatar_url);

    G_OBJECT_CLASS(gnostr_chat_message_row_parent_class)->finalize(object);
}

static void
on_avatar_clicked(GtkButton *button, GnostrChatMessageRow *self)
{
    (void)button;
    if (self->author_pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->author_pubkey);
    }
}

static void
on_reply_clicked(GtkButton *button, GnostrChatMessageRow *self)
{
    (void)button;
    if (self->message_id) {
        g_signal_emit(self, signals[SIGNAL_REPLY], 0, self->message_id);
    }
}

static void
on_hide_clicked(GtkButton *button, GnostrChatMessageRow *self)
{
    (void)button;
    if (self->message_id) {
        g_signal_emit(self, signals[SIGNAL_HIDE], 0, self->message_id);
    }
}

static void
on_mute_clicked(GtkButton *button, GnostrChatMessageRow *self)
{
    (void)button;
    if (self->author_pubkey) {
        g_signal_emit(self, signals[SIGNAL_MUTE], 0, self->author_pubkey);
    }
}

static void
gnostr_chat_message_row_class_init(GnostrChatMessageRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_chat_message_row_dispose;
    object_class->finalize = gnostr_chat_message_row_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-chat-message-row.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, content_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, btn_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, avatar_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, lbl_author);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, lbl_timestamp);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, lbl_content);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, action_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, btn_reply);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, btn_hide);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatMessageRow, btn_mute);

    /* Signals */
    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_REPLY] = g_signal_new(
        "reply",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_HIDE] = g_signal_new(
        "hide",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_MUTE] = g_signal_new(
        "mute",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "chat-message");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_chat_message_row_init(GnostrChatMessageRow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->message_id = NULL;
    self->author_pubkey = NULL;
    self->author_name = NULL;
    self->avatar_url = NULL;
    self->is_own = FALSE;
    self->is_hidden = FALSE;

    /* Connect signals */
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
    g_signal_connect(self->btn_reply, "clicked", G_CALLBACK(on_reply_clicked), self);
    g_signal_connect(self->btn_hide, "clicked", G_CALLBACK(on_hide_clicked), self);
    g_signal_connect(self->btn_mute, "clicked", G_CALLBACK(on_mute_clicked), self);

    /* Hide mod actions by default */
    gtk_widget_set_visible(GTK_WIDGET(self->btn_hide), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_mute), FALSE);
}

GnostrChatMessageRow *
gnostr_chat_message_row_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHAT_MESSAGE_ROW, NULL);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("?");

    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

static char *
format_time(gint64 timestamp)
{
    time_t t = (time_t)timestamp;
    struct tm *tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%l:%M %p", tm);
    return g_strdup(g_strstrip(buf));
}

void
gnostr_chat_message_row_set_message(GnostrChatMessageRow *self,
                                     const GnostrChatMessage *msg)
{
    g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self));
    g_return_if_fail(msg != NULL);

    /* Store data */
    g_free(self->message_id);
    self->message_id = g_strdup(msg->event_id);

    g_free(self->author_pubkey);
    self->author_pubkey = g_strdup(msg->author_pubkey);

    /* Set content */
    gtk_label_set_text(self->lbl_content, msg->content ? msg->content : "");

    /* Set timestamp */
    if (msg->created_at > 0) {
        char *ts = format_time(msg->created_at);
        gtk_label_set_text(self->lbl_timestamp, ts);
        g_free(ts);

        /* Set tooltip with full date */
        GDateTime *dt = g_date_time_new_from_unix_local(msg->created_at);
        if (dt) {
            gchar *full_date = g_date_time_format(dt, "%B %d, %Y at %l:%M:%S %p");
            if (full_date) {
                gtk_widget_set_tooltip_text(GTK_WIDGET(self->lbl_timestamp), full_date);
                g_free(full_date);
            }
            g_date_time_unref(dt);
        }
    }

    /* Set author name (fallback to truncated pubkey) */
    if (!self->author_name || !*self->author_name) {
        if (msg->author_pubkey && strlen(msg->author_pubkey) >= 8) {
            char *truncated = g_strdup_printf("%.8s...", msg->author_pubkey);
            gtk_label_set_text(self->lbl_author, truncated);
            char *initials = get_initials(truncated);
            gtk_label_set_text(self->avatar_initials, initials);
            g_free(initials);
            g_free(truncated);
        } else {
            gtk_label_set_text(self->lbl_author, "Anonymous");
            gtk_label_set_text(self->avatar_initials, "?");
        }
    }

    /* Update hidden state */
    gnostr_chat_message_row_set_hidden(self, msg->is_hidden);
}

const char *
gnostr_chat_message_row_get_message_id(GnostrChatMessageRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self), NULL);
    return self->message_id;
}

const char *
gnostr_chat_message_row_get_author_pubkey(GnostrChatMessageRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self), NULL);
    return self->author_pubkey;
}

const char *
gnostr_chat_message_row_get_author_name(GnostrChatMessageRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self), NULL);
    return self->author_name ? self->author_name : self->author_pubkey;
}

void
gnostr_chat_message_row_set_author_profile(GnostrChatMessageRow *self,
                                            const char *display_name,
                                            const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self));

    /* Update author name */
    if (display_name && *display_name) {
        g_free(self->author_name);
        self->author_name = g_strdup(display_name);
        gtk_label_set_text(self->lbl_author, display_name);

        char *initials = get_initials(display_name);
        gtk_label_set_text(self->avatar_initials, initials);
        g_free(initials);
    }

    /* Update avatar */
    if (avatar_url && *avatar_url) {
        g_free(self->avatar_url);
        self->avatar_url = g_strdup(avatar_url);
        gnostr_avatar_download_async(avatar_url,
                                      GTK_WIDGET(self->avatar_image),
                                      GTK_WIDGET(self->avatar_initials));
    }
}

void
gnostr_chat_message_row_set_is_own(GnostrChatMessageRow *self,
                                    gboolean is_own)
{
    g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self));

    self->is_own = is_own;

    if (is_own) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "own-message");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "own-message");
    }
}

void
gnostr_chat_message_row_set_show_mod_actions(GnostrChatMessageRow *self,
                                              gboolean show_mod_actions)
{
    g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self));

    gtk_widget_set_visible(GTK_WIDGET(self->btn_hide), show_mod_actions);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_mute), show_mod_actions);
}

void
gnostr_chat_message_row_set_hidden(GnostrChatMessageRow *self,
                                    gboolean is_hidden)
{
    g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ROW(self));

    self->is_hidden = is_hidden;

    if (is_hidden) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "hidden-message");
        gtk_label_set_text(self->lbl_content, "[Message hidden by moderator]");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "hidden-message");
    }
}
