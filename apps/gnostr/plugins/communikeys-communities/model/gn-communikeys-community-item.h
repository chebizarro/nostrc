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
/* Identity is the exact definition address "32222:<owner>:<communityId>".
 * The display name comes from the definition's required `name` tag —
 * a kind-0 profile MUST NOT supply community metadata (Communikeys V2). */
GnCommunikeysCommunityItem *gn_communikeys_community_item_new(
    const nostr_communikeys_definition_t *definition,
    const char *definition_address,
    const char *definition_id, gint64 created_at);
const char *gn_communikeys_community_item_get_address(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_owner_pubkey(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_community_id(GnCommunikeysCommunityItem *self);
const char *gn_communikeys_community_item_get_name(GnCommunikeysCommunityItem *self);
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
