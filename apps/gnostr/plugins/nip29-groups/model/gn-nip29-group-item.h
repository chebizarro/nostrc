/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-item.h - GObject representing a NIP-29 group for list display
 *
 * Snapshots the service state into an owned GObject suitable for GListModel.
 * Distinguishes unknown (snapshot absent) vs empty (snapshot present, 0 entries).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_GROUP_ITEM_H
#define GN_NIP29_GROUP_ITEM_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUP_ITEM (gn_nip29_group_item_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupItem, gn_nip29_group_item,
                     GN, NIP29_GROUP_ITEM, GObject)

GnNip29GroupItem *gn_nip29_group_item_new(const char *key,
                                          const char *relay_url,
                                          const char *group_id,
                                          const char *alias,
                                          const char *name,
                                          const char *picture,
                                          const char *about,
                                          gboolean    is_private,
                                          gboolean    is_restricted,
                                          gboolean    is_hidden,
                                          gboolean    is_closed,
                                          gboolean    admins_loaded,
                                          gboolean    members_loaded,
                                          gboolean    members_may_be_partial,
                                          gboolean    roles_loaded,
                                          guint       admin_count,
                                          guint       member_count,
                                          guint       message_count);

const char *gn_nip29_group_item_get_key        (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_relay_url   (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_group_id    (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_alias       (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_name        (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_picture     (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_about       (GnNip29GroupItem *self);
const char *gn_nip29_group_item_get_display_name(GnNip29GroupItem *self);

gboolean gn_nip29_group_item_get_is_private   (GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_is_restricted(GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_is_hidden    (GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_is_closed    (GnNip29GroupItem *self);

gboolean gn_nip29_group_item_get_admins_loaded         (GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_members_loaded        (GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_members_may_be_partial(GnNip29GroupItem *self);
gboolean gn_nip29_group_item_get_roles_loaded          (GnNip29GroupItem *self);

guint gn_nip29_group_item_get_admin_count  (GnNip29GroupItem *self);
guint gn_nip29_group_item_get_member_count (GnNip29GroupItem *self);
guint gn_nip29_group_item_get_message_count(GnNip29GroupItem *self);

G_END_DECLS

#endif /* GN_NIP29_GROUP_ITEM_H */
