#ifndef GH_CONVERSATION_ACTIONS_H
#define GH_CONVERSATION_ACTIONS_H

#include "gh-notifier.h"
#include "gh-store-conversations.h"

G_BEGIN_DECLS

/*
 * The local-only actions on a private conversation that change what
 * Groundhog notifies (privacy charter §3.8, §5.2 N3/N8, §7.9; P8; G19): each
 * changes the account's encrypted store (the room's row, never GSettings)
 * and then withdraws whatever the notifier shows for the room. Nothing is
 * published and nobody is told. Conversation Info uses them; so can a
 * conversation row's menu or Message Requests. notifier may be NULL (no
 * notifications in this process). Errors are the store's.
 */

/* Mutes @room_id until @muted_until (unix seconds, or
 * GH_STORE_CONVERSATIONS_MUTED_ALWAYS); 0 unmutes (nothing to withdraw). */
gboolean gh_conversation_actions_mute(GhStoreConversations *conversations, GhNotifier *notifier,
                                      const gchar *room_id, gint64 muted_until, GError **error);
/* Blocks (gh_store_conversations_set_blocked()) and withdraws; unblocking
 * lists the room again and withdraws nothing. */
gboolean gh_conversation_actions_block(GhStoreConversations *conversations, GhNotifier *notifier,
                                       const gchar *room_id, gboolean blocked, GError **error);
/* Forget on this device (gh_store_conversations_forget(), ST-9) and
 * withdraw. */
gboolean gh_conversation_actions_forget(GhStoreConversations *conversations,
                                        GhNotifier *notifier, const gchar *room_id,
                                        GError **error);

G_END_DECLS
#endif
