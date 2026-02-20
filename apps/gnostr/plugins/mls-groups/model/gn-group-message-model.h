/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-model.h - GListModel adapter for group messages
 *
 * Presents the messages for a specific MLS group as a GListModel.
 * Listens to GnMarmotService::message-received to auto-append new messages.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_MESSAGE_MODEL_H
#define GN_GROUP_MESSAGE_MODEL_H

#include <gio/gio.h>
#include "../gn-marmot-service.h"

G_BEGIN_DECLS

#define GN_TYPE_GROUP_MESSAGE_MODEL (gn_group_message_model_get_type())
G_DECLARE_FINAL_TYPE(GnGroupMessageModel, gn_group_message_model,
                     GN, GROUP_MESSAGE_MODEL, GObject)

/**
 * gn_group_message_model_new:
 * @service: The marmot service
 * @mls_group_id_hex: The MLS group ID (hex)
 *
 * Creates a message model for the given group.
 * Performs an initial load of recent messages.
 *
 * Returns: (transfer full): A new #GnGroupMessageModel implementing GListModel
 */
GnGroupMessageModel *gn_group_message_model_new(GnMarmotService *service,
                                                  const gchar     *mls_group_id_hex);

/**
 * gn_group_message_model_load_more:
 * @self: The model
 *
 * Load an older page of messages (pagination).
 * New items are prepended and items-changed is emitted.
 */
void gn_group_message_model_load_more(GnGroupMessageModel *self);

/**
 * gn_group_message_model_get_group_id_hex:
 * @self: The model
 *
 * Returns: (transfer none): The MLS group ID hex
 */
const gchar *gn_group_message_model_get_group_id_hex(GnGroupMessageModel *self);

G_END_DECLS

#endif /* GN_GROUP_MESSAGE_MODEL_H */
