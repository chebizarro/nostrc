/**
 * GnostrAppsPage - App Handler Discovery and Preferences
 *
 * A page for browsing NIP-89 app handlers and managing user preferences.
 *
 * Features:
 * - Browse all known app handlers
 * - Filter by event kind
 * - See which handlers are recommended by followed users
 * - Set preferred handlers for each event kind
 * - View handler details and supported platforms
 */

#ifndef GNOSTR_APPS_PAGE_H
#define GNOSTR_APPS_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_APPS_PAGE (gnostr_apps_page_get_type())

G_DECLARE_FINAL_TYPE(GnostrAppsPage, gnostr_apps_page, GNOSTR, APPS_PAGE, GtkWidget)

/**
 * Signals:
 * "open-handler-website" (gchar* url, gpointer user_data)
 *   - Emitted when user wants to visit the handler's website
 * "preference-changed" (guint kind, gchar* handler_a_tag, gpointer user_data)
 *   - Emitted when user changes their preferred handler for a kind
 */

/**
 * gnostr_apps_page_new:
 *
 * Creates a new Apps page widget.
 *
 * Returns: (transfer full): A new #GnostrAppsPage
 */
GnostrAppsPage *gnostr_apps_page_new(void);

/**
 * gnostr_apps_page_refresh:
 * @self: The apps page
 *
 * Refreshes the list of handlers from cache and network.
 */
void gnostr_apps_page_refresh(GnostrAppsPage *self);

/**
 * gnostr_apps_page_set_loading:
 * @self: The apps page
 * @is_loading: Whether to show loading state
 *
 * Shows or hides the loading indicator.
 */
void gnostr_apps_page_set_loading(GnostrAppsPage *self, gboolean is_loading);

/**
 * gnostr_apps_page_filter_by_kind:
 * @self: The apps page
 * @kind: Event kind to filter by, or 0 for all kinds
 *
 * Filters the handler list to show only handlers for a specific kind.
 */
void gnostr_apps_page_filter_by_kind(GnostrAppsPage *self, guint kind);

/**
 * gnostr_apps_page_get_handler_count:
 * @self: The apps page
 *
 * Gets the number of handlers currently displayed.
 *
 * Returns: Number of visible handlers
 */
guint gnostr_apps_page_get_handler_count(GnostrAppsPage *self);

/**
 * gnostr_apps_page_set_followed_pubkeys:
 * @self: The apps page
 * @pubkeys: NULL-terminated array of followed pubkey hex strings
 *
 * Sets the list of followed pubkeys for recommendation display.
 */
void gnostr_apps_page_set_followed_pubkeys(GnostrAppsPage *self, const char **pubkeys);

G_END_DECLS

#endif /* GNOSTR_APPS_PAGE_H */
