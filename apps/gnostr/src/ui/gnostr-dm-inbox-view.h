#ifndef GNOSTR_DM_INBOX_VIEW_H
#define GNOSTR_DM_INBOX_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_DM_INBOX_VIEW (gnostr_dm_inbox_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrDmInboxView, gnostr_dm_inbox_view, GNOSTR, DM_INBOX_VIEW, GtkWidget)

/**
 * Signals:
 * "open-conversation" (gchar* peer_pubkey_hex, gpointer user_data)
 *   - Emitted when user selects a conversation
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks avatar to view profile
 * "compose-dm" (gpointer user_data)
 *   - Emitted when user clicks to compose new DM
 */

typedef struct _GnostrDmInboxView GnostrDmInboxView;

/**
 * Conversation summary for display in inbox
 */
typedef struct {
    char *peer_pubkey;      /* Peer's pubkey (hex) */
    char *display_name;     /* Peer's display name */
    char *handle;           /* Peer's handle (@user or npub...) */
    char *avatar_url;       /* URL to avatar */
    char *last_message;     /* Preview of last message */
    gint64 last_timestamp;  /* Timestamp of last message */
    guint unread_count;     /* Number of unread messages */
    gboolean is_outgoing;   /* TRUE if last message was from us */
} GnostrDmConversation;

GnostrDmInboxView *gnostr_dm_inbox_view_new(void);

/**
 * Add or update a conversation in the inbox
 * @self: the inbox view
 * @conv: conversation data (copied internally)
 */
void gnostr_dm_inbox_view_upsert_conversation(GnostrDmInboxView *self,
                                               const GnostrDmConversation *conv);

/**
 * Remove a conversation from the inbox
 * @self: the inbox view
 * @peer_pubkey: pubkey of conversation to remove
 */
void gnostr_dm_inbox_view_remove_conversation(GnostrDmInboxView *self,
                                               const char *peer_pubkey);

/**
 * Clear all conversations from the inbox
 */
void gnostr_dm_inbox_view_clear(GnostrDmInboxView *self);

/**
 * Mark a conversation as read
 * @self: the inbox view
 * @peer_pubkey: pubkey of conversation to mark read
 */
void gnostr_dm_inbox_view_mark_read(GnostrDmInboxView *self,
                                     const char *peer_pubkey);

/**
 * Set the logged-in user's pubkey (for determining outgoing messages)
 */
void gnostr_dm_inbox_view_set_user_pubkey(GnostrDmInboxView *self,
                                           const char *pubkey_hex);

/**
 * Show/hide empty state
 */
void gnostr_dm_inbox_view_set_empty(GnostrDmInboxView *self, gboolean is_empty);

/**
 * Show/hide loading state
 */
void gnostr_dm_inbox_view_set_loading(GnostrDmInboxView *self, gboolean is_loading);

/* Helper to free conversation data */
void gnostr_dm_conversation_free(GnostrDmConversation *conv);

G_END_DECLS

#endif /* GNOSTR_DM_INBOX_VIEW_H */
