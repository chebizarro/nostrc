/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-dm-list-view.h - MLS Direct Messages List View
 *
 * Shows all active MLS DirectMessage groups (1-on-1 conversations)
 * and provides a button to start a new encrypted DM.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_DM_LIST_VIEW_H
#define GN_MLS_DM_LIST_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"
#include "../gn-mls-dm-manager.h"

G_BEGIN_DECLS

#define GN_TYPE_MLS_DM_LIST_VIEW (gn_mls_dm_list_view_get_type())
G_DECLARE_FINAL_TYPE(GnMlsDmListView, gn_mls_dm_list_view,
                     GN, MLS_DM_LIST_VIEW, GtkBox)

/**
 * gn_mls_dm_list_view_new:
 * @service: The marmot service
 * @router: The MLS event router
 * @dm_manager: The DM manager
 * @plugin_context: The plugin context (borrowed)
 *
 * Returns: (transfer full): A new #GnMlsDmListView
 */
GnMlsDmListView *gn_mls_dm_list_view_new(GnMarmotService     *service,
                                            GnMlsEventRouter   *router,
                                            GnMlsDmManager     *dm_manager,
                                            GnostrPluginContext *plugin_context);

G_END_DECLS

#endif /* GN_MLS_DM_LIST_VIEW_H */
