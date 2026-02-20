/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-welcome-list-view.h - Group Invitations View
 *
 * Displays pending MLS group invitations (welcomes) and allows the user
 * to accept or decline them.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_WELCOME_LIST_VIEW_H
#define GN_WELCOME_LIST_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"

G_BEGIN_DECLS

#define GN_TYPE_WELCOME_LIST_VIEW (gn_welcome_list_view_get_type())
G_DECLARE_FINAL_TYPE(GnWelcomeListView, gn_welcome_list_view,
                     GN, WELCOME_LIST_VIEW, GtkBox)

/**
 * gn_welcome_list_view_new:
 * @service: The marmot service
 * @router: The MLS event router
 * @plugin_context: The plugin context (borrowed)
 *
 * Creates a new invitations view showing pending MLS group welcomes.
 *
 * Returns: (transfer full): A new #GnWelcomeListView
 */
GnWelcomeListView *gn_welcome_list_view_new(GnMarmotService     *service,
                                              GnMlsEventRouter   *router,
                                              GnostrPluginContext *plugin_context);

/**
 * gn_welcome_list_view_refresh:
 * @self: The view
 *
 * Reload the pending welcomes from the marmot service.
 */
void gn_welcome_list_view_refresh(GnWelcomeListView *self);

G_END_DECLS

#endif /* GN_WELCOME_LIST_VIEW_H */
