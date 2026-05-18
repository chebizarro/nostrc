/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip29-groups-plugin.c - NIP-29 relay groups plugin
 */

#include "nip29-groups-plugin.h"
#include "gn-nip29-group-service.h"
#include "ui/gn-nip29-groups-panel.h"

#include <gnostr-plugin-api.h>
#include <libpeas.h>

#define PANEL_ID_NIP29_GROUPS "nip29-groups"

struct _Nip29GroupsPlugin
{
  GObject parent_instance;

  GnostrPluginContext  *context;
  GnNip29GroupService  *service;
  gboolean              active;
};

static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip29GroupsPlugin, nip29_groups_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN,
                                              gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION,
                                              gnostr_ui_extension_iface_init))

static void
nip29_groups_plugin_dispose(GObject *object)
{
  Nip29GroupsPlugin *self = NIP29_GROUPS_PLUGIN(object);

  if (self->service != NULL)
    gn_nip29_group_service_shutdown(self->service);
  g_clear_object(&self->service);
  self->context = NULL;

  G_OBJECT_CLASS(nip29_groups_plugin_parent_class)->dispose(object);
}

static void
nip29_groups_plugin_class_init(Nip29GroupsPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip29_groups_plugin_dispose;
}

static void
nip29_groups_plugin_init(Nip29GroupsPlugin *self)
{
  self->active = FALSE;
}

static void
nip29_groups_plugin_activate(GnostrPlugin        *plugin,
                             GnostrPluginContext *context)
{
  Nip29GroupsPlugin *self = NIP29_GROUPS_PLUGIN(plugin);

  g_info("NIP-29 Groups plugin: activating");

  self->context = context;
  self->active = TRUE;

  if (self->service == NULL)
    self->service = gn_nip29_group_service_new(context);

  gn_nip29_group_service_set_current_pubkey(
    self->service,
    gnostr_plugin_context_get_user_pubkey(context));
}

static void
nip29_groups_plugin_deactivate(GnostrPlugin        *plugin,
                               GnostrPluginContext *context)
{
  Nip29GroupsPlugin *self = NIP29_GROUPS_PLUGIN(plugin);

  g_info("NIP-29 Groups plugin: deactivating");

  if (self->service != NULL)
    gn_nip29_group_service_shutdown(self->service);
  g_clear_object(&self->service);

  self->context = NULL;
  self->active = FALSE;
}

static const char *
nip29_groups_plugin_get_name(GnostrPlugin *plugin)
{
  return "NIP-29 Relay Groups";
}

static const char *
nip29_groups_plugin_get_description(GnostrPlugin *plugin)
{
  return "Relay-authoritative NIP-29 group chat support.";
}

static const char * const *
nip29_groups_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  return authors;
}

static const char *
nip29_groups_plugin_get_version(GnostrPlugin *plugin)
{
  return "0.1";
}

static const int *
nip29_groups_plugin_get_supported_kinds(GnostrPlugin *plugin,
                                        gsize         *n_kinds)
{
  if (n_kinds != NULL)
    *n_kinds = 0;
  return NULL;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip29_groups_plugin_activate;
  iface->deactivate = nip29_groups_plugin_deactivate;
  iface->get_name = nip29_groups_plugin_get_name;
  iface->get_description = nip29_groups_plugin_get_description;
  iface->get_authors = nip29_groups_plugin_get_authors;
  iface->get_version = nip29_groups_plugin_get_version;
  iface->get_supported_kinds = nip29_groups_plugin_get_supported_kinds;
}

static GList *
nip29_groups_get_sidebar_items(GnostrUIExtension   *extension,
                               GnostrPluginContext *context)
{
  GList *items = NULL;

  GnostrSidebarItem *item = gnostr_sidebar_item_new(PANEL_ID_NIP29_GROUPS,
                                                    "Groups",
                                                    "system-users-symbolic");
  gnostr_sidebar_item_set_requires_auth(item, FALSE);
  gnostr_sidebar_item_set_position(item, 28);
  items = g_list_append(items, item);

  return items;
}

static GtkWidget *
nip29_groups_create_panel_widget(GnostrUIExtension   *extension,
                                 GnostrPluginContext *context,
                                 const char          *panel_id)
{
  if (g_strcmp0(panel_id, PANEL_ID_NIP29_GROUPS) != 0)
    return NULL;

  Nip29GroupsPlugin *self = NIP29_GROUPS_PLUGIN(extension);
  if (self->service == NULL)
    self->service = gn_nip29_group_service_new(context);

  self->context = context;
  gn_nip29_group_service_set_current_pubkey(
    self->service,
    gnostr_plugin_context_get_user_pubkey(context));

  /* Retrieve optional navigation view from the plugin context */
  GtkWidget *nav_widget = gnostr_plugin_context_get_navigation_view(context);
  AdwNavigationView *nav = NULL;
  if (nav_widget != NULL && ADW_IS_NAVIGATION_VIEW(nav_widget))
    nav = ADW_NAVIGATION_VIEW(nav_widget);

  GnNip29GroupsPanel *panel = gn_nip29_groups_panel_new(
    self->service, nav, context);
  return GTK_WIDGET(panel);
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->get_sidebar_items = nip29_groups_get_sidebar_items;
  iface->create_panel_widget = nip29_groups_create_panel_widget;
}

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_PLUGIN,
                                             NIP29_TYPE_GROUPS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_UI_EXTENSION,
                                             NIP29_TYPE_GROUPS_PLUGIN);
}
