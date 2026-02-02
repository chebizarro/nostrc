/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-manager.h - Plugin Manager for Gnostr
 *
 * Singleton manager for discovering, loading, and managing plugins via libpeas.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_PLUGIN_MANAGER_H
#define GNOSTR_PLUGIN_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PLUGIN_MANAGER (gnostr_plugin_manager_get_type())
G_DECLARE_FINAL_TYPE(GnostrPluginManager, gnostr_plugin_manager, GNOSTR, PLUGIN_MANAGER, GObject)

/**
 * gnostr_plugin_manager_get_default:
 *
 * Get the singleton plugin manager instance.
 *
 * Returns: (transfer none): The plugin manager
 */
GnostrPluginManager *gnostr_plugin_manager_get_default(void);

/**
 * gnostr_plugin_manager_init_with_app:
 * @manager: The plugin manager
 * @app: The GtkApplication instance
 *
 * Initialize the plugin manager with the application.
 * This must be called before loading plugins.
 */
void gnostr_plugin_manager_init_with_app(GnostrPluginManager *manager,
                                         GtkApplication      *app);

/**
 * gnostr_plugin_manager_discover_plugins:
 * @manager: The plugin manager
 *
 * Discover plugins from standard search paths:
 * - ~/.local/share/gnostr/plugins/
 * - /usr/share/gnostr/plugins/
 * - ${prefix}/share/gnostr/plugins/
 */
void gnostr_plugin_manager_discover_plugins(GnostrPluginManager *manager);

/**
 * gnostr_plugin_manager_load_enabled_plugins:
 * @manager: The plugin manager
 *
 * Load all plugins that are enabled in GSettings.
 */
void gnostr_plugin_manager_load_enabled_plugins(GnostrPluginManager *manager);

/**
 * gnostr_plugin_manager_enable_plugin:
 * @manager: The plugin manager
 * @plugin_id: Plugin identifier (module name)
 * @error: Return location for error
 *
 * Enable and load a plugin.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_enable_plugin(GnostrPluginManager *manager,
                                             const char          *plugin_id,
                                             GError             **error);

/**
 * gnostr_plugin_manager_disable_plugin:
 * @manager: The plugin manager
 * @plugin_id: Plugin identifier
 *
 * Disable and unload a plugin.
 */
void gnostr_plugin_manager_disable_plugin(GnostrPluginManager *manager,
                                          const char          *plugin_id);

/**
 * gnostr_plugin_manager_is_plugin_enabled:
 * @manager: The plugin manager
 * @plugin_id: Plugin identifier
 *
 * Check if a plugin is enabled.
 *
 * Returns: %TRUE if enabled
 */
gboolean gnostr_plugin_manager_is_plugin_enabled(GnostrPluginManager *manager,
                                                 const char          *plugin_id);

/**
 * gnostr_plugin_manager_get_available_plugins:
 * @manager: The plugin manager
 *
 * Get list of discovered plugin IDs.
 *
 * Returns: (transfer full) (element-type utf8): Array of plugin IDs.
 *          Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_plugin_manager_get_available_plugins(GnostrPluginManager *manager);

/**
 * gnostr_plugin_manager_get_plugin_info:
 * @manager: The plugin manager
 * @plugin_id: Plugin identifier
 * @name: (out) (optional) (transfer none): Plugin name
 * @description: (out) (optional) (transfer none): Plugin description
 * @version: (out) (optional) (transfer none): Plugin version
 * @authors: (out) (optional) (transfer none): Plugin authors
 *
 * Get metadata for a plugin.
 *
 * Returns: %TRUE if plugin found
 */
gboolean gnostr_plugin_manager_get_plugin_info(GnostrPluginManager  *manager,
                                               const char           *plugin_id,
                                               const char          **name,
                                               const char          **description,
                                               const char          **version,
                                               const char *const   **authors);

/**
 * gnostr_plugin_manager_shutdown:
 * @manager: The plugin manager
 *
 * Deactivate and unload all plugins. Call before application exit.
 */
void gnostr_plugin_manager_shutdown(GnostrPluginManager *manager);

/**
 * gnostr_plugin_manager_set_main_window:
 * @manager: The plugin manager
 * @window: The main #GtkWindow
 *
 * Set the main window for the plugin context.
 * Call this when the main window is created.
 */
void gnostr_plugin_manager_set_main_window(GnostrPluginManager *manager,
                                           GtkWindow           *window);

/**
 * gnostr_plugin_manager_get_plugin_settings_widget:
 * @manager: The plugin manager
 * @plugin_id: Plugin identifier
 *
 * Get the settings page widget for a plugin.
 * The plugin must be loaded and implement GnostrUIExtension.
 *
 * Returns: (transfer full) (nullable): Settings widget, or %NULL
 */
GtkWidget *gnostr_plugin_manager_get_plugin_settings_widget(GnostrPluginManager *manager,
                                                            const char          *plugin_id);

/**
 * gnostr_plugin_manager_dispatch_action:
 * @manager: The plugin manager
 * @plugin_id: Target plugin identifier (e.g., "nip34-git")
 * @action_name: Action to dispatch (e.g., "open-git-client")
 * @parameter: (nullable): Action parameter as GVariant
 *
 * Dispatch an action to a specific plugin.
 * The plugin must be loaded and have registered the action.
 *
 * Returns: %TRUE if the action was handled, %FALSE otherwise
 */
gboolean gnostr_plugin_manager_dispatch_action(GnostrPluginManager *manager,
                                                const char          *plugin_id,
                                                const char          *action_name,
                                                GVariant            *parameter);

G_END_DECLS

#endif /* GNOSTR_PLUGIN_MANAGER_H */
