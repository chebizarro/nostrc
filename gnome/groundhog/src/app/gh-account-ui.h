#ifndef GH_ACCOUNT_UI_H
#define GH_ACCOUNT_UI_H

#include <adwaita.h>
#include "gh-account-controller.h"
#include "gh-window.h"

G_BEGIN_DECLS

/* Adds the account pages and account menu (data/ui/gh-account-ui.blp) to the
 * window's sidebar stack and header, the "account" action group (select,
 * refresh) to window, and keeps the sidebar's account page, the header
 * subtitle, the content page's read-only reason and the window GhStatus's
 * account, network and signer inputs in step with the controller and the
 * network. Everything is released with window. */
void gh_account_ui_attach(GhWindow *window, GhAccountController *controller,
                          GSettings *settings);

G_END_DECLS
#endif
