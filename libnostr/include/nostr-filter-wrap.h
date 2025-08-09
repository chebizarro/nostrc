#ifndef __NOSTR_FILTER_WRAP_H__
#define __NOSTR_FILTER_WRAP_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Forward declaration only to avoid pulling internal types into GIR */
typedef struct _Filter NostrFilter;

#ifdef __cplusplus
extern "C" {
#endif

/* GI-friendly accessors for NostrFilter fields using primitives */

/* ids */
/**
 * nostr_filter_ids_len:
 * @filter: (nullable): filter
 *
 * Returns: length of ids
 */
size_t     nostr_filter_ids_len(const NostrFilter *filter);

/**
 * nostr_filter_ids_get:
 * @filter: (nullable): filter
 * @index: index into ids
 *
 * Returns: (transfer none) (nullable): internal string pointer
 */
const char *nostr_filter_ids_get(const NostrFilter *filter, size_t index);

/* kinds */
/**
 * nostr_filter_kinds_len:
 * @filter: (nullable): filter
 *
 * Returns: number of kinds
 */
size_t     nostr_filter_kinds_len(const NostrFilter *filter);

/**
 * nostr_filter_kinds_get:
 * @filter: (nullable): filter
 * @index: index into kinds
 *
 * Returns: kind value
 */
int        nostr_filter_kinds_get(const NostrFilter *filter, size_t index);

/* authors */
/**
 * nostr_filter_authors_len:
 * @filter: (nullable): filter
 *
 * Returns: number of authors
 */
size_t     nostr_filter_authors_len(const NostrFilter *filter);

/**
 * nostr_filter_authors_get:
 * @filter: (nullable): filter
 * @index: index into authors
 *
 * Returns: (transfer none) (nullable): internal string pointer
 */
const char *nostr_filter_authors_get(const NostrFilter *filter, size_t index);

/* timestamps as int64 for GI */
/**
 * nostr_filter_get_since_i64:
 * @filter: (nullable): filter
 *
 * Returns: since timestamp (seconds)
 */
int64_t    nostr_filter_get_since_i64(const NostrFilter *filter);

/**
 * nostr_filter_set_since_i64:
 * @filter: (nullable): filter
 * @since: since timestamp (seconds)
 */
void       nostr_filter_set_since_i64(NostrFilter *filter, int64_t since);

/**
 * nostr_filter_get_until_i64:
 * @filter: (nullable): filter
 *
 * Returns: until timestamp (seconds)
 */
int64_t    nostr_filter_get_until_i64(const NostrFilter *filter);

/**
 * nostr_filter_set_until_i64:
 * @filter: (nullable): filter
 * @until: until timestamp (seconds)
 */
void       nostr_filter_set_until_i64(NostrFilter *filter, int64_t until);

/* Tags as 2D array-like accessors */
/**
 * nostr_filter_tags_len:
 * @filter: (nullable): filter
 *
 * Returns: number of tags rows
 */
size_t     nostr_filter_tags_len(const NostrFilter *filter);

/**
 * nostr_filter_tag_len:
 * @filter: (nullable): filter
 * @tag_index: row index
 *
 * Returns: number of items in the row
 */
size_t     nostr_filter_tag_len(const NostrFilter *filter, size_t tag_index);

/**
 * nostr_filter_tag_get:
 * @filter: (nullable): filter
 * @tag_index: row index
 * @item_index: column index
 *
 * Returns: (transfer none) (nullable): internal string pointer
 */
const char *nostr_filter_tag_get(const NostrFilter *filter, size_t tag_index, size_t item_index);

/* Mutating helpers (GI-friendly) */
/**
 * nostr_filter_add_id:
 * @filter: (nullable): filter
 * @id: (nullable): id string
 *
 * Adds an id. Copies the string; caller retains ownership.
 */
void    nostr_filter_add_id(NostrFilter *filter, const char *id);

/**
 * nostr_filter_add_kind:
 * @filter: (nullable): filter
 * @kind: kind value
 */
void    nostr_filter_add_kind(NostrFilter *filter, int kind);

/**
 * nostr_filter_add_author:
 * @filter: (nullable): filter
 * @author: (nullable): author string
 *
 * Adds an author. Copies the string; caller retains ownership.
 */
void    nostr_filter_add_author(NostrFilter *filter, const char *author);

/**
 * nostr_filter_tags_append:
 * @filter: (nullable): filter
 * @key: (not nullable): tag name (e.g. "#e", "#p")
 * @value: (nullable): value
 * @relay: (nullable): relay hint
 *
 * Appends a tag row like [key, value, relay?]. Copies inputs; caller retains ownership.
 */
void    nostr_filter_tags_append(NostrFilter *filter, const char *key, const char *value, const char *relay);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_FILTER_WRAP_H__ */
