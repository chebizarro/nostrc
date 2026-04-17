/*
 * gnostr-chat-message-item.h - GObject wrapper for GnostrChatMessage
 *
 * Wraps a NIP-28 chat message as a GObject for GListStore use.
 */

#ifndef GNOSTR_CHAT_MESSAGE_ITEM_H
#define GNOSTR_CHAT_MESSAGE_ITEM_H

#include <glib-object.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHAT_MESSAGE_ITEM (gnostr_chat_message_item_get_type())

G_DECLARE_FINAL_TYPE(GnostrChatMessageItem, gnostr_chat_message_item, GNOSTR, CHAT_MESSAGE_ITEM, GObject)

GnostrChatMessageItem *gnostr_chat_message_item_new(const GnostrChatMessage *msg);
const GnostrChatMessage *gnostr_chat_message_item_get_message(GnostrChatMessageItem *self);
const char *gnostr_chat_message_item_get_event_id(GnostrChatMessageItem *self);
const char *gnostr_chat_message_item_get_author_pubkey(GnostrChatMessageItem *self);
void gnostr_chat_message_item_update(GnostrChatMessageItem *self, const GnostrChatMessage *msg);

G_END_DECLS

#endif /* GNOSTR_CHAT_MESSAGE_ITEM_H */
