#ifndef NIPS_NIP73_NOSTR_NIP73_NIP73_H
#define NIPS_NIP73_NOSTR_NIP73_NIP73_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-73: External Content IDs
 *
 * Allows referencing external content (URLs, ISBNs, podcasts, movies,
 * etc.) via "i" tags. The tag value format is "type:identifier".
 *
 * Tag format: ["i", "isbn:978-0-13-468599-1"]
 *             ["i", "doi:10.1000/xyz123"]
 *             ["i", "https://example.com/article"]
 *
 * Uppercase "I" tags are used in NIP-22 for thread root references.
 */

/**
 * External content type classification.
 */
typedef enum {
    NOSTR_NIP73_UNKNOWN = 0,
    NOSTR_NIP73_URL,            /**< http/https URL */
    NOSTR_NIP73_ISBN,           /**< isbn:978-... */
    NOSTR_NIP73_DOI,            /**< doi:10.1000/xyz */
    NOSTR_NIP73_IMDB,           /**< imdb:tt0111161 */
    NOSTR_NIP73_TMDB,           /**< tmdb:movie/278 */
    NOSTR_NIP73_SPOTIFY,        /**< spotify:track:xxx */
    NOSTR_NIP73_YOUTUBE,        /**< youtube:dQw4... */
    NOSTR_NIP73_PODCAST_GUID,   /**< podcast:guid:xxx */
} NostrNip73Type;

/**
 * A parsed external content entry.
 *
 * Pointers are borrowed from the event's tag data and valid only
 * while the source event is alive and unmodified.
 */
typedef struct {
    NostrNip73Type type;        /**< Detected content type */
    const char *value;          /**< Full tag value (borrowed, e.g. "isbn:978-...") */
    const char *identifier;     /**< Points past the type prefix (borrowed) */
} NostrNip73Entry;

/**
 * nostr_nip73_parse:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @entries: (out caller-allocates): array to fill
 * @max_entries: maximum entries to store
 * @out_count: (out): number of entries parsed
 *
 * Parses all lowercase "i" tags from the event.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip73_parse(const NostrEvent *ev, NostrNip73Entry *entries,
                       size_t max_entries, size_t *out_count);

/**
 * nostr_nip73_count:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Counts the number of "i" tags in the event.
 *
 * Returns: number of external content entries
 */
size_t nostr_nip73_count(const NostrEvent *ev);

/**
 * nostr_nip73_detect_type:
 * @value: (in) (not nullable): tag value string (e.g. "isbn:978-...")
 *
 * Detects the external content type from a tag value string.
 *
 * Returns: detected type enum
 */
NostrNip73Type nostr_nip73_detect_type(const char *value);

/**
 * nostr_nip73_build_tag:
 * @type_prefix: type prefix string (e.g. "isbn", "doi")
 * @identifier: the identifier value
 *
 * Builds an "i" tag: ["i", "type:identifier"].
 *
 * Returns: (transfer full) (nullable): new NostrTag, caller frees
 */
NostrTag *nostr_nip73_build_tag(const char *type_prefix, const char *identifier);

/**
 * nostr_nip73_build_url_tag:
 * @url: full URL string
 *
 * Builds an "i" tag for a URL: ["i", "https://..."].
 *
 * Returns: (transfer full) (nullable): new NostrTag, caller frees
 */
NostrTag *nostr_nip73_build_url_tag(const char *url);

/**
 * nostr_nip73_add:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @type_prefix: type prefix (e.g. "isbn")
 * @identifier: identifier value
 *
 * Adds an "i" tag to the event.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip73_add(NostrEvent *ev, const char *type_prefix,
                     const char *identifier);

/**
 * nostr_nip73_is_media_type:
 * @type: content type
 *
 * Returns: true if this is media content (video, music)
 */
bool nostr_nip73_is_media_type(NostrNip73Type type);

/**
 * nostr_nip73_is_reference_type:
 * @type: content type
 *
 * Returns: true if this is reference content (books, papers)
 */
bool nostr_nip73_is_reference_type(NostrNip73Type type);

/**
 * nostr_nip73_type_to_string:
 * @type: content type enum
 *
 * Returns: static string for the type prefix (e.g. "isbn"), or "unknown"
 */
const char *nostr_nip73_type_to_string(NostrNip73Type type);

/**
 * nostr_nip73_type_from_string:
 * @type_str: type prefix string
 *
 * Returns: content type enum
 */
NostrNip73Type nostr_nip73_type_from_string(const char *type_str);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP73_NOSTR_NIP73_NIP73_H */
