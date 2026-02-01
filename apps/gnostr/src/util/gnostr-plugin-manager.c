/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-manager.c - Plugin Manager for Gnostr
 *
 * Singleton manager for discovering, loading, and managing plugins via libpeas.
 *
 * nostrc-oxt: Initial implementation of libpeas plugin system.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-manager.h"
#include "../gnostr-plugin-api.h"
#include <gio/gio.h>

#ifdef HAVE_LIBPEAS
#include <libpeas.h>
#endif

/* Plugin search paths */
static const char *s_plugin_search_paths[] = {
  NULL, /* User dir: ~/.local/share/gnostr/plugins - set at runtime */
  "/usr/share/gnostr/plugins",
  "/usr/local/share/gnostr/plugins",
  NULL
};

/* GSettings schema for plugin state */
#define PLUGIN_SETTINGS_SCHEMA "org.gnostr.Client.plugins"
#define PLUGIN_SETTINGS_KEY_ENABLED "enabled-plugins"

struct _GnostrPluginManager
{
  GObject parent_instance;

  GtkApplication *app;
  GSettings *settings;

#ifdef HAVE_LIBPEAS
  PeasEngine *engine;
  PeasExtensionSet *extension_set;
#endif

  /* Plugin context (shared with all plugins) */
  GnostrPluginContext *context;

  /* Track loaded plugins */
  GHashTable *loaded_plugins; /* plugin_id -> GnostrPlugin* */

  gboolean initialized;
  gboolean shutdown;
};

G_DEFINE_TYPE(GnostrPluginManager, gnostr_plugin_manager, G_TYPE_OBJECT)

#ifdef HAVE_LIBPEAS
/* Forward declarations */
static void on_extension_added(PeasExtensionSet *set, PeasPluginInfo *info,
                               GObject *exten, gpointer user_data);
static void on_extension_removed(PeasExtensionSet *set, PeasPluginInfo *info,
                                 GObject *exten, gpointer user_data);
#endif

/* Singleton instance */
static GnostrPluginManager *s_default_manager = NULL;

static void
gnostr_plugin_manager_dispose(GObject *obj)
{
  GnostrPluginManager *self = GNOSTR_PLUGIN_MANAGER(obj);

  if (!self->shutdown) {
    gnostr_plugin_manager_shutdown(self);
  }

  g_clear_object(&self->settings);
  g_clear_pointer(&self->loaded_plugins, g_hash_table_unref);

  /* Free plugin context */
  if (self->context) {
    gnostr_plugin_context_free(self->context);
    self->context = NULL;
  }

#ifdef HAVE_LIBPEAS
  g_clear_object(&self->extension_set);
  /* Engine is a singleton, don't unref */
#endif

  G_OBJECT_CLASS(gnostr_plugin_manager_parent_class)->dispose(obj);
}

static void
gnostr_plugin_manager_class_init(GnostrPluginManagerClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  obj_class->dispose = gnostr_plugin_manager_dispose;
}

static void
gnostr_plugin_manager_init(GnostrPluginManager *self)
{
  self->loaded_plugins = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_object_unref);
  self->initialized = FALSE;
  self->shutdown = FALSE;

  /* Try to get settings, but don't fail if schema doesn't exist yet */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(source,
                                PLUGIN_SETTINGS_SCHEMA, TRUE);
    if (schema) {
      self->settings = g_settings_new(PLUGIN_SETTINGS_SCHEMA);
      g_settings_schema_unref(schema);
    }
  }

#ifdef HAVE_LIBPEAS
  self->engine = peas_engine_get_default();
#endif
}

GnostrPluginManager *
gnostr_plugin_manager_get_default(void)
{
  if (!s_default_manager) {
    s_default_manager = g_object_new(GNOSTR_TYPE_PLUGIN_MANAGER, NULL);
  }
  return s_default_manager;
}

void
gnostr_plugin_manager_init_with_app(GnostrPluginManager *manager,
                                    GtkApplication      *app)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));
  g_return_if_fail(GTK_IS_APPLICATION(app));

  if (manager->initialized) {
    g_warning("Plugin manager already initialized");
    return;
  }

  manager->app = app;
  manager->initialized = TRUE;

  /* Create the shared plugin context */
  manager->context = gnostr_plugin_context_new(app, "gnostr");

  g_debug("[PLUGIN] Plugin manager initialized with application");
}

void
gnostr_plugin_manager_discover_plugins(GnostrPluginManager *manager)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));

#ifdef HAVE_LIBPEAS
  if (!manager->engine) {
    g_warning("[PLUGIN] No PeasEngine available");
    return;
  }

  /* Set up user plugin directory */
  char *user_plugin_dir = g_build_filename(g_get_user_data_dir(),
                                           "gnostr", "plugins", NULL);

  /* Add search paths */
  peas_engine_add_search_path(manager->engine, user_plugin_dir, NULL);
  g_debug("[PLUGIN] Added user plugin path: %s", user_plugin_dir);
  g_free(user_plugin_dir);

  for (const char **path = &s_plugin_search_paths[1]; *path; path++) {
    peas_engine_add_search_path(manager->engine, *path, NULL);
    g_debug("[PLUGIN] Added system plugin path: %s", *path);
  }

  /* Add development build plugin directory if defined */
#ifdef GNOSTR_DEV_PLUGIN_DIR
  peas_engine_add_search_path(manager->engine, GNOSTR_DEV_PLUGIN_DIR, NULL);
  g_debug("[PLUGIN] Added dev build plugin path: %s", GNOSTR_DEV_PLUGIN_DIR);
#endif

  /* Check environment variable for additional plugin paths */
  const char *env_plugin_path = g_getenv("GNOSTR_PLUGIN_PATH");
  if (env_plugin_path && *env_plugin_path) {
    /* Support colon-separated paths like PATH */
    char **paths = g_strsplit(env_plugin_path, ":", -1);
    for (char **p = paths; *p; p++) {
      if (*p && **p) {
        peas_engine_add_search_path(manager->engine, *p, NULL);
        g_debug("[PLUGIN] Added env plugin path: %s", *p);
      }
    }
    g_strfreev(paths);
  }

  /* Rescan for plugins */
  peas_engine_rescan_plugins(manager->engine);

  /* Log discovered plugins - PeasEngine is a GListModel in libpeas 2 */
  guint count = g_list_model_get_n_items(G_LIST_MODEL(manager->engine));
  g_debug("[PLUGIN] Discovered %u plugins", count);

  for (guint i = 0; i < count; i++) {
    PeasPluginInfo *info = g_list_model_get_item(G_LIST_MODEL(manager->engine), i);
    if (info) {
      g_debug("[PLUGIN]   - %s: %s",
              peas_plugin_info_get_module_name(info),
              peas_plugin_info_get_name(info));
      g_object_unref(info);
    }
  }
#else
  g_debug("[PLUGIN] libpeas not available - plugin system disabled");
#endif
}

void
gnostr_plugin_manager_load_enabled_plugins(GnostrPluginManager *manager)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));

#ifdef HAVE_LIBPEAS
  if (!manager->engine || !manager->initialized) {
    return;
  }

  /* Get enabled plugins from settings */
  char **enabled = NULL;
  if (manager->settings) {
    enabled = g_settings_get_strv(manager->settings, PLUGIN_SETTINGS_KEY_ENABLED);
  }

  if (!enabled || !*enabled) {
    g_debug("[PLUGIN] No plugins enabled in settings");
    g_strfreev(enabled);
    return;
  }

  /* Create extension set for GnostrPlugin interface */
  if (!manager->extension_set) {
    manager->extension_set = peas_extension_set_new(manager->engine,
                                                    GNOSTR_TYPE_PLUGIN,
                                                    NULL);
    g_signal_connect(manager->extension_set, "extension-added",
                     G_CALLBACK(on_extension_added), manager);
    g_signal_connect(manager->extension_set, "extension-removed",
                     G_CALLBACK(on_extension_removed), manager);
  }

  /* Load each enabled plugin */
  for (char **id = enabled; *id; id++) {
    PeasPluginInfo *info = peas_engine_get_plugin_info(manager->engine, *id);
    if (info && !peas_plugin_info_is_loaded(info)) {
      g_debug("[PLUGIN] Loading enabled plugin: %s", *id);
      peas_engine_load_plugin(manager->engine, info);
    }
  }

  g_strfreev(enabled);
#endif
}

gboolean
gnostr_plugin_manager_enable_plugin(GnostrPluginManager *manager,
                                    const char          *plugin_id,
                                    GError             **error)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager), FALSE);
  g_return_val_if_fail(plugin_id != NULL, FALSE);

#ifdef HAVE_LIBPEAS
  if (!manager->engine) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                "Plugin system not initialized");
    return FALSE;
  }

  PeasPluginInfo *info = peas_engine_get_plugin_info(manager->engine, plugin_id);
  if (!info) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Plugin not found: %s", plugin_id);
    return FALSE;
  }

  /* Create extension set if needed */
  if (!manager->extension_set) {
    manager->extension_set = peas_extension_set_new(manager->engine,
                                                    GNOSTR_TYPE_PLUGIN,
                                                    NULL);
    g_signal_connect(manager->extension_set, "extension-added",
                     G_CALLBACK(on_extension_added), manager);
    g_signal_connect(manager->extension_set, "extension-removed",
                     G_CALLBACK(on_extension_removed), manager);
  }

  /* Load the plugin */
  if (!peas_engine_load_plugin(manager->engine, info)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to load plugin: %s", plugin_id);
    return FALSE;
  }

  /* Update settings */
  if (manager->settings) {
    char **enabled = g_settings_get_strv(manager->settings, PLUGIN_SETTINGS_KEY_ENABLED);
    GPtrArray *new_enabled = g_ptr_array_new();

    /* Add existing */
    if (enabled) {
      for (char **id = enabled; *id; id++) {
        if (g_strcmp0(*id, plugin_id) != 0) {
          g_ptr_array_add(new_enabled, g_strdup(*id));
        }
      }
      g_strfreev(enabled);
    }

    /* Add new */
    g_ptr_array_add(new_enabled, g_strdup(plugin_id));
    g_ptr_array_add(new_enabled, NULL);

    g_settings_set_strv(manager->settings, PLUGIN_SETTINGS_KEY_ENABLED,
                        (const char *const *)new_enabled->pdata);
    g_ptr_array_unref(new_enabled);
  }

  g_debug("[PLUGIN] Enabled plugin: %s", plugin_id);
  return TRUE;
#else
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
              "Plugin system not available (libpeas not found)");
  return FALSE;
#endif
}

void
gnostr_plugin_manager_disable_plugin(GnostrPluginManager *manager,
                                     const char          *plugin_id)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));
  g_return_if_fail(plugin_id != NULL);

#ifdef HAVE_LIBPEAS
  if (!manager->engine) return;

  PeasPluginInfo *info = peas_engine_get_plugin_info(manager->engine, plugin_id);
  if (info && peas_plugin_info_is_loaded(info)) {
    peas_engine_unload_plugin(manager->engine, info);
  }

  /* Update settings */
  if (manager->settings) {
    char **enabled = g_settings_get_strv(manager->settings, PLUGIN_SETTINGS_KEY_ENABLED);
    GPtrArray *new_enabled = g_ptr_array_new();

    if (enabled) {
      for (char **id = enabled; *id; id++) {
        if (g_strcmp0(*id, plugin_id) != 0) {
          g_ptr_array_add(new_enabled, g_strdup(*id));
        }
      }
      g_strfreev(enabled);
    }

    g_ptr_array_add(new_enabled, NULL);
    g_settings_set_strv(manager->settings, PLUGIN_SETTINGS_KEY_ENABLED,
                        (const char *const *)new_enabled->pdata);
    g_ptr_array_unref(new_enabled);
  }

  g_debug("[PLUGIN] Disabled plugin: %s", plugin_id);
#endif
}

gboolean
gnostr_plugin_manager_is_plugin_enabled(GnostrPluginManager *manager,
                                        const char          *plugin_id)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager), FALSE);
  g_return_val_if_fail(plugin_id != NULL, FALSE);

#ifdef HAVE_LIBPEAS
  if (!manager->engine) return FALSE;

  PeasPluginInfo *info = peas_engine_get_plugin_info(manager->engine, plugin_id);
  return info && peas_plugin_info_is_loaded(info);
#else
  return FALSE;
#endif
}

GPtrArray *
gnostr_plugin_manager_get_available_plugins(GnostrPluginManager *manager)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager), NULL);

  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);

#ifdef HAVE_LIBPEAS
  if (manager->engine) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(manager->engine));
    for (guint i = 0; i < n; i++) {
      PeasPluginInfo *info = g_list_model_get_item(G_LIST_MODEL(manager->engine), i);
      if (info) {
        g_ptr_array_add(result, g_strdup(peas_plugin_info_get_module_name(info)));
        g_object_unref(info);
      }
    }
  }
#endif

  return result;
}

gboolean
gnostr_plugin_manager_get_plugin_info(GnostrPluginManager  *manager,
                                      const char           *plugin_id,
                                      const char          **name,
                                      const char          **description,
                                      const char          **version,
                                      const char *const   **authors)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager), FALSE);
  g_return_val_if_fail(plugin_id != NULL, FALSE);

#ifdef HAVE_LIBPEAS
  if (!manager->engine) return FALSE;

  PeasPluginInfo *info = peas_engine_get_plugin_info(manager->engine, plugin_id);
  if (!info) return FALSE;

  if (name) *name = peas_plugin_info_get_name(info);
  if (description) *description = peas_plugin_info_get_description(info);
  if (version) *version = peas_plugin_info_get_version(info);
  if (authors) *authors = peas_plugin_info_get_authors(info);

  return TRUE;
#else
  return FALSE;
#endif
}

void
gnostr_plugin_manager_shutdown(GnostrPluginManager *manager)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));

  if (manager->shutdown) return;
  manager->shutdown = TRUE;

  g_debug("[PLUGIN] Shutting down plugin manager");

#ifdef HAVE_LIBPEAS
  /* Unload all plugins */
  if (manager->engine) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(manager->engine));
    for (guint i = 0; i < n; i++) {
      PeasPluginInfo *info = g_list_model_get_item(G_LIST_MODEL(manager->engine), i);
      if (info) {
        if (peas_plugin_info_is_loaded(info)) {
          peas_engine_unload_plugin(manager->engine, info);
        }
        g_object_unref(info);
      }
    }
  }

  g_clear_object(&manager->extension_set);
#endif

  g_hash_table_remove_all(manager->loaded_plugins);
}

/* Extension callbacks */
#ifdef HAVE_LIBPEAS
static void
on_extension_added(PeasExtensionSet *set,
                   PeasPluginInfo   *info,
                   GObject          *exten,
                   gpointer          user_data)
{
  GnostrPluginManager *manager = GNOSTR_PLUGIN_MANAGER(user_data);
  (void)set;

  const char *id = peas_plugin_info_get_module_name(info);
  g_debug("[PLUGIN] Extension added: %s", id);

  if (GNOSTR_IS_PLUGIN(exten)) {
    GnostrPlugin *plugin = GNOSTR_PLUGIN(exten);

    /* Store reference */
    g_hash_table_insert(manager->loaded_plugins,
                        g_strdup(id),
                        g_object_ref(plugin));

    /* Activate the plugin */
    gnostr_plugin_activate(plugin, manager->context);

    g_debug("[PLUGIN] Activated plugin: %s (%s)",
            gnostr_plugin_get_name(plugin),
            id);
  }
}

static void
on_extension_removed(PeasExtensionSet *set,
                     PeasPluginInfo   *info,
                     GObject          *exten,
                     gpointer          user_data)
{
  GnostrPluginManager *manager = GNOSTR_PLUGIN_MANAGER(user_data);
  (void)set;

  const char *id = peas_plugin_info_get_module_name(info);
  g_debug("[PLUGIN] Extension removed: %s", id);

  if (GNOSTR_IS_PLUGIN(exten)) {
    GnostrPlugin *plugin = GNOSTR_PLUGIN(exten);

    /* Deactivate the plugin */
    gnostr_plugin_deactivate(plugin, manager->context);

    /* Remove reference */
    g_hash_table_remove(manager->loaded_plugins, id);

    g_debug("[PLUGIN] Deactivated plugin: %s", id);
  }
}
#endif

void
gnostr_plugin_manager_set_main_window(GnostrPluginManager *manager,
                                      GtkWindow           *window)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER(manager));

  if (manager->context) {
    gnostr_plugin_context_set_main_window(manager->context, window);
    g_debug("[PLUGIN] Set main window on plugin context");
  }
}

/* ============================================================================
 * Plugin API version check implementation
 * ============================================================================
 */

gboolean
gnostr_plugin_api_check_version(guint required_major, guint required_minor)
{
  if (required_major != GNOSTR_PLUGIN_API_MAJOR_VERSION) {
    return FALSE;
  }
  return GNOSTR_PLUGIN_API_MINOR_VERSION >= required_minor;
}
