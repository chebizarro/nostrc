#ifndef PAGE_DISCOVER_H
#define PAGE_DISCOVER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PAGE_DISCOVER (gnostr_page_discover_get_type())

G_DECLARE_FINAL_TYPE(GnostrPageDiscover, gnostr_page_discover, GNOSTR, PAGE_DISCOVER, GtkWidget)

/**
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user selects a profile from search results
 * "follow-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to follow a profile (NIP-02)
 * "unfollow-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to unfollow a profile (NIP-02)
 * "mute-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to mute a profile (NIP-51)
 * "copy-npub-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to copy npub to clipboard
 * "open-communities" (gpointer user_data)
 *   - Emitted when user clicks the Communities button (NIP-72)
 * "watch-live" (gchar* event_id_hex, gpointer user_data)
 *   - Emitted when user clicks Watch Live on a live activity (NIP-53)
 * "open-article" (gchar* event_id_hex, gint kind, gpointer user_data)
 *   - Emitted when user clicks to open an article (NIP-23/NIP-54)
 *   - kind is 30023 for long-form, 30818 for wiki
 * "zap-article-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *   - Emitted when user requests to zap an article author
 * "search-hashtag" (gchar* hashtag, gpointer user_data)
 *   - Emitted when user clicks a trending hashtag chip
 *   - hashtag is without '#' prefix (e.g. "bitcoin", "nostr")
 */

/**
 * gnostr_page_discover_new:
 *
 * Creates a new Discover page widget.
 *
 * Returns: (transfer full): A new #GnostrPageDiscover
 */
GnostrPageDiscover *gnostr_page_discover_new(void);

/**
 * gnostr_page_discover_set_loading:
 * @self: the discover page
 * @is_loading: whether to show loading state
 *
 * Show/hide loading spinner.
 */
void gnostr_page_discover_set_loading(GnostrPageDiscover *self, gboolean is_loading);

/**
 * gnostr_page_discover_clear_results:
 * @self: the discover page
 *
 * Clear all search results and show empty state.
 */
void gnostr_page_discover_clear_results(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_get_search_text:
 * @self: the discover page
 *
 * Get the current search text.
 *
 * Returns: (transfer none): The search text, or NULL if empty
 */
const char *gnostr_page_discover_get_search_text(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_is_network_search_enabled:
 * @self: the discover page
 *
 * Check if network (index relay) search is enabled.
 *
 * Returns: TRUE if network search toggle is active
 */
gboolean gnostr_page_discover_is_network_search_enabled(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_is_local_search_enabled:
 * @self: the discover page
 *
 * Check if local (nostrdb cache) search is enabled.
 *
 * Returns: TRUE if local search toggle is active
 */
gboolean gnostr_page_discover_is_local_search_enabled(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_get_result_count:
 * @self: the discover page
 *
 * Get the number of search results currently displayed.
 *
 * Returns: Number of results in the list
 */
guint gnostr_page_discover_get_result_count(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_load_profiles:
 * @self: the discover page
 *
 * Load all cached profiles from nostrdb. Call this when the page becomes visible.
 */
void gnostr_page_discover_load_profiles(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_set_following:
 * @self: the discover page
 * @pubkeys: NULL-terminated array of pubkey hex strings that are followed
 *
 * Set the list of pubkeys the current user follows.
 */
void gnostr_page_discover_set_following(GnostrPageDiscover *self, const char **pubkeys);

/**
 * gnostr_page_discover_set_muted:
 * @self: the discover page
 * @pubkeys: NULL-terminated array of pubkey hex strings that are muted (NIP-51)
 *
 * Set the list of pubkeys that are muted. Muted profiles will be shown grayed out with a badge.
 */
void gnostr_page_discover_set_muted(GnostrPageDiscover *self, const char **pubkeys);

/**
 * gnostr_page_discover_set_blocked:
 * @self: the discover page
 * @pubkeys: NULL-terminated array of pubkey hex strings that are blocked
 *
 * Set the list of pubkeys that are blocked. Blocked profiles will be filtered out entirely.
 */
void gnostr_page_discover_set_blocked(GnostrPageDiscover *self, const char **pubkeys);

/**
 * gnostr_page_discover_refresh:
 * @self: the discover page
 *
 * Force reload profiles from the database.
 */
void gnostr_page_discover_refresh(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_load_live_activities:
 * @self: the discover page
 *
 * Load live activities (NIP-53) from the network.
 */
void gnostr_page_discover_load_live_activities(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_is_live_mode:
 * @self: the discover page
 *
 * Check if the discover page is in live activities mode.
 *
 * Returns: TRUE if in live mode, FALSE if in people mode
 */
gboolean gnostr_page_discover_is_live_mode(GnostrPageDiscover *self);

/**
 * gnostr_page_discover_is_articles_mode:
 * @self: the discover page
 *
 * Check if the discover page is in articles mode.
 *
 * Returns: TRUE if in articles mode (NIP-23/NIP-54)
 */
gboolean gnostr_page_discover_is_articles_mode(GnostrPageDiscover *self);

G_END_DECLS

#endif /* PAGE_DISCOVER_H */
