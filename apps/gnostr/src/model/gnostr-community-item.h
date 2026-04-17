/*
 * gnostr-community-item.h - GObject wrapper for GnostrCommunity
 *
 * Wraps a NIP-72 GnostrCommunity struct as a GObject so it can be stored
 * in a GListStore and used with GtkListView.
 */

#ifndef GNOSTR_COMMUNITY_ITEM_H
#define GNOSTR_COMMUNITY_ITEM_H

#include <glib-object.h>
#include "../util/nip72_communities.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMMUNITY_ITEM (gnostr_community_item_get_type())

G_DECLARE_FINAL_TYPE(GnostrCommunityItem, gnostr_community_item, GNOSTR, COMMUNITY_ITEM, GObject)

GnostrCommunityItem *gnostr_community_item_new(const GnostrCommunity *community);
const GnostrCommunity *gnostr_community_item_get_community(GnostrCommunityItem *self);
const char *gnostr_community_item_get_a_tag(GnostrCommunityItem *self);
const char *gnostr_community_item_get_name(GnostrCommunityItem *self);
const char *gnostr_community_item_get_description(GnostrCommunityItem *self);
void gnostr_community_item_update(GnostrCommunityItem *self, const GnostrCommunity *community);

gboolean gnostr_community_item_get_joined(GnostrCommunityItem *self);
void gnostr_community_item_set_joined(GnostrCommunityItem *self, gboolean joined);

G_END_DECLS

#endif /* GNOSTR_COMMUNITY_ITEM_H */
