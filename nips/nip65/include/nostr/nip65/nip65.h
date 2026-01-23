/**
 * NIP-65: Relay List Metadata
 *
 * This library provides functions for creating, parsing, and manipulating
 * NIP-65 relay list metadata events (kind 10002).
 *
 * NIP-65 defines how users advertise their preferred relays:
 * - "read" relays: where users primarily read content from
 * - "write" relays: where users primarily publish content to
 * - No marker: relay is used for both reading and writing
 *
 * Format: ["r", "wss://relay.url", "read"|"write"]
 *
 * Reference: https://github.com/nostr-protocol/nips/blob/master/65.md
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** Kind number for NIP-65 relay list metadata events */
#define NOSTR_NIP65_KIND 10002

/**
 * NostrRelayPermission:
 * Defines the read/write permissions for a relay entry.
 */
typedef enum {
  NOSTR_RELAY_PERM_READWRITE = 0,  /**< Both read and write (no marker) */
  NOSTR_RELAY_PERM_READ      = 1,  /**< Read-only ("read" marker) */
  NOSTR_RELAY_PERM_WRITE     = 2,  /**< Write-only ("write" marker) */
} NostrRelayPermission;

/**
 * NostrRelayEntry:
 * Represents a single relay in a NIP-65 relay list.
 */
typedef struct {
  char *url;                      /**< Relay URL (wss:// or ws://) */
  NostrRelayPermission permission; /**< Read/write permission */
} NostrRelayEntry;

/**
 * NostrRelayList:
 * Represents a NIP-65 relay list containing multiple relay entries.
 */
typedef struct {
  NostrRelayEntry *entries;  /**< Array of relay entries */
  size_t count;              /**< Number of entries */
} NostrRelayList;

/* --------------------------------------------------------------------------
 * Relay Entry Functions
 * -------------------------------------------------------------------------- */

/**
 * nostr_nip65_entry_new:
 * @url: Relay URL (must be ws:// or wss://)
 * @permission: Read/write permission for this relay
 *
 * Creates a new relay entry. The URL is duplicated internally.
 *
 * Returns: (transfer full) (nullable): new relay entry, or NULL on error
 */
NostrRelayEntry *nostr_nip65_entry_new(const char *url,
                                       NostrRelayPermission permission);

/**
 * nostr_nip65_entry_free:
 * @entry: (transfer full) (nullable): entry to free
 *
 * Frees a relay entry and its associated URL string.
 */
void nostr_nip65_entry_free(NostrRelayEntry *entry);

/**
 * nostr_nip65_entry_copy:
 * @entry: (nullable): entry to copy
 *
 * Creates a deep copy of a relay entry.
 *
 * Returns: (transfer full) (nullable): copied entry, or NULL on error
 */
NostrRelayEntry *nostr_nip65_entry_copy(const NostrRelayEntry *entry);

/**
 * nostr_nip65_entry_is_readable:
 * @entry: (nullable): entry to check
 *
 * Checks if the relay can be used for reading content.
 *
 * Returns: true if READ or READWRITE permission
 */
bool nostr_nip65_entry_is_readable(const NostrRelayEntry *entry);

/**
 * nostr_nip65_entry_is_writable:
 * @entry: (nullable): entry to check
 *
 * Checks if the relay can be used for publishing content.
 *
 * Returns: true if WRITE or READWRITE permission
 */
bool nostr_nip65_entry_is_writable(const NostrRelayEntry *entry);

/* --------------------------------------------------------------------------
 * Relay List Functions
 * -------------------------------------------------------------------------- */

/**
 * nostr_nip65_list_new:
 *
 * Creates a new empty relay list.
 *
 * Returns: (transfer full): new empty relay list
 */
NostrRelayList *nostr_nip65_list_new(void);

/**
 * nostr_nip65_list_free:
 * @list: (transfer full) (nullable): list to free
 *
 * Frees a relay list and all its entries.
 */
void nostr_nip65_list_free(NostrRelayList *list);

/**
 * nostr_nip65_list_copy:
 * @list: (nullable): list to copy
 *
 * Creates a deep copy of a relay list.
 *
 * Returns: (transfer full) (nullable): copied list, or NULL on error
 */
NostrRelayList *nostr_nip65_list_copy(const NostrRelayList *list);

/**
 * nostr_nip65_add_relay:
 * @list: (inout): relay list to modify
 * @url: relay URL
 * @permission: read/write permission
 *
 * Adds a relay to the list. If the URL already exists, updates its permission.
 * The URL is normalized (lowercased, trailing slash removed) before adding.
 *
 * Returns: 0 on success, -EINVAL on invalid arguments, -ENOMEM on allocation failure
 */
int nostr_nip65_add_relay(NostrRelayList *list,
                          const char *url,
                          NostrRelayPermission permission);

/**
 * nostr_nip65_remove_relay:
 * @list: (inout): relay list to modify
 * @url: relay URL to remove
 *
 * Removes a relay from the list by URL.
 *
 * Returns: 0 on success, -ENOENT if not found
 */
int nostr_nip65_remove_relay(NostrRelayList *list, const char *url);

/**
 * nostr_nip65_find_relay:
 * @list: (nullable): relay list to search
 * @url: relay URL to find
 *
 * Finds a relay entry by URL.
 *
 * Returns: (transfer none) (nullable): pointer to entry, or NULL if not found
 */
NostrRelayEntry *nostr_nip65_find_relay(const NostrRelayList *list,
                                        const char *url);

/**
 * nostr_nip65_get_read_relays:
 * @list: (nullable): source relay list
 * @out_count: (out) (optional): number of URLs returned
 *
 * Returns an array of URLs for all readable relays (READ or READWRITE).
 *
 * Returns: (transfer full) (array length=out_count): NULL-terminated array of URL strings
 */
char **nostr_nip65_get_read_relays(const NostrRelayList *list, size_t *out_count);

/**
 * nostr_nip65_get_write_relays:
 * @list: (nullable): source relay list
 * @out_count: (out) (optional): number of URLs returned
 *
 * Returns an array of URLs for all writable relays (WRITE or READWRITE).
 *
 * Returns: (transfer full) (array length=out_count): NULL-terminated array of URL strings
 */
char **nostr_nip65_get_write_relays(const NostrRelayList *list, size_t *out_count);

/* --------------------------------------------------------------------------
 * Event Building and Parsing
 * -------------------------------------------------------------------------- */

/**
 * nostr_nip65_create_relay_list:
 * @ev: (nullable): event to populate (caller owns)
 * @author_pk: (array fixed-size=32): author pubkey for event.pubkey
 * @list: (nullable): relay list; NULL or empty produces empty list
 * @created_at: unix timestamp
 *
 * Builds a kind 10002 NIP-65 relay list event.
 * Sets kind, pubkey, created_at, content (empty), and tags with "r" entries.
 *
 * Returns: 0 on success, -errno on error
 */
int nostr_nip65_create_relay_list(NostrEvent *ev,
                                  const unsigned char author_pk[32],
                                  const NostrRelayList *list,
                                  uint32_t created_at);

/**
 * nostr_nip65_parse_relay_list:
 * @ev: (nullable): event to parse
 * @out: (out) (transfer full): relay list; free with nostr_nip65_list_free()
 *
 * Parses a kind 10002 event into a relay list.
 *
 * Returns: 0 on success, -ENOENT if kind!=10002, -EINVAL on invalid args
 */
int nostr_nip65_parse_relay_list(const NostrEvent *ev, NostrRelayList **out);

/**
 * nostr_nip65_update_relay_list:
 * @ev: (inout): existing kind 10002 event to update
 * @list: (nullable): new relay list
 *
 * Updates an existing event's tags with a new relay list.
 * Preserves other fields (pubkey, etc).
 *
 * Returns: 0 on success, -errno on error
 */
int nostr_nip65_update_relay_list(NostrEvent *ev, const NostrRelayList *list);

/* --------------------------------------------------------------------------
 * Utility Functions
 * -------------------------------------------------------------------------- */

/**
 * nostr_nip65_normalize_url:
 * @url: relay URL to normalize
 *
 * Normalizes a relay URL:
 * - Lowercases scheme and host
 * - Removes trailing slashes
 * - Validates ws:// or wss:// scheme
 *
 * Returns: (transfer full) (nullable): normalized URL, or NULL if invalid
 */
char *nostr_nip65_normalize_url(const char *url);

/**
 * nostr_nip65_is_valid_relay_url:
 * @url: URL to validate
 *
 * Checks if a URL is a valid Nostr relay URL (ws:// or wss://).
 *
 * Returns: true if valid, false otherwise
 */
bool nostr_nip65_is_valid_relay_url(const char *url);

/**
 * nostr_nip65_permission_to_string:
 * @permission: permission value
 *
 * Converts a permission enum to its NIP-65 string representation.
 *
 * Returns: (transfer none): "read", "write", or NULL for READWRITE
 */
const char *nostr_nip65_permission_to_string(NostrRelayPermission permission);

/**
 * nostr_nip65_permission_from_string:
 * @str: (nullable): permission string ("read" or "write")
 *
 * Parses a permission string from a tag.
 *
 * Returns: permission enum value (READWRITE if NULL or unrecognized)
 */
NostrRelayPermission nostr_nip65_permission_from_string(const char *str);

/**
 * nostr_nip65_free_string_array:
 * @arr: (transfer full) (nullable): NULL-terminated string array to free
 *
 * Frees a string array returned by nostr_nip65_get_read_relays() or
 * nostr_nip65_get_write_relays().
 */
void nostr_nip65_free_string_array(char **arr);

#ifdef __cplusplus
}
#endif
