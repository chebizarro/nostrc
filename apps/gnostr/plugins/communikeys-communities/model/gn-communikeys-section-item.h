#ifndef GN_COMMUNIKEYS_SECTION_ITEM_H
#define GN_COMMUNIKEYS_SECTION_ITEM_H

#include <gio/gio.h>
#include <glib-object.h>
#include <nip_communikeys.h>

G_BEGIN_DECLS

typedef enum {
  GN_COMMUNIKEYS_ACL_UNRESOLVED,
  GN_COMMUNIKEYS_ACL_VERIFIED,
  GN_COMMUNIKEYS_ACL_MISSING,
  GN_COMMUNIKEYS_ACL_INVALID,
  GN_COMMUNIKEYS_ACL_UNTRUSTED_PUBLISHER
} GnCommunikeysAclState;

#define GN_TYPE_COMMUNIKEYS_SECTION_ITEM (gn_communikeys_section_item_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysSectionItem, gn_communikeys_section_item,
                     GN, COMMUNIKEYS_SECTION_ITEM, GObject)

GnCommunikeysSectionItem *gn_communikeys_section_item_new(
    const char *community_pubkey,
    const nostr_communikeys_section_t *section);

const char *gn_communikeys_section_item_get_community_pubkey(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_name(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_acl_publisher(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_acl_identifier(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_acl_relay(
    GnCommunikeysSectionItem *self);
guint gn_communikeys_section_item_get_assignment_count(
    GnCommunikeysSectionItem *self);
gboolean gn_communikeys_section_item_get_assignment(
    GnCommunikeysSectionItem *self, guint index, int *kind,
    const char **subtype);
guint gn_communikeys_section_item_get_badge_count(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_badge(
    GnCommunikeysSectionItem *self, guint index);
GnCommunikeysAclState gn_communikeys_section_item_get_acl_state(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_acl_status(
    GnCommunikeysSectionItem *self);
guint gn_communikeys_section_item_get_member_count(
    GnCommunikeysSectionItem *self);
const char *gn_communikeys_section_item_get_member(
    GnCommunikeysSectionItem *self, guint index);
gboolean gn_communikeys_section_item_has_member(
    GnCommunikeysSectionItem *self, const char *pubkey);

/* Service-owned mutation helpers. */
void gn_communikeys_section_item_set_acl(
    GnCommunikeysSectionItem *self, GnCommunikeysAclState state,
    const char *status, char * const *members, gsize members_len);

G_END_DECLS
#endif
