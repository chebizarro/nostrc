/*
 * gnostr-chat-message-item.c - GObject wrapper for GnostrChatMessage
 */

#include "gnostr-chat-message-item.h"

struct _GnostrChatMessageItem {
  GObject parent_instance;
  GnostrChatMessage *msg;
};

G_DEFINE_FINAL_TYPE(GnostrChatMessageItem, gnostr_chat_message_item, G_TYPE_OBJECT)

static void
gnostr_chat_message_item_finalize(GObject *obj)
{
  GnostrChatMessageItem *self = GNOSTR_CHAT_MESSAGE_ITEM(obj);
  g_clear_pointer(&self->msg, gnostr_chat_message_free);
  G_OBJECT_CLASS(gnostr_chat_message_item_parent_class)->finalize(obj);
}

static void
gnostr_chat_message_item_class_init(GnostrChatMessageItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gnostr_chat_message_item_finalize;
}

static void
gnostr_chat_message_item_init(GnostrChatMessageItem *self)
{
  self->msg = NULL;
}

GnostrChatMessageItem *
gnostr_chat_message_item_new(const GnostrChatMessage *msg)
{
  GnostrChatMessageItem *self = g_object_new(GNOSTR_TYPE_CHAT_MESSAGE_ITEM, NULL);
  if (msg)
    self->msg = gnostr_chat_message_copy(msg);
  return self;
}

const GnostrChatMessage *
gnostr_chat_message_item_get_message(GnostrChatMessageItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ITEM(self), NULL);
  return self->msg;
}

const char *
gnostr_chat_message_item_get_event_id(GnostrChatMessageItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ITEM(self), NULL);
  return self->msg ? self->msg->event_id : NULL;
}

const char *
gnostr_chat_message_item_get_author_pubkey(GnostrChatMessageItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHAT_MESSAGE_ITEM(self), NULL);
  return self->msg ? self->msg->author_pubkey : NULL;
}

void
gnostr_chat_message_item_update(GnostrChatMessageItem *self, const GnostrChatMessage *msg)
{
  g_return_if_fail(GNOSTR_IS_CHAT_MESSAGE_ITEM(self));
  g_return_if_fail(msg != NULL);

  g_clear_pointer(&self->msg, gnostr_chat_message_free);
  self->msg = gnostr_chat_message_copy(msg);
}
