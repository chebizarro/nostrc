#ifndef NOSTR_NIP51_H
#define NOSTR_NIP51_H

#include <nostr/nip51/export.h>

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

/* ---- Lossless synchronous codecs ---- */

/* No cryptography, signing or I/O. Pointer results are owned unless documented
 * borrowed; NULL results set errno. Mutations return negative errno and leave
 * the document unchanged on failure. Legacy structs below remain ABI-stable. */
typedef struct NostrListDocument NostrListDocument;
typedef struct NostrListTag NostrListTag;
typedef struct NostrListSchema NostrListSchema;
typedef enum {
    NOSTR_LIST_PRIVATE_EMPTY,
    NOSTR_LIST_PRIVATE_ENCRYPTED_NIP44,
    NOSTR_LIST_PRIVATE_LEGACY_NIP04,
    NOSTR_LIST_PRIVATE_DECODED,
    NOSTR_LIST_PRIVATE_UNREADABLE
} NostrListPrivateState;
typedef enum {
    NOSTR_LIST_ITEM_INVALID = -1,
    NOSTR_LIST_ITEM_VALID = 0,
    NOSTR_LIST_ITEM_EXTENSION = 1
} NostrListItemValidation;
typedef enum {
    NOSTR_LIST_PRIVACY_OPTIONAL,
    NOSTR_LIST_PRIVACY_PUBLIC,
    NOSTR_LIST_PRIVACY_PRIVATE
} NostrListPrivacy;

#define NOSTR_LIST_MAX_EVENT_BYTES (128u * 1024u)
#define NOSTR_LIST_MAX_FOLLOW_BYTES (1024u * 1024u)
#define NOSTR_LIST_MAX_TAGS 4096u
#define NOSTR_LIST_MAX_FOLLOW_TAGS 16384u
#define NOSTR_LIST_MAX_FIELDS 32u
#define NOSTR_LIST_MAX_FIELD_BYTES (16u * 1024u)
#define NOSTR_LIST_MAX_IDENTIFIER_BYTES 256u
#define NOSTR_LIST_MAX_PRIVATE_BYTES 60000u

NOSTR_NIP51_API NostrListTag *nostr_nip51_tag_new(const char *const *fields, size_t count);
NOSTR_NIP51_API void nostr_nip51_tag_free(NostrListTag *tag);
NOSTR_NIP51_API size_t nostr_nip51_tag_size(const NostrListTag *tag);
/* Borrowed; NULL is absent, "" is present and empty. */
NOSTR_NIP51_API const char *nostr_nip51_tag_field(const NostrListTag *tag, size_t field);
NOSTR_NIP51_API const NostrListSchema *nostr_nip51_schema_lookup(int kind);
NOSTR_NIP51_API size_t nostr_nip51_schema_count(void);
NOSTR_NIP51_API const NostrListSchema *nostr_nip51_schema_at(size_t index);
NOSTR_NIP51_API int nostr_nip51_schema_kind(const NostrListSchema *schema);
NOSTR_NIP51_API bool nostr_nip51_schema_requires_identifier(const NostrListSchema *schema);
NOSTR_NIP51_API bool nostr_nip51_schema_read_only(const NostrListSchema *schema);
NOSTR_NIP51_API NostrListPrivacy nostr_nip51_schema_privacy(const NostrListSchema *schema);
/* Space-separated known item names; metadata remains in the public tag vector. */
NOSTR_NIP51_API const char *nostr_nip51_schema_items(const NostrListSchema *schema);
NOSTR_NIP51_API const char *nostr_nip51_schema_metadata(const NostrListSchema *schema);
NOSTR_NIP51_API NostrListItemValidation nostr_nip51_validate_item(const NostrListSchema *schema, const NostrListTag *tag);
NOSTR_NIP51_API NostrListDocument *nostr_nip51_document_new(int kind, const char *identifier);
NOSTR_NIP51_API NostrListDocument *nostr_nip51_document_parse_event(const NostrEvent *event);
NOSTR_NIP51_API void nostr_nip51_document_free(NostrListDocument *document);
NOSTR_NIP51_API int nostr_nip51_document_kind(const NostrListDocument *document);
NOSTR_NIP51_API const char *nostr_nip51_document_identifier(const NostrListDocument *document);
NOSTR_NIP51_API bool nostr_nip51_document_deprecated(const NostrListDocument *document);
NOSTR_NIP51_API size_t nostr_nip51_document_tag_count(const NostrListDocument *document, bool private_tags);
NOSTR_NIP51_API const NostrListTag *nostr_nip51_document_tag(const NostrListDocument *document, bool private_tags, size_t index);
/* Copies the tag; insertion appends. Replace/remove address an occurrence. */
NOSTR_NIP51_API int nostr_nip51_document_append(NostrListDocument *document, bool private_tag, const NostrListTag *tag);
NOSTR_NIP51_API int nostr_nip51_document_replace(NostrListDocument *document, bool private_tag, size_t index, const NostrListTag *tag);
NOSTR_NIP51_API int nostr_nip51_document_remove(NostrListDocument *document, bool private_tag, size_t index);
NOSTR_NIP51_API NostrListPrivateState nostr_nip51_document_private_state(const NostrListDocument *document);
/* Marks a failed external decryption without clearing retained private tags. */
NOSTR_NIP51_API void nostr_nip51_document_mark_unreadable(NostrListDocument *document);
/* Atomically replaces the private vector; length is bytes, no embedded NUL. */
NOSTR_NIP51_API int nostr_nip51_document_parse_private_tags(NostrListDocument *document, const char *plaintext, size_t length);
NOSTR_NIP51_API char *nostr_nip51_document_serialize_private_tags(const NostrListDocument *document);
NOSTR_NIP51_API int nostr_nip51_document_validate(const NostrListDocument *document);
/* encrypted_content must be supplied after decoding/editing private tags. NULL
 * preserves untouched source ciphertext. Unreadable and read-only documents
 * cannot be built. Unknown extension tags are preserved, never filtered. */
NOSTR_NIP51_API NostrEvent *nostr_nip51_document_build_unsigned(const NostrListDocument *document, const char *author, int64_t created_at, const char *encrypted_content);

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

/* Legacy void mutators set errno and invalidate the builder on any failure:
 * discard/free it; all subsequent create_* calls fail rather than publishing
 * a shortened replacement. The document API above offers atomic mutations. */

/**
 * nostr_nip51_list_new:
 *
 * Creates a new empty list.
 *
 * Returns: (transfer full): new list, or NULL on error
 */
NOSTR_NIP51_API NostrList *nostr_nip51_list_new(void);

/**
 * nostr_nip51_list_free:
 * @list: (transfer full) (nullable): list to free
 *
 * Frees a list and all its entries.
 */
NOSTR_NIP51_API void nostr_nip51_list_free(NostrList *list);

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
NOSTR_NIP51_API NostrListEntry *nostr_nip51_entry_new(const char *tag_name,
                                       const char *value,
                                       const char *extra,
                                       bool is_private);

/**
 * nostr_nip51_entry_free:
 * @entry: (transfer full) (nullable): entry to free
 *
 * Frees a list entry.
 */
NOSTR_NIP51_API void nostr_nip51_entry_free(NostrListEntry *entry);

/**
 * nostr_nip51_list_add_entry:
 * @list: list to add to
 * @entry: (transfer full): entry to add (ownership transferred)
 *
 * Adds an entry to the list.
 */
NOSTR_NIP51_API void nostr_nip51_list_add_entry(NostrList *list, NostrListEntry *entry);

/**
 * nostr_nip51_list_set_identifier:
 * @list: list to modify
 * @identifier: d-tag value for addressable lists
 *
 * Sets the identifier (d-tag) for addressable list types.
 */
NOSTR_NIP51_API void nostr_nip51_list_set_identifier(NostrList *list, const char *identifier);

/**
 * nostr_nip51_list_set_title:
 * @list: list to modify
 * @title: title for the list
 *
 * Sets the optional title for the list.
 */
NOSTR_NIP51_API void nostr_nip51_list_set_title(NostrList *list, const char *title);

/* ---- Convenience Entry Builders ---- */

/**
 * nostr_nip51_mute_user:
 * @list: list to add to
 * @pubkey_hex: user's public key (64 hex chars)
 * @is_private: true to encrypt this mute
 *
 * Adds a user mute entry (p-tag).
 */
NOSTR_NIP51_API void nostr_nip51_mute_user(NostrList *list, const char *pubkey_hex, bool is_private);

/**
 * nostr_nip51_mute_word:
 * @list: list to add to
 * @word: word to mute (lowercase recommended)
 * @is_private: true to encrypt this mute
 *
 * Adds a word mute entry (word-tag).
 */
NOSTR_NIP51_API void nostr_nip51_mute_word(NostrList *list, const char *word, bool is_private);

/**
 * nostr_nip51_mute_hashtag:
 * @list: list to add to
 * @hashtag: hashtag to mute (without #)
 * @is_private: true to encrypt this mute
 *
 * Adds a hashtag mute entry (t-tag).
 */
NOSTR_NIP51_API void nostr_nip51_mute_hashtag(NostrList *list, const char *hashtag, bool is_private);

/**
 * nostr_nip51_mute_event:
 * @list: list to add to
 * @event_id_hex: event ID to mute (64 hex chars)
 * @is_private: true to encrypt this mute
 *
 * Adds an event mute entry (e-tag).
 */
NOSTR_NIP51_API void nostr_nip51_mute_event(NostrList *list, const char *event_id_hex, bool is_private);

/**
 * nostr_nip51_bookmark_event:
 * @list: list to add to
 * @event_id_hex: event ID to bookmark (64 hex chars)
 * @relay_hint: (nullable): relay URL hint
 * @is_private: true to encrypt this bookmark
 *
 * Adds an event bookmark entry (e-tag with optional relay).
 */
NOSTR_NIP51_API void nostr_nip51_bookmark_event(NostrList *list,
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
NOSTR_NIP51_API void nostr_nip51_bookmark_url(NostrList *list, const char *url, bool is_private);

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
NOSTR_NIP51_API NostrEvent *nostr_nip51_create_mute_list(NostrList *list, const char *sk_hex);

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
NOSTR_NIP51_API NostrEvent *nostr_nip51_create_bookmark_list(NostrList *list, const char *sk_hex);

/**
 * nostr_nip51_create_pin_list:
 * @list: list containing pin entries
 * @sk_hex: private key for signing (64 hex chars)
 *
 * Creates a kind 10001 pin list event.
 *
 * Returns: (transfer full) (nullable): signed event, or NULL on error
 */
NOSTR_NIP51_API NostrEvent *nostr_nip51_create_pin_list(NostrList *list, const char *sk_hex);

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
NOSTR_NIP51_API NostrEvent *nostr_nip51_create_list(int kind, NostrList *list, const char *sk_hex);

/* ---- Event Parsing ---- */

/**
 * nostr_nip51_parse_list:
 * @event: list event to parse
 * @sk_hex: (nullable): private key for decrypting private entries
 *
 * Parses a list event and extracts all entries.
 * If @sk_hex is provided, attempts to decrypt private entries.
 * If @sk_hex is NULL, only public entries are returned.
 * Decryption failure returns NULL, never a partial public list. Tags that cannot
 * fit the legacy three-field representation return NULL/ENOTSUP; use the
 * lossless document API for extension tags and full positional fields.
 *
 * Returns: (transfer full) (nullable): parsed list, or NULL on error
 */
NOSTR_NIP51_API NostrList *nostr_nip51_parse_list(NostrEvent *event, const char *sk_hex);

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
NOSTR_NIP51_API char *nostr_nip51_encrypt_private_entries(NostrListEntry **entries,
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
NOSTR_NIP51_API NostrListEntry **nostr_nip51_decrypt_private_entries(const char *content,
                                                       const char *sk_hex,
                                                       size_t *count_out);

/**
 * nostr_nip51_free_entries:
 * @entries: array of entries to free
 * @count: number of entries
 *
 * Frees an array of entries returned by decrypt_private_entries.
 */
NOSTR_NIP51_API void nostr_nip51_free_entries(NostrListEntry **entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP51_H */
