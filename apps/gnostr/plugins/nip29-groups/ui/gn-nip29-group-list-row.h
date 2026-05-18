/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-list-row.h - Row widget for a NIP-29 group in the list
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_GROUP_LIST_ROW_H
#define GN_NIP29_GROUP_LIST_ROW_H

#include <gtk/gtk.h>
#include "../model/gn-nip29-group-item.h"

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUP_LIST_ROW (gn_nip29_group_list_row_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupListRow, gn_nip29_group_list_row,
                     GN, NIP29_GROUP_LIST_ROW, GtkBox)

GnNip29GroupListRow *gn_nip29_group_list_row_new(void);
void gn_nip29_group_list_row_bind  (GnNip29GroupListRow *self, GnNip29GroupItem *item);
void gn_nip29_group_list_row_unbind(GnNip29GroupListRow *self);

G_END_DECLS

#endif /* GN_NIP29_GROUP_LIST_ROW_H */
