/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-chat-view.h - Chat view for a NIP-29 group
 *
 * Displays chronological messages, metadata header, and a composer shell.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_GROUP_CHAT_VIEW_H
#define GN_NIP29_GROUP_CHAT_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-nip29-group-service.h"
#include "../model/gn-nip29-group-item.h"

#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUP_CHAT_VIEW (gn_nip29_group_chat_view_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupChatView, gn_nip29_group_chat_view,
                     GN, NIP29_GROUP_CHAT_VIEW, GtkBox)

GnNip29GroupChatView *gn_nip29_group_chat_view_new(GnNip29GroupService *service,
                                                    GnNip29GroupItem    *group_item,
                                                    GnostrPluginContext *plugin_context);

G_END_DECLS

#endif /* GN_NIP29_GROUP_CHAT_VIEW_H */
