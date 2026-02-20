/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-create-group-dialog.h - Create Group Dialog
 *
 * AdwDialog for creating a new MLS group. Allows the user to set
 * a group name, description, and add initial members by pubkey.
 * Fetches key packages from relays and sends welcome messages.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_CREATE_GROUP_DIALOG_H
#define GN_CREATE_GROUP_DIALOG_H

#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"

/* Forward declaration — full definition in gnostr-plugin-api.h */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_CREATE_GROUP_DIALOG (gn_create_group_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnCreateGroupDialog, gn_create_group_dialog,
                     GN, CREATE_GROUP_DIALOG, AdwDialog)

/**
 * gn_create_group_dialog_new:
 * @service: The marmot service
 * @router: The MLS event router (for sending welcomes)
 * @plugin_context: The plugin context (for relay access and key package queries)
 *
 * Returns: (transfer full): A new #GnCreateGroupDialog
 */
GnCreateGroupDialog *gn_create_group_dialog_new(GnMarmotService     *service,
                                                  GnMlsEventRouter   *router,
                                                  GnostrPluginContext *plugin_context);

/**
 * GnCreateGroupDialog::group-created:
 * @dialog: The dialog
 * @group: (transfer none): The newly created MarmotGobjectGroup
 *
 * Emitted when the group is successfully created.
 */

G_END_DECLS

#endif /* GN_CREATE_GROUP_DIALOG_H */
