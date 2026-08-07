#ifndef GN_COMMUNIKEYS_COMMUNITY_VIEW_H
#define GN_COMMUNIKEYS_COMMUNITY_VIEW_H
#include <adwaita.h>
#include "../gn-communikeys-community-service.h"
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITY_VIEW (gn_communikeys_community_view_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityView, gn_communikeys_community_view,
                     GN, COMMUNIKEYS_COMMUNITY_VIEW, GtkBox)
GnCommunikeysCommunityView *gn_communikeys_community_view_new(
    GnCommunikeysCommunityService *service,
    GnCommunikeysCommunityItem *item);
G_END_DECLS
#endif
