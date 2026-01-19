#ifndef NOSTR_NIP51_H
#define NOSTR_NIP51_H

/**
 * NIP-51: User Lists
 *
 * Implements user list management for mutes, bookmarks, and other lists.
 * Supports both public and private (NIP-44 encrypted) entries.
 *
 * List Types:
 * - Kind 10000: Mute list (users, words, hashtags, events)
 * - Kind 10001: Pin list
 * - Kind 10003: Bookmark list
 * - Kind 30000: Categorized people lists (addressable)
 * - Kind 30003: Bookmark sets (addressable)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nostr-event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Data Structures ---- */

/**
 * NostrListEntry:
 *
 * A single entry in a NIP-51 list.
 * Can be public (in tags array) or private (encrypted in content).
 */
typedef struct {
    char *tag_name;     /* Tag type: "p", "e", "t", "word", "a", "r" */
    char *value;        /* Primary value: pubkey, event_id, hashtag, etc. */
    char *extra;        /* Optional: relay hint or additional param */
    bool is_private;    /* true = encrypted in content field */
} NostrListEntry;

/**
 * NostrList:
 *
 * Container for a NIP-51 list with public and private entries.
 */
typedef struct {
    NostrListEntry **entries;   /* Array of entry pointers */
    size_t count;               /* Number of entries */
    size_t capacity;            /* Allocated capacity */
    char *identifier;           /* d-tag value for addressable lists */
    char *title;                /* Optional title tag */
    char *description;          /* Optional description tag */
} NostrList;

/* ---- Memory Management ---- */

/**
 * nostr_nip51_list_new:
 *
 * Creates a new empty list.
 *
 * Returns: (transfer full): new list, or NULL on error
 */
NostrList *nostr_nip51_list_new(void);

/**
 * nostr_nip51_list_free:
 * @list: (transfer full) (nullable): list to free
 *
 * Frees a list and all its entries.
 */
void nostr_nip51_list_free(NostrList *list);

/**
 * nostr_nip51_entry_new:
 * @tag_name: tag type (e.g., "p", "e", "t", "word")
 * @value: primary value
 * @extra: (nullable): optional relay hint or extra param
 * @is_private: true to encrypt this entry
 *
 * Creates a new list entry.
 *
 * Returns: (transfer full) (nullable): new entry, or NULL on error
 */
NostrListEntry *nostr_nip51_entry_new(const char *tag_name,
                                       const char *value,
                                       const char *extra,
                                       bool is_private);

/**
 * nostr_nip51_entry_free:
 * @entry: (transfer full) (nullable): entry to free
 *
 * Frees a list entry.
 */
void nostr_nip51_entry_free(NostrListEntry *entry);

/**
 * nostr_nip51_list_add_entry:
 * @list: list to add to
 * @entry: (transfer full): entry to add (ownership transferred)
 *
 * Adds an entry to the list.
 */
void nostr_nip51_list_add_entry(NostrList *list, NostrListEntry *entry);

/**
 * nostr_nip51_list_set_identifier:
 * @list: list to modify
 * @identifier: d-tag value for addressable lists
 *
 * Sets the identifier (d-tag) for addressable list types.
 */
void nostr_nip51_list_set_identifier(NostrList *list, const char *identifier);

/**
 * nostr_nip51_list_set_title:
 * @list: list to modify
 * @title: title for the list
 *
 * Sets the optional title for the list.
 */
void nostr_nip51_list_set_title(NostrList *list, const char *title);

/* ---- Convenience Entry Builders ---- */

/**
 * nostr_nip51_mute_user:
 * @list: list to add to
 * @pubkey_hex: user's public key (64 hex chars)
 * @is_private: true to encrypt this mute
 *
 * Adds a user mute entry (p-tag).
 */
void nostr_nip51_mute_user(NostrList *list, const char *pubkey_hex, bool is_private);

/**
 * nostr_nip51_mute_word:
 * @list: list to add to
 * @word: word to mute (lowercase recommended)
 * @is_private: true to encrypt this mute
 *
 * Adds a word mute entry (word-tag).
 */
void nostr_nip51_mute_word(NostrList *list, const char *word, bool is_private);

/**
 * nostr_nip51_mute_hashtag:
 * @list: list to add to
 * @hashtag: hashtag to mute (without #)
 * @is_private: true to encrypt this mute
 *
 * Adds a hashtag mute entry (t-tag).
 */
void nostr_nip51_mute_hashtag(NostrList *list, const char *hashtag, bool is_private);

/**
 * nostr_nip51_mute_event:
 * @list: list to add to
 * @event_id_hex: event ID to mute (64 hex chars)
 * @is_private: true to encrypt this mute
 *
 * Adds an event mute entry (e-tag).
 */
void nostr_nip51_mute_event(NostrList *list, const char *event_id_hex, bool is_private);

/**
 * nostr_nip51_bookmark_event:
 * @list: list to add to
 * @event_id_hex: event ID to bookmark (64 hex chars)
 * @relay_hint: (nullable): relay URL hint
 * @is_private: true to encrypt this bookmark
 *
 * Adds an event bookmark entry (e-tag with optional relay).
 */
void nostr_nip51_bookmark_event(NostrList *list,
                                  const char *event_id_hex,
                                  const char *relay_hint,
                                  bool is_private);

/**
 * nostr_nip51_bookmark_url:
 * @list: list to add to
 * @url: URL to bookmark
 * @is_private: true to encrypt this bookmark
 *
 * Adds a URL bookmark entry (r-tag).
 */
void nostr_nip51_bookmark_url(NostrList *list, const char *url, bool is_private);

/* ---- Event Creation ---- */

/**
 * nostr_nip51_create_mute_list:
 * @list: list containing mute entries
 * @sk_hex: private key for signing (64 hex chars)
 *
 * Creates a kind 10000 mute list event.
 * Public entries go to tags, private entries are NIP-44 encrypted.
 *
 * Returns: (transfer full) (nullable): signed event, or NULL on error
 */
NostrEvent *nostr_nip51_create_mute_list(NostrList *list, const char *sk_hex);

/**
 * nostr_nip51_create_bookmark_list:
 * @list: list containing bookmark entries
 * @sk_hex: private key for signing (64 hex chars)
 *
 * Creates a kind 10003 bookmark list event.
 * Public entries go to tags, private entries are NIP-44 encrypted.
 *
 * Returns: (transfer full) (nullable): signed event, or NULL on error
 */
NostrEvent *nostr_nip51_create_bookmark_list(NostrList *list, const char *sk_hex);

/**
 * nostr_nip51_create_pin_list:
 * @list: list containing pin entries
 * @sk_hex: private key for signing (64 hex chars)
 *
 * Creates a kind 10001 pin list event.
 *
 * Returns: (transfer full) (nullable): signed event, or NULL on error
 */
NostrEvent *nostr_nip51_create_pin_list(NostrList *list, const char *sk_hex);

/**
 * nostr_nip51_create_list:
 * @kind: event kind (10000, 10001, 10003, 30000, etc.)
 * @list: list containing entries
 * @sk_hex: private key for signing (64 hex chars)
 *
 * Creates a list event of the specified kind.
 * For addressable kinds (30000+), use list->identifier for d-tag.
 *
 * Returns: (transfer full) (nullable): signed event, or NULL on error
 */
NostrEvent *nostr_nip51_create_list(int kind, NostrList *list, const char *sk_hex);

/* ---- Event Parsing ---- */

/**
 * nostr_nip51_parse_list:
 * @event: list event to parse
 * @sk_hex: (nullable): private key for decrypting private entries
 *
 * Parses a list event and extracts all entries.
 * If @sk_hex is provided, attempts to decrypt private entries.
 * If @sk_hex is NULL, only public entries are returned.
 *
 * Returns: (transfer full) (nullable): parsed list, or NULL on error
 */
NostrList *nostr_nip51_parse_list(NostrEvent *event, const char *sk_hex);

/* ---- Private Entry Encryption ---- */

/**
 * nostr_nip51_encrypt_private_entries:
 * @entries: array of private entries
 * @count: number of entries
 * @sk_hex: private key for encryption (64 hex chars)
 *
 * Encrypts entries to JSON and then NIP-44 encrypts to self.
 * Result is base64-encoded ciphertext for event content field.
 *
 * Returns: (transfer full) (nullable): encrypted content, or NULL on error
 */
char *nostr_nip51_encrypt_private_entries(NostrListEntry **entries,
                                           size_t count,
                                           const char *sk_hex);

/**
 * nostr_nip51_decrypt_private_entries:
 * @content: encrypted content from event
 * @sk_hex: private key for decryption (64 hex chars)
 * @count_out: (out): number of entries returned
 *
 * Decrypts NIP-44 encrypted content and parses entries.
 *
 * Returns: (transfer full) (nullable): array of entries, or NULL on error
 */
NostrListEntry **nostr_nip51_decrypt_private_entries(const char *content,
                                                       const char *sk_hex,
                                                       size_t *count_out);

/**
 * nostr_nip51_free_entries:
 * @entries: array of entries to free
 * @count: number of entries
 *
 * Frees an array of entries returned by decrypt_private_entries.
 */
void nostr_nip51_free_entries(NostrListEntry **entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP51_H */
