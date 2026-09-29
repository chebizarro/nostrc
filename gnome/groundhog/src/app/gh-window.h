#ifndef GH_WINDOW_H
#define GH_WINDOW_H

#include <adwaita.h>
#include "gh-shell.h"
#include "gh-status.h"

G_BEGIN_DECLS

/* The main window, a composite template (data/ui/gh-window.blp): toast
 * overlay, root stack ("main": the AdwNavigationSplitView with the sidebar
 * and content pages; full-window flows such as "onboarding" are added by
 * their owners, see gh-onboarding-view.h), the
 * 360×294 minimum size and the 600sp AdwBreakpoint that collapses the split
 * view. It owns the window's GhStatus (shown by the sidebar banner), the
 * keyboard shortcuts window (data/ui/gh-shortcuts-window.blp) and these
 * actions (charter §7.13):
 *   win.search                  reveal and focus the conversation search
 *   win.previous-conversation   select the previous / next conversation
 *   win.next-conversation
 *   win.new-message             New Message (charter G18): runs the handler set
 *                               with gh_window_set_new_message_handler();
 *                               disabled until one is set and enabled
 *   win.show-help-overlay       the shortcuts window
 * When collapsed, a press on a row, Enter, or the previous/next actions open
 * the selected conversation (show-content), while the arrow keys only move
 * the selection; returning to the list clears the selection so the same row
 * reopens it.
 * Account, conversation and inbox behaviour is attached by the caller; see
 * gh_account_ui_attach() and gh_conversation_list_attach(). */
#define GH_TYPE_WINDOW (gh_window_get_type())
G_DECLARE_FINAL_TYPE(GhWindow, gh_window, GH, WINDOW, AdwApplicationWindow)

/* app may be NULL (tests). */
GhWindow *gh_window_new(GtkApplication *app);

AdwToastOverlay *gh_window_get_toasts(GhWindow *self);
/* The window's root pages. While a page other than "main" shows, typing no
 * longer starts a conversation search. */
GtkStack *gh_window_get_root_stack(GhWindow *self);
AdwNavigationSplitView *gh_window_get_split(GhWindow *self);
GhSidebarPage *gh_window_get_sidebar(GhWindow *self);
GhContentPage *gh_window_get_content(GhWindow *self);
GhStatus *gh_window_get_status(GhWindow *self);
/* TRUE when the content page is on screen: expanded, or collapsed and shown. */
gboolean gh_window_get_content_visible(GhWindow *self);
/* Opens item (a conversation of the attached list) as if chosen from the
 * sidebar: selected in whichever list holds it (gh_sidebar_page_select_item())
 * and shown in the content page, collapsed or not. FALSE when no list holds
 * it. */
gboolean gh_window_open_item(GhWindow *self, gpointer item);

/* The New Message flow behind win.new-message (Ctrl+N, the header's New
 * Message button and the empty list's): func runs on activation.
 * gh_new_message_attach() sets it. data is released with destroy when
 * replaced or when the window is disposed. The action is disabled while no
 * handler is set or gh_window_set_new_message_enabled() turned it off. */
typedef void (*GhWindowNewMessageFunc)(GhWindow *window, gpointer data);
void gh_window_set_new_message_handler(GhWindow *self, GhWindowNewMessageFunc func,
                                       gpointer data, GDestroyNotify destroy);
/* Whether New Message can run now (e.g. an account is active). */
void gh_window_set_new_message_enabled(GhWindow *self, gboolean enabled);

/* Adds app.quit and binds every accelerator of the shortcuts window to its
 * action on app (Ctrl+F, Alt+Up/Down and Ctrl+Page Up/Down, Ctrl+N, Ctrl+?,
 * Ctrl+, for app.preferences, which gh-app-services.c adds, Ctrl+W, Ctrl+Q).
 * Call once, from the application's startup. */
void gh_window_setup_application(GtkApplication *app);

G_END_DECLS
#endif
