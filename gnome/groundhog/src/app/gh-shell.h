#ifndef GH_SHELL_H
#define GH_SHELL_H

#include <adwaita.h>
#include "gh-status.h"

G_BEGIN_DECLS

/* The sidebar and content pages of the window, shared between the
 * application (main.c) and the GUI tests. The widget trees are Blueprint
 * composite templates (data/ui/gh-sidebar-page.blp, gh-content-page.blp)
 * bundled in the Groundhog GResource, so groundhog_register_resource() must
 * run before either type is first used. This unit knows no model type: the
 * conversation list binder (src/ui/gh-conversation-list.c) supplies the
 * models, row factories and titles. It never touches GSettings or the
 * signer. */

/* The sidebar navigation page (charter §7.5): header, search bar, status
 * banner and a stack whose pages are "conversations" (the Message Requests
 * entry and the list), "empty", "no-results", "error", plus any account
 * page added from C. Properties: "selected" (the selected item, read-only),
 * "search-text" (the trimmed search, "" when none; read-only),
 * "show-requests" (the list shows message requests instead of
 * conversations) and "show-previews" (rows may show message text; default
 * FALSE, see gh_conversation_list_attach()). The action
 * "sidebar.show-requests" (boolean parameter) switches the list; Escape
 * leaves Message Requests. */
#define GH_TYPE_SIDEBAR_PAGE (gh_sidebar_page_get_type())
G_DECLARE_FINAL_TYPE(GhSidebarPage, gh_sidebar_page, GH, SIDEBAR_PAGE, AdwNavigationPage)

AdwHeaderBar *gh_sidebar_page_get_header(GhSidebarPage *self);
AdwWindowTitle *gh_sidebar_page_get_window_title(GhSidebarPage *self);
GtkStack *gh_sidebar_page_get_stack(GhSidebarPage *self);
GtkListView *gh_sidebar_page_get_list(GhSidebarPage *self);
AdwBanner *gh_sidebar_page_get_banner(GhSidebarPage *self);

/* For builds without account support: adds the "onboarding" page
 * (data/ui/gh-onboarding-page.blp) to the stack and shows it. */
void gh_sidebar_page_show_onboarding(GhSidebarPage *self);

/* Shows the named account page added to the stack (see gh-account-ui.c)
 * instead of the conversation pages; NULL returns to them. */
void gh_sidebar_page_set_account_page(GhSidebarPage *self, const gchar *name);

/* The banner and the error page follow status (the window's own). */
void gh_sidebar_page_set_status(GhSidebarPage *self, GhStatus *status);

/* Binds the list: conversations are listed; requests (message requests) are
 * counted in the Message Requests entry and listed in show-requests mode.
 * Both are already filtered and ordered, and neither may contain the other's
 * items. Set once; the page takes references. */
void gh_sidebar_page_set_models(GhSidebarPage *self, GListModel *conversations,
                                GListModel *requests);
/* The selected item of the visible list; borrowed, NULL for none. */
gpointer gh_sidebar_page_get_selected(GhSidebarPage *self);
void gh_sidebar_page_unselect(GhSidebarPage *self);
/* Selects the previous (delta < 0) or next item of the visible list, or the
 * last or first when none is selected, and scrolls it into view; keyboard
 * focus follows only if it was in the list. FALSE when there is none. */
gboolean gh_sidebar_page_select_relative(GhSidebarPage *self, gint delta);

/* Reveals the search bar and focuses its entry. */
void gh_sidebar_page_start_search(GhSidebarPage *self);
/* Typing into widget starts a search (GtkSearchBar key capture). */
void gh_sidebar_page_set_key_capture_widget(GhSidebarPage *self, GtkWidget *widget);
const gchar *gh_sidebar_page_get_search_text(GhSidebarPage *self);

gboolean gh_sidebar_page_get_show_requests(GhSidebarPage *self);
void gh_sidebar_page_set_show_requests(GhSidebarPage *self, gboolean show_requests);
gboolean gh_sidebar_page_get_show_previews(GhSidebarPage *self);
void gh_sidebar_page_set_show_previews(GhSidebarPage *self, gboolean show_previews);

/* The explicit focus target of the visible conversation page (charter
 * §7.14: never a page's implicit first child): the search entry on
 * "no-results", nothing on the others. NULL while an account page shows;
 * gh-account-ui.c names those pages' targets. */
GtkWidget *gh_sidebar_page_get_focus_target(GhSidebarPage *self);

/* The content navigation page: the selected conversation's messages as a
 * read-only list ("conversation") with a line below it stating why sending is
 * unavailable, or "No Conversation Selected" ("none"). The message rows'
 * factory is set by the caller. */
#define GH_TYPE_CONTENT_PAGE (gh_content_page_get_type())
G_DECLARE_FINAL_TYPE(GhContentPage, gh_content_page, GH, CONTENT_PAGE, AdwNavigationPage)

GtkStack *gh_content_page_get_stack(GhContentPage *self);
GtkListView *gh_content_page_get_message_list(GhContentPage *self);
AdwWindowTitle *gh_content_page_get_window_title(GhContentPage *self);
/* messages NULL shows "none"; otherwise the list shows them (scrolled to
 * the newest). */
void gh_content_page_set_messages(GhContentPage *self, GListModel *messages);
GListModel *gh_content_page_get_messages(GhContentPage *self);
/* The page and header title; NULL restores the template's "Messages". */
void gh_content_page_set_title(GhContentPage *self, const gchar *title,
                               const gchar *subtitle);
/* Why sending is unavailable (gh_account_describe_limits()); NULL or ""
 * hides the line. */
void gh_content_page_set_read_only_reason(GhContentPage *self, const gchar *reason);
const gchar *gh_content_page_get_read_only_reason(GhContentPage *self);

G_END_DECLS
#endif
