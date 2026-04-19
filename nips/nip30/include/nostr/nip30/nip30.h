#ifndef NIPS_NIP30_NOSTR_NIP30_NIP30_H
#define NIPS_NIP30_NOSTR_NIP30_NIP30_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-30: Custom Emoji
 *
 * Custom emoji shortcodes in event content: :shortcode:
 * Resolved via "emoji" tags: ["emoji", "shortcode", "https://url/to/emoji.png"]
 */

/**
 * A parsed custom emoji entry with borrowed pointers.
 */
typedef struct {
    const char *shortcode; /**< Emoji name without colons (borrowed) */
    const char *url;       /**< Image URL (borrowed) */
} NostrNip30Emoji;

/**
 * A shortcode match found in content text.
 */
typedef struct {
    const char *name;   /**< Points into content at the name (borrowed) */
    size_t name_len;    /**< Length of the name (without colons) */
    size_t start;       /**< Start offset of :name: in content */
    size_t end;         /**< End offset (past closing colon) */
} NostrNip30Match;

/**
 * nostr_nip30_parse:
 * @ev: (in): event to inspect
 * @entries: (out caller-allocates): array to fill
 * @max_entries: maximum entries
 * @out_count: (out): number parsed
 *
 * Parses all "emoji" tags from the event.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip30_parse(const NostrEvent *ev, NostrNip30Emoji *entries,
                       size_t max_entries, size_t *out_count);

/**
 * nostr_nip30_count:
 * @ev: (in): event to inspect
 *
 * Returns: number of "emoji" tags
 */
size_t nostr_nip30_count(const NostrEvent *ev);

/**
 * nostr_nip30_get_url:
 * @ev: (in): event to inspect
 * @shortcode: (in): emoji name (without colons)
 *
 * Looks up the URL for a specific emoji shortcode.
 *
 * Returns: (transfer none) (nullable): borrowed URL or NULL
 */
const char *nostr_nip30_get_url(const NostrEvent *ev, const char *shortcode);

/**
 * nostr_nip30_find_all:
 * @content: (in): text content to scan
 * @matches: (out caller-allocates): array of match results
 * @max: maximum matches
 * @out_count: (out): number of matches found
 *
 * Finds all :shortcode: patterns in content.
 * Shortcode names are word characters (a-z, A-Z, 0-9, _).
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip30_find_all(const char *content, NostrNip30Match *matches,
                          size_t max, size_t *out_count);

/**
 * Callback for replacing shortcodes.
 * @name: shortcode name (NOT null-terminated — use name_len)
 * @name_len: length of the name
 * @user_data: caller-provided context
 *
 * Returns: replacement string (static/borrowed, not freed by caller),
 *          or NULL to keep the original :shortcode: text
 */
typedef const char *(*NostrNip30Replacer)(const char *name, size_t name_len,
                                           void *user_data);

/**
 * nostr_nip30_replace_all:
 * @content: (in): text content
 * @replacer: callback for each shortcode
 * @user_data: passed to replacer
 *
 * Replaces all :shortcode: patterns using the callback.
 *
 * Returns: (transfer full) (nullable): new string with replacements,
 *          caller must free(). NULL on allocation error.
 */
char *nostr_nip30_replace_all(const char *content,
                               NostrNip30Replacer replacer,
                               void *user_data);

/**
 * nostr_nip30_add_emoji:
 * @ev: (inout): event to modify
 * @shortcode: (in): emoji name (without colons)
 * @url: (in): image URL
 *
 * Adds an "emoji" tag: ["emoji", "shortcode", "url"]
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip30_add_emoji(NostrEvent *ev, const char *shortcode,
                           const char *url);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP30_NOSTR_NIP30_NIP30_H */
