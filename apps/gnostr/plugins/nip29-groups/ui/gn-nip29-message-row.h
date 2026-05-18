/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-row.h - Row widget for a NIP-29 group message
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_MESSAGE_ROW_H
#define GN_NIP29_MESSAGE_ROW_H

#include <gtk/gtk.h>
#include "../model/gn-nip29-message-item.h"

#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_NIP29_MESSAGE_ROW (gn_nip29_message_row_get_type())
G_DECLARE_FINAL_TYPE(GnNip29MessageRow, gn_nip29_message_row,
                     GN, NIP29_MESSAGE_ROW, GtkBox)

GnNip29MessageRow *gn_nip29_message_row_new(void);

void gn_nip29_message_row_bind  (GnNip29MessageRow   *self,
                                  GnNip29MessageItem  *item,
                                  const char          *user_pubkey_hex,
                                  GnostrPluginContext *plugin_context);
void gn_nip29_message_row_unbind(GnNip29MessageRow *self);

G_END_DECLS

#endif /* GN_NIP29_MESSAGE_ROW_H */
