/**
 * GnostrChannelListView - NIP-28 Public Chat Channel Browser
 *
 * Displays a scrollable list of public chat channels with search/filter
 * capabilities and a button to create new channels.
 */

#ifndef GNOSTR_CHANNEL_LIST_VIEW_H
#define GNOSTR_CHANNEL_LIST_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHANNEL_LIST_VIEW (gnostr_channel_list_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrChannelListView, gnostr_channel_list_view, GNOSTR, CHANNEL_LIST_VIEW, GtkWidget)

/**
 * Signals:
 * "channel-selected" (gchar* channel_id, gpointer user_data)
 *   - Emitted when user selects a channel to join
 * "create-channel" (gpointer user_data)
 *   - Emitted when user clicks to create a new channel
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user wants to view a channel creator's profile
 */

typedef struct _GnostrChannelListView GnostrChannelListView;

/**
 * Create a new channel list view
 */
GnostrChannelListView *gnostr_channel_list_view_new(void);

/**
 * Add or update a channel in the list
 * @self: the channel list view
 * @channel: channel data (copied internally)
 */
void gnostr_channel_list_view_upsert_channel(GnostrChannelListView *self,
                                              const GnostrChannel *channel);

/**
 * Remove a channel from the list
 * @self: the channel list view
 * @channel_id: ID of channel to remove
 */
void gnostr_channel_list_view_remove_channel(GnostrChannelListView *self,
                                              const char *channel_id);

/**
 * Clear all channels from the list
 */
void gnostr_channel_list_view_clear(GnostrChannelListView *self);

/**
 * Set the loading state
 * @self: the channel list view
 * @is_loading: TRUE to show loading spinner
 */
void gnostr_channel_list_view_set_loading(GnostrChannelListView *self,
                                           gboolean is_loading);

/**
 * Set the empty state
 * @self: the channel list view
 * @is_empty: TRUE to show empty state
 */
void gnostr_channel_list_view_set_empty(GnostrChannelListView *self,
                                         gboolean is_empty);

/**
 * Get the currently selected channel ID
 * @return: channel ID or NULL if none selected
 */
const char *gnostr_channel_list_view_get_selected_id(GnostrChannelListView *self);

/**
 * Set the current user's pubkey (for showing owned channels)
 */
void gnostr_channel_list_view_set_user_pubkey(GnostrChannelListView *self,
                                               const char *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_CHANNEL_LIST_VIEW_H */
