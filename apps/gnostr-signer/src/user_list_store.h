/* user_list_store.h - User list management (follows, mutes)
 *
 * Manages Nostr user lists:
 * - Follow list (kind:3 contact list)
 * - Mute list (kind:10000 mute list)
 *
 * Lists are stored locally and can be published as Nostr events.
 */
#pragma once

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

G_END_DECLS
