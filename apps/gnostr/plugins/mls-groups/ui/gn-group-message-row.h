/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-row.h - Group chat message bubble widget
 *
 * Displays a single message in a group conversation. Includes
 * sender name, content, and timestamp.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_MESSAGE_ROW_H
#define GN_GROUP_MESSAGE_ROW_H

#include <gtk/gtk.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

G_BEGIN_DECLS

#define GN_TYPE_GROUP_MESSAGE_ROW (gn_group_message_row_get_type())
G_DECLARE_FINAL_TYPE(GnGroupMessageRow, gn_group_message_row,
                     GN, GROUP_MESSAGE_ROW, GtkBox)

GnGroupMessageRow *gn_group_message_row_new(void);

void gn_group_message_row_bind(GnGroupMessageRow    *self,
                                MarmotGobjectMessage *message,
                                const gchar          *user_pubkey_hex);

void gn_group_message_row_unbind(GnGroupMessageRow *self);

G_END_DECLS

#endif /* GN_GROUP_MESSAGE_ROW_H */
