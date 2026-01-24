/**
 * GnostrChatRoomView - NIP-28 Public Chat Room View
 *
 * Displays a chat room with messages, a message composer, and channel info header.
 * Supports real-time message updates and reply threading.
 */

#ifndef GNOSTR_CHAT_ROOM_VIEW_H
#define GNOSTR_CHAT_ROOM_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHAT_ROOM_VIEW (gnostr_chat_room_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrChatRoomView, gnostr_chat_room_view, GNOSTR, CHAT_ROOM_VIEW, GtkWidget)

/**
 * Signals:
 * "send-message" (gchar* content, gchar* reply_to_id, gpointer user_data)
 *   - Emitted when user sends a message
 * "leave-channel" (gpointer user_data)
 *   - Emitted when user clicks back/leave
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on a profile avatar
 * "edit-channel" (gpointer user_data)
 *   - Emitted when channel owner clicks to edit metadata
 * "hide-message" (gchar* message_id, gpointer user_data)
 *   - Emitted when moderator hides a message (kind-43)
 * "mute-user" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when moderator mutes a user (kind-44)
 */

typedef struct _GnostrChatRoomView GnostrChatRoomView;

/**
 * Create a new chat room view
 */
GnostrChatRoomView *gnostr_chat_room_view_new(void);

/**
 * Set the channel for this chat room
 * @self: the chat room view
 * @channel: channel data (copied internally)
 */
void gnostr_chat_room_view_set_channel(GnostrChatRoomView *self,
                                        const GnostrChannel *channel);

/**
 * Get the current channel ID
 */
const char *gnostr_chat_room_view_get_channel_id(GnostrChatRoomView *self);

/**
 * Add a message to the chat room
 * @self: the chat room view
 * @msg: message data (copied internally)
 */
void gnostr_chat_room_view_add_message(GnostrChatRoomView *self,
                                        const GnostrChatMessage *msg);

/**
 * Update an existing message (e.g., mark as hidden)
 * @self: the chat room view
 * @msg: updated message data
 */
void gnostr_chat_room_view_update_message(GnostrChatRoomView *self,
                                           const GnostrChatMessage *msg);

/**
 * Remove a message from display
 * @self: the chat room view
 * @message_id: ID of message to remove
 */
void gnostr_chat_room_view_remove_message(GnostrChatRoomView *self,
                                           const char *message_id);

/**
 * Clear all messages
 */
void gnostr_chat_room_view_clear_messages(GnostrChatRoomView *self);

/**
 * Set the loading state
 * @self: the chat room view
 * @is_loading: TRUE to show loading indicator
 */
void gnostr_chat_room_view_set_loading(GnostrChatRoomView *self,
                                        gboolean is_loading);

/**
 * Set the current user's pubkey (for determining message ownership)
 */
void gnostr_chat_room_view_set_user_pubkey(GnostrChatRoomView *self,
                                            const char *pubkey_hex);

/**
 * Set whether the current user is a channel moderator/owner
 */
void gnostr_chat_room_view_set_is_moderator(GnostrChatRoomView *self,
                                             gboolean is_moderator);

/**
 * Scroll to the bottom of the message list
 */
void gnostr_chat_room_view_scroll_to_bottom(GnostrChatRoomView *self);

/**
 * Set reply mode - show reply indicator and track reply target
 * @self: the chat room view
 * @message_id: ID of message being replied to, or NULL to clear
 * @author_name: display name of the author being replied to
 */
void gnostr_chat_room_view_set_reply_to(GnostrChatRoomView *self,
                                         const char *message_id,
                                         const char *author_name);

/**
 * Get the current reply target ID
 */
const char *gnostr_chat_room_view_get_reply_to(GnostrChatRoomView *self);

/**
 * Update profile info for messages from a specific author
 * @self: the chat room view
 * @pubkey_hex: author's pubkey
 * @display_name: author's display name
 * @avatar_url: author's avatar URL
 */
void gnostr_chat_room_view_update_author_profile(GnostrChatRoomView *self,
                                                  const char *pubkey_hex,
                                                  const char *display_name,
                                                  const char *avatar_url);

G_END_DECLS

#endif /* GNOSTR_CHAT_ROOM_VIEW_H */
