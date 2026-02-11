#ifndef GN_NOSTR_EVENT_ITEM_H
#define GN_NOSTR_EVENT_ITEM_H

#include "gn-nostr-profile.h"
#include "../util/content_renderer.h"
#include <glib-object.h>
#include <stdint.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_EVENT_ITEM (gn_nostr_event_item_get_type())
G_DECLARE_FINAL_TYPE(GnNostrEventItem, gn_nostr_event_item, GN, NOSTR_EVENT_ITEM, GObject)

/* Create item from nostrdb note key (preferred - uses lazy loading) */
GnNostrEventItem *gn_nostr_event_item_new_from_key(uint64_t note_key, gint64 created_at);

/* Get the nostrdb note key */
uint64_t gn_nostr_event_item_get_note_key(GnNostrEventItem *self);

/* Legacy constructor - creates item from hex event id (deprecated) */
GnNostrEventItem *gn_nostr_event_item_new(const char *event_id);

const char *gn_nostr_event_item_get_event_id(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_pubkey(GnNostrEventItem *self);
gint64 gn_nostr_event_item_get_created_at(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_content(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_tags_json(GnNostrEventItem *self);
const char * const *gn_nostr_event_item_get_hashtags(GnNostrEventItem *self);
gint gn_nostr_event_item_get_kind(GnNostrEventItem *self);
GnNostrProfile *gn_nostr_event_item_get_profile(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_thread_root_id(GnNostrEventItem *self);
const char *gn_nostr_event_item_get_parent_id(GnNostrEventItem *self);
guint gn_nostr_event_item_get_reply_depth(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_root(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_reply(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_repost(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_muted(GnNostrEventItem *self);

/* nostrc-7o7: Animation control for notes added outside visible viewport */
gboolean gn_nostr_event_item_get_skip_animation(GnNostrEventItem *self);
void gn_nostr_event_item_set_skip_animation(GnNostrEventItem *self, gboolean skip);

/* nostrc-0hp Phase 3: Reveal animation state for "New Notes" button */
gboolean gn_nostr_event_item_get_revealing(GnNostrEventItem *self);
void gn_nostr_event_item_set_revealing(GnNostrEventItem *self, gboolean revealing);

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

/* NIP-25: Reaction count support */
guint gn_nostr_event_item_get_like_count(GnNostrEventItem *self);
void gn_nostr_event_item_set_like_count(GnNostrEventItem *self, guint count);
gboolean gn_nostr_event_item_get_is_liked(GnNostrEventItem *self);
void gn_nostr_event_item_set_is_liked(GnNostrEventItem *self, gboolean is_liked);

/* NIP-18: Repost count support */
guint gn_nostr_event_item_get_repost_count(GnNostrEventItem *self);
void gn_nostr_event_item_set_repost_count(GnNostrEventItem *self, guint count);

/* NIP-57: Zap stats support */
guint gn_nostr_event_item_get_zap_count(GnNostrEventItem *self);
void gn_nostr_event_item_set_zap_count(GnNostrEventItem *self, guint count);
gint64 gn_nostr_event_item_get_zap_total_msat(GnNostrEventItem *self);
void gn_nostr_event_item_set_zap_total_msat(GnNostrEventItem *self, gint64 total_msat);

/* NIP-40: Expiration timestamp support */
gint64 gn_nostr_event_item_get_expiration(GnNostrEventItem *self);
gboolean gn_nostr_event_item_get_is_expired(GnNostrEventItem *self);

/* NIP-18: Repost support - extract referenced event ID from kind 6 repost tags
 * Returns newly allocated string or NULL if not a repost or no "e" tag found.
 * Caller must g_free() the result. */
char *gn_nostr_event_item_get_reposted_event_id(GnNostrEventItem *self);

/* nostrc-slot: Populate item data from a note pointer (avoids opening new transaction).
 * Call this during batch processing while the transaction is still open.
 * The note pointer must be valid (from storage_ndb_get_note_ptr with open txn). */
struct ndb_note;
void gn_nostr_event_item_populate_from_note(GnNostrEventItem *self, struct ndb_note *note);

/* nostrc-dqwq.1: Cached render result for Pango markup + media URLs.
 * On first call, lazily renders content via gnostr_render_content() and caches.
 * On subsequent calls, returns the cached result directly (content is immutable).
 * Returns: (transfer none)(nullable): cached render result, or NULL if content
 *          is not yet loaded. Owned by the item; do NOT free. */
const GnContentRenderResult *gn_nostr_event_item_get_render_result(GnNostrEventItem *self);

/* nostrc-dqwq.1: Store a pre-built render result on the item.
 * Takes ownership of @render (will be freed when the item is finalized).
 * Intended for callers that build the result externally (e.g. imeta-aware path). */
void gn_nostr_event_item_set_render_result(GnNostrEventItem *self, GnContentRenderResult *render);

G_END_DECLS

#endif
