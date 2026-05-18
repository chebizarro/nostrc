/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-groups-panel.h - Main NIP-29 groups list/detail panel
 *
 * Top-level panel returned by the plugin's create_panel_widget.
 * Contains a group list, empty/loading/error states, and pushes
 * chat views onto an AdwNavigationView when available.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_GROUPS_PANEL_H
#define GN_NIP29_GROUPS_PANEL_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-nip29-group-service.h"

#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUPS_PANEL (gn_nip29_groups_panel_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupsPanel, gn_nip29_groups_panel,
                     GN, NIP29_GROUPS_PANEL, GtkBox)

GnNip29GroupsPanel *gn_nip29_groups_panel_new(GnNip29GroupService *service,
                                               AdwNavigationView   *nav_view,
                                               GnostrPluginContext *plugin_context);

G_END_DECLS

#endif /* GN_NIP29_GROUPS_PANEL_H */
