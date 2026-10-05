/* user_list_store.h - User list management (follows, mutes)
 *
 * Manages Nostr user lists:
 * - Follow list (kind:3 contact list)
 * - Mute list (kind:10000 mute list)
 *
 * Lists are stored locally and can be published as Nostr events.
 */
#ifndef APPS_GNOSTR_SIGNER_USER_LIST_STORE_H
#define APPS_GNOSTR_SIGNER_USER_LIST_STORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _UserListStore UserListStore;

/* User list type */
typedef enum {
  USER_LIST_FOLLOWS,    /* kind:3 contact list */
  USER_LIST_MUTES       /* kind:10000 mute list */
} UserListType;

/* User entry */
typedef struct {
  gchar *pubkey;        /* Hex public key */
  gchar *relay_hint;    /* Optional relay hint */
  gchar *petname;       /* Optional petname (follows only) */
  /* Cached profile info (populated via profile_store lookup) */
  gchar *display_name;  /* Cached display name from profile */
  gchar *avatar_url;    /* Cached avatar URL from profile */
  gchar *nip05;         /* Cached NIP-05 identifier */
} UserListEntry;

/* Create a new user list store */
UserListStore *user_list_store_new(UserListType type);

/* Free the store */
void user_list_store_free(UserListStore *store);

/* Free a user entry */
void user_list_entry_free(UserListEntry *entry);

/* Load from local cache */
void user_list_store_load(UserListStore *store);

/* Save to local cache */
void user_list_store_save(UserListStore *store);

/* Add a user. Returns FALSE if already exists. */
gboolean user_list_store_add(UserListStore *store, const gchar *pubkey,
                             const gchar *relay_hint, const gchar *petname);

/* Remove a user by pubkey */
gboolean user_list_store_remove(UserListStore *store, const gchar *pubkey);

/* Check if user is in list */
gboolean user_list_store_contains(UserListStore *store, const gchar *pubkey);

/* Update petname for a user */
gboolean user_list_store_set_petname(UserListStore *store, const gchar *pubkey,
                                     const gchar *petname);

/* List all users.
 * Returns: GPtrArray of UserListEntry* (caller owns)
 */
GPtrArray *user_list_store_list(UserListStore *store);

/* Get user count */
guint user_list_store_count(UserListStore *store);

/* Build event JSON for publishing */
gchar *user_list_store_build_event_json(UserListStore *store);

/* Parse event and update store */
gboolean user_list_store_parse_event(UserListStore *store, const gchar *event_json);

/* Get the kind for this list type */
gint user_list_store_get_kind(UserListStore *store);

/* Clear all entries */
void user_list_store_clear(UserListStore *store);

/* Search entries by pubkey prefix or petname */
GPtrArray *user_list_store_search(UserListStore *store, const gchar *query);

/* Get the list type */
UserListType user_list_store_get_type(UserListStore *store);

/* Get last sync timestamp (0 if never synced) */
gint64 user_list_store_get_last_sync(UserListStore *store);

/* Set last sync timestamp */
void user_list_store_set_last_sync(UserListStore *store, gint64 timestamp);

/* Merge entries from an event (doesn't replace, just adds missing) */
guint user_list_store_merge_event(UserListStore *store, const gchar *event_json);

/* Update cached profile info for a user entry.
 * Call this after fetching profile from relay or cache.
 */
gboolean user_list_store_update_profile(UserListStore *store, const gchar *pubkey,
                                        const gchar *display_name,
                                        const gchar *avatar_url,
                                        const gchar *nip05);

/* Get entry by pubkey (returns internal pointer, do not free) */
const UserListEntry *user_list_store_get_entry(UserListStore *store, const gchar *pubkey);

/* Get display name for a user (returns petname if set, else display_name, else truncated pubkey) */
gchar *user_list_store_get_display_name(UserListStore *store, const gchar *pubkey);

/* Callback type for profile fetch requests */
typedef void (*UserListProfileFetchCb)(const gchar *pubkey,
                                       const gchar *display_name,
                                       const gchar *avatar_url,
                                       const gchar *nip05,
                                       gpointer user_data);

/* Request profile info for all users in the list (calls callback for each) */
void user_list_store_request_profiles(UserListStore *store,
                                      UserListProfileFetchCb callback,
                                      gpointer user_data);

/* Set the owner pubkey (for generating signed events) */
void user_list_store_set_owner(UserListStore *store, const gchar *owner_pubkey);

/* Get the owner pubkey */
const gchar *user_list_store_get_owner(UserListStore *store);

/* Relay sync status */
typedef enum {
  USER_LIST_SYNC_IDLE,
  USER_LIST_SYNC_FETCHING,
  USER_LIST_SYNC_PUBLISHING,
  USER_LIST_SYNC_SUCCESS,
  USER_LIST_SYNC_ERROR
} UserListSyncStatus;

/* Sync status callback */
typedef void (*UserListSyncCb)(UserListSyncStatus status,
                               const gchar *message,
                               gpointer user_data);

/* Build a subscription filter for fetching user list from relay
 * Returns JSON filter string (caller frees)
 */
gchar *user_list_store_build_fetch_filter(UserListStore *store, const gchar *pubkey);

/* Mark store as synced with current timestamp */
void user_list_store_mark_synced(UserListStore *store);

/* Check if store needs sync (based on last_sync time and threshold) */
gboolean user_list_store_needs_sync(UserListStore *store, gint64 threshold_seconds);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_USER_LIST_STORE_H */
