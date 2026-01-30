/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-row.h - Plugin list row widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_PLUGIN_ROW_H
#define GNOSTR_PLUGIN_ROW_H

#include <gtk/gtk.h>
#include <libpeas.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PLUGIN_ROW (gnostr_plugin_row_get_type())
G_DECLARE_FINAL_TYPE(GnostrPluginRow, gnostr_plugin_row, GNOSTR, PLUGIN_ROW, GtkWidget)

/**
 * GnostrPluginState:
 * @GNOSTR_PLUGIN_STATE_UNLOADED: Plugin is not loaded
 * @GNOSTR_PLUGIN_STATE_LOADED: Plugin is loaded but not activated
 * @GNOSTR_PLUGIN_STATE_ACTIVE: Plugin is active and running
 * @GNOSTR_PLUGIN_STATE_ERROR: Plugin failed to load or activate
 * @GNOSTR_PLUGIN_STATE_NEEDS_RESTART: Plugin state change requires restart
 * @GNOSTR_PLUGIN_STATE_INCOMPATIBLE: Plugin API version incompatible
 *
 * Plugin runtime state.
 */
typedef enum
{
  GNOSTR_PLUGIN_STATE_UNLOADED,
  GNOSTR_PLUGIN_STATE_LOADED,
  GNOSTR_PLUGIN_STATE_ACTIVE,
  GNOSTR_PLUGIN_STATE_ERROR,
  GNOSTR_PLUGIN_STATE_NEEDS_RESTART,
  GNOSTR_PLUGIN_STATE_INCOMPATIBLE,
} GnostrPluginState;

/**
 * gnostr_plugin_row_new:
 * @info: The #PeasPluginInfo for the plugin
 *
 * Create a new plugin row widget.
 *
 * Returns: (transfer full): A new #GnostrPluginRow
 */
GtkWidget *gnostr_plugin_row_new(PeasPluginInfo *info);

/**
 * gnostr_plugin_row_get_plugin_info:
 * @self: A #GnostrPluginRow
 *
 * Get the PeasPluginInfo associated with this row.
 *
 * Returns: (transfer none): The plugin info
 */
PeasPluginInfo *gnostr_plugin_row_get_plugin_info(GnostrPluginRow *self);

/**
 * gnostr_plugin_row_set_enabled:
 * @self: A #GnostrPluginRow
 * @enabled: Whether the plugin should be enabled
 *
 * Set the enabled state of the plugin checkbox.
 */
void gnostr_plugin_row_set_enabled(GnostrPluginRow *self, gboolean enabled);

/**
 * gnostr_plugin_row_get_enabled:
 * @self: A #GnostrPluginRow
 *
 * Get the enabled state of the plugin.
 *
 * Returns: %TRUE if enabled
 */
gboolean gnostr_plugin_row_get_enabled(GnostrPluginRow *self);

/**
 * gnostr_plugin_row_set_state:
 * @self: A #GnostrPluginRow
 * @state: The plugin state
 *
 * Set the plugin state (affects status display).
 */
void gnostr_plugin_row_set_state(GnostrPluginRow *self, GnostrPluginState state);

/**
 * gnostr_plugin_row_get_state:
 * @self: A #GnostrPluginRow
 *
 * Get the current plugin state.
 *
 * Returns: The plugin state
 */
GnostrPluginState gnostr_plugin_row_get_state(GnostrPluginRow *self);

/**
 * gnostr_plugin_row_set_has_settings:
 * @self: A #GnostrPluginRow
 * @has_settings: Whether the plugin has a settings page
 *
 * Set whether the plugin has configurable settings.
 * This enables/disables the settings button.
 */
void gnostr_plugin_row_set_has_settings(GnostrPluginRow *self, gboolean has_settings);

/**
 * gnostr_plugin_row_set_status_message:
 * @self: A #GnostrPluginRow
 * @message: (nullable): Status message to display, or %NULL to hide
 *
 * Set a status message (e.g., error message, "needs restart").
 */
void gnostr_plugin_row_set_status_message(GnostrPluginRow *self, const char *message);

/**
 * gnostr_plugin_row_update_from_info:
 * @self: A #GnostrPluginRow
 *
 * Refresh the row display from the current plugin info.
 */
void gnostr_plugin_row_update_from_info(GnostrPluginRow *self);

G_END_DECLS

#endif /* GNOSTR_PLUGIN_ROW_H */
