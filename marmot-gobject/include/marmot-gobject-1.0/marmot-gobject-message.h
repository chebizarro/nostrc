/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectMessage: GObject wrapper for MarmotMessage.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_MESSAGE_H
#define MARMOT_GOBJECT_MESSAGE_H

#include <glib-object.h>
#include "marmot-gobject-enums.h"

G_BEGIN_DECLS

#define MARMOT_GOBJECT_TYPE_MESSAGE (marmot_gobject_message_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectMessage, marmot_gobject_message, MARMOT_GOBJECT, MESSAGE, GObject)

/**
 * MarmotGobjectMessage:
 *
 * A GObject wrapper for a decrypted Marmot group message.
 *
 * ## Properties
 *
 * - #MarmotGobjectMessage:event-id - Event ID as hex string
 * - #MarmotGobjectMessage:pubkey - Author pubkey as hex string
 * - #MarmotGobjectMessage:content - Decrypted message content
 * - #MarmotGobjectMessage:kind - Event kind
 * - #MarmotGobjectMessage:created-at - Creation timestamp
 * - #MarmotGobjectMessage:processed-at - Processing timestamp
 * - #MarmotGobjectMessage:mls-group-id - MLS group ID as hex string
 * - #MarmotGobjectMessage:epoch - MLS epoch
 * - #MarmotGobjectMessage:state - Message state
 * - #MarmotGobjectMessage:event-json - Full unsigned event JSON
 *
 * Since: 1.0
 */

/**
 * marmot_gobject_message_new_from_data:
 * @event_id_hex: event ID as hex string
 * @pubkey_hex: author pubkey as hex string
 * @content: (nullable): decrypted content
 * @kind: event kind
 * @created_at: creation timestamp
 * @mls_group_id_hex: MLS group ID as hex string
 *
 * Creates a new MarmotGobjectMessage.
 *
 * Returns: (transfer full): a new #MarmotGobjectMessage
 */
MarmotGobjectMessage *marmot_gobject_message_new_from_data(const gchar *event_id_hex,
                                                            const gchar *pubkey_hex,
                                                            const gchar *content,
                                                            guint kind,
                                                            gint64 created_at,
                                                            const gchar *mls_group_id_hex);

/* ── Property accessors ──────────────────────────────────────────── */

const gchar *marmot_gobject_message_get_event_id(MarmotGobjectMessage *self);
const gchar *marmot_gobject_message_get_pubkey(MarmotGobjectMessage *self);
const gchar *marmot_gobject_message_get_content(MarmotGobjectMessage *self);
guint        marmot_gobject_message_get_kind(MarmotGobjectMessage *self);
gint64       marmot_gobject_message_get_created_at(MarmotGobjectMessage *self);
gint64       marmot_gobject_message_get_processed_at(MarmotGobjectMessage *self);
const gchar *marmot_gobject_message_get_mls_group_id(MarmotGobjectMessage *self);
guint64      marmot_gobject_message_get_epoch(MarmotGobjectMessage *self);
MarmotGobjectMessageState marmot_gobject_message_get_state(MarmotGobjectMessage *self);
const gchar *marmot_gobject_message_get_event_json(MarmotGobjectMessage *self);

G_END_DECLS

#endif /* MARMOT_GOBJECT_MESSAGE_H */
