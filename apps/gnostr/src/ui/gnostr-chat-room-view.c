/**
 * GnostrChatRoomView - NIP-28 Public Chat Room View
 *
 * Displays a chat room with messages (via GListStore + GtkListView) and composition.
 */

#include "gnostr-chat-room-view.h"
#include "gnostr-chat-message-row.h"
#include "gnostr-avatar-cache.h"
#include "../model/gnostr-chat-message-item.h"

struct _GnostrChatRoomView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkBox *header_box;
    GtkButton *btn_back;
    GtkButton *btn_avatar;
    GtkPicture *channel_avatar;
    GtkLabel *channel_avatar_initials;
    GtkLabel *lbl_channel_name;
    GtkLabel *lbl_channel_about;
    GtkButton *btn_channel_menu;
    GtkScrolledWindow *scroller;
    GtkListView *message_list;
    GtkStack *content_stack;
    GtkSpinner *loading_spinner;
    GtkBox *composer_box;
    GtkTextView *message_entry;
    GtkButton *btn_send;
    GtkRevealer *reply_revealer;
    GtkLabel *lbl_reply_to;
    GtkButton *btn_cancel_reply;

    /* Model pipeline */
    GListStore *store;

    /* Data */
    GnostrChannel *channel;
    char *user_pubkey;
    char *reply_to_id;
    gboolean is_moderator;
    GHashTable *index;          /* message_id -> guint position */
    GHashTable *author_names;   /* pubkey -> display_name */
    GHashTable *author_avatars; /* pubkey -> avatar_url */
};

G_DEFINE_TYPE(GnostrChatRoomView, gnostr_chat_room_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_SEND_MESSAGE,
    SIGNAL_LEAVE_CHANNEL,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_EDIT_CHANNEL,
    SIGNAL_HIDE_MESSAGE,
    SIGNAL_MUTE_USER,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Index rebuild --- */

static void
rebuild_index(GnostrChatRoomView *self)
{
    g_hash_table_remove_all(self->index);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GnostrChatMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), i);
        const char *eid = gnostr_chat_message_item_get_event_id(item);
        if (eid)
            g_hash_table_insert(self->index, (gpointer)eid, GUINT_TO_POINTER(i));
    }
}

/* --- Signal callbacks --- */

static void
on_back_clicked(GtkButton *button, GnostrChatRoomView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_LEAVE_CHANNEL], 0);
}

static void
on_channel_avatar_clicked(GtkButton *button, GnostrChatRoomView *self)
{
    (void)button;
    if (self->channel && self->channel->creator_pubkey)
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->channel->creator_pubkey);
}

static void
on_send_clicked(GtkButton *button, GnostrChatRoomView *self)
{
    (void)button;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->message_entry);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (content && *content) {
        g_signal_emit(self, signals[SIGNAL_SEND_MESSAGE], 0, content, self->reply_to_id);
        gtk_text_buffer_set_text(buffer, "", 0);
        gnostr_chat_room_view_set_reply_to(self, NULL, NULL);
    }

    g_free(content);
}

static void
on_cancel_reply_clicked(GtkButton *button, GnostrChatRoomView *self)
{
    (void)button;
    gnostr_chat_room_view_set_reply_to(self, NULL, NULL);
}

static void
on_message_row_open_profile(GnostrChatMessageRow *row, const char *pubkey, gpointer user_data)
{
    (void)row;
    g_signal_emit(GNOSTR_CHAT_ROOM_VIEW(user_data), signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_message_row_reply(GnostrChatMessageRow *row, const char *message_id, gpointer user_data)
{
    GnostrChatRoomView *self = GNOSTR_CHAT_ROOM_VIEW(user_data);
    const char *author_name = gnostr_chat_message_row_get_author_name(row);
    gnostr_chat_room_view_set_reply_to(self, message_id, author_name);
    gtk_widget_grab_focus(GTK_WIDGET(self->message_entry));
}

static void
on_message_row_hide(GnostrChatMessageRow *row, const char *message_id, gpointer user_data)
{
    (void)row;
    g_signal_emit(GNOSTR_CHAT_ROOM_VIEW(user_data), signals[SIGNAL_HIDE_MESSAGE], 0, message_id);
}

static void
on_message_row_mute(GnostrChatMessageRow *row, const char *pubkey, gpointer user_data)
{
    (void)row;
    g_signal_emit(GNOSTR_CHAT_ROOM_VIEW(user_data), signals[SIGNAL_MUTE_USER], 0, pubkey);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, GnostrChatRoomView *self)
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

/* --- Factory callbacks --- */

static void
factory_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
              GtkListItem *list_item,
              gpointer user_data G_GNUC_UNUSED)
{
    GnostrChatMessageRow *row = gnostr_chat_message_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
factory_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
             GtkListItem *list_item,
             gpointer user_data)
{
    GnostrChatRoomView *self = GNOSTR_CHAT_ROOM_VIEW(user_data);
    GnostrChatMessageItem *item = gtk_list_item_get_item(list_item);
    GnostrChatMessageRow *row = GNOSTR_CHAT_MESSAGE_ROW(gtk_list_item_get_child(list_item));

    if (!item || !row)
        return;

    const GnostrChatMessage *msg = gnostr_chat_message_item_get_message(item);
    if (msg) {
        gnostr_chat_message_row_set_message(row, msg);

        /* Apply cached profile info */
        const char *name = g_hash_table_lookup(self->author_names, msg->author_pubkey);
        const char *avatar = g_hash_table_lookup(self->author_avatars, msg->author_pubkey);
        if (name || avatar)
            gnostr_chat_message_row_set_author_profile(row, name, avatar);

        /* Set ownership and moderator status */
        gboolean is_own = self->user_pubkey && g_strcmp0(self->user_pubkey, msg->author_pubkey) == 0;
        gnostr_chat_message_row_set_is_own(row, is_own);
        gnostr_chat_message_row_set_show_mod_actions(row, self->is_moderator && !is_own);
    }

    g_signal_connect(row, "open-profile", G_CALLBACK(on_message_row_open_profile), self);
    g_signal_connect(row, "reply", G_CALLBACK(on_message_row_reply), self);
    g_signal_connect(row, "hide", G_CALLBACK(on_message_row_hide), self);
    g_signal_connect(row, "mute", G_CALLBACK(on_message_row_mute), self);
}

static void
factory_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
               GtkListItem *list_item,
               gpointer user_data)
{
    GnostrChatMessageRow *row = GNOSTR_CHAT_MESSAGE_ROW(gtk_list_item_get_child(list_item));
    if (row)
        g_signal_handlers_disconnect_by_data(row, user_data);
}

/* --- GObject lifecycle --- */

static void
gnostr_chat_room_view_dispose(GObject *object)
{
    GnostrChatRoomView *self = GNOSTR_CHAT_ROOM_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    g_clear_object(&self->store);

    G_OBJECT_CLASS(gnostr_chat_room_view_parent_class)->dispose(object);
}

static void
gnostr_chat_room_view_finalize(GObject *object)
{
    GnostrChatRoomView *self = GNOSTR_CHAT_ROOM_VIEW(object);

    g_clear_pointer(&self->channel, gnostr_channel_free);
    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_pointer(&self->reply_to_id, g_free);
    g_clear_pointer(&self->index, g_hash_table_destroy);
    g_clear_pointer(&self->author_names, g_hash_table_destroy);
    g_clear_pointer(&self->author_avatars, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_chat_room_view_parent_class)->finalize(object);
}

static void
gnostr_chat_room_view_class_init(GnostrChatRoomViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_chat_room_view_dispose;
    object_class->finalize = gnostr_chat_room_view_finalize;

    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-chat-room-view.ui");

    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, header_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, btn_back);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, btn_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, channel_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, channel_avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, lbl_channel_name);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, lbl_channel_about);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, btn_channel_menu);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, message_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, composer_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, message_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, btn_send);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, reply_revealer);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, lbl_reply_to);
    gtk_widget_class_bind_template_child(widget_class, GnostrChatRoomView, btn_cancel_reply);

    signals[SIGNAL_SEND_MESSAGE] = g_signal_new(
        "send-message", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_LEAVE_CHANNEL] = g_signal_new(
        "leave-channel", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);

    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_EDIT_CHANNEL] = g_signal_new(
        "edit-channel", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);

    signals[SIGNAL_HIDE_MESSAGE] = g_signal_new(
        "hide-message", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_MUTE_USER] = g_signal_new(
        "mute-user", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    gtk_widget_class_set_css_name(widget_class, "chat-room");
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_chat_room_view_init(GnostrChatRoomView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->channel = NULL;
    self->user_pubkey = NULL;
    self->reply_to_id = NULL;
    self->is_moderator = FALSE;
    self->index = g_hash_table_new(g_str_hash, g_str_equal);
    self->author_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->author_avatars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /* Model pipeline */
    self->store = g_list_store_new(GNOSTR_TYPE_CHAT_MESSAGE_ITEM);

    GtkNoSelection *selection = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(self->store)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), self);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);

    gtk_list_view_set_model(self->message_list, GTK_SELECTION_MODEL(selection));
    gtk_list_view_set_factory(self->message_list, factory);
    g_object_unref(selection);
    g_object_unref(factory);

    /* Connect UI signals */
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_channel_avatar_clicked), self);
    g_signal_connect(self->btn_send, "clicked", G_CALLBACK(on_send_clicked), self);
    g_signal_connect(self->btn_cancel_reply, "clicked", G_CALLBACK(on_cancel_reply_clicked), self);

    /* Key press for Enter to send */
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->message_entry), key_controller);
}

/* --- Public API (preserved) --- */

GnostrChatRoomView *
gnostr_chat_room_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHAT_ROOM_VIEW, NULL);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("#");

    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

void
gnostr_chat_room_view_set_channel(GnostrChatRoomView *self,
                                   const GnostrChannel *channel)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    g_return_if_fail(channel != NULL);

    gnostr_channel_free(self->channel);
    self->channel = gnostr_channel_copy(channel);

    const char *name = channel->name;
    if (!name || !*name) {
        if (channel->channel_id && strlen(channel->channel_id) >= 8) {
            g_autofree char *truncated = g_strdup_printf("#%.8s...", channel->channel_id);
            gtk_label_set_text(self->lbl_channel_name, truncated);
        } else {
            gtk_label_set_text(self->lbl_channel_name, "Unnamed Channel");
        }
        name = channel->channel_id;
    } else {
        gtk_label_set_text(self->lbl_channel_name, channel->name);
    }

    if (channel->about && *channel->about) {
        gtk_label_set_text(self->lbl_channel_about, channel->about);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_channel_about), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_channel_about), FALSE);
    }

    char *initials = get_initials(name);
    gtk_label_set_text(self->channel_avatar_initials, initials);
    g_free(initials);

    if (channel->picture && *channel->picture) {
        gnostr_avatar_download_async(channel->picture,
                                      GTK_WIDGET(self->channel_avatar),
                                      GTK_WIDGET(self->channel_avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->channel_avatar), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->channel_avatar_initials), TRUE);
    }

    gnostr_chat_room_view_clear_messages(self);
}

const char *
gnostr_chat_room_view_get_channel_id(GnostrChatRoomView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self), NULL);
    return self->channel ? self->channel->channel_id : NULL;
}

void
gnostr_chat_room_view_add_message(GnostrChatRoomView *self,
                                   const GnostrChatMessage *msg)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    g_return_if_fail(msg != NULL);
    g_return_if_fail(msg->event_id != NULL);

    /* Check if message already exists */
    if (g_hash_table_contains(self->index, msg->event_id))
        return;

    g_autoptr(GnostrChatMessageItem) item = gnostr_chat_message_item_new(msg);
    g_list_store_append(self->store, item);
    rebuild_index(self);

    gtk_stack_set_visible_child_name(self->content_stack, "messages");
}

void
gnostr_chat_room_view_update_message(GnostrChatRoomView *self,
                                      const GnostrChatMessage *msg)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    g_return_if_fail(msg != NULL);
    g_return_if_fail(msg->event_id != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, msg->event_id, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_autoptr(GnostrChatMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), pos);
        if (item) {
            gnostr_chat_message_item_update(item, msg);
            g_list_store_remove(self->store, pos);
            g_list_store_insert(self->store, pos, item);
        }
    }
}

void
gnostr_chat_room_view_remove_message(GnostrChatRoomView *self,
                                      const char *message_id)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    g_return_if_fail(message_id != NULL);

    gpointer pos_ptr;
    if (g_hash_table_lookup_extended(self->index, message_id, NULL, &pos_ptr)) {
        guint pos = GPOINTER_TO_UINT(pos_ptr);
        g_list_store_remove(self->store, pos);
        rebuild_index(self);
    }
}

void
gnostr_chat_room_view_clear_messages(GnostrChatRoomView *self)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));

    g_list_store_remove_all(self->store);
    g_hash_table_remove_all(self->index);
}

void
gnostr_chat_room_view_set_loading(GnostrChatRoomView *self,
                                   gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
        gtk_spinner_start(self->loading_spinner);
    } else {
        gtk_spinner_stop(self->loading_spinner);
        guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
        if (n > 0)
            gtk_stack_set_visible_child_name(self->content_stack, "messages");
        else
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_chat_room_view_set_user_pubkey(GnostrChatRoomView *self,
                                       const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}

void
gnostr_chat_room_view_set_is_moderator(GnostrChatRoomView *self,
                                        gboolean is_moderator)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    self->is_moderator = is_moderator;
}

void
gnostr_chat_room_view_scroll_to_bottom(GnostrChatRoomView *self)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(self->scroller);
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

void
gnostr_chat_room_view_set_reply_to(GnostrChatRoomView *self,
                                    const char *message_id,
                                    const char *author_name)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));

    g_free(self->reply_to_id);
    self->reply_to_id = g_strdup(message_id);

    if (message_id && author_name) {
        g_autofree char *reply_text = g_strdup_printf("Replying to %s", author_name);
        gtk_label_set_text(self->lbl_reply_to, reply_text);
        gtk_revealer_set_reveal_child(self->reply_revealer, TRUE);
    } else {
        gtk_revealer_set_reveal_child(self->reply_revealer, FALSE);
    }
}

const char *
gnostr_chat_room_view_get_reply_to(GnostrChatRoomView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self), NULL);
    return self->reply_to_id;
}

void
gnostr_chat_room_view_update_author_profile(GnostrChatRoomView *self,
                                             const char *pubkey_hex,
                                             const char *display_name,
                                             const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_CHAT_ROOM_VIEW(self));
    g_return_if_fail(pubkey_hex != NULL);

    /* Cache the profile info */
    if (display_name)
        g_hash_table_insert(self->author_names, g_strdup(pubkey_hex), g_strdup(display_name));
    if (avatar_url)
        g_hash_table_insert(self->author_avatars, g_strdup(pubkey_hex), g_strdup(avatar_url));

    /* Force rebind for all messages from this author */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GnostrChatMessageItem) item = g_list_model_get_item(G_LIST_MODEL(self->store), i);
        const char *author = gnostr_chat_message_item_get_author_pubkey(item);
        if (author && g_strcmp0(author, pubkey_hex) == 0) {
            /* Remove and reinsert to trigger factory rebind */
            g_list_store_remove(self->store, i);
            g_list_store_insert(self->store, i, item);
        }
    }
    rebuild_index(self);
}
