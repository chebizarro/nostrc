#include "communikeys-communities-plugin.h"
#include "gn-communikeys-community-service.h"
#include "ui/gn-communikeys-communities-panel.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

#define PANEL_ID "communikeys-communities"

struct _CommunikeysCommunitiesPlugin {
  GObject parent_instance;
  GnostrPluginContext *context;
  GnCommunikeysCommunityService *service;
};

static void plugin_iface_init(GnostrPluginInterface *iface);
static void ui_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(CommunikeysCommunitiesPlugin,
                        communikeys_communities_plugin, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, plugin_iface_init)
  G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, ui_iface_init))

static void communikeys_communities_plugin_dispose(GObject *object) {
  CommunikeysCommunitiesPlugin *self =
    COMMUNIKEYS_COMMUNITIES_PLUGIN(object);
  if (self->service) gn_communikeys_community_service_shutdown(self->service);
  g_clear_object(&self->service);
  self->context = NULL;
  G_OBJECT_CLASS(communikeys_communities_plugin_parent_class)->dispose(object);
}
static void communikeys_communities_plugin_class_init(
    CommunikeysCommunitiesPluginClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = communikeys_communities_plugin_dispose;
}
static void communikeys_communities_plugin_init(
    CommunikeysCommunitiesPlugin *self) { (void)self; }

static void activate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  CommunikeysCommunitiesPlugin *self =
    COMMUNIKEYS_COMMUNITIES_PLUGIN(plugin);
  self->context = context;
  if (!self->service)
    self->service = gn_communikeys_community_service_new(context);
}
static void deactivate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  (void)context;
  CommunikeysCommunitiesPlugin *self =
    COMMUNIKEYS_COMMUNITIES_PLUGIN(plugin);
  if (self->service) gn_communikeys_community_service_shutdown(self->service);
  g_clear_object(&self->service);
  self->context = NULL;
}
static const char *get_name(GnostrPlugin *plugin) {
  (void)plugin; return "Communikeys Communities";
}
static const char *get_description(GnostrPlugin *plugin) {
  (void)plugin;
  return "Pubkey-owned communities using NIP-CAS-0007 profile-list access.";
}
static const char * const *get_authors(GnostrPlugin *plugin) {
  (void)plugin;
  static const char *authors[] = { "Gnostr Contributors", NULL };
  return authors;
}
static const char *get_version(GnostrPlugin *plugin) {
  (void)plugin; return "0.1";
}
static const int *get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds) {
  (void)plugin;
  static const int kinds[] = { 9, 11, 10222, 30000, 30222 };
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}
static void plugin_iface_init(GnostrPluginInterface *iface) {
  iface->activate = activate; iface->deactivate = deactivate;
  iface->get_name = get_name; iface->get_description = get_description;
  iface->get_authors = get_authors; iface->get_version = get_version;
  iface->get_supported_kinds = get_supported_kinds;
}
static GList *get_sidebar_items(GnostrUIExtension *extension,
                                GnostrPluginContext *context) {
  (void)extension; (void)context;
  GnostrSidebarItem *item = gnostr_sidebar_item_new(
    PANEL_ID, "Communities", "system-users-symbolic");
  gnostr_sidebar_item_set_requires_auth(item, FALSE);
  gnostr_sidebar_item_set_position(item, 29);
  return g_list_append(NULL, item);
}
static GtkWidget *create_panel(GnostrUIExtension *extension,
                               GnostrPluginContext *context,
                               const char *panel_id) {
  if (g_strcmp0(panel_id, PANEL_ID) != 0) return NULL;
  CommunikeysCommunitiesPlugin *self =
    COMMUNIKEYS_COMMUNITIES_PLUGIN(extension);
  if (!self->service)
    self->service = gn_communikeys_community_service_new(context);
  self->context = context;
  return GTK_WIDGET(gn_communikeys_communities_panel_new(self->service));
}
static void ui_iface_init(GnostrUIExtensionInterface *iface) {
  iface->get_sidebar_items = get_sidebar_items;
  iface->create_panel_widget = create_panel;
}
G_MODULE_EXPORT void peas_register_types(PeasObjectModule *module) {
  peas_object_module_register_extension_type(
    module, GNOSTR_TYPE_PLUGIN, COMMUNIKEYS_TYPE_COMMUNITIES_PLUGIN);
  peas_object_module_register_extension_type(
    module, GNOSTR_TYPE_UI_EXTENSION, COMMUNIKEYS_TYPE_COMMUNITIES_PLUGIN);
}
