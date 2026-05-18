/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-list-model.h - GListModel for NIP-29 group messages
 *
 * Presents messages for a specific group as a GListModel of GnNip29MessageItem.
 * Listens to GnNip29GroupService::group-updated for live updates.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_MESSAGE_LIST_MODEL_H
#define GN_NIP29_MESSAGE_LIST_MODEL_H

#include <gio/gio.h>
#include "../gn-nip29-group-service.h"

G_BEGIN_DECLS

#define GN_TYPE_NIP29_MESSAGE_LIST_MODEL (gn_nip29_message_list_model_get_type())
G_DECLARE_FINAL_TYPE(GnNip29MessageListModel, gn_nip29_message_list_model,
                     GN, NIP29_MESSAGE_LIST_MODEL, GObject)

GnNip29MessageListModel *gn_nip29_message_list_model_new(GnNip29GroupService *service,
                                                          const char          *group_key);
const char *gn_nip29_message_list_model_get_group_key(GnNip29MessageListModel *self);

G_END_DECLS

#endif /* GN_NIP29_MESSAGE_LIST_MODEL_H */
