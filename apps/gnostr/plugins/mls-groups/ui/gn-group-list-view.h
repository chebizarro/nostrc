/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-view.h - Group list panel
 *
 * Main sidebar panel showing all MLS groups as a GtkListView.
 * Selecting a group pushes a GnGroupChatView onto the navigation stack.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_LIST_VIEW_H
#define GN_GROUP_LIST_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"

G_BEGIN_DECLS

#define GN_TYPE_GROUP_LIST_VIEW (gn_group_list_view_get_type())
G_DECLARE_FINAL_TYPE(GnGroupListView, gn_group_list_view,
                     GN, GROUP_LIST_VIEW, GtkBox)

/**
 * gn_group_list_view_new:
 * @service: The marmot service
 * @router: The MLS event router (for sending messages)
 * @navigation_view: The host navigation view (to push chat pages)
 *
 * Returns: (transfer full): A new #GnGroupListView
 */
GnGroupListView *gn_group_list_view_new(GnMarmotService     *service,
                                         GnMlsEventRouter    *router,
                                         AdwNavigationView   *navigation_view);

G_END_DECLS

#endif /* GN_GROUP_LIST_VIEW_H */
