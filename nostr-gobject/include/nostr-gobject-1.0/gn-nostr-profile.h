#ifndef GNOSTR_PROFILE_H
#define GNOSTR_PROFILE_H

#include "nostr-gobject-export.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE (gnostr_profile_get_type())
NOSTR_GOBJECT_API G_DECLARE_FINAL_TYPE(GNostrProfile, gnostr_profile, GNOSTR, PROFILE, GObject)

NOSTR_GOBJECT_API GNostrProfile *gnostr_profile_new(const char *pubkey);

NOSTR_GOBJECT_API const char *gnostr_profile_get_pubkey(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_display_name(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_name(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_about(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_picture_url(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_nip05(GNostrProfile *self);
NOSTR_GOBJECT_API const char *gnostr_profile_get_lud16(GNostrProfile *self);

NOSTR_GOBJECT_API void gnostr_profile_set_display_name(GNostrProfile *self, const char *display_name);
NOSTR_GOBJECT_API void gnostr_profile_set_name(GNostrProfile *self, const char *name);
NOSTR_GOBJECT_API void gnostr_profile_set_picture_url(GNostrProfile *self, const char *picture_url);
NOSTR_GOBJECT_API void gnostr_profile_set_nip05(GNostrProfile *self, const char *nip05);

NOSTR_GOBJECT_API void gnostr_profile_update_from_json(GNostrProfile *self, const char *json);

G_END_DECLS

#endif
