/*
 * gnostr-classifieds-view.h - NIP-99 Classified Listings Grid View
 *
 * Displays a responsive grid of classified listing cards with:
 * - Filter bar (category, price, location)
 * - Sort options (newest, price low/high)
 * - Empty state and loading spinner
 * - Infinite scroll pagination
 */

#ifndef GNOSTR_CLASSIFIEDS_VIEW_H
#define GNOSTR_CLASSIFIEDS_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip99_classifieds.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CLASSIFIEDS_VIEW (gnostr_classifieds_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrClassifiedsView, gnostr_classifieds_view, GNOSTR, CLASSIFIEDS_VIEW, GtkWidget)

/**
 * GnostrClassifiedsSortOrder:
 * @GNOSTR_CLASSIFIEDS_SORT_NEWEST: Sort by publication date, newest first
 * @GNOSTR_CLASSIFIEDS_SORT_OLDEST: Sort by publication date, oldest first
 * @GNOSTR_CLASSIFIEDS_SORT_PRICE_LOW: Sort by price, lowest first
 * @GNOSTR_CLASSIFIEDS_SORT_PRICE_HIGH: Sort by price, highest first
 *
 * Sort order options for the classifieds grid.
 */
typedef enum {
  GNOSTR_CLASSIFIEDS_SORT_NEWEST,
  GNOSTR_CLASSIFIEDS_SORT_OLDEST,
  GNOSTR_CLASSIFIEDS_SORT_PRICE_LOW,
  GNOSTR_CLASSIFIEDS_SORT_PRICE_HIGH
} GnostrClassifiedsSortOrder;

/**
 * Signals:
 *
 * "listing-clicked" (gchar* event_id, gchar* naddr, gpointer user_data)
 *   - Emitted when user clicks to view a listing's details
 *
 * "filter-changed" (gchar* category, gchar* location, gdouble min_price,
 *                   gdouble max_price, gchar* currency, gpointer user_data)
 *   - Emitted when filter settings change
 *
 * "contact-seller" (gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *   - Emitted when user wants to contact a seller
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user wants to view a seller's profile
 *
 * "category-clicked" (gchar* category, gpointer user_data)
 *   - Emitted when user clicks a category tag to filter
 */

typedef struct _GnostrClassifiedsView GnostrClassifiedsView;

/**
 * gnostr_classifieds_view_new:
 *
 * Creates a new classifieds grid view widget.
 *
 * Returns: (transfer full): A new GnostrClassifiedsView widget.
 */
GnostrClassifiedsView *gnostr_classifieds_view_new(void);

/* ============== Listing Management ============== */

/**
 * gnostr_classifieds_view_add_listing:
 * @self: The classifieds view widget.
 * @classified: The classified listing to add.
 *
 * Adds a new listing to the grid. The widget copies the data internally.
 */
void gnostr_classifieds_view_add_listing(GnostrClassifiedsView *self,
                                          const GnostrClassified *classified);

/**
 * gnostr_classifieds_view_add_listings:
 * @self: The classifieds view widget.
 * @classifieds: (element-type GnostrClassified): Array of listings to add.
 *
 * Adds multiple listings to the grid at once.
 */
void gnostr_classifieds_view_add_listings(GnostrClassifiedsView *self,
                                           GPtrArray *classifieds);

/**
 * gnostr_classifieds_view_remove_listing:
 * @self: The classifieds view widget.
 * @event_id: Event ID of the listing to remove.
 *
 * Removes a listing from the grid by its event ID.
 */
void gnostr_classifieds_view_remove_listing(GnostrClassifiedsView *self,
                                             const char *event_id);

/**
 * gnostr_classifieds_view_clear:
 * @self: The classifieds view widget.
 *
 * Removes all listings from the grid.
 */
void gnostr_classifieds_view_clear(GnostrClassifiedsView *self);

/**
 * gnostr_classifieds_view_get_listing_count:
 * @self: The classifieds view widget.
 *
 * Returns the number of listings currently displayed.
 *
 * Returns: Number of listings.
 */
guint gnostr_classifieds_view_get_listing_count(GnostrClassifiedsView *self);

/* ============== Filtering ============== */

/**
 * gnostr_classifieds_view_set_category_filter:
 * @self: The classifieds view widget.
 * @category: Category to filter by, or NULL to show all.
 *
 * Filters listings by category.
 */
void gnostr_classifieds_view_set_category_filter(GnostrClassifiedsView *self,
                                                  const char *category);

/**
 * gnostr_classifieds_view_set_location_filter:
 * @self: The classifieds view widget.
 * @location: Location to filter by, or NULL to show all.
 *
 * Filters listings by location.
 */
void gnostr_classifieds_view_set_location_filter(GnostrClassifiedsView *self,
                                                  const char *location);

/**
 * gnostr_classifieds_view_set_price_range:
 * @self: The classifieds view widget.
 * @min_price: Minimum price, or -1 for no minimum.
 * @max_price: Maximum price, or -1 for no maximum.
 * @currency: Currency code for the price filter.
 *
 * Filters listings by price range.
 */
void gnostr_classifieds_view_set_price_range(GnostrClassifiedsView *self,
                                              gdouble min_price,
                                              gdouble max_price,
                                              const char *currency);

/**
 * gnostr_classifieds_view_clear_filters:
 * @self: The classifieds view widget.
 *
 * Clears all active filters.
 */
void gnostr_classifieds_view_clear_filters(GnostrClassifiedsView *self);

/**
 * gnostr_classifieds_view_get_category_filter:
 * @self: The classifieds view widget.
 *
 * Gets the current category filter.
 *
 * Returns: (transfer none): Current category filter or NULL.
 */
const char *gnostr_classifieds_view_get_category_filter(GnostrClassifiedsView *self);

/**
 * gnostr_classifieds_view_get_location_filter:
 * @self: The classifieds view widget.
 *
 * Gets the current location filter.
 *
 * Returns: (transfer none): Current location filter or NULL.
 */
const char *gnostr_classifieds_view_get_location_filter(GnostrClassifiedsView *self);

/* ============== Sorting ============== */

/**
 * gnostr_classifieds_view_set_sort_order:
 * @self: The classifieds view widget.
 * @order: The sort order to apply.
 *
 * Sets the sort order for the listings grid.
 */
void gnostr_classifieds_view_set_sort_order(GnostrClassifiedsView *self,
                                             GnostrClassifiedsSortOrder order);

/**
 * gnostr_classifieds_view_get_sort_order:
 * @self: The classifieds view widget.
 *
 * Gets the current sort order.
 *
 * Returns: Current sort order.
 */
GnostrClassifiedsSortOrder gnostr_classifieds_view_get_sort_order(GnostrClassifiedsView *self);

/* ============== View State ============== */

/**
 * gnostr_classifieds_view_set_loading:
 * @self: The classifieds view widget.
 * @is_loading: TRUE to show loading spinner.
 *
 * Sets the loading state.
 */
void gnostr_classifieds_view_set_loading(GnostrClassifiedsView *self,
                                          gboolean is_loading);

/**
 * gnostr_classifieds_view_is_loading:
 * @self: The classifieds view widget.
 *
 * Checks if the view is in loading state.
 *
 * Returns: TRUE if loading.
 */
gboolean gnostr_classifieds_view_is_loading(GnostrClassifiedsView *self);

/**
 * gnostr_classifieds_view_set_logged_in:
 * @self: The classifieds view widget.
 * @logged_in: TRUE if user is logged in.
 *
 * Sets the login state (affects card button sensitivity).
 */
void gnostr_classifieds_view_set_logged_in(GnostrClassifiedsView *self,
                                            gboolean logged_in);

/**
 * gnostr_classifieds_view_set_user_pubkey:
 * @self: The classifieds view widget.
 * @pubkey_hex: Current user's public key (for showing own listings).
 *
 * Sets the current user's pubkey.
 */
void gnostr_classifieds_view_set_user_pubkey(GnostrClassifiedsView *self,
                                              const char *pubkey_hex);

/**
 * gnostr_classifieds_view_show_filter_bar:
 * @self: The classifieds view widget.
 * @show: TRUE to show the filter bar.
 *
 * Shows or hides the filter bar.
 */
void gnostr_classifieds_view_show_filter_bar(GnostrClassifiedsView *self,
                                              gboolean show);

/**
 * gnostr_classifieds_view_set_columns:
 * @self: The classifieds view widget.
 * @columns: Number of columns (0 for auto/responsive).
 *
 * Sets the number of columns in the grid.
 */
void gnostr_classifieds_view_set_columns(GnostrClassifiedsView *self,
                                          guint columns);

/* ============== Available Categories ============== */

/**
 * gnostr_classifieds_view_set_available_categories:
 * @self: The classifieds view widget.
 * @categories: (element-type utf8): Array of category strings.
 *
 * Sets the list of available categories for the filter dropdown.
 */
void gnostr_classifieds_view_set_available_categories(GnostrClassifiedsView *self,
                                                       GPtrArray *categories);

/**
 * gnostr_classifieds_view_add_category:
 * @self: The classifieds view widget.
 * @category: Category to add to the filter options.
 *
 * Adds a single category to the filter options.
 */
void gnostr_classifieds_view_add_category(GnostrClassifiedsView *self,
                                           const char *category);

/* ============== Search ============== */

/**
 * gnostr_classifieds_view_set_search_text:
 * @self: The classifieds view widget.
 * @text: Search text to filter by.
 *
 * Filters listings by search text (matches title, summary, description).
 */
void gnostr_classifieds_view_set_search_text(GnostrClassifiedsView *self,
                                              const char *text);

/**
 * gnostr_classifieds_view_get_search_text:
 * @self: The classifieds view widget.
 *
 * Gets the current search text.
 *
 * Returns: (transfer none): Current search text or NULL.
 */
const char *gnostr_classifieds_view_get_search_text(GnostrClassifiedsView *self);

/* ============== Async Loading ============== */

/**
 * gnostr_classifieds_view_fetch_listings:
 * @self: The classifieds view widget.
 *
 * Fetches listings from relays using current filter settings.
 */
void gnostr_classifieds_view_fetch_listings(GnostrClassifiedsView *self);

/**
 * gnostr_classifieds_view_cancel_fetch:
 * @self: The classifieds view widget.
 *
 * Cancels any ongoing fetch operation.
 */
void gnostr_classifieds_view_cancel_fetch(GnostrClassifiedsView *self);

G_END_DECLS

#endif /* GNOSTR_CLASSIFIEDS_VIEW_H */
