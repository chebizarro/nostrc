/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-member-row.h - Group member row widget
 *
 * Displays a single group member with pubkey, optional display name,
 * admin badge, and optional remove button.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MEMBER_ROW_H
#define GN_MEMBER_ROW_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define GN_TYPE_MEMBER_ROW (gn_member_row_get_type())
G_DECLARE_FINAL_TYPE(GnMemberRow, gn_member_row, GN, MEMBER_ROW, GtkBox)

/**
 * gn_member_row_new:
 *
 * Returns: (transfer full): A new empty #GnMemberRow
 */
GnMemberRow *gn_member_row_new(void);

/**
 * gn_member_row_set_pubkey:
 * @self: A #GnMemberRow
 * @pubkey_hex: The member's public key as hex
 * @is_admin: Whether the member is an admin
 * @is_self: Whether the member is the current user
 *
 * Set the member displayed in this row.
 */
void gn_member_row_set_pubkey(GnMemberRow *self,
                               const gchar *pubkey_hex,
                               gboolean     is_admin,
                               gboolean     is_self);

/**
 * gn_member_row_get_pubkey_hex:
 * @self: A #GnMemberRow
 *
 * Returns: (transfer none) (nullable): The member's pubkey hex
 */
const gchar *gn_member_row_get_pubkey_hex(GnMemberRow *self);

/**
 * gn_member_row_set_removable:
 * @self: A #GnMemberRow
 * @removable: Whether to show the remove button
 *
 * When removable is TRUE, a remove button is shown and the
 * "remove-requested" signal can be emitted.
 */
void gn_member_row_set_removable(GnMemberRow *self,
                                  gboolean     removable);

/**
 * GnMemberRow::remove-requested:
 * @self: The member row
 * @pubkey_hex: The member's pubkey that should be removed
 *
 * Emitted when the user clicks the remove button.
 */

G_END_DECLS

#endif /* GN_MEMBER_ROW_H */
