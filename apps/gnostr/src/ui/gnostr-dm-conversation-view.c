/**
 * GnostrDmConversationView - NIP-17 DM Conversation Thread View
 *
 * Displays a 1-to-1 encrypted DM conversation with message bubbles
 * and a composer for sending new messages.
 */

#include "gnostr-dm-conversation-view.h"
#include "gnostr-avatar-cache.h"
#include <time.h>

struct _GnostrDmConversationView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkBox *header_box;
    GtkButton *btn_back;
    GtkButton *btn_peer_avatar;
    GtkPicture *peer_avatar_image;
    GtkLabel *peer_avatar_initials;
    GtkLabel *lbl_peer_name;
    GtkScrolledWindow *scroller;
    GtkListBox *message_list;
    GtkStack *content_stack;
    GtkSpinner *loading_spinner;
    GtkBox *composer_box;
    GtkTextView *message_entry;
    GtkButton *btn_send;

    /* Data */
    char *peer_pubkey;
    char *user_pubkey;
    guint message_count;
};

G_DEFINE_TYPE(GnostrDmConversationView, gnostr_dm_conversation_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_SEND_MESSAGE,
    SIGNAL_GO_BACK,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ---- Helpers ---- */

GnostrDmMessage *
gnostr_dm_message_copy(const GnostrDmMessage *msg)
{
    if (!msg) return NULL;
    GnostrDmMessage *copy = g_new0(GnostrDmMessage, 1);
    copy->event_id = g_strdup(msg->event_id);
    copy->content = g_strdup(msg->content);
    copy->created_at = msg->created_at;
    copy->is_outgoing = msg->is_outgoing;
    return copy;
}

void
gnostr_dm_message_free(GnostrDmMessage *msg)
{
    if (!msg) return;
    g_free(msg->event_id);
    g_free(msg->content);
    g_free(msg);
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
format_msg_time(gint64 timestamp)
{
    if (timestamp <= 0) return g_strdup("");

    GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
    if (!dt) return g_strdup("");

    GDateTime *now = g_date_time_new_now_local();
    if (!now) {
        g_date_time_unref(dt);
        return g_strdup("");
    }

    gint64 diff = g_date_time_to_unix(now) - g_date_time_to_unix(dt);

    char *result;
    if (diff < 60)
        result = g_strdup("now");
    else if (diff < 3600)
        result = g_strdup_printf("%" G_GINT64_FORMAT "m ago", diff / 60);
    else if (diff < 86400)
        result = g_date_time_format(dt, "%l:%M %p");
    else
        result = g_date_time_format(dt, "%b %d, %l:%M %p");

    g_date_time_unref(dt);
    g_date_time_unref(now);
    return result;
}

/* Create a message bubble row widget */
static GtkWidget *
create_message_row(const GnostrDmMessage *msg)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(outer, 12);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_set_margin_top(outer, 4);
    gtk_widget_set_margin_bottom(outer, 4);

    /* Message bubble */
    GtkWidget *bubble = gtk_label_new(msg->content);
    gtk_label_set_wrap(GTK_LABEL(bubble), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(bubble), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(bubble), 0.0);
    gtk_label_set_selectable(GTK_LABEL(bubble), TRUE);
    gtk_widget_set_margin_start(bubble, 8);
    gtk_widget_set_margin_end(bubble, 8);
    gtk_widget_set_margin_top(bubble, 6);
    gtk_widget_set_margin_bottom(bubble, 6);

    GtkWidget *bubble_frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(bubble_frame), bubble);
    gtk_widget_set_hexpand(bubble_frame, FALSE);

    if (msg->is_outgoing) {
        gtk_widget_set_halign(bubble_frame, GTK_ALIGN_END);
        gtk_widget_add_css_class(bubble_frame, "dm-bubble-outgoing");
        gtk_widget_set_halign(outer, GTK_ALIGN_END);
    } else {
        gtk_widget_set_halign(bubble_frame, GTK_ALIGN_START);
        gtk_widget_add_css_class(bubble_frame, "dm-bubble-incoming");
        gtk_widget_set_halign(outer, GTK_ALIGN_START);
    }

    /* Constrain bubble width to ~70% */
    gtk_widget_set_size_request(bubble_frame, -1, -1);
    gtk_label_set_max_width_chars(GTK_LABEL(bubble), 50);

    gtk_box_append(GTK_BOX(outer), bubble_frame);

    /* Timestamp */
    char *time_str = format_msg_time(msg->created_at);
    GtkWidget *time_label = gtk_label_new(time_str);
    g_free(time_str);
    gtk_widget_add_css_class(time_label, "dim-label");
    gtk_widget_add_css_class(time_label, "caption");

    if (msg->is_outgoing) {
        gtk_widget_set_halign(time_label, GTK_ALIGN_END);
    } else {
        gtk_widget_set_halign(time_label, GTK_ALIGN_START);
    }

    gtk_box_append(GTK_BOX(outer), time_label);

    return outer;
}

/* ---- Signal handlers ---- */

static void
on_back_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_GO_BACK], 0);
}

static void
on_peer_avatar_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;
    if (self->peer_pubkey)
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->peer_pubkey);
}

static void
on_send_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->message_entry);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (content && *content) {
        g_signal_emit(self, signals[SIGNAL_SEND_MESSAGE], 0, content);
        gtk_text_buffer_set_text(buffer, "", 0);
    }

    g_free(content);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, GnostrDmConversationView *self)
{
    (void)controller;
    (void)keycode;

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (!(state & GDK_SHIFT_MASK)) {
            on_send_clicked(NULL, self);
            return TRUE;
        }
    }

    return FALSE;
}

/* ---- GObject lifecycle ---- */

static void
gnostr_dm_conversation_view_dispose(GObject *object)
{
    GnostrDmConversationView *self = GNOSTR_DM_CONVERSATION_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_dm_conversation_view_parent_class)->dispose(object);
}

static void
gnostr_dm_conversation_view_finalize(GObject *object)
{
    GnostrDmConversationView *self = GNOSTR_DM_CONVERSATION_VIEW(object);

    g_free(self->peer_pubkey);
    g_free(self->user_pubkey);

    G_OBJECT_CLASS(gnostr_dm_conversation_view_parent_class)->finalize(object);
}

static void
gnostr_dm_conversation_view_class_init(GnostrDmConversationViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_dm_conversation_view_dispose;
    object_class->finalize = gnostr_dm_conversation_view_finalize;

    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-dm-conversation-view.ui");

    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, header_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_back);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_peer_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, peer_avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, peer_avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, lbl_peer_name);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, message_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, composer_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, message_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_send);

    signals[SIGNAL_SEND_MESSAGE] = g_signal_new(
        "send-message",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_GO_BACK] = g_signal_new(
        "go-back",
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

    gtk_widget_class_set_css_name(widget_class, "dm-conversation");
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_dm_conversation_view_init(GnostrDmConversationView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->peer_pubkey = NULL;
    self->user_pubkey = NULL;
    self->message_count = 0;

    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->btn_peer_avatar, "clicked", G_CALLBACK(on_peer_avatar_clicked), self);
    g_signal_connect(self->btn_send, "clicked", G_CALLBACK(on_send_clicked), self);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->message_entry), key_controller);

    gtk_list_box_set_selection_mode(self->message_list, GTK_SELECTION_NONE);

    /* Start with empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

/* ---- Public API ---- */

GnostrDmConversationView *
gnostr_dm_conversation_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_CONVERSATION_VIEW, NULL);
}

void
gnostr_dm_conversation_view_set_peer(GnostrDmConversationView *self,
                                      const char *pubkey_hex,
                                      const char *display_name,
                                      const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    g_free(self->peer_pubkey);
    self->peer_pubkey = g_strdup(pubkey_hex);

    /* Set display name */
    const char *name = display_name;
    if (!name || !*name) {
        /* Truncate pubkey as fallback name */
        if (pubkey_hex && strlen(pubkey_hex) >= 12) {
            char npub_short[16];
            snprintf(npub_short, sizeof(npub_short), "%.8s...", pubkey_hex);
            gtk_label_set_text(self->lbl_peer_name, npub_short);
        } else {
            gtk_label_set_text(self->lbl_peer_name, pubkey_hex ? pubkey_hex : "Unknown");
        }
    } else {
        gtk_label_set_text(self->lbl_peer_name, name);
    }

    /* Set initials */
    char *initials = get_initials(name);
    gtk_label_set_text(self->peer_avatar_initials, initials);
    g_free(initials);

    /* Load avatar if available */
    if (avatar_url && *avatar_url) {
        gnostr_avatar_download_async(avatar_url,
                                     GTK_WIDGET(self->peer_avatar_image),
                                     GTK_WIDGET(self->peer_avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->peer_avatar_image), FALSE);
    }
}

const char *
gnostr_dm_conversation_view_get_peer_pubkey(GnostrDmConversationView *self)
{
    g_return_val_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self), NULL);
    return self->peer_pubkey;
}

void
gnostr_dm_conversation_view_set_user_pubkey(GnostrDmConversationView *self,
                                             const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}

void
gnostr_dm_conversation_view_add_message(GnostrDmConversationView *self,
                                         const GnostrDmMessage *msg)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));
    g_return_if_fail(msg != NULL);

    GtkWidget *row = create_message_row(msg);
    gtk_list_box_append(self->message_list, row);
    self->message_count++;

    if (self->message_count == 1) {
        gtk_stack_set_visible_child_name(self->content_stack, "messages");
    }
}

static gint
compare_messages_by_time(gconstpointer a, gconstpointer b)
{
    const GnostrDmMessage *ma = *(const GnostrDmMessage **)a;
    const GnostrDmMessage *mb = *(const GnostrDmMessage **)b;
    if (ma->created_at < mb->created_at) return -1;
    if (ma->created_at > mb->created_at) return 1;
    return 0;
}

void
gnostr_dm_conversation_view_set_messages(GnostrDmConversationView *self,
                                          GPtrArray *messages)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    /* Clear existing */
    gnostr_dm_conversation_view_clear(self);

    if (!messages || messages->len == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
        return;
    }

    /* Sort by timestamp */
    g_ptr_array_sort(messages, compare_messages_by_time);

    /* Add all messages */
    for (guint i = 0; i < messages->len; i++) {
        GnostrDmMessage *msg = g_ptr_array_index(messages, i);
        GtkWidget *row = create_message_row(msg);
        gtk_list_box_append(self->message_list, row);
        self->message_count++;
    }

    gtk_stack_set_visible_child_name(self->content_stack, "messages");
}

void
gnostr_dm_conversation_view_clear(GnostrDmConversationView *self)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    /* Remove all rows from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->message_list))) != NULL) {
        gtk_list_box_remove(self->message_list, child);
    }
    self->message_count = 0;

    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

void
gnostr_dm_conversation_view_set_loading(GnostrDmConversationView *self,
                                         gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    if (is_loading) {
        gtk_spinner_set_spinning(self->loading_spinner, TRUE);
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
    } else {
        gtk_spinner_set_spinning(self->loading_spinner, FALSE);
        if (self->message_count > 0)
            gtk_stack_set_visible_child_name(self->content_stack, "messages");
        else
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_dm_conversation_view_scroll_to_bottom(GnostrDmConversationView *self)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(self->scroller);
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}
