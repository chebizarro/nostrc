/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectWelcome: GObject wrapper for MarmotWelcome.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_WELCOME_H
#define MARMOT_GOBJECT_WELCOME_H

#include <glib-object.h>
#include "marmot-gobject-enums.h"

G_BEGIN_DECLS

#define MARMOT_GOBJECT_TYPE_WELCOME (marmot_gobject_welcome_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectWelcome, marmot_gobject_welcome, MARMOT_GOBJECT, WELCOME, GObject)

/**
 * MarmotGobjectWelcome:
 *
 * A GObject wrapper for a Marmot welcome (group invitation).
 *
 * ## Properties
 *
 * - #MarmotGobjectWelcome:event-id - Rumor event ID as hex string
 * - #MarmotGobjectWelcome:group-name - Group name
 * - #MarmotGobjectWelcome:group-description - Group description
 * - #MarmotGobjectWelcome:welcomer - Welcomer pubkey as hex string
 * - #MarmotGobjectWelcome:member-count - Number of members at invite time
 * - #MarmotGobjectWelcome:state - Welcome state
 * - #MarmotGobjectWelcome:mls-group-id - MLS group ID as hex string
 * - #MarmotGobjectWelcome:nostr-group-id - Nostr group ID as hex string
 *
 * Since: 1.0
 */

MarmotGobjectWelcome *marmot_gobject_welcome_new_from_data(const gchar *event_id_hex,
                                                            const gchar *group_name,
                                                            const gchar *group_description,
                                                            const gchar *welcomer_hex,
                                                            guint member_count,
                                                            MarmotGobjectWelcomeState state,
                                                            const gchar *mls_group_id_hex,
                                                            const gchar *nostr_group_id_hex);

/* ── Property accessors ──────────────────────────────────────────── */

const gchar *marmot_gobject_welcome_get_event_id(MarmotGobjectWelcome *self);
const gchar *marmot_gobject_welcome_get_group_name(MarmotGobjectWelcome *self);
const gchar *marmot_gobject_welcome_get_group_description(MarmotGobjectWelcome *self);
const gchar *marmot_gobject_welcome_get_welcomer(MarmotGobjectWelcome *self);
guint        marmot_gobject_welcome_get_member_count(MarmotGobjectWelcome *self);
MarmotGobjectWelcomeState marmot_gobject_welcome_get_state(MarmotGobjectWelcome *self);
const gchar *marmot_gobject_welcome_get_mls_group_id(MarmotGobjectWelcome *self);
const gchar *marmot_gobject_welcome_get_nostr_group_id(MarmotGobjectWelcome *self);

/**
 * marmot_gobject_welcome_get_relay_urls:
 * @self: a #MarmotGobjectWelcome
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): relay URLs
 */
const gchar * const *marmot_gobject_welcome_get_relay_urls(MarmotGobjectWelcome *self);

G_END_DECLS

#endif /* MARMOT_GOBJECT_WELCOME_H */
