#ifndef GN_COMMUNIKEYS_COMMUNITIES_PANEL_H
#define GN_COMMUNIKEYS_COMMUNITIES_PANEL_H
#include <adwaita.h>
#include "../gn-communikeys-community-service.h"
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITIES_PANEL (gn_communikeys_communities_panel_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunitiesPanel,
                     gn_communikeys_communities_panel,
                     GN, COMMUNIKEYS_COMMUNITIES_PANEL, AdwBin)
GnCommunikeysCommunitiesPanel *gn_communikeys_communities_panel_new(
    GnCommunikeysCommunityService *service);
G_END_DECLS
#endif
