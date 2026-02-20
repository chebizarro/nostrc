/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-chat-view.h - Group conversation view
 *
 * Full chat interface for an MLS group conversation.
 * Displays a scrollable message list and a composer for sending.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_CHAT_VIEW_H
#define GN_GROUP_CHAT_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

G_BEGIN_DECLS

#define GN_TYPE_GROUP_CHAT_VIEW (gn_group_chat_view_get_type())
G_DECLARE_FINAL_TYPE(GnGroupChatView, gn_group_chat_view,
                     GN, GROUP_CHAT_VIEW, GtkBox)

/**
 * gn_group_chat_view_new:
 * @service: The marmot service
 * @router: The MLS event router
 * @group: The group to display
 *
 * Returns: (transfer full): A new #GnGroupChatView
 */
GnGroupChatView *gn_group_chat_view_new(GnMarmotService      *service,
                                          GnMlsEventRouter    *router,
                                          MarmotGobjectGroup  *group);

G_END_DECLS

#endif /* GN_GROUP_CHAT_VIEW_H */
