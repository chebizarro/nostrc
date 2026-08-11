#include "concord-communities-plugin.h"
#include "gn-concord-community-service.h"
#include "ui/gn-concord-communities-panel.h"

#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <nip_concord.h>

#define PANEL_ID "concord-communities"

struct _ConcordCommunitiesPlugin {
  GObject parent_instance;
  GnostrPluginContext *context;
  GnConcordCommunityService *service;
};

static void plugin_iface_init(GnostrPluginInterface *iface);
static void ui_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(ConcordCommunitiesPlugin, concord_communities_plugin,
                        G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, plugin_iface_init)
  G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, ui_iface_init))

static void concord_communities_plugin_dispose(GObject *object) {
  ConcordCommunitiesPlugin *self = CONCORD_COMMUNITIES_PLUGIN(object);
  if (self->service) gn_concord_community_service_shutdown(self->service);
  g_clear_object(&self->service);
  self->context = NULL;
  G_OBJECT_CLASS(concord_communities_plugin_parent_class)->dispose(object);
}

static void concord_communities_plugin_class_init(
    ConcordCommunitiesPluginClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = concord_communities_plugin_dispose;
}

static void concord_communities_plugin_init(ConcordCommunitiesPlugin *self) {
  (void)self;
}

static void activate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  ConcordCommunitiesPlugin *self = CONCORD_COMMUNITIES_PLUGIN(plugin);
  self->context = context;
  if (!self->service)
    self->service = gn_concord_community_service_new(context);
}

static void deactivate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  (void)context;
  ConcordCommunitiesPlugin *self = CONCORD_COMMUNITIES_PLUGIN(plugin);
  if (self->service) gn_concord_community_service_shutdown(self->service);
  g_clear_object(&self->service);
  self->context = NULL;
}

static const char *get_name(GnostrPlugin *plugin) {
  (void)plugin;
  return "Concord Communities";
}

static const char *get_description(GnostrPlugin *plugin) {
  (void)plugin;
  return "End-to-end encrypted communities over private streams "
         "(NIP-CAS-0008, CORD-01/02/03/05).";
}

static const char * const *get_authors(GnostrPlugin *plugin) {
  (void)plugin;
  static const char *authors[] = { "Gnostr Contributors", NULL };
  return authors;
}

static const char *get_version(GnostrPlugin *plugin) {
  (void)plugin;
  return "0.1";
}

static const int *get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds) {
  (void)plugin;
  /* The relay-visible surface of a Concord community is only this: the stream
   * wraps and the two addressable/replaceable documents. Every functional
   * kind is an inner rumor that never appears on the wire (NIP-CAS-0008). */
  static const int kinds[] = {
    CONCORD_STREAM_WRAP, CONCORD_EPHEMERAL_STREAM_WRAP,
    CONCORD_INVITE_BUNDLE, CONCORD_COMMUNITY_LIST, CONCORD_INVITE_LIST
  };
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void plugin_iface_init(GnostrPluginInterface *iface) {
  iface->activate = activate;
  iface->deactivate = deactivate;
  iface->get_name = get_name;
  iface->get_description = get_description;
  iface->get_authors = get_authors;
  iface->get_version = get_version;
  iface->get_supported_kinds = get_supported_kinds;
}

static GList *get_sidebar_items(GnostrUIExtension *extension,
                                GnostrPluginContext *context) {
  (void)extension;
  (void)context;
  GnostrSidebarItem *item = gnostr_sidebar_item_new(
    PANEL_ID, "Concord", "channel-secure-symbolic");
  gnostr_sidebar_item_set_requires_auth(item, FALSE);
  gnostr_sidebar_item_set_position(item, 30);
  return g_list_append(NULL, item);
}

static GtkWidget *create_panel(GnostrUIExtension *extension,
                               GnostrPluginContext *context,
                               const char *panel_id) {
  if (g_strcmp0(panel_id, PANEL_ID) != 0) return NULL;
  ConcordCommunitiesPlugin *self = CONCORD_COMMUNITIES_PLUGIN(extension);
  if (!self->service)
    self->service = gn_concord_community_service_new(context);
  self->context = context;
  return GTK_WIDGET(gn_concord_communities_panel_new(self->service));
}

static void ui_iface_init(GnostrUIExtensionInterface *iface) {
  iface->get_sidebar_items = get_sidebar_items;
  iface->create_panel_widget = create_panel;
}

G_MODULE_EXPORT void peas_register_types(PeasObjectModule *module) {
  peas_object_module_register_extension_type(
    module, GNOSTR_TYPE_PLUGIN, CONCORD_TYPE_COMMUNITIES_PLUGIN);
  peas_object_module_register_extension_type(
    module, GNOSTR_TYPE_UI_EXTENSION, CONCORD_TYPE_COMMUNITIES_PLUGIN);
}
