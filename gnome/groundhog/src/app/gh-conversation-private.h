#ifndef GH_CONVERSATION_PRIVATE_H
#define GH_CONVERSATION_PRIVATE_H

#include "gh-conversation.h"

G_BEGIN_DECLS

/* Store-only: the conversation of message's room for its account. */
GhConversation *gh_conversation_new_for_message(GhMessage *message);
/* Store-only: inserts message in order. FALSE (nothing changed) when its
 * rumor id is already present or it belongs to another room or account. */
gboolean gh_conversation_insert(GhConversation *self, GhMessage *message);

G_END_DECLS
#endif
