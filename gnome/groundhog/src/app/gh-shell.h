#ifndef GH_SHELL_H
#define GH_SHELL_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Read-only shell chrome shared between the application (main.c) and the
 * headless layout/accessibility test (tests/app/test_shell_layout.c). This
 * unit never touches GResource, GSettings, or the signer, so it can be
 * exercised without a compiled resource bundle or a display session that
 * supports mapping a window. */

/* An AdwStatusPage with the "groundhog-shell-status" CSS class. Every empty,
 * error, and onboarding state in the shell uses this so a screen reader
 * always hears a title and description instead of a blank pane. */
GtkWidget *gh_shell_status_page(const char *icon, const char *title, const char *description);

/* Builds the sidebar navigation page: header, window title, and the stack
 * that hosts the (currently always-empty) conversations list plus its
 * empty/error/onboarding states. The list and the stack both carry an
 * accessible label so their purpose is announced even though no rows exist
 * until a real conversation backend lands. */
AdwNavigationPage *gh_shell_sidebar_page(GtkWidget **header_out, GtkWidget **title_out,
                                         GtkWidget **stack_out);

/* Builds the content navigation page: header, the always-revealed read-only
 * banner, and the "no conversation selected" status page. */
AdwNavigationPage *gh_shell_content_page(GtkWidget **banner_out);

G_END_DECLS
#endif
