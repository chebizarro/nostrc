#ifndef GH_ACCOUNT_UI_H
#define GH_ACCOUNT_UI_H

#include <adwaita.h>
#include "gh-account-controller.h"

G_BEGIN_DECLS

/* Adds the account pages to the sidebar stack, an account menu to the
 * sidebar header, the "account" action group (select, refresh) to window,
 * and keeps the stack, header subtitle and read-only banner in step with the
 * controller and network. Everything is released with window. */
void gh_account_ui_attach(GtkWidget *window, GhAccountController *controller,
                          GSettings *settings, AdwHeaderBar *sidebar_header,
                          AdwWindowTitle *title, GtkStack *sidebar_stack,
                          AdwBanner *banner, AdwToastOverlay *toasts);

G_END_DECLS
#endif
