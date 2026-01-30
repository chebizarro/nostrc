/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-manager.h - Plugin lifecycle management
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_PLUGIN_MANAGER_H
#define GNOSTR_PLUGIN_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include <libpeas.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PLUGIN_MANAGER (gnostr_plugin_manager_get_type())
G_DECLARE_FINAL_TYPE(GnostrPluginManager, gnostr_plugin_manager, GNOSTR, PLUGIN_MANAGER, GObject)

/**
 * GnostrPluginManagerError:
 * @GNOSTR_PLUGIN_MANAGER_ERROR_LOAD_FAILED: Failed to load plugin
 * @GNOSTR_PLUGIN_MANAGER_ERROR_UNLOAD_FAILED: Failed to unload plugin
 * @GNOSTR_PLUGIN_MANAGER_ERROR_NOT_FOUND: Plugin not found
 * @GNOSTR_PLUGIN_MANAGER_ERROR_ALREADY_LOADED: Plugin already loaded
 * @GNOSTR_PLUGIN_MANAGER_ERROR_INCOMPATIBLE: Plugin API version incompatible
 * @GNOSTR_PLUGIN_MANAGER_ERROR_DEPENDENCY: Missing plugin dependency
 * @GNOSTR_PLUGIN_MANAGER_ERROR_INVALID: Invalid plugin file
 *
 * Plugin manager error codes.
 */
typedef enum
{
  GNOSTR_PLUGIN_MANAGER_ERROR_LOAD_FAILED,
  GNOSTR_PLUGIN_MANAGER_ERROR_UNLOAD_FAILED,
  GNOSTR_PLUGIN_MANAGER_ERROR_NOT_FOUND,
  GNOSTR_PLUGIN_MANAGER_ERROR_ALREADY_LOADED,
  GNOSTR_PLUGIN_MANAGER_ERROR_INCOMPATIBLE,
  GNOSTR_PLUGIN_MANAGER_ERROR_DEPENDENCY,
  GNOSTR_PLUGIN_MANAGER_ERROR_INVALID,
} GnostrPluginManagerError;

#define GNOSTR_PLUGIN_MANAGER_ERROR (gnostr_plugin_manager_error_quark())
GQuark gnostr_plugin_manager_error_quark(void);

/* ============================================================================
 * SINGLETON ACCESS
 * ============================================================================ */

/**
 * gnostr_plugin_manager_get_default:
 *
 * Get the default plugin manager instance.
 * Creates the manager on first call.
 *
 * Returns: (transfer none): The default plugin manager
 */
GnostrPluginManager *gnostr_plugin_manager_get_default(void);

/* ============================================================================
 * PLUGIN DISCOVERY & LOADING
 * ============================================================================ */

/**
 * gnostr_plugin_manager_get_engine:
 * @self: A #GnostrPluginManager
 *
 * Get the underlying PeasEngine.
 * Use this for advanced operations not covered by the manager API.
 *
 * Returns: (transfer none): The PeasEngine
 */
PeasEngine *gnostr_plugin_manager_get_engine(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_rescan:
 * @self: A #GnostrPluginManager
 *
 * Rescan plugin directories for new plugins.
 * Emits "plugins-changed" signal if new plugins are found.
 */
void gnostr_plugin_manager_rescan(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_get_plugins:
 * @self: A #GnostrPluginManager
 *
 * Get the list of all discovered plugins.
 *
 * Returns: (transfer none) (element-type PeasPluginInfo):
 *          List of plugin infos. The list is owned by the engine.
 */
GListModel *gnostr_plugin_manager_get_plugins(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_get_plugin_info:
 * @self: A #GnostrPluginManager
 * @plugin_id: The plugin module name
 *
 * Get info for a specific plugin by ID.
 *
 * Returns: (transfer none) (nullable): The plugin info, or %NULL if not found
 */
PeasPluginInfo *gnostr_plugin_manager_get_plugin_info(GnostrPluginManager *self,
                                                      const char          *plugin_id);

/* ============================================================================
 * PLUGIN LIFECYCLE
 * ============================================================================ */

/**
 * gnostr_plugin_manager_load_plugin:
 * @self: A #GnostrPluginManager
 * @info: The plugin to load
 * @error: (out) (optional): Return location for error
 *
 * Load a plugin. This makes the plugin's types available
 * but does not activate it.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_load_plugin(GnostrPluginManager *self,
                                           PeasPluginInfo      *info,
                                           GError             **error);

/**
 * gnostr_plugin_manager_unload_plugin:
 * @self: A #GnostrPluginManager
 * @info: The plugin to unload
 * @error: (out) (optional): Return location for error
 *
 * Unload a plugin. The plugin must be deactivated first.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_unload_plugin(GnostrPluginManager *self,
                                             PeasPluginInfo      *info,
                                             GError             **error);

/**
 * gnostr_plugin_manager_enable_plugin:
 * @self: A #GnostrPluginManager
 * @info: The plugin to enable
 * @error: (out) (optional): Return location for error
 *
 * Enable a plugin. This loads (if needed) and activates the plugin.
 * The plugin's activate() method is called.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_enable_plugin(GnostrPluginManager *self,
                                             PeasPluginInfo      *info,
                                             GError             **error);

/**
 * gnostr_plugin_manager_disable_plugin:
 * @self: A #GnostrPluginManager
 * @info: The plugin to disable
 * @error: (out) (optional): Return location for error
 *
 * Disable a plugin. The plugin's deactivate() method is called.
 * The plugin remains loaded but inactive.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_disable_plugin(GnostrPluginManager *self,
                                              PeasPluginInfo      *info,
                                              GError             **error);

/**
 * gnostr_plugin_manager_is_plugin_loaded:
 * @self: A #GnostrPluginManager
 * @info: The plugin to check
 *
 * Check if a plugin is loaded.
 *
 * Returns: %TRUE if loaded
 */
gboolean gnostr_plugin_manager_is_plugin_loaded(GnostrPluginManager *self,
                                                PeasPluginInfo      *info);

/**
 * gnostr_plugin_manager_is_plugin_enabled:
 * @self: A #GnostrPluginManager
 * @info: The plugin to check
 *
 * Check if a plugin is enabled (active).
 *
 * Returns: %TRUE if enabled
 */
gboolean gnostr_plugin_manager_is_plugin_enabled(GnostrPluginManager *self,
                                                 PeasPluginInfo      *info);

/* ============================================================================
 * PLUGIN INSTALLATION
 * ============================================================================ */

/**
 * gnostr_plugin_manager_install_from_file:
 * @self: A #GnostrPluginManager
 * @file: Path to the .so or .plugin file
 * @error: (out) (optional): Return location for error
 *
 * Install a plugin from a local file.
 * The plugin is copied to the user plugins directory.
 *
 * Returns: (transfer none) (nullable): The installed plugin info, or %NULL on error
 */
PeasPluginInfo *gnostr_plugin_manager_install_from_file(GnostrPluginManager *self,
                                                        GFile               *file,
                                                        GError             **error);

/**
 * gnostr_plugin_manager_uninstall:
 * @self: A #GnostrPluginManager
 * @info: The plugin to uninstall
 * @error: (out) (optional): Return location for error
 *
 * Uninstall a plugin. The plugin must be disabled first.
 * Only user-installed plugins can be uninstalled.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_plugin_manager_uninstall(GnostrPluginManager *self,
                                         PeasPluginInfo      *info,
                                         GError             **error);

/**
 * gnostr_plugin_manager_is_user_plugin:
 * @self: A #GnostrPluginManager
 * @info: The plugin to check
 *
 * Check if a plugin is installed in the user directory
 * (as opposed to system-wide).
 *
 * Returns: %TRUE if user-installed
 */
gboolean gnostr_plugin_manager_is_user_plugin(GnostrPluginManager *self,
                                              PeasPluginInfo      *info);

/* ============================================================================
 * PLUGIN DIRECTORIES
 * ============================================================================ */

/**
 * gnostr_plugin_manager_get_user_plugins_dir:
 * @self: A #GnostrPluginManager
 *
 * Get the user plugins directory path.
 * This is typically ~/.local/share/gnostr/plugins/
 *
 * Returns: (transfer none): The user plugins directory path
 */
const char *gnostr_plugin_manager_get_user_plugins_dir(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_get_system_plugins_dir:
 * @self: A #GnostrPluginManager
 *
 * Get the system plugins directory path.
 *
 * Returns: (transfer none): The system plugins directory path
 */
const char *gnostr_plugin_manager_get_system_plugins_dir(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_open_plugins_folder:
 * @self: A #GnostrPluginManager
 *
 * Open the user plugins folder in the system file manager.
 */
void gnostr_plugin_manager_open_plugins_folder(GnostrPluginManager *self);

/* ============================================================================
 * EXTENSION ACCESS
 * ============================================================================ */

/**
 * gnostr_plugin_manager_get_extension:
 * @self: A #GnostrPluginManager
 * @info: The plugin
 * @extension_type: The extension interface type
 *
 * Get an extension instance from a loaded plugin.
 *
 * Returns: (transfer full) (nullable): The extension, or %NULL if not provided.
 *          Free with g_object_unref().
 */
GObject *gnostr_plugin_manager_get_extension(GnostrPluginManager *self,
                                             PeasPluginInfo      *info,
                                             GType                extension_type);

/**
 * gnostr_plugin_manager_create_extension_set:
 * @self: A #GnostrPluginManager
 * @extension_type: The extension interface type
 *
 * Create an extension set for all plugins implementing the interface.
 * The set automatically tracks plugin loading/unloading.
 *
 * Returns: (transfer full): A new extension set. Free with g_object_unref().
 */
PeasExtensionSet *gnostr_plugin_manager_create_extension_set(GnostrPluginManager *self,
                                                             GType                extension_type);

/* ============================================================================
 * SETTINGS PERSISTENCE
 * ============================================================================ */

/**
 * gnostr_plugin_manager_save_enabled_plugins:
 * @self: A #GnostrPluginManager
 *
 * Save the current list of enabled plugins to settings.
 */
void gnostr_plugin_manager_save_enabled_plugins(GnostrPluginManager *self);

/**
 * gnostr_plugin_manager_restore_enabled_plugins:
 * @self: A #GnostrPluginManager
 *
 * Restore and enable plugins from the saved settings.
 * Called at startup after plugin discovery.
 */
void gnostr_plugin_manager_restore_enabled_plugins(GnostrPluginManager *self);

G_END_DECLS

#endif /* GNOSTR_PLUGIN_MANAGER_H */
