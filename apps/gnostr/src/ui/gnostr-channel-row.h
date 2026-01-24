/**
 * GnostrChannelRow - A row widget for displaying a NIP-28 channel in the list
 *
 * Shows channel avatar, name, description, and member/message counts.
 */

#ifndef GNOSTR_CHANNEL_ROW_H
#define GNOSTR_CHANNEL_ROW_H

#include <gtk/gtk.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHANNEL_ROW (gnostr_channel_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrChannelRow, gnostr_channel_row, GNOSTR, CHANNEL_ROW, GtkWidget)

/**
 * Signals:
 * "channel-selected" (gchar* channel_id, gpointer user_data)
 *   - Emitted when user clicks to enter the channel
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks to view the channel creator's profile
 */

typedef struct _GnostrChannelRow GnostrChannelRow;

/**
 * Create a new channel row widget
 */
GnostrChannelRow *gnostr_channel_row_new(void);

/**
 * Set the channel data for this row
 * @self: the row widget
 * @channel: channel data (copied internally)
 */
void gnostr_channel_row_set_channel(GnostrChannelRow *self,
                                     const GnostrChannel *channel);

/**
 * Get the channel ID for this row
 */
const char *gnostr_channel_row_get_channel_id(GnostrChannelRow *self);

/**
 * Get the channel name
 */
const char *gnostr_channel_row_get_name(GnostrChannelRow *self);

/**
 * Get the channel about/description
 */
const char *gnostr_channel_row_get_about(GnostrChannelRow *self);

/**
 * Get the creator's pubkey
 */
const char *gnostr_channel_row_get_creator_pubkey(GnostrChannelRow *self);

G_END_DECLS

#endif /* GNOSTR_CHANNEL_ROW_H */
