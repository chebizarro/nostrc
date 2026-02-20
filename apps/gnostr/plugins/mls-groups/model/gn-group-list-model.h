/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-model.h - GListModel adapter for MLS groups
 *
 * Wraps the marmot client's group list as a GListModel for use
 * with GtkListView / GtkNoSelection / GtkSingleSelection.
 *
 * The model is populated by calling reload(), which queries all groups
 * from the marmot client.  The GnMarmotService's "group-created",
 * "group-joined", and "group-updated" signals trigger automatic reload.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_LIST_MODEL_H
#define GN_GROUP_LIST_MODEL_H

#include <gio/gio.h>
#include "../gn-marmot-service.h"

G_BEGIN_DECLS

#define GN_TYPE_GROUP_LIST_MODEL (gn_group_list_model_get_type())
G_DECLARE_FINAL_TYPE(GnGroupListModel, gn_group_list_model,
                     GN, GROUP_LIST_MODEL, GObject)

/**
 * gn_group_list_model_new:
 * @service: The marmot service (strong ref kept)
 *
 * Returns: (transfer full): A new #GnGroupListModel implementing GListModel
 */
GnGroupListModel *gn_group_list_model_new(GnMarmotService *service);

/**
 * gn_group_list_model_reload:
 * @self: The model
 *
 * Reload all groups from the marmot client.
 * Emits items-changed for the diff.
 */
void gn_group_list_model_reload(GnGroupListModel *self);

G_END_DECLS

#endif /* GN_GROUP_LIST_MODEL_H */
