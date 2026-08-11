#ifndef GN_CONCORD_COMMUNITY_ITEM_H
#define GN_CONCORD_COMMUNITY_ITEM_H

#include <gio/gio.h>
#include <glib-object.h>

#include "gn-concord-channel-item.h"

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_COMMUNITY_ITEM (gn_concord_community_item_get_type())
G_DECLARE_FINAL_TYPE(GnConcordCommunityItem, gn_concord_community_item,
                     GN, CONCORD_COMMUNITY_ITEM, GObject)

/* The presentation face of one membership. The secrets themselves stay in the
 * service; this object carries only what the UI renders plus the identifiers
 * the service keys its lookups on. */
GnConcordCommunityItem *gn_concord_community_item_new(const char *community_id,
                                                      const char *owner,
                                                      const char *name,
                                                      guint64 root_epoch,
                                                      gboolean has_control_pk);

const char *gn_concord_community_item_get_community_id(
    GnConcordCommunityItem *self);
const char *gn_concord_community_item_get_owner(GnConcordCommunityItem *self);
const char *gn_concord_community_item_get_name(GnConcordCommunityItem *self);
guint64 gn_concord_community_item_get_root_epoch(GnConcordCommunityItem *self);
/* CORD-05 §1: an absent control_pk marks a legacy, pre-split Community whose
 * Control Plane folds at the legacy address instead. */
gboolean gn_concord_community_item_get_has_control_pk(
    GnConcordCommunityItem *self);

GListModel *gn_concord_community_item_get_channels(
    GnConcordCommunityItem *self);
void gn_concord_community_item_add_channel(GnConcordCommunityItem *self,
                                           GnConcordChannelItem *channel);
GnConcordChannelItem *gn_concord_community_item_find_channel(
    GnConcordCommunityItem *self, const char *channel_id);
guint gn_concord_community_item_get_channel_count(
    GnConcordCommunityItem *self);

const char *gn_concord_community_item_get_primary_relay(
    GnConcordCommunityItem *self);
void gn_concord_community_item_set_relays(GnConcordCommunityItem *self,
                                          const char *const *relays,
                                          guint n_relays);
const char *const *gn_concord_community_item_get_relays(
    GnConcordCommunityItem *self, guint *n_relays);

G_END_DECLS
#endif
