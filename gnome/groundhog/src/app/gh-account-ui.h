#ifndef GH_ACCOUNT_UI_H
#define GH_ACCOUNT_UI_H

#include <adwaita.h>
#include "gh-account-controller.h"
#include "gh-window.h"

G_BEGIN_DECLS

/* Adds the account pages (data/ui/gh-account-ui.blp) to the window's
 * sidebar stack and the account menu as the first entry ("Account") of the
 * sidebar's main menu, the "account" action group (select, refresh) to
 * window, and keeps the sidebar's account page, the header title
 * (the active account's name, else "Groundhog"; no subtitle) and the window
 * GhStatus's account, network and signer inputs in
 * step with the controller and the network. (Why sending is unavailable is
 * the composer's, gh-send-ui.h.) On a real account-page transition it
 * announces the limit (gh_account_describe_limits()) and moves keyboard
 * focus to the page's action, except that focus never leaves a text field or
 * the composer while someone types there (nostrc-qp24.8.1). An inactive
 * window gets no announcement (charter §7.14); if it is on screen, the focus
 * move waits for its activation.
 * Everything is released with window. */
void gh_account_ui_attach(GhWindow *window, GhAccountController *controller,
                          GSettings *settings);
/* A source of display names for the sidebar title (the contact directory):
 * the active account's cached kind-0 name is the title when known, else the
 * key's label, else its shortened npub. name(data, pubkey_hex) returns a
 * borrowed name or NULL; source's "profile-changed" (s) signal refreshes. */
typedef const gchar *(*GhAccountNameFunc)(gpointer data, const gchar *pubkey);
void gh_account_ui_set_name_source(GhWindow *window, GhAccountNameFunc name, GObject *source);
/* The account-state announcements made so far (tests). */
guint gh_account_ui_get_announcements(GhWindow *window);

G_END_DECLS
#endif
