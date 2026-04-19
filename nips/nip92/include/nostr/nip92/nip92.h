#ifndef NIPS_NIP92_NOSTR_NIP92_NIP92_H
#define NIPS_NIP92_NOSTR_NIP92_NIP92_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-92: Image Metadata (imeta)
 *
 * Events can include "imeta" tags to attach metadata about images
 * referenced in the event content. Each tag element after "imeta"
 * is a "key value" pair separated by a space.
 *
 * Tag format:
 *   ["imeta",
 *    "url https://example.com/image.jpg",
 *    "blurhash eDG*7p~AE34;...",
 *    "dim 800x600",
 *    "alt Description text"]
 */

/**
 * Parsed image metadata entry.
 *
 * The url, blurhash, and alt pointers are borrowed from the event's
 * tag data (pointing past the key prefix). They are valid only while
 * the source event is alive and unmodified.
 *
 * width and height are parsed from the "dim WxH" field.
 */
typedef struct {
    const char *url;       /**< Image URL (borrowed, may be NULL) */
    const char *blurhash;  /**< Blurhash string (borrowed, may be NULL) */
    const char *alt;       /**< Alt text (borrowed, may be NULL) */
    int width;             /**< Image width in pixels (0 if unknown) */
    int height;            /**< Image height in pixels (0 if unknown) */
} NostrNip92Entry;

/**
 * nostr_nip92_parse:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @entries: (out caller-allocates): array to fill with parsed entries
 * @max_entries: maximum number of entries to parse
 * @out_count: (out): number of entries actually parsed
 *
 * Parses all "imeta" tags from the event into the caller-provided array.
 * Each imeta tag must have at least 3 elements (key + at least 2 fields)
 * to be considered valid.
 *
 * The entry pointers are borrowed from the event's tag data.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip92_parse(const NostrEvent *ev, NostrNip92Entry *entries,
                       size_t max_entries, size_t *out_count);

/**
 * nostr_nip92_find_url:
 * @ev: (in) (transfer none) (not nullable): event to search
 * @url: URL to look up
 * @out: (out): entry for the matching URL
 *
 * Finds the imeta entry for a specific URL.
 *
 * Returns: 0 on success, -ENOENT if not found, -EINVAL on error
 */
int nostr_nip92_find_url(const NostrEvent *ev, const char *url,
                          NostrNip92Entry *out);

/**
 * nostr_nip92_count:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Counts the number of valid imeta tags in the event.
 *
 * Returns: number of imeta entries, or 0 if none
 */
size_t nostr_nip92_count(const NostrEvent *ev);

/**
 * nostr_nip92_build_tag:
 * @entry: (in) (not nullable): entry with metadata to encode
 *
 * Constructs an "imeta" NostrTag from the given entry data.
 * The caller owns the returned tag and must free it with nostr_tag_free().
 *
 * At minimum, url must be set. Other fields are included if non-NULL/non-zero.
 *
 * Returns: (transfer full) (nullable): new NostrTag, or NULL on error
 */
NostrTag *nostr_nip92_build_tag(const NostrNip92Entry *entry);

/**
 * nostr_nip92_add:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @entry: (in) (not nullable): entry with metadata to add
 *
 * Adds an "imeta" tag to the event's existing tags.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip92_add(NostrEvent *ev, const NostrNip92Entry *entry);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP92_NOSTR_NIP92_NIP92_H */
