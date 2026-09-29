#ifndef GH_BLOCKED_CONVERSATIONS_H
#define GH_BLOCKED_CONVERSATIONS_H

#include "gh-blocked-page.h"
#include "gh-store-conversations.h"

G_BEGIN_DECLS

/* Preferences' Blocked Conversations over an account's encrypted store
 * (nostrc-qp24.72). The blocks live only there: nothing is published. */

/* gh_store_conversations_list_blocked() as GhBlockedConversation (the
 * people other than account_pubkey, as npubs), newest first. */
GPtrArray *gh_blocked_conversations_list(GhStoreConversations *conversations,
                                         const gchar *account_pubkey, GError **error);
/* gh_conversation_actions_block(..., FALSE): the room is listed again with
 * its kept history, as a message request unless the account wrote in it. */
gboolean gh_blocked_conversations_unblock(GhStoreConversations *conversations,
                                          const gchar *room_id, GError **error);

G_END_DECLS
#endif
