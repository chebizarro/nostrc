#ifndef GH_HIDE_ACCOUNT_H
#define GH_HIDE_ACCOUNT_H
#include <adwaita.h>
#include "gh-social-lists.h"
G_BEGIN_DECLS
/* app.hide-account takes (source account hex, target hex), so stale menus
 * cannot act for the next account. One generation-bound confirmation at a time. */
void gh_hide_account_install(GtkApplication *app, GhSocialLists *lists);
void gh_hide_account_uninstall(GtkApplication *app);
G_END_DECLS
#endif
