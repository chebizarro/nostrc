#ifndef GNOSTR_PROFILE_H
#define GNOSTR_PROFILE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE (gnostr_profile_get_type())
G_DECLARE_FINAL_TYPE(GNostrProfile, gnostr_profile, GNOSTR, PROFILE, GObject)

GNostrProfile *gnostr_profile_new(const char *pubkey);

const char *gnostr_profile_get_pubkey(GNostrProfile *self);
const char *gnostr_profile_get_display_name(GNostrProfile *self);
const char *gnostr_profile_get_name(GNostrProfile *self);
const char *gnostr_profile_get_about(GNostrProfile *self);
const char *gnostr_profile_get_picture_url(GNostrProfile *self);
const char *gnostr_profile_get_nip05(GNostrProfile *self);
const char *gnostr_profile_get_lud16(GNostrProfile *self);

void gnostr_profile_set_display_name(GNostrProfile *self, const char *display_name);
void gnostr_profile_set_name(GNostrProfile *self, const char *name);
void gnostr_profile_set_picture_url(GNostrProfile *self, const char *picture_url);
void gnostr_profile_set_nip05(GNostrProfile *self, const char *nip05);

void gnostr_profile_update_from_json(GNostrProfile *self, const char *json);

G_END_DECLS

#endif
