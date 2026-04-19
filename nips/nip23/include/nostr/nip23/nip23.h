#ifndef NIPS_NIP23_NOSTR_NIP23_NIP23_H
#define NIPS_NIP23_NOSTR_NIP23_NIP23_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-23: Long-form Content
 *
 * Kind 30023 for published articles, kind 30024 for drafts.
 * Addressable events with "d" tag identifier.
 *
 * Tags:
 *   - "d"            — unique identifier (required)
 *   - "title"        — article title
 *   - "summary"      — short description
 *   - "image"        — cover image URL
 *   - "published_at" — original publication timestamp
 *   - "t"            — hashtags/topics (multiple)
 *   - "client"       — creating client application
 */

#define NOSTR_NIP23_KIND_LONG_FORM 30023
#define NOSTR_NIP23_KIND_DRAFT     30024

/**
 * Parsed article metadata with borrowed pointers.
 * Valid while the source event is alive and unmodified.
 */
typedef struct {
    const char *identifier;  /**< "d" tag value (borrowed, required) */
    const char *title;       /**< "title" tag value (borrowed, nullable) */
    const char *summary;     /**< "summary" tag value (borrowed, nullable) */
    const char *image;       /**< "image" tag URL (borrowed, nullable) */
    int64_t published_at;    /**< "published_at" parsed timestamp, 0 if absent */
    const char *client;      /**< "client" tag value (borrowed, nullable) */
} NostrNip23Article;

/**
 * nostr_nip23_parse:
 * @ev: (in): event to inspect
 * @out: (out): parsed article metadata
 *
 * Parses article metadata from event tags.
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no "d" tag
 */
int nostr_nip23_parse(const NostrEvent *ev, NostrNip23Article *out);

/**
 * nostr_nip23_is_article:
 * @ev: (in): event to check
 *
 * Returns: true if event kind is 30023 (published long-form)
 */
bool nostr_nip23_is_article(const NostrEvent *ev);

/**
 * nostr_nip23_is_draft:
 * @ev: (in): event to check
 *
 * Returns: true if event kind is 30024 (draft long-form)
 */
bool nostr_nip23_is_draft(const NostrEvent *ev);

/**
 * nostr_nip23_is_long_form:
 * @ev: (in): event to check
 *
 * Returns: true if event kind is 30023 or 30024
 */
bool nostr_nip23_is_long_form(const NostrEvent *ev);

/**
 * nostr_nip23_get_hashtags:
 * @ev: (in): event to inspect
 * @tags: (out caller-allocates): array of borrowed hashtag pointers
 * @max: maximum hashtags to store
 * @out_count: (out): number of hashtags stored
 *
 * Extracts hashtag values from "t" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip23_get_hashtags(const NostrEvent *ev, const char **tags,
                              size_t max, size_t *out_count);

/**
 * nostr_nip23_count_hashtags:
 * @ev: (in): event to inspect
 *
 * Returns: number of "t" tags in the event
 */
size_t nostr_nip23_count_hashtags(const NostrEvent *ev);

/**
 * nostr_nip23_estimate_reading_time:
 * @content: (in) (nullable): text content
 * @words_per_minute: reading speed (0 for default 200 WPM)
 *
 * Estimates reading time by counting words.
 *
 * Returns: estimated minutes (minimum 1 if content is non-empty)
 */
int nostr_nip23_estimate_reading_time(const char *content,
                                       int words_per_minute);

/**
 * nostr_nip23_create_article:
 * @ev: (inout): event to populate
 * @article: (in): article metadata
 *
 * Sets kind to 30023, adds all metadata tags.
 * Content should be set separately via nostr_event_set_content().
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip23_create_article(NostrEvent *ev, const NostrNip23Article *article);

/**
 * nostr_nip23_create_draft:
 * @ev: (inout): event to populate
 * @article: (in): article metadata
 *
 * Same as create_article but sets kind to 30024 (draft).
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip23_create_draft(NostrEvent *ev, const NostrNip23Article *article);

/**
 * nostr_nip23_add_hashtag:
 * @ev: (inout): event to modify
 * @hashtag: (in): hashtag value (without #)
 *
 * Appends a "t" tag to the event.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip23_add_hashtag(NostrEvent *ev, const char *hashtag);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP23_NOSTR_NIP23_NIP23_H */
