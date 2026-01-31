/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-api.c - Plugin API implementation
 *
 * Implementation of GObject interfaces for the plugin system.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-api.h"

/* ============================================================================
 * GNOSTR_PLUGIN INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrPlugin, gnostr_plugin, G_TYPE_OBJECT)

static void
gnostr_plugin_default_init(GnostrPluginInterface *iface)
{
  (void)iface;
  /* No default implementations - all methods are abstract */
}

void
gnostr_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *context)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN(plugin));

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->activate)
    iface->activate(plugin, context);
}

void
gnostr_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *context)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN(plugin));

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->deactivate)
    iface->deactivate(plugin, context);
}

const char *
gnostr_plugin_get_name(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_name)
    return iface->get_name(plugin);
  return NULL;
}

const char *
gnostr_plugin_get_description(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_description)
    return iface->get_description(plugin);
  return NULL;
}

const char *const *
gnostr_plugin_get_authors(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_authors)
    return iface->get_authors(plugin);
  return NULL;
}

const char *
gnostr_plugin_get_version(GnostrPlugin *plugin)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_version)
    return iface->get_version(plugin);
  return NULL;
}

const int *
gnostr_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN(plugin), NULL);

  GnostrPluginInterface *iface = GNOSTR_PLUGIN_GET_IFACE(plugin);
  if (iface->get_supported_kinds)
    return iface->get_supported_kinds(plugin, n_kinds);

  if (n_kinds) *n_kinds = 0;
  return NULL;
}

/* ============================================================================
 * GNOSTR_EVENT_HANDLER INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrEventHandler, gnostr_event_handler, G_TYPE_OBJECT)

static void
gnostr_event_handler_default_init(GnostrEventHandlerInterface *iface)
{
  (void)iface;
}

gboolean
gnostr_event_handler_handle_event(GnostrEventHandler  *handler,
                                  GnostrPluginContext *context,
                                  GnostrPluginEvent   *event)
{
  g_return_val_if_fail(GNOSTR_IS_EVENT_HANDLER(handler), FALSE);

  GnostrEventHandlerInterface *iface = GNOSTR_EVENT_HANDLER_GET_IFACE(handler);
  if (iface->handle_event)
    return iface->handle_event(handler, context, event);
  return FALSE;
}

gboolean
gnostr_event_handler_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  g_return_val_if_fail(GNOSTR_IS_EVENT_HANDLER(handler), FALSE);

  GnostrEventHandlerInterface *iface = GNOSTR_EVENT_HANDLER_GET_IFACE(handler);
  if (iface->can_handle_kind)
    return iface->can_handle_kind(handler, kind);
  return FALSE;
}

/* ============================================================================
 * GNOSTR_UI_EXTENSION INTERFACE
 * ============================================================================ */

G_DEFINE_INTERFACE(GnostrUIExtension, gnostr_ui_extension, G_TYPE_OBJECT)

static void
gnostr_ui_extension_default_init(GnostrUIExtensionInterface *iface)
{
  (void)iface;
}

GList *
gnostr_ui_extension_create_menu_items(GnostrUIExtension     *extension,
                                      GnostrPluginContext   *context,
                                      GnostrUIExtensionPoint point,
                                      gpointer               target_data)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_menu_items)
    return iface->create_menu_items(extension, context, point, target_data);
  return NULL;
}

GtkWidget *
gnostr_ui_extension_create_settings_page(GnostrUIExtension   *extension,
                                         GnostrPluginContext *context)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_settings_page)
    return iface->create_settings_page(extension, context);
  return NULL;
}

GtkWidget *
gnostr_ui_extension_create_note_decoration(GnostrUIExtension   *extension,
                                           GnostrPluginContext *context,
                                           GnostrPluginEvent   *event)
{
  g_return_val_if_fail(GNOSTR_IS_UI_EXTENSION(extension), NULL);

  GnostrUIExtensionInterface *iface = GNOSTR_UI_EXTENSION_GET_IFACE(extension);
  if (iface->create_note_decoration)
    return iface->create_note_decoration(extension, context, event);
  return NULL;
}

/* ============================================================================
 * ERROR DOMAIN
 * ============================================================================ */

G_DEFINE_QUARK(gnostr-plugin-error-quark, gnostr_plugin_error)
