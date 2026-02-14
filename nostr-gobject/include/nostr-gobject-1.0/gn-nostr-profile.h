#ifndef GN_NOSTR_PROFILE_H
#define GN_NOSTR_PROFILE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_PROFILE (gn_nostr_profile_get_type())
G_DECLARE_FINAL_TYPE(GnNostrProfile, gn_nostr_profile, GN, NOSTR_PROFILE, GObject)

GnNostrProfile *gn_nostr_profile_new(const char *pubkey);

const char *gn_nostr_profile_get_pubkey(GnNostrProfile *self);
const char *gn_nostr_profile_get_display_name(GnNostrProfile *self);
const char *gn_nostr_profile_get_name(GnNostrProfile *self);
const char *gn_nostr_profile_get_about(GnNostrProfile *self);
const char *gn_nostr_profile_get_picture_url(GnNostrProfile *self);
const char *gn_nostr_profile_get_nip05(GnNostrProfile *self);
const char *gn_nostr_profile_get_lud16(GnNostrProfile *self);

void gn_nostr_profile_set_display_name(GnNostrProfile *self, const char *display_name);
void gn_nostr_profile_set_name(GnNostrProfile *self, const char *name);
void gn_nostr_profile_set_picture_url(GnNostrProfile *self, const char *picture_url);
void gn_nostr_profile_set_nip05(GnNostrProfile *self, const char *nip05);

void gn_nostr_profile_update_from_json(GnNostrProfile *self, const char *json);

G_END_DECLS

#endif
