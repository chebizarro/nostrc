#ifndef GN_CONCORD_COMMUNITIES_PANEL_H
#define GN_CONCORD_COMMUNITIES_PANEL_H

#include <adwaita.h>

#include "../gn-concord-community-service.h"

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_COMMUNITIES_PANEL (gn_concord_communities_panel_get_type())
G_DECLARE_FINAL_TYPE(GnConcordCommunitiesPanel, gn_concord_communities_panel,
                     GN, CONCORD_COMMUNITIES_PANEL, AdwBin)

GnConcordCommunitiesPanel *gn_concord_communities_panel_new(
    GnConcordCommunityService *service);

G_END_DECLS
#endif
