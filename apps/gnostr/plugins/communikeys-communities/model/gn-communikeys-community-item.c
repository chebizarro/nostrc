#include "gn-communikeys-community-item.h"

struct _GnCommunikeysCommunityItem {
  GObject parent_instance;
  gchar *pubkey;
  gchar *main_relay;
  gchar *description;
  gchar *definition_id;
  gint64 created_at;
  GListStore *sections;
};

G_DEFINE_TYPE(GnCommunikeysCommunityItem, gn_communikeys_community_item,
              G_TYPE_OBJECT)

static void gn_communikeys_community_item_finalize(GObject *object) {
  GnCommunikeysCommunityItem *self = GN_COMMUNIKEYS_COMMUNITY_ITEM(object);
  g_free(self->pubkey);
  g_free(self->main_relay);
  g_free(self->description);
  g_free(self->definition_id);
  g_clear_object(&self->sections);
  G_OBJECT_CLASS(gn_communikeys_community_item_parent_class)->finalize(object);
}
static void gn_communikeys_community_item_class_init(
    GnCommunikeysCommunityItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_communikeys_community_item_finalize;
}
static void gn_communikeys_community_item_init(
    GnCommunikeysCommunityItem *self) {
  self->sections = g_list_store_new(GN_TYPE_COMMUNIKEYS_SECTION_ITEM);
}

GnCommunikeysCommunityItem *gn_communikeys_community_item_new(
    const nostr_communikeys_definition_t *definition,
    const char *definition_id, gint64 created_at) {
  g_return_val_if_fail(definition != NULL && definition->valid, NULL);
  GnCommunikeysCommunityItem *self =
    g_object_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM, NULL);
  self->pubkey = g_strdup(definition->pubkey);
  self->main_relay = definition->relays_len
    ? g_strdup(definition->relays[0]) : NULL;
  self->description = g_strdup(definition->description);
  self->definition_id = g_strdup(definition_id);
  self->created_at = created_at;
  for (gsize i = 0; i < definition->sections_len; i++) {
    g_autoptr(GnCommunikeysSectionItem) section =
      gn_communikeys_section_item_new(definition->pubkey,
                                      &definition->sections[i]);
    g_list_store_append(self->sections, section);
  }
  return self;
}

const char *gn_communikeys_community_item_get_pubkey(
    GnCommunikeysCommunityItem *self) { return self->pubkey; }
const char *gn_communikeys_community_item_get_main_relay(
    GnCommunikeysCommunityItem *self) { return self->main_relay; }
const char *gn_communikeys_community_item_get_description(
    GnCommunikeysCommunityItem *self) { return self->description; }
const char *gn_communikeys_community_item_get_definition_id(
    GnCommunikeysCommunityItem *self) { return self->definition_id; }
guint gn_communikeys_community_item_get_section_count(
    GnCommunikeysCommunityItem *self) {
  return g_list_model_get_n_items(G_LIST_MODEL(self->sections));
}
gint64 gn_communikeys_community_item_get_created_at(
    GnCommunikeysCommunityItem *self) { return self->created_at; }
GListModel *gn_communikeys_community_item_get_sections(
    GnCommunikeysCommunityItem *self) {
  return G_LIST_MODEL(self->sections);
}
GnCommunikeysSectionItem *gn_communikeys_community_item_get_section(
    GnCommunikeysCommunityItem *self, guint position) {
  return g_list_model_get_item(G_LIST_MODEL(self->sections), position);
}
GnCommunikeysSectionItem *gn_communikeys_community_item_find_section(
    GnCommunikeysCommunityItem *self, const char *name) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->sections));
  for (guint i = 0; i < n; i++) {
    GnCommunikeysSectionItem *section =
      g_list_model_get_item(G_LIST_MODEL(self->sections), i);
    if (g_strcmp0(gn_communikeys_section_item_get_name(section), name) == 0)
      return section;
    g_object_unref(section);
  }
  return NULL;
}
