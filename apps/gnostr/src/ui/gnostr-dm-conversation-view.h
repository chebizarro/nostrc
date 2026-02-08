/**
 * GnostrDmConversationView - NIP-17 DM Conversation Thread View
 *
 * Displays a 1-to-1 encrypted DM conversation with message bubbles
 * and a composer for sending new messages.
 */

#ifndef GNOSTR_DM_CONVERSATION_VIEW_H
#define GNOSTR_DM_CONVERSATION_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_DM_CONVERSATION_VIEW (gnostr_dm_conversation_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrDmConversationView, gnostr_dm_conversation_view,
                     GNOSTR, DM_CONVERSATION_VIEW, GtkWidget)

/**
 * Signals:
 * "send-message" (gchar* content, gpointer user_data)
 *   - Emitted when user sends a message
 * "go-back" (gpointer user_data)
 *   - Emitted when user clicks back button
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on peer avatar
 */

/**
 * GnostrDmMessage:
 *
 * A single DM message for display in the conversation view.
 */
typedef struct {
    char *event_id;         /* Unique event ID (hex), may be NULL for pending */
    char *content;          /* Message text (plaintext) */
    gint64 created_at;      /* Unix timestamp */
    gboolean is_outgoing;   /* TRUE if sent by us */
} GnostrDmMessage;

GnostrDmMessage *gnostr_dm_message_copy(const GnostrDmMessage *msg);
void             gnostr_dm_message_free(GnostrDmMessage *msg);

GnostrDmConversationView *gnostr_dm_conversation_view_new(void);

/**
 * Set the peer for this conversation.
 */
void gnostr_dm_conversation_view_set_peer(GnostrDmConversationView *self,
                                           const char *pubkey_hex,
                                           const char *display_name,
                                           const char *avatar_url);

/**
 * Get the current peer pubkey (hex).
 */
const char *gnostr_dm_conversation_view_get_peer_pubkey(GnostrDmConversationView *self);

/**
 * Set the current user's pubkey (for message direction).
 */
void gnostr_dm_conversation_view_set_user_pubkey(GnostrDmConversationView *self,
                                                  const char *pubkey_hex);

/**
 * Add a single message to the view.
 */
void gnostr_dm_conversation_view_add_message(GnostrDmConversationView *self,
                                              const GnostrDmMessage *msg);

/**
 * Bulk-set messages (replaces existing), sorted by timestamp.
 */
void gnostr_dm_conversation_view_set_messages(GnostrDmConversationView *self,
                                               GPtrArray *messages);

/**
 * Clear all messages.
 */
void gnostr_dm_conversation_view_clear(GnostrDmConversationView *self);

/**
 * Show/hide loading state.
 */
void gnostr_dm_conversation_view_set_loading(GnostrDmConversationView *self,
                                              gboolean is_loading);

/**
 * Scroll to the bottom of the message list.
 */
void gnostr_dm_conversation_view_scroll_to_bottom(GnostrDmConversationView *self);

G_END_DECLS

#endif /* GNOSTR_DM_CONVERSATION_VIEW_H */
