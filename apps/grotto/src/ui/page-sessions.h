/* page-sessions.h - UI page for managing active client sessions
 *
 * Displays active NIP-46 client sessions with:
 * - Session list with app name, identity, and status
 * - Last activity and remaining time indicators
 * - Revoke session buttons
 * - Revoke all sessions action
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_UI_PAGE_SESSIONS_H
#define APPS_GNOSTR_SIGNER_UI_PAGE_SESSIONS_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GN_TYPE_PAGE_SESSIONS (gn_page_sessions_get_type())

G_DECLARE_FINAL_TYPE(GnPageSessions, gn_page_sessions, GN, PAGE_SESSIONS, AdwPreferencesPage)

/**
 * gn_page_sessions_new:
 *
 * Creates a new sessions management page.
 *
 * Returns: (transfer full): A new #GnPageSessions
 */
GnPageSessions *gn_page_sessions_new(void);

/**
 * gn_page_sessions_refresh:
 * @self: A #GnPageSessions
 *
 * Refreshes the session list from the session manager.
 */
void gn_page_sessions_refresh(GnPageSessions *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_PAGE_SESSIONS_H */
