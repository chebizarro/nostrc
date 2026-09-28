#ifndef GH_SHELL_H
#define GH_SHELL_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Read-only shell chrome shared between the application (main.c) and the
 * layout/accessibility test (tests/app/test_shell_layout.c). The widget trees
 * are Blueprint composite templates (data/ui/gh-sidebar-page.blp and
 * data/ui/gh-content-page.blp) bundled in the Groundhog GResource, so
 * groundhog_register_resource() must run before either type is first used.
 * This unit never touches GSettings or the signer. */

/* The sidebar navigation page: header, window title, and the stack that
 * hosts the (currently always-empty) conversations list plus its empty and
 * error states. The list and the stack both carry an accessible label so
 * their purpose is announced even though no rows exist until a real
 * conversation backend lands. */
#define GH_TYPE_SIDEBAR_PAGE (gh_sidebar_page_get_type())
G_DECLARE_FINAL_TYPE(GhSidebarPage, gh_sidebar_page, GH, SIDEBAR_PAGE, AdwNavigationPage)

AdwHeaderBar *gh_sidebar_page_get_header(GhSidebarPage *self);
AdwWindowTitle *gh_sidebar_page_get_window_title(GhSidebarPage *self);
GtkStack *gh_sidebar_page_get_stack(GhSidebarPage *self);

/* For builds without account support: adds the "onboarding" page
 * (data/ui/gh-onboarding-page.blp) to the stack and shows it. */
void gh_sidebar_page_show_onboarding(GhSidebarPage *self);

/* The content navigation page: header, the always-revealed read-only banner,
 * and the "no conversation selected" status page. */
#define GH_TYPE_CONTENT_PAGE (gh_content_page_get_type())
G_DECLARE_FINAL_TYPE(GhContentPage, gh_content_page, GH, CONTENT_PAGE, AdwNavigationPage)

AdwBanner *gh_content_page_get_banner(GhContentPage *self);

G_END_DECLS
#endif
