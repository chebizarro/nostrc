/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectGroup: GObject wrapper for MarmotGroup.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_GROUP_H
#define MARMOT_GOBJECT_GROUP_H

#include <glib-object.h>
#include "marmot-gobject-enums.h"

G_BEGIN_DECLS

#define MARMOT_GOBJECT_TYPE_GROUP (marmot_gobject_group_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectGroup, marmot_gobject_group, MARMOT_GOBJECT, GROUP, GObject)

/**
 * MarmotGobjectGroup:
 *
 * A GObject wrapper for a Marmot group. Wraps the underlying C
 * MarmotGroup struct and exposes its fields as GObject properties.
 *
 * ## Properties
 *
 * - #MarmotGobjectGroup:mls-group-id - MLS group ID as hex string
 * - #MarmotGobjectGroup:nostr-group-id - Nostr group ID as hex string
 * - #MarmotGobjectGroup:name - Group name
 * - #MarmotGobjectGroup:description - Group description
 * - #MarmotGobjectGroup:state - Group state (active/inactive/pending)
 * - #MarmotGobjectGroup:epoch - Current MLS epoch
 * - #MarmotGobjectGroup:admin-count - Number of admins
 * - #MarmotGobjectGroup:last-message-at - Timestamp of last message
 *
 * Since: 1.0
 */

/**
 * marmot_gobject_group_new_from_data:
 * @mls_group_id_hex: MLS group ID as hex string
 * @nostr_group_id_hex: Nostr group ID as hex string
 * @name: (nullable): group name
 * @description: (nullable): group description
 * @state: group state
 * @epoch: MLS epoch
 *
 * Creates a new MarmotGobjectGroup from individual fields.
 *
 * Returns: (transfer full): a new #MarmotGobjectGroup
 */
MarmotGobjectGroup *marmot_gobject_group_new_from_data(const gchar *mls_group_id_hex,
                                                        const gchar *nostr_group_id_hex,
                                                        const gchar *name,
                                                        const gchar *description,
                                                        MarmotGobjectGroupState state,
                                                        guint64 epoch);

/* ── Property accessors ──────────────────────────────────────────── */

/**
 * marmot_gobject_group_get_mls_group_id:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: (transfer none): the MLS group ID as a hex string
 */
const gchar *marmot_gobject_group_get_mls_group_id(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_nostr_group_id:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: (transfer none): the Nostr group ID as a hex string
 */
const gchar *marmot_gobject_group_get_nostr_group_id(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_name:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: (transfer none) (nullable): the group name
 */
const gchar *marmot_gobject_group_get_name(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_description:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: (transfer none) (nullable): the group description
 */
const gchar *marmot_gobject_group_get_description(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_state:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: the group state
 */
MarmotGobjectGroupState marmot_gobject_group_get_state(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_epoch:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: the current MLS epoch
 */
guint64 marmot_gobject_group_get_epoch(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_admin_count:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: number of admins in the group
 */
guint marmot_gobject_group_get_admin_count(MarmotGobjectGroup *self);

/**
 * marmot_gobject_group_get_admin_pubkey_hex:
 * @self: a #MarmotGobjectGroup
 * @index: admin index (0-based)
 *
 * Returns: (transfer full) (nullable): admin pubkey as hex, or NULL if out of range
 */
gchar *marmot_gobject_group_get_admin_pubkey_hex(MarmotGobjectGroup *self, guint index);

/**
 * marmot_gobject_group_get_last_message_at:
 * @self: a #MarmotGobjectGroup
 *
 * Returns: timestamp of last message, or 0 if none
 */
gint64 marmot_gobject_group_get_last_message_at(MarmotGobjectGroup *self);

G_END_DECLS

#endif /* MARMOT_GOBJECT_GROUP_H */
