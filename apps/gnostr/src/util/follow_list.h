#ifndef APPS_GNOSTR_UTIL_FOLLOW_LIST_H
#define APPS_GNOSTR_UTIL_FOLLOW_LIST_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrFollowEntry:
 * @pubkey_hex: 64-char hex pubkey of followed user
 * @relay_hint: optional relay URL hint (may be NULL)
 * @petname: optional petname/alias (may be NULL)
 *
 * Represents a single follow entry from a NIP-02 contact list.
 */
typedef struct {
  gchar *pubkey_hex;
  gchar *relay_hint;
  gchar *petname;
} GnostrFollowEntry;

/**
 * gnostr_follow_entry_free:
 * @entry: (nullable): entry to free
 *
 * Frees a GnostrFollowEntry and its string members.
 */
void gnostr_follow_entry_free(GnostrFollowEntry *entry);

/**
 * GnostrFollowListCallback:
 * @entries: (element-type GnostrFollowEntry) (nullable): array of follow entries, or NULL on error
 * @user_data: user data passed to the async function
 *
 * Callback invoked when follow list fetch completes.
 * Caller receives ownership of entries array and must free with g_ptr_array_unref().
 */
typedef void (*GnostrFollowListCallback)(GPtrArray *entries, gpointer user_data);

/**
 * gnostr_follow_list_fetch_async:
 * @pubkey_hex: 64-char hex pubkey of the user whose follow list to fetch
 * @cancellable: (nullable): optional cancellable
 * @callback: callback to invoke when done
 * @user_data: user data for callback
 *
 * Fetches a user's NIP-02 contact list (kind 3) from relays asynchronously.
 * First tries local nostrdb cache, then queries relays.
 * Results are cached in nostrdb for future lookups.
 *
 * The callback receives a GPtrArray of GnostrFollowEntry* with full info
 * including relay hints and petnames. Caller owns the array.
 */
void gnostr_follow_list_fetch_async(const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GnostrFollowListCallback callback,
                                     gpointer user_data);

/**
 * gnostr_follow_list_get_cached:
 * @pubkey_hex: 64-char hex pubkey of the user
 *
 * Gets cached follow list from local nostrdb.
 *
 * Returns: (element-type GnostrFollowEntry) (transfer full) (nullable):
 *          Array of follow entries or NULL if not cached.
 *          Caller owns and must free with g_ptr_array_unref().
 */
GPtrArray *gnostr_follow_list_get_cached(const gchar *pubkey_hex);

/**
 * gnostr_follow_list_get_pubkeys_cached:
 * @pubkey_hex: 64-char hex pubkey of the user
 *
 * Gets just the pubkeys from cached follow list (convenience wrapper).
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1):
 *          NULL-terminated array of pubkey hex strings or NULL.
 *          Caller must g_strfreev() the result.
 */
gchar **gnostr_follow_list_get_pubkeys_cached(const gchar *pubkey_hex);

G_END_DECLS
#endif /* APPS_GNOSTR_UTIL_FOLLOW_LIST_H */
