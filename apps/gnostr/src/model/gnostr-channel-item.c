/*
 * gnostr-channel-item.c - GObject wrapper for GnostrChannel
 */

#include "gnostr-channel-item.h"

struct _GnostrChannelItem {
  GObject parent_instance;
  GnostrChannel *channel;
};

G_DEFINE_FINAL_TYPE(GnostrChannelItem, gnostr_channel_item, G_TYPE_OBJECT)

static void
gnostr_channel_item_finalize(GObject *obj)
{
  GnostrChannelItem *self = GNOSTR_CHANNEL_ITEM(obj);
  g_clear_pointer(&self->channel, gnostr_channel_free);
  G_OBJECT_CLASS(gnostr_channel_item_parent_class)->finalize(obj);
}

static void
gnostr_channel_item_class_init(GnostrChannelItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_channel_item_finalize;
}

static void
gnostr_channel_item_init(GnostrChannelItem *self)
{
  self->channel = NULL;
}

GnostrChannelItem *
gnostr_channel_item_new(const GnostrChannel *channel)
{
  GnostrChannelItem *self = g_object_new(GNOSTR_TYPE_CHANNEL_ITEM, NULL);
  if (channel)
    self->channel = gnostr_channel_copy(channel);
  return self;
}

const GnostrChannel *
gnostr_channel_item_get_channel(GnostrChannelItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHANNEL_ITEM(self), NULL);
  return self->channel;
}

const char *
gnostr_channel_item_get_channel_id(GnostrChannelItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHANNEL_ITEM(self), NULL);
  return self->channel ? self->channel->channel_id : NULL;
}

const char *
gnostr_channel_item_get_name(GnostrChannelItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHANNEL_ITEM(self), NULL);
  return self->channel ? self->channel->name : NULL;
}

const char *
gnostr_channel_item_get_about(GnostrChannelItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_CHANNEL_ITEM(self), NULL);
  return self->channel ? self->channel->about : NULL;
}

void
gnostr_channel_item_update(GnostrChannelItem *self, const GnostrChannel *channel)
{
  g_return_if_fail(GNOSTR_IS_CHANNEL_ITEM(self));
  g_return_if_fail(channel != NULL);

  g_clear_pointer(&self->channel, gnostr_channel_free);
  self->channel = gnostr_channel_copy(channel);
}
