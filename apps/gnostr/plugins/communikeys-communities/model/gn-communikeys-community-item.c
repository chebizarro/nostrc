#include "gn-communikeys-community-item.h"
struct _GnCommunikeysCommunityItem {
  GObject parent_instance;
  gchar *pubkey;
  gchar *main_relay;
  gchar *description;
  guint section_count;
  gint64 created_at;
};
G_DEFINE_TYPE(GnCommunikeysCommunityItem, gn_communikeys_community_item, G_TYPE_OBJECT)
static void gn_communikeys_community_item_finalize(GObject *object) {
  GnCommunikeysCommunityItem *self = GN_COMMUNIKEYS_COMMUNITY_ITEM(object);
  g_free(self->pubkey); g_free(self->main_relay); g_free(self->description);
  G_OBJECT_CLASS(gn_communikeys_community_item_parent_class)->finalize(object);
}
static void gn_communikeys_community_item_class_init(GnCommunikeysCommunityItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_communikeys_community_item_finalize;
}
static void gn_communikeys_community_item_init(GnCommunikeysCommunityItem *self) { (void)self; }
GnCommunikeysCommunityItem *gn_communikeys_community_item_new(
    const char *pubkey, const char *main_relay, const char *description,
    guint section_count, gint64 created_at) {
  GnCommunikeysCommunityItem *self = g_object_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM, NULL);
  self->pubkey = g_strdup(pubkey); self->main_relay = g_strdup(main_relay);
  self->description = g_strdup(description); self->section_count = section_count;
  self->created_at = created_at; return self;
}
const char *gn_communikeys_community_item_get_pubkey(GnCommunikeysCommunityItem *self) { return self->pubkey; }
const char *gn_communikeys_community_item_get_main_relay(GnCommunikeysCommunityItem *self) { return self->main_relay; }
const char *gn_communikeys_community_item_get_description(GnCommunikeysCommunityItem *self) { return self->description; }
guint gn_communikeys_community_item_get_section_count(GnCommunikeysCommunityItem *self) { return self->section_count; }
gint64 gn_communikeys_community_item_get_created_at(GnCommunikeysCommunityItem *self) { return self->created_at; }
