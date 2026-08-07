#ifndef GN_COMMUNIKEYS_COMMUNITY_ROW_H
#define GN_COMMUNIKEYS_COMMUNITY_ROW_H
#include <gtk/gtk.h>
#include "../model/gn-communikeys-community-item.h"
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITY_ROW (gn_communikeys_community_row_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityRow, gn_communikeys_community_row,
                     GN, COMMUNIKEYS_COMMUNITY_ROW, GtkBox)
GnCommunikeysCommunityRow *gn_communikeys_community_row_new(void);
void gn_communikeys_community_row_bind(
    GnCommunikeysCommunityRow *self, GnCommunikeysCommunityItem *item);
void gn_communikeys_community_row_unbind(GnCommunikeysCommunityRow *self);
G_END_DECLS
#endif
