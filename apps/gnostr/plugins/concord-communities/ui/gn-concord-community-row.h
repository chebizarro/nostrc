#ifndef GN_CONCORD_COMMUNITY_ROW_H
#define GN_CONCORD_COMMUNITY_ROW_H

#include <gtk/gtk.h>

#include "../model/gn-concord-community-item.h"

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_COMMUNITY_ROW (gn_concord_community_row_get_type())
G_DECLARE_FINAL_TYPE(GnConcordCommunityRow, gn_concord_community_row,
                     GN, CONCORD_COMMUNITY_ROW, GtkBox)

GnConcordCommunityRow *gn_concord_community_row_new(void);
void gn_concord_community_row_bind(GnConcordCommunityRow *self,
                                   GnConcordCommunityItem *item);
void gn_concord_community_row_unbind(GnConcordCommunityRow *self);

G_END_DECLS
#endif
