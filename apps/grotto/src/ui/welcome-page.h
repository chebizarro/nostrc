/* welcome-page.h - Welcome screen shown when no profile exists
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPS_GNOSTR_SIGNER_UI_WELCOME_PAGE_H
#define APPS_GNOSTR_SIGNER_UI_WELCOME_PAGE_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_WELCOME_PAGE (welcome_page_get_type())
G_DECLARE_FINAL_TYPE(WelcomePage, welcome_page, WELCOME, PAGE, AdwBin)

/**
 * welcome_page_new:
 *
 * Creates a new #WelcomePage widget.
 *
 * Returns: (transfer full): a new #WelcomePage
 */
WelcomePage *welcome_page_new(void);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_WELCOME_PAGE_H */
