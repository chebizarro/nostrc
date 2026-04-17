/*
 * gnostr-channel-item.h - GObject wrapper for GnostrChannel
 *
 * Wraps a NIP-28 GnostrChannel struct as a GObject so it can be stored
 * in a GListStore and used with GtkListView.
 */

#ifndef GNOSTR_CHANNEL_ITEM_H
#define GNOSTR_CHANNEL_ITEM_H

#include <glib-object.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHANNEL_ITEM (gnostr_channel_item_get_type())

G_DECLARE_FINAL_TYPE(GnostrChannelItem, gnostr_channel_item, GNOSTR, CHANNEL_ITEM, GObject)

/**
 * gnostr_channel_item_new:
 * @channel: Channel data to wrap (deep copied internally).
 *
 * Returns: (transfer full): A new GnostrChannelItem.
 */
GnostrChannelItem *gnostr_channel_item_new(const GnostrChannel *channel);

/**
 * gnostr_channel_item_get_channel:
 * @self: A channel item.
 *
 * Returns: (transfer none): The underlying channel data.
 */
const GnostrChannel *gnostr_channel_item_get_channel(GnostrChannelItem *self);

/**
 * gnostr_channel_item_get_channel_id:
 * @self: A channel item.
 *
 * Returns: (transfer none) (nullable): The channel ID.
 */
const char *gnostr_channel_item_get_channel_id(GnostrChannelItem *self);

/**
 * gnostr_channel_item_get_name:
 * @self: A channel item.
 *
 * Returns: (transfer none) (nullable): The channel name.
 */
const char *gnostr_channel_item_get_name(GnostrChannelItem *self);

/**
 * gnostr_channel_item_get_about:
 * @self: A channel item.
 *
 * Returns: (transfer none) (nullable): The channel description.
 */
const char *gnostr_channel_item_get_about(GnostrChannelItem *self);

/**
 * gnostr_channel_item_update:
 * @self: A channel item.
 * @channel: New channel data (deep copied).
 *
 * Replaces the underlying channel data. Does NOT emit items-changed
 * on any parent GListStore — caller is responsible for that.
 */
void gnostr_channel_item_update(GnostrChannelItem *self, const GnostrChannel *channel);

G_END_DECLS

#endif /* GNOSTR_CHANNEL_ITEM_H */
