#ifndef GN_CONCORD_CHANNEL_ITEM_H
#define GN_CONCORD_CHANNEL_ITEM_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_CHANNEL_ITEM (gn_concord_channel_item_get_type())
G_DECLARE_FINAL_TYPE(GnConcordChannelItem, gn_concord_channel_item,
                     GN, CONCORD_CHANNEL_ITEM, GObject)

/* One granted Channel from an invite bundle (CORD-05 §1) or a Control Plane
 * channel edition (CORD-03). `key_hex` is the Channel's own key for a private
 * Channel, or the community_root for a public one — both feed the same
 * `concord/channel` derivation, so the item does not distinguish them beyond
 * the flag. */
GnConcordChannelItem *gn_concord_channel_item_new(const char *id_hex,
                                                  const char *key_hex,
                                                  guint64 epoch,
                                                  const char *name,
                                                  gboolean is_private);

const char *gn_concord_channel_item_get_id(GnConcordChannelItem *self);
const char *gn_concord_channel_item_get_key(GnConcordChannelItem *self);
guint64 gn_concord_channel_item_get_epoch(GnConcordChannelItem *self);
const char *gn_concord_channel_item_get_name(GnConcordChannelItem *self);
gboolean gn_concord_channel_item_get_is_private(GnConcordChannelItem *self);

/* A Channel is *defined* in the Control Plane (CORD-03 §2), so a rename or a
 * visibility flip arrives as an authorized edition rather than a new Channel.
 * The `channel_id` never changes across any conversion. */
void gn_concord_channel_item_set_name(GnConcordChannelItem *self,
                                      const char *name);
void gn_concord_channel_item_set_is_private(GnConcordChannelItem *self,
                                            gboolean is_private);

/* A rekey replaces the key and climbs the epoch together (CORD-06 §1): the
 * pair *is* the rotation, and the Channel's plane address derives from both,
 * so a caller that moved one without the other would address a plane nobody
 * writes to. The key material is wiped on replacement. */
void gn_concord_channel_item_set_key(GnConcordChannelItem *self,
                                     const char *key_hex, guint64 epoch);

/* The derived Chat Plane address, x-only hex — the `authors` filter for this
 * Channel's stream. NULL until the service derives it. */
const char *gn_concord_channel_item_get_stream_pubkey(GnConcordChannelItem *self);
void gn_concord_channel_item_set_stream_pubkey(GnConcordChannelItem *self,
                                               const char *pubkey_hex);

G_END_DECLS
#endif
