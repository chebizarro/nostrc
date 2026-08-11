#ifndef GN_CONCORD_COMMUNITY_VIEW_H
#define GN_CONCORD_COMMUNITY_VIEW_H

#include <gtk/gtk.h>

#include "../gn-concord-community-service.h"

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_COMMUNITY_VIEW (gn_concord_community_view_get_type())
G_DECLARE_FINAL_TYPE(GnConcordCommunityView, gn_concord_community_view,
                     GN, CONCORD_COMMUNITY_VIEW, GtkBox)

GnConcordCommunityView *gn_concord_community_view_new(
    GnConcordCommunityService *service, GnConcordCommunityItem *item);

G_END_DECLS
#endif
