#define G_LOG_DOMAIN "gnostr-main-window-dm-ui"

#include "gnostr-main-window-private.h"

#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "gnostr-dm-service.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>

#include <string.h>

void
gnostr_main_window_navigate_to_dm_conversation_internal(GnostrMainWindow *self,
                                                        const char *peer_pubkey)
{
    g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
    g_return_if_fail(peer_pubkey != NULL);

    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    GtkWidget *dm_conv = gnostr_session_view_get_dm_conversation(self->session_view);
    if (!dm_stack || !dm_conv || !GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv))
        return;

    GnostrDmConversationView *conv_view = GNOSTR_DM_CONVERSATION_VIEW(dm_conv);

    const char *display_name = NULL;
    const char *avatar_url = NULL;
    GnostrProfileMeta *meta = gnostr_profile_provider_get(peer_pubkey);
    if (meta) {
        display_name = meta->display_name;
        avatar_url = meta->picture;
    }

    gnostr_dm_conversation_view_set_peer(conv_view, peer_pubkey,
                                         display_name, avatar_url);

    if (self->user_pubkey_hex) {
        gnostr_dm_conversation_view_set_user_pubkey(conv_view, self->user_pubkey_hex);
    }

    gnostr_dm_conversation_view_set_loading(conv_view, TRUE);

    GPtrArray *messages = gnostr_dm_service_get_messages(self->dm_service, peer_pubkey);
    if (messages && messages->len > 0) {
        gnostr_dm_conversation_view_set_messages(conv_view, messages);
        gnostr_dm_conversation_view_set_loading(conv_view, FALSE);
        gnostr_dm_conversation_view_scroll_to_bottom(conv_view);
    } else {
        gnostr_dm_conversation_view_clear(conv_view);
        gnostr_dm_conversation_view_set_loading(conv_view, FALSE);
    }

    if (meta) gnostr_profile_meta_free(meta);

    gnostr_dm_service_mark_read(self->dm_service, peer_pubkey);
    gtk_stack_set_visible_child_name(dm_stack, "conversation");
}

static void
on_dm_inbox_open_conversation(GnostrDmInboxView *inbox,
                              const char *peer_pubkey,
                              gpointer user_data)
{
    (void)inbox;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !peer_pubkey) return;

    g_message("[DM] Opening conversation with %.8s", peer_pubkey);
    gnostr_main_window_navigate_to_dm_conversation_internal(self, peer_pubkey);
}

static void
on_dm_inbox_compose(GnostrDmInboxView *inbox, gpointer user_data)
{
    (void)inbox;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
        gnostr_session_view_show_toast(self->session_view,
                                       "Compose DM: Enter an npub or pubkey to start a conversation");
    }
}

static void
on_dm_conversation_go_back(GnostrDmConversationView *view, gpointer user_data)
{
    (void)view;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    if (dm_stack) {
        gtk_stack_set_visible_child_name(dm_stack, "inbox");
    }
}

static void
on_dm_send_complete(GnostrDmSendResult *result, gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self)) {
        gnostr_dm_send_result_free(result);
        return;
    }

    if (!result->success) {
        g_warning("[DM] Send failed: %s", result->error_message ? result->error_message : "unknown");
        if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
            gnostr_session_view_show_toast(self->session_view, "Failed to send message");
        }
    } else {
        g_message("[DM] Message sent to %u relays", result->relays_published);
    }

    gnostr_dm_send_result_free(result);
}

static void
on_dm_conversation_send_message(GnostrDmConversationView *view,
                                const char *content,
                                gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !content || !*content) return;

    const char *peer_pubkey = gnostr_dm_conversation_view_get_peer_pubkey(view);
    if (!peer_pubkey) return;

    g_message("[DM] Sending message to %.8s", peer_pubkey);

    GnostrDmMessage msg = {
        .event_id = NULL,
        .content = (char *)content,
        .created_at = (gint64)(g_get_real_time() / 1000000),
        .is_outgoing = TRUE,
    };
    gnostr_dm_conversation_view_add_message(view, &msg);
    gnostr_dm_conversation_view_scroll_to_bottom(view);

    gnostr_dm_service_send_dm_async(self->dm_service,
                                    peer_pubkey,
                                    content,
                                    NULL,
                                    on_dm_send_complete,
                                    self);
}

static void
on_dm_conversation_send_file(GnostrDmConversationView *view,
                             const char *file_path,
                             gpointer user_data)
{
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !file_path || !*file_path) return;

    const char *peer_pubkey = gnostr_dm_conversation_view_get_peer_pubkey(view);
    if (!peer_pubkey) return;

    g_message("[DM] Sending file to %.8s: %s", peer_pubkey, file_path);

    char *basename = g_path_get_basename(file_path);
    g_autofree char *preview = g_strdup_printf("Sending %s...", basename);
    GnostrDmMessage msg = {
        .event_id = NULL,
        .content = preview,
        .created_at = (gint64)(g_get_real_time() / 1000000),
        .is_outgoing = TRUE,
    };
    gnostr_dm_conversation_view_add_message(view, &msg);
    gnostr_dm_conversation_view_scroll_to_bottom(view);
    g_free(basename);

    gnostr_dm_service_send_file_async(self->dm_service,
                                      peer_pubkey,
                                      file_path,
                                      NULL,
                                      on_dm_send_complete,
                                      self);
}

static void
on_dm_conversation_open_profile(GnostrDmConversationView *view,
                                const char *pubkey_hex,
                                gpointer user_data)
{
    (void)view;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex) return;
    gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

static void
on_dm_service_message_received(GnostrDmService *service,
                               const char *peer_pubkey,
                               GnostrDmMessage *msg,
                               gpointer user_data)
{
    (void)service;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
    if (!GNOSTR_IS_MAIN_WINDOW(self) || !peer_pubkey || !msg) return;

    if (!self->session_view || !GNOSTR_IS_SESSION_VIEW(self->session_view))
        return;

    GtkStack *dm_stack = gnostr_session_view_get_dm_stack(self->session_view);
    GtkWidget *dm_conv = gnostr_session_view_get_dm_conversation(self->session_view);
    if (!dm_stack || !dm_conv || !GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv))
        return;

    GnostrDmConversationView *conv_view = GNOSTR_DM_CONVERSATION_VIEW(dm_conv);
    const char *current_peer = gnostr_dm_conversation_view_get_peer_pubkey(conv_view);
    const char *visible_child = gtk_stack_get_visible_child_name(dm_stack);
    if (current_peer && visible_child &&
        strcmp(current_peer, peer_pubkey) == 0 &&
        strcmp(visible_child, "conversation") == 0) {
        gnostr_dm_conversation_view_add_message(conv_view, msg);
        gnostr_dm_conversation_view_scroll_to_bottom(conv_view);
    }
}

void
gnostr_main_window_connect_dm_handlers_internal(GnostrMainWindow *self)
{
    g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

    GtkWidget *dm_inbox = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                            ? gnostr_session_view_get_dm_inbox(self->session_view)
                            : NULL;
    if (dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(dm_inbox)) {
        if (self->dm_inbox_open_handler_id == 0) {
            self->dm_inbox_open_handler_id = g_signal_connect(dm_inbox, "open-conversation",
                                                             G_CALLBACK(on_dm_inbox_open_conversation), self);
        }
        if (self->dm_inbox_compose_handler_id == 0) {
            self->dm_inbox_compose_handler_id = g_signal_connect(dm_inbox, "compose-dm",
                                                                 G_CALLBACK(on_dm_inbox_compose), self);
        }
    }

    GtkWidget *dm_conv = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                           ? gnostr_session_view_get_dm_conversation(self->session_view)
                           : NULL;
    if (dm_conv && GNOSTR_IS_DM_CONVERSATION_VIEW(dm_conv)) {
        if (self->dm_conv_back_handler_id == 0) {
            self->dm_conv_back_handler_id = g_signal_connect(dm_conv, "go-back",
                                                            G_CALLBACK(on_dm_conversation_go_back), self);
        }
        if (self->dm_conv_send_handler_id == 0) {
            self->dm_conv_send_handler_id = g_signal_connect(dm_conv, "send-message",
                                                            G_CALLBACK(on_dm_conversation_send_message), self);
        }
        if (self->dm_conv_send_file_handler_id == 0) {
            self->dm_conv_send_file_handler_id = g_signal_connect(dm_conv, "send-file",
                                                                 G_CALLBACK(on_dm_conversation_send_file), self);
        }
        if (self->dm_conv_open_profile_handler_id == 0) {
            self->dm_conv_open_profile_handler_id = g_signal_connect(dm_conv, "open-profile",
                                                                    G_CALLBACK(on_dm_conversation_open_profile), self);
        }
    }

    if (self->dm_service && self->dm_service_message_handler_id == 0) {
        self->dm_service_message_handler_id = g_signal_connect(self->dm_service, "message-received",
                                                              G_CALLBACK(on_dm_service_message_received), self);
    }
}
