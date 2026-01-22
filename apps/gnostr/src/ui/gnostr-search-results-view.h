#ifndef GNOSTR_SEARCH_RESULTS_VIEW_H
#define GNOSTR_SEARCH_RESULTS_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_SEARCH_RESULTS_VIEW (gnostr_search_results_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrSearchResultsView, gnostr_search_results_view, GNOSTR, SEARCH_RESULTS_VIEW, GtkWidget)

/**
 * Signals:
 * "open-note" (gchar* event_id_hex, gpointer user_data)
 *   - Emitted when user selects a note from search results
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on a profile in search results
 * "search-hashtag" (gchar* hashtag, gpointer user_data)
 *   - Emitted when user clicks on a hashtag to search for it
 */

typedef struct _GnostrSearchResultsView GnostrSearchResultsView;

/**
 * gnostr_search_results_view_new:
 *
 * Creates a new search results view widget.
 *
 * Returns: (transfer full): A new #GnostrSearchResultsView
 */
GnostrSearchResultsView *gnostr_search_results_view_new(void);

/**
 * gnostr_search_results_view_set_loading:
 * @self: the search results view
 * @is_loading: whether to show loading state
 *
 * Show/hide loading spinner.
 */
void gnostr_search_results_view_set_loading(GnostrSearchResultsView *self, gboolean is_loading);

/**
 * gnostr_search_results_view_clear_results:
 * @self: the search results view
 *
 * Clear all search results and show empty state.
 */
void gnostr_search_results_view_clear_results(GnostrSearchResultsView *self);

/**
 * gnostr_search_results_view_get_search_text:
 * @self: the search results view
 *
 * Get the current search text.
 *
 * Returns: (transfer none): The search text, or NULL if empty
 */
const char *gnostr_search_results_view_get_search_text(GnostrSearchResultsView *self);

/**
 * gnostr_search_results_view_set_search_text:
 * @self: the search results view
 * @text: the search text to set
 *
 * Set the search text and trigger a search.
 */
void gnostr_search_results_view_set_search_text(GnostrSearchResultsView *self, const char *text);

/**
 * gnostr_search_results_view_execute_search:
 * @self: the search results view
 *
 * Execute a search with the current search text and settings.
 */
void gnostr_search_results_view_execute_search(GnostrSearchResultsView *self);

/**
 * gnostr_search_results_view_is_local_search:
 * @self: the search results view
 *
 * Check if local search mode is active.
 *
 * Returns: TRUE if local search is active, FALSE for relay search
 */
gboolean gnostr_search_results_view_is_local_search(GnostrSearchResultsView *self);

G_END_DECLS

#endif /* GNOSTR_SEARCH_RESULTS_VIEW_H */
