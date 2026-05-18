/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-list-model.h - GListModel adapter for NIP-29 groups
 *
 * Wraps GnNip29GroupService as a GListModel of GnNip29GroupItem.
 * Automatically reloads when the service emits "groups-changed" or
 * "group-updated".
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_GROUP_LIST_MODEL_H
#define GN_NIP29_GROUP_LIST_MODEL_H

#include <gio/gio.h>
#include "../gn-nip29-group-service.h"

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUP_LIST_MODEL (gn_nip29_group_list_model_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupListModel, gn_nip29_group_list_model,
                     GN, NIP29_GROUP_LIST_MODEL, GObject)

GnNip29GroupListModel *gn_nip29_group_list_model_new(GnNip29GroupService *service);
void                   gn_nip29_group_list_model_reload(GnNip29GroupListModel *self);

G_END_DECLS

#endif /* GN_NIP29_GROUP_LIST_MODEL_H */
