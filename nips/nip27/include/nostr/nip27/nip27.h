#ifndef NIPS_NIP27_NOSTR_NIP27_NIP27_H
#define NIPS_NIP27_NOSTR_NIP27_NIP27_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr/nip19/nip19.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-27: Text Note References
 *
 * Defines how nostr: URIs are embedded in event content to reference
 * other nostr entities. This module parses content text into a sequence
 * of blocks: plain text segments and nostr entity references.
 *
 * Example input:
 *   "Hello nostr:npub1abc... check nostr:nevent1def..."
 *
 * Produces blocks:
 *   [TEXT "Hello "] [MENTION npub1abc...] [TEXT " check "] [MENTION nevent1def...]
 */

/**
 * Block types produced by the content parser.
 */
typedef enum {
    NOSTR_NIP27_TEXT,       /**< Plain text segment */
    NOSTR_NIP27_MENTION,    /**< nostr: URI reference to an entity */
} NostrNip27BlockType;

/**
 * A parsed block from event content.
 *
 * For TEXT blocks: text/length contain the plain text segment.
 * For MENTION blocks: text/length contain the full "nostr:..." string,
 *   bech32 points past the "nostr:" prefix, and bech32_type identifies
 *   the entity kind (npub, note, nprofile, nevent, naddr).
 *
 * All pointers are borrowed from the original content string and are
 * valid only while that string is alive.
 */
typedef struct {
    NostrNip27BlockType type;   /**< Block type */
    const char *text;           /**< Start of this block's text (borrowed) */
    size_t length;              /**< Length of this block's text */
    const char *bech32;         /**< bech32 portion, MENTION only (borrowed) */
    NostrBech32Type bech32_type; /**< Entity type, MENTION only */
} NostrNip27Block;

/**
 * nostr_nip27_parse:
 * @content: (in) (not nullable): event content text to parse
 * @blocks: (out caller-allocates): array to fill with parsed blocks
 * @max_blocks: maximum number of blocks to store
 * @out_count: (out): number of blocks actually produced
 *
 * Parses event content into a sequence of text and mention blocks.
 * Scans for "nostr:" URIs, validates them against NIP-19, and splits
 * the content accordingly.
 *
 * All block pointers are borrowed from @content (zero-alloc).
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip27_parse(const char *content, NostrNip27Block *blocks,
                       size_t max_blocks, size_t *out_count);

/**
 * nostr_nip27_count_mentions:
 * @content: (in) (not nullable): event content text
 *
 * Counts the number of valid nostr: URI references in the content.
 *
 * Returns: number of valid mentions found
 */
size_t nostr_nip27_count_mentions(const char *content);

/**
 * Callback for reference replacement.
 *
 * @bech32: the NIP-19 bech32 string (without "nostr:" prefix)
 * @bech32_type: detected entity type
 * @out_text: (out): replacement text to substitute (borrowed or owned by caller)
 * @out_len: (out): length of replacement text
 * @user_data: caller-provided context
 *
 * Returns: true to replace this reference, false to keep the original URI
 */
typedef bool (*NostrNip27Formatter)(const char *bech32,
                                     NostrBech32Type bech32_type,
                                     const char **out_text,
                                     size_t *out_len,
                                     void *user_data);

/**
 * nostr_nip27_replace:
 * @content: (in) (not nullable): event content text
 * @formatter: callback that provides replacement text for each mention
 * @user_data: passed to formatter callback
 *
 * Creates a new string with nostr: URI references replaced by
 * the text provided by the formatter callback.
 *
 * Returns: (transfer full) (nullable): new string with replacements,
 *   caller frees with free(). NULL on error.
 */
char *nostr_nip27_replace(const char *content,
                           NostrNip27Formatter formatter,
                           void *user_data);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP27_NOSTR_NIP27_NIP27_H */
