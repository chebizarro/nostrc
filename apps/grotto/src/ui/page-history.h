/* page-history.h - UI page for viewing transaction/event history
 *
 * Displays signing operation history with:
 * - Paginated list of events
 * - Filtering by event kind, date range, client
 * - Export to JSON/CSV
 * - Clear history option
 * - Copy event ID to clipboard
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_UI_PAGE_HISTORY_H
#define APPS_GNOSTR_SIGNER_UI_PAGE_HISTORY_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GN_TYPE_PAGE_HISTORY (gn_page_history_get_type())

G_DECLARE_FINAL_TYPE(GnPageHistory, gn_page_history, GN, PAGE_HISTORY, AdwPreferencesPage)

/**
 * gn_page_history_new:
 *
 * Creates a new event history page.
 *
 * Returns: (transfer full): A new #GnPageHistory
 */
GnPageHistory *gn_page_history_new(void);

/**
 * gn_page_history_refresh:
 * @self: A #GnPageHistory
 *
 * Refreshes the history list from storage.
 */
void gn_page_history_refresh(GnPageHistory *self);

/**
 * gn_page_history_clear_filters:
 * @self: A #GnPageHistory
 *
 * Clears all active filters.
 */
void gn_page_history_clear_filters(GnPageHistory *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_PAGE_HISTORY_H */
