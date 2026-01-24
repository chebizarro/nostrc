/**
 * GnostrChatMessageRow - A row widget for displaying a chat message
 *
 * Shows author avatar, name, timestamp, message content, and action buttons.
 */

#ifndef GNOSTR_CHAT_MESSAGE_ROW_H
#define GNOSTR_CHAT_MESSAGE_ROW_H

#include <gtk/gtk.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHAT_MESSAGE_ROW (gnostr_chat_message_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrChatMessageRow, gnostr_chat_message_row, GNOSTR, CHAT_MESSAGE_ROW, GtkWidget)

/**
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on the author avatar
 * "reply" (gchar* message_id, gpointer user_data)
 *   - Emitted when user clicks reply button
 * "hide" (gchar* message_id, gpointer user_data)
 *   - Emitted when moderator clicks hide button
 * "mute" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when moderator clicks mute button
 */

typedef struct _GnostrChatMessageRow GnostrChatMessageRow;

/**
 * Create a new chat message row widget
 */
GnostrChatMessageRow *gnostr_chat_message_row_new(void);

/**
 * Set the message data for this row
 * @self: the row widget
 * @msg: message data (copied internally)
 */
void gnostr_chat_message_row_set_message(GnostrChatMessageRow *self,
                                          const GnostrChatMessage *msg);

/**
 * Get the message event ID
 */
const char *gnostr_chat_message_row_get_message_id(GnostrChatMessageRow *self);

/**
 * Get the author's pubkey
 */
const char *gnostr_chat_message_row_get_author_pubkey(GnostrChatMessageRow *self);

/**
 * Get the author's display name
 */
const char *gnostr_chat_message_row_get_author_name(GnostrChatMessageRow *self);

/**
 * Update the author's profile information
 * @self: the row widget
 * @display_name: author's display name (nullable)
 * @avatar_url: author's avatar URL (nullable)
 */
void gnostr_chat_message_row_set_author_profile(GnostrChatMessageRow *self,
                                                 const char *display_name,
                                                 const char *avatar_url);

/**
 * Set whether this is the current user's own message
 * (affects styling and action visibility)
 */
void gnostr_chat_message_row_set_is_own(GnostrChatMessageRow *self,
                                         gboolean is_own);

/**
 * Set whether to show moderator actions (hide/mute)
 */
void gnostr_chat_message_row_set_show_mod_actions(GnostrChatMessageRow *self,
                                                   gboolean show_mod_actions);

/**
 * Set the hidden state (dimmed appearance)
 */
void gnostr_chat_message_row_set_hidden(GnostrChatMessageRow *self,
                                         gboolean is_hidden);

G_END_DECLS

#endif /* GNOSTR_CHAT_MESSAGE_ROW_H */
