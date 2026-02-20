/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-row.h - Group list row widget
 *
 * A row shown in the group list sidebar. Displays group name,
 * member count, and last-message preview.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_LIST_ROW_H
#define GN_GROUP_LIST_ROW_H

#include <gtk/gtk.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

G_BEGIN_DECLS

#define GN_TYPE_GROUP_LIST_ROW (gn_group_list_row_get_type())
G_DECLARE_FINAL_TYPE(GnGroupListRow, gn_group_list_row,
                     GN, GROUP_LIST_ROW, GtkBox)

/**
 * gn_group_list_row_new:
 *
 * Returns: (transfer full): A new #GnGroupListRow
 */
GnGroupListRow *gn_group_list_row_new(void);

/**
 * gn_group_list_row_bind:
 * @self: The row
 * @group: The marmot group to display
 *
 * Bind a group's data to this row.
 */
void gn_group_list_row_bind(GnGroupListRow   *self,
                             MarmotGobjectGroup *group);

/**
 * gn_group_list_row_unbind:
 * @self: The row
 *
 * Clear the row's displayed data (for recycling).
 */
void gn_group_list_row_unbind(GnGroupListRow *self);

G_END_DECLS

#endif /* GN_GROUP_LIST_ROW_H */
