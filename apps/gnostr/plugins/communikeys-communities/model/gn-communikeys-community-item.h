#ifndef GN_COMMUNIKEYS_COMMUNITY_ITEM_H
#define GN_COMMUNIKEYS_COMMUNITY_ITEM_H
#include <glib-object.h>
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM (gn_communikeys_community_item_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityItem, gn_communikeys_community_item,
                     GN, COMMUNIKEYS_COMMUNITY_ITEM, GObject)
GnCommunikeysCommunityItem *gn_communikeys_community_item_new(
    const char *pubkey, const char *main_relay, const char *description,
    guint section_count, gint64 created_at);
const char *gn_communikeys_community_item_get_pubkey(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_main_relay(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_description(GnCommunikeysCommunityItem *self);
guint gn_communikeys_community_item_get_section_count(GnCommunikeysCommunityItem *self);
gint64 gn_communikeys_community_item_get_created_at(GnCommunikeysCommunityItem *self);
G_END_DECLS
#endif
