#ifndef GN_NOSTR_EVENT_ITEM_H
#define GN_NOSTR_EVENT_ITEM_H

#include "gn-nostr-profile.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_EVENT_ITEM (gn_nostr_event_item_get_type())
G_DECLARE_FINAL_TYPE(GnNostrEventItem, gn_nostr_event_item, GN, NOSTR_EVENT_ITEM, GObject)

GnNostrEventItem *gn_nostr_event_item_new(const char *event_id);

const char *gn_nostr_event_item_get_event_id(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_pubkey(GnNostrEventItem *self);
gint64 gn_nostr_event_item_get_created_at(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_content(GnNostrEventItem *self);
gint gn_nostr_event_item_get_kind(GnNostrEventItem *self);
GnNostrProfile *gn_nostr_event_item_get_profile(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_thread_root_id(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_parent_id(GnNostrEventItem *self);
guint gn_nostr_event_item_get_reply_depth(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_root(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_reply(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_repost(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_muted(GnNostrEventItem *self);

void gn_nostr_event_item_set_profile(GnNostrEventItem *self, GnNostrProfile *profile);
void gn_nostr_event_item_set_thread_info(GnNostrEventItem *self,
					 const char *root_id,
					 const char *parent_id,
					 guint depth);
void gn_nostr_event_item_update_from_event(GnNostrEventItem *self,
					   const char *pubkey,
					   gint64 created_at,
					   const char *content,
					   gint kind);

G_END_DECLS

#endif
