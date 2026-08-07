#ifndef GN_COMMUNIKEYS_COMMUNITY_ITEM_H
#define GN_COMMUNIKEYS_COMMUNITY_ITEM_H
#include <gio/gio.h>
#include <glib-object.h>
#include <nip_communikeys.h>
#include "gn-communikeys-section-item.h"
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM (gn_communikeys_community_item_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityItem, gn_communikeys_community_item,
                     GN, COMMUNIKEYS_COMMUNITY_ITEM, GObject)
GnCommunikeysCommunityItem *gn_communikeys_community_item_new(
    const nostr_communikeys_definition_t *definition,
    const char *definition_id, gint64 created_at);
const char *gn_communikeys_community_item_get_pubkey(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_main_relay(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_description(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_definition_id(GnCommunikeysCommunityItem *self);
guint gn_communikeys_community_item_get_section_count(GnCommunikeysCommunityItem *self);
gint64 gn_communikeys_community_item_get_created_at(GnCommunikeysCommunityItem *self);
GListModel *gn_communikeys_community_item_get_sections(GnCommunikeysCommunityItem *self);
GnCommunikeysSectionItem *gn_communikeys_community_item_get_section(
    GnCommunikeysCommunityItem *self, guint position);
GnCommunikeysSectionItem *gn_communikeys_community_item_find_section(
    GnCommunikeysCommunityItem *self, const char *name);
G_END_DECLS
#endif
