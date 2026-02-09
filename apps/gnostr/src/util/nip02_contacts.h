/**
 * NIP-02 Contact List Service
 *
 * Provides contact list management for the gnostr GTK app.
 * Handles fetching kind 3 events from relays, parsing p tags with
 * full metadata (relay hints, petnames), and caching in nostrdb.
 *
 * NIP-02 defines contact lists as kind 3 events where the content
 * contains relay URLs (deprecated) and tags contain p-tags with format:
 *   ["p", "<pubkey>", "<relay_url>", "<petname>"]
 */

#ifndef GNOSTR_NIP02_CONTACTS_H
#define GNOSTR_NIP02_CONTACTS_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---- Contact Entry Structure ---- */

/**
 * GnostrContactEntry:
 * @pubkey_hex: The 64-char hex pubkey of the contact
 * @relay_hint: Optional relay URL where to find this contact (may be NULL)
 * @petname: Optional local nickname for the contact (may be NULL)
 *
 * Represents a single contact from a NIP-02 kind 3 event.
 */
typedef struct {
    char *pubkey_hex;   /* 64-char hex pubkey (always present) */
    char *relay_hint;   /* Optional relay URL hint */
    char *petname;      /* Optional local petname */
} GnostrContactEntry;

/**
 * gnostr_contact_entry_free:
 * @entry: A contact entry to free
 *
 * Frees a GnostrContactEntry and its contents.
 */
void gnostr_contact_entry_free(GnostrContactEntry *entry);

/* ---- Opaque Contact List Handle ---- */
typedef struct _GnostrContactList GnostrContactList;

/* ---- Initialization ---- */

/**
 * gnostr_contact_list_get_default:
 *
 * Gets the singleton contact list instance for the app.
 * Creates it on first call. Thread-safe.
 *
 * Returns: (transfer none): the shared contact list instance
 */
GnostrContactList *gnostr_contact_list_get_default(void);

/**
 * gnostr_contact_list_shutdown:
 *
 * Releases the singleton instance. Call at app shutdown.
 */
void gnostr_contact_list_shutdown(void);

/* ---- Loading ---- */

/**
 * gnostr_contact_list_load_from_json:
 * @self: contact list instance
 * @event_json: JSON string of kind 3 event
 *
 * Parses a kind 3 contact list event and caches entries.
 * This is for loading from local storage or relay response.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_contact_list_load_from_json(GnostrContactList *self,
                                             const char *event_json);

/**
 * gnostr_contact_list_fetch_async:
 * @self: contact list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: NULL-terminated array of relay URLs (or NULL for defaults)
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's contact list from relays asynchronously.
 * If relays is NULL, uses configured relays.
 * Results are cached in nostrdb automatically.
 */
typedef void (*GnostrContactListFetchCallback)(GnostrContactList *self,
                                                gboolean success,
                                                gpointer user_data);

void gnostr_contact_list_fetch_async(GnostrContactList *self,
                                      const char *pubkey_hex,
                                      const char * const *relays,
                                      GnostrContactListFetchCallback callback,
                                      gpointer user_data);

/* ---- Query Functions ---- */

/**
 * gnostr_contact_list_is_following:
 * @self: contact list instance
 * @pubkey_hex: public key to check (64 hex chars)
 *
 * Checks if a pubkey is in the contact list.
 *
 * Returns: TRUE if following
 */
gboolean gnostr_contact_list_is_following(GnostrContactList *self,
                                           const char *pubkey_hex);

/**
 * gnostr_contact_list_get_relay_hint:
 * @self: contact list instance
 * @pubkey_hex: public key to look up (64 hex chars)
 *
 * Gets the relay hint for a followed pubkey.
 *
 * Returns: (transfer none) (nullable): relay URL hint or NULL if none
 */
const char *gnostr_contact_list_get_relay_hint(GnostrContactList *self,
                                                const char *pubkey_hex);

/**
 * gnostr_contact_list_get_petname:
 * @self: contact list instance
 * @pubkey_hex: public key to look up (64 hex chars)
 *
 * Gets the petname for a followed pubkey.
 *
 * Returns: (transfer none) (nullable): petname or NULL if none
 */
const char *gnostr_contact_list_get_petname(GnostrContactList *self,
                                             const char *pubkey_hex);

/**
 * gnostr_contact_list_get_entry:
 * @self: contact list instance
 * @pubkey_hex: public key to look up (64 hex chars)
 *
 * Gets the full contact entry for a followed pubkey.
 *
 * Returns: (transfer none) (nullable): contact entry or NULL if not found
 */
const GnostrContactEntry *gnostr_contact_list_get_entry(GnostrContactList *self,
                                                         const char *pubkey_hex);

/* ---- Accessors ---- */

/**
 * gnostr_contact_list_get_pubkeys:
 * @self: contact list instance
 * @count: (out): number of contacts
 *
 * Gets all followed pubkeys.
 *
 * Returns: (transfer container): array of pubkey hex strings (do not free strings)
 */
const char **gnostr_contact_list_get_pubkeys(GnostrContactList *self, size_t *count);

/**
 * gnostr_contact_list_get_entries:
 * @self: contact list instance
 * @count: (out): number of contacts
 *
 * Gets all contact entries with full metadata.
 *
 * Returns: (transfer container): array of GnostrContactEntry pointers (do not free entries)
 */
const GnostrContactEntry **gnostr_contact_list_get_entries(GnostrContactList *self, size_t *count);

/**
 * gnostr_contact_list_get_count:
 * @self: contact list instance
 *
 * Gets the number of contacts in the list.
 *
 * Returns: number of contacts
 */
size_t gnostr_contact_list_get_count(GnostrContactList *self);

/**
 * gnostr_contact_list_get_user_pubkey:
 * @self: contact list instance
 *
 * Gets the pubkey of the user whose contact list this is.
 *
 * Returns: (transfer none) (nullable): user's pubkey hex or NULL if not set
 */
const char *gnostr_contact_list_get_user_pubkey(GnostrContactList *self);

/**
 * gnostr_contact_list_get_last_update:
 * @self: contact list instance
 *
 * Gets the created_at timestamp of the loaded contact list event.
 *
 * Returns: Unix timestamp or 0 if not loaded
 */
gint64 gnostr_contact_list_get_last_update(GnostrContactList *self);

/* ---- Convenience Functions ---- */

/**
 * gnostr_contact_list_load_from_ndb:
 * @self: contact list instance
 * @pubkey_hex: user's public key (64 hex chars)
 *
 * Loads the contact list from local nostrdb cache.
 * Faster than fetching from relays but may be stale.
 *
 * Returns: TRUE if found and loaded
 */
gboolean gnostr_contact_list_load_from_ndb(GnostrContactList *self,
                                            const char *pubkey_hex);

/**
 * gnostr_contact_list_get_pubkeys_with_relay_hints:
 * @self: contact list instance
 * @relay_hints: (out): GPtrArray of gchar* relay URLs (parallel to pubkeys)
 *
 * Gets all followed pubkeys along with their relay hints.
 * Useful for efficiently querying multiple relays for profiles.
 * relay_hints array may contain NULL entries for contacts without hints.
 *
 * Returns: (transfer container): array of pubkey hex strings
 */
GPtrArray *gnostr_contact_list_get_pubkeys_with_relay_hints(GnostrContactList *self,
                                                             GPtrArray **relay_hints);

/* ---- Mutation Functions (nostrc-s0e0) ---- */

/**
 * gnostr_contact_list_add:
 * @self: contact list instance
 * @pubkey_hex: public key to add (64 hex chars)
 * @relay_hint: (nullable): relay URL hint for this contact
 *
 * Adds a contact to the in-memory list. Call save_async to publish.
 *
 * Returns: TRUE if added, FALSE if already following or invalid
 */
gboolean gnostr_contact_list_add(GnostrContactList *self,
                                  const char *pubkey_hex,
                                  const char *relay_hint);

/**
 * gnostr_contact_list_remove:
 * @self: contact list instance
 * @pubkey_hex: public key to remove (64 hex chars)
 *
 * Removes a contact from the in-memory list. Call save_async to publish.
 *
 * Returns: TRUE if removed, FALSE if not found
 */
gboolean gnostr_contact_list_remove(GnostrContactList *self,
                                     const char *pubkey_hex);

/**
 * GnostrContactListSaveCallback:
 * @self: the contact list
 * @success: TRUE if published to at least one relay
 * @error_msg: (nullable): error message on failure
 * @user_data: user data
 *
 * Callback for gnostr_contact_list_save_async().
 */
typedef void (*GnostrContactListSaveCallback)(GnostrContactList *self,
                                               gboolean success,
                                               const char *error_msg,
                                               gpointer user_data);

/**
 * gnostr_contact_list_save_async:
 * @self: contact list instance
 * @callback: (nullable): callback when save completes
 * @user_data: user data for callback
 *
 * Builds a kind 3 event from the current contact list state,
 * signs it via the signer service, and publishes to relays.
 */
void gnostr_contact_list_save_async(GnostrContactList *self,
                                     GnostrContactListSaveCallback callback,
                                     gpointer user_data);

G_END_DECLS

#endif /* GNOSTR_NIP02_CONTACTS_H */
