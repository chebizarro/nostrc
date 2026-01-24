/**
 * GnostrChannelCreateDialog - Dialog for creating a new NIP-28 channel
 *
 * Allows the user to set channel name, description, and picture URL.
 */

#ifndef GNOSTR_CHANNEL_CREATE_DIALOG_H
#define GNOSTR_CHANNEL_CREATE_DIALOG_H

#include <adwaita.h>
#include "../util/nip28_chat.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHANNEL_CREATE_DIALOG (gnostr_channel_create_dialog_get_type())

G_DECLARE_FINAL_TYPE(GnostrChannelCreateDialog, gnostr_channel_create_dialog, GNOSTR, CHANNEL_CREATE_DIALOG, AdwDialog)

/**
 * Signals:
 * "create-channel" (GnostrChannel* channel, gpointer user_data)
 *   - Emitted when user confirms channel creation
 */

typedef struct _GnostrChannelCreateDialog GnostrChannelCreateDialog;

/**
 * Create a new channel creation dialog
 */
GnostrChannelCreateDialog *gnostr_channel_create_dialog_new(void);

/**
 * Present the dialog for channel creation (new channel)
 * @self: the dialog
 * @parent: parent widget
 */
void gnostr_channel_create_dialog_present(GnostrChannelCreateDialog *self,
                                           GtkWidget *parent);

/**
 * Present the dialog for editing an existing channel
 * @self: the dialog
 * @parent: parent widget
 * @channel: existing channel to edit
 */
void gnostr_channel_create_dialog_present_edit(GnostrChannelCreateDialog *self,
                                                GtkWidget *parent,
                                                const GnostrChannel *channel);

/**
 * Get the channel data from the dialog
 * @self: the dialog
 * @return: newly allocated GnostrChannel with form data, caller must free
 */
GnostrChannel *gnostr_channel_create_dialog_get_channel(GnostrChannelCreateDialog *self);

G_END_DECLS

#endif /* GNOSTR_CHANNEL_CREATE_DIALOG_H */
