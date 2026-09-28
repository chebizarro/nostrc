#ifndef GH_WINDOW_H
#define GH_WINDOW_H

#include <adwaita.h>
#include "gh-shell.h"

G_BEGIN_DECLS

/* The main window, a composite template (data/ui/gh-window.blp): toast
 * overlay, AdwNavigationSplitView with the sidebar and content pages, and the
 * 600sp AdwBreakpoint that collapses the split view. Behaviour (account
 * state, actions) is attached by the caller; see gh_account_ui_attach(). */
#define GH_TYPE_WINDOW (gh_window_get_type())
G_DECLARE_FINAL_TYPE(GhWindow, gh_window, GH, WINDOW, AdwApplicationWindow)

/* app may be NULL (tests). */
GhWindow *gh_window_new(GtkApplication *app);

AdwToastOverlay *gh_window_get_toasts(GhWindow *self);
AdwNavigationSplitView *gh_window_get_split(GhWindow *self);
GhSidebarPage *gh_window_get_sidebar(GhWindow *self);
GhContentPage *gh_window_get_content(GhWindow *self);

G_END_DECLS
#endif
