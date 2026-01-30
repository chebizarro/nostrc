/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-manager-panel.h - Plugin manager settings panel
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_PLUGIN_MANAGER_PANEL_H
#define GNOSTR_PLUGIN_MANAGER_PANEL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PLUGIN_MANAGER_PANEL (gnostr_plugin_manager_panel_get_type())
G_DECLARE_FINAL_TYPE(GnostrPluginManagerPanel, gnostr_plugin_manager_panel, GNOSTR, PLUGIN_MANAGER_PANEL, GtkWidget)

/**
 * gnostr_plugin_manager_panel_new:
 *
 * Create a new plugin manager panel widget.
 * This panel displays the list of installed plugins and
 * allows enabling/disabling and configuring them.
 *
 * Returns: (transfer full): A new #GnostrPluginManagerPanel
 */
GtkWidget *gnostr_plugin_manager_panel_new(void);

/**
 * gnostr_plugin_manager_panel_refresh:
 * @self: A #GnostrPluginManagerPanel
 *
 * Refresh the plugin list display.
 * Call after installing or uninstalling plugins.
 */
void gnostr_plugin_manager_panel_refresh(GnostrPluginManagerPanel *self);

/**
 * gnostr_plugin_manager_panel_filter:
 * @self: A #GnostrPluginManagerPanel
 * @search_text: (nullable): Text to filter by, or %NULL to show all
 *
 * Filter the displayed plugins by search text.
 * Matches against plugin name and description.
 */
void gnostr_plugin_manager_panel_filter(GnostrPluginManagerPanel *self,
                                        const char               *search_text);

/**
 * gnostr_plugin_manager_panel_show_plugin_settings:
 * @self: A #GnostrPluginManagerPanel
 * @plugin_id: The plugin module name
 *
 * Show the settings dialog for a specific plugin.
 */
void gnostr_plugin_manager_panel_show_plugin_settings(GnostrPluginManagerPanel *self,
                                                      const char               *plugin_id);

/**
 * gnostr_plugin_manager_panel_show_plugin_info:
 * @self: A #GnostrPluginManagerPanel
 * @plugin_id: The plugin module name
 *
 * Show the information dialog for a specific plugin.
 */
void gnostr_plugin_manager_panel_show_plugin_info(GnostrPluginManagerPanel *self,
                                                  const char               *plugin_id);

G_END_DECLS

#endif /* GNOSTR_PLUGIN_MANAGER_PANEL_H */
