/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-item.h - GObject representing a NIP-29 group message
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_MESSAGE_ITEM_H
#define GN_NIP29_MESSAGE_ITEM_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_NIP29_MESSAGE_ITEM (gn_nip29_message_item_get_type())
G_DECLARE_FINAL_TYPE(GnNip29MessageItem, gn_nip29_message_item,
                     GN, NIP29_MESSAGE_ITEM, GObject)

GnNip29MessageItem *gn_nip29_message_item_new(const char *id,
                                               const char *event_json,
                                               gint64      created_at,
                                               gint        kind,
                                               const char *pubkey,
                                               const char *content);

const char *gn_nip29_message_item_get_id        (GnNip29MessageItem *self);
const char *gn_nip29_message_item_get_event_json(GnNip29MessageItem *self);
gint64      gn_nip29_message_item_get_created_at(GnNip29MessageItem *self);
gint        gn_nip29_message_item_get_kind      (GnNip29MessageItem *self);
const char *gn_nip29_message_item_get_pubkey    (GnNip29MessageItem *self);
const char *gn_nip29_message_item_get_content   (GnNip29MessageItem *self);

G_END_DECLS

#endif /* GN_NIP29_MESSAGE_ITEM_H */
