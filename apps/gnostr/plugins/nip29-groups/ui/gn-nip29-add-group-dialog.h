/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-add-group-dialog.h - Dialog for manually tracking a NIP-29 group
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_ADD_GROUP_DIALOG_H
#define GN_NIP29_ADD_GROUP_DIALOG_H

#include <adwaita.h>
#include "../gn-nip29-group-service.h"

G_BEGIN_DECLS

#define GN_TYPE_NIP29_ADD_GROUP_DIALOG (gn_nip29_add_group_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnNip29AddGroupDialog, gn_nip29_add_group_dialog,
                     GN, NIP29_ADD_GROUP_DIALOG, AdwDialog)

GnNip29AddGroupDialog *gn_nip29_add_group_dialog_new       (GnNip29GroupService *service);
GnNip29AddGroupDialog *gn_nip29_add_group_dialog_new_create(GnNip29GroupService *service);

G_END_DECLS

#endif /* GN_NIP29_ADD_GROUP_DIALOG_H */
