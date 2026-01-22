/**
 * discover-search.h - NIP-50 Index Relay Search for Discover Tab
 *
 * Provides network search functionality for profile discovery:
 * - Search configurable index relays (nostr.band, search.nos.today)
 * - Support NIP-50 search queries for profiles (kind 0)
 * - Parse npub, NIP-05, display name, and keywords
 * - Merge results with local nostrdb cache
 * - Handle search errors gracefully
 */

#ifndef GNOSTR_DISCOVER_SEARCH_H
#define GNOSTR_DISCOVER_SEARCH_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrSearchResultType:
 * @GNOSTR_SEARCH_RESULT_PROFILE: Profile metadata (kind 0)
 *
 * Type of search result.
 */
typedef enum {
  GNOSTR_SEARCH_RESULT_PROFILE = 0
} GnostrSearchResultType;

/**
 * GnostrSearchResult:
 * @type: Result type (currently only profile)
 * @pubkey_hex: 64-char hex pubkey
 * @display_name: Display name from profile metadata
 * @name: Username/handle from profile
 * @nip05: NIP-05 identifier if present
 * @picture: Avatar URL
 * @about: Profile bio/description
 * @from_network: TRUE if from network search, FALSE if from local cache
 * @created_at: Event timestamp
 *
 * A single search result from network or local cache.
 */
typedef struct {
  GnostrSearchResultType type;
  char *pubkey_hex;
  char *display_name;
  char *name;
  char *nip05;
  char *picture;
  char *about;
  gboolean from_network;
  gint64 created_at;
} GnostrSearchResult;

/**
 * gnostr_search_result_free:
 * @result: A #GnostrSearchResult
 *
 * Free a search result and all its fields.
 */
void gnostr_search_result_free(GnostrSearchResult *result);

/**
 * GnostrSearchQueryType:
 * @GNOSTR_SEARCH_QUERY_TEXT: Plain text/keyword search
 * @GNOSTR_SEARCH_QUERY_NPUB: npub1... bech32 pubkey
 * @GNOSTR_SEARCH_QUERY_HEX: 64-char hex pubkey
 * @GNOSTR_SEARCH_QUERY_NIP05: user@domain.com identifier
 *
 * Type of search query detected from user input.
 */
typedef enum {
  GNOSTR_SEARCH_QUERY_TEXT = 0,
  GNOSTR_SEARCH_QUERY_NPUB,
  GNOSTR_SEARCH_QUERY_HEX,
  GNOSTR_SEARCH_QUERY_NIP05
} GnostrSearchQueryType;

/**
 * GnostrSearchQuery:
 * @type: Detected query type
 * @original: Original query string
 * @normalized: Normalized query (e.g., npub -> hex)
 *
 * Parsed search query.
 */
typedef struct {
  GnostrSearchQueryType type;
  char *original;
  char *normalized;
} GnostrSearchQuery;

/**
 * gnostr_search_query_free:
 * @query: A #GnostrSearchQuery
 *
 * Free a parsed search query.
 */
void gnostr_search_query_free(GnostrSearchQuery *query);

/**
 * gnostr_search_parse_query:
 * @text: User input text
 *
 * Parse user input to determine query type and normalize.
 * Detects:
 * - npub1... -> NPUB, converts to hex
 * - 64-char hex -> HEX
 * - user@domain.com -> NIP05
 * - everything else -> TEXT
 *
 * Returns: (transfer full): Parsed query, caller frees with gnostr_search_query_free()
 */
GnostrSearchQuery *gnostr_search_parse_query(const char *text);

/**
 * GnostrSearchCallback:
 * @results: (element-type GnostrSearchResult) (transfer full): Array of results
 * @error: Error if search failed, or NULL
 * @user_data: User data
 *
 * Callback for async search completion.
 * Caller takes ownership of results array and must free with g_ptr_array_unref().
 */
typedef void (*GnostrSearchCallback)(GPtrArray *results,
                                      GError *error,
                                      gpointer user_data);

/**
 * gnostr_discover_search_async:
 * @query: Parsed search query
 * @search_network: Whether to search index relays
 * @search_local: Whether to search local nostrdb cache
 * @limit: Maximum results to return (0 = default 50)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when search completes
 * @user_data: User data for callback
 *
 * Perform async profile search combining network and/or local results.
 * Network search uses NIP-50 on configured index relays.
 * Local search queries nostrdb text search index.
 * Results are deduplicated and merged by pubkey.
 */
void gnostr_discover_search_async(GnostrSearchQuery *query,
                                   gboolean search_network,
                                   gboolean search_local,
                                   int limit,
                                   GCancellable *cancellable,
                                   GnostrSearchCallback callback,
                                   gpointer user_data);

/**
 * gnostr_load_index_relays_into:
 * @out: Array to append relay URLs to
 *
 * Load index relay URLs from GSettings into provided array.
 */
void gnostr_load_index_relays_into(GPtrArray *out);

/**
 * gnostr_search_error_quark:
 *
 * Error domain for search errors.
 */
#define GNOSTR_SEARCH_ERROR (gnostr_search_error_quark())
GQuark gnostr_search_error_quark(void);

typedef enum {
  GNOSTR_SEARCH_ERROR_INVALID_QUERY,
  GNOSTR_SEARCH_ERROR_NETWORK_FAILED,
  GNOSTR_SEARCH_ERROR_CANCELLED,
  GNOSTR_SEARCH_ERROR_NO_RESULTS
} GnostrSearchError;

G_END_DECLS

#endif /* GNOSTR_DISCOVER_SEARCH_H */
