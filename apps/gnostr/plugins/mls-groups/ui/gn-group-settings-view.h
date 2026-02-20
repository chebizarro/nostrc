/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-settings-view.h - Group Settings / Info View
 *
 * Displays group metadata (name, description, epoch, admins, member list)
 * and provides management actions (add member, leave group).
 *
 * Pushed as an AdwNavigationPage onto the navigation stack from
 * the group chat view's info/settings button.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_SETTINGS_VIEW_H
#define GN_GROUP_SETTINGS_VIEW_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../gn-marmot-service.h"
#include "../gn-mls-event-router.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

/* Forward declaration — full definition in gnostr-plugin-api.h */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_GROUP_SETTINGS_VIEW (gn_group_settings_view_get_type())
G_DECLARE_FINAL_TYPE(GnGroupSettingsView, gn_group_settings_view,
                     GN, GROUP_SETTINGS_VIEW, GtkBox)

/**
 * gn_group_settings_view_new:
 * @service: The marmot service
 * @router: The MLS event router
 * @group: The group to display settings for
 * @plugin_context: The plugin context
 *
 * Returns: (transfer full): A new #GnGroupSettingsView
 */
GnGroupSettingsView *gn_group_settings_view_new(GnMarmotService      *service,
                                                  GnMlsEventRouter    *router,
                                                  MarmotGobjectGroup  *group,
                                                  GnostrPluginContext *plugin_context);

/**
 * GnGroupSettingsView::member-added:
 * @view: The settings view
 * @pubkey_hex: Public key of the added member
 *
 * Emitted when a member is successfully added.
 */

/**
 * GnGroupSettingsView::left-group:
 * @view: The settings view
 *
 * Emitted when the user leaves the group.
 */

G_END_DECLS

#endif /* GN_GROUP_SETTINGS_VIEW_H */
