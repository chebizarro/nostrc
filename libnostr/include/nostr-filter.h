#ifndef __NOSTR_FILTER_H__
#define __NOSTR_FILTER_H__

/* Transitional header: exposes standardized names for filter APIs.
 * For now, it forwards to the existing implementation in filter.h. */

#include <stddef.h>
#include <stdbool.h>
#include "nostr-timestamp.h"
#include "nostr-tag.h"
#include "filter.h" /* defines Filter, Filters and old function names */

#ifdef __cplusplus
extern "C" {
#endif

/* Provide canonical type names */
typedef struct _NostrEvent NostrEvent; /* already declared elsewhere */
typedef Filter  NostrFilter;
typedef Filters NostrFilters;

/* New API names mapped to legacy implementations */
#define nostr_filter_new                         create_filter
#define nostr_filter_free                        free_filter
#define nostr_filter_matches                     filter_matches
#define nostr_filter_match_ignoring_timestamp    filter_match_ignoring_timestamp

#define nostr_filters_new                        create_filters
#define nostr_filters_add                        filters_add
#define nostr_filters_free                       free_filters
#define nostr_filters_match                      filters_match
#define nostr_filters_match_ignoring_timestamp   filters_match_ignoring_timestamp

/* Field getters/setters (for future GObject properties) */
/**
 * nostr_filter_get_ids:
 * @filter: (nullable): filter
 *
 * Returns: (transfer none) (nullable): internal StringArray view of IDs
 */
const StringArray *nostr_filter_get_ids(const NostrFilter *filter);
/**
 * nostr_filter_set_ids:
 * @filter: (nullable): filter (no-op if NULL)
 * @ids: (nullable) (array length=count) (element-type utf8): IDs to set; copied internally
 * @count: number of IDs
 */
void nostr_filter_set_ids(NostrFilter *filter, const char *const *ids, size_t count);

/**
 * nostr_filter_get_kinds:
 * @filter: (nullable): filter
 *
 * Returns: (transfer none) (nullable): internal IntArray view of kinds
 */
const IntArray *nostr_filter_get_kinds(const NostrFilter *filter);
/**
 * nostr_filter_set_kinds:
 * @filter: (nullable): filter (no-op if NULL)
 * @kinds: (nullable) (array length=count): kinds to set; copied internally
 */
void nostr_filter_set_kinds(NostrFilter *filter, const int *kinds, size_t count);

/**
 * nostr_filter_get_authors:
 * @filter: (nullable): filter
 *
 * Returns: (transfer none) (nullable): internal StringArray view of authors
 */
const StringArray *nostr_filter_get_authors(const NostrFilter *filter);
/**
 * nostr_filter_set_authors:
 * @filter: (nullable): filter (no-op if NULL)
 * @authors: (nullable) (array length=count) (element-type utf8): copied internally
 */
void nostr_filter_set_authors(NostrFilter *filter, const char *const *authors, size_t count);

/**
 * nostr_filter_get_tags:
 * @filter: (nullable): filter
 *
 * Returns: (transfer none) (nullable): owned tags pointer
 */
NostrTags *nostr_filter_get_tags(const NostrFilter *filter);
/**
 * nostr_filter_set_tags:
 * @filter: (nullable): filter (no-op if NULL)
 * @tags: (transfer full) (nullable): new tags; previous freed if different
 *
 * Ownership: takes full ownership of @tags.
 */
void nostr_filter_set_tags(NostrFilter *filter, NostrTags *tags);

/**
 * nostr_filter_get_since:
 * @filter: (nullable): filter
 *
 * Returns: since timestamp
 */
Timestamp nostr_filter_get_since(const NostrFilter *filter);
/**
 * nostr_filter_set_since:
 * @filter: (nullable): filter (no-op if NULL)
 * @since: timestamp
 */
void nostr_filter_set_since(NostrFilter *filter, Timestamp since);

/**
 * nostr_filter_get_until:
 * @filter: (nullable): filter
 *
 * Returns: until timestamp
 */
Timestamp nostr_filter_get_until(const NostrFilter *filter);
/**
 * nostr_filter_set_until:
 * @filter: (nullable): filter (no-op if NULL)
 * @until: timestamp
 */
void nostr_filter_set_until(NostrFilter *filter, Timestamp until);

/**
 * nostr_filter_get_limit:
 * @filter: (nullable): filter
 *
 * Returns: limit value (or 0 if unset)
 */
int nostr_filter_get_limit(const NostrFilter *filter);
/**
 * nostr_filter_set_limit:
 * @filter: (nullable): filter (no-op if NULL)
 * @limit: integer limit
 */
void nostr_filter_set_limit(NostrFilter *filter, int limit);

/**
 * nostr_filter_get_search:
 * @filter: (nullable): filter
 *
 * Returns: (transfer none) (nullable): search string
 */
const char *nostr_filter_get_search(const NostrFilter *filter);
/**
 * nostr_filter_set_search:
 * @filter: (nullable): filter (no-op if NULL)
 * @search: (nullable): duplicated internally
 */
void nostr_filter_set_search(NostrFilter *filter, const char *search);

/**
 * nostr_filter_get_limit_zero:
 * @filter: (nullable): filter
 *
 * Returns: whether non-standard limit_zero is set
 */
bool nostr_filter_get_limit_zero(const NostrFilter *filter);
/**
 * nostr_filter_set_limit_zero:
 * @filter: (nullable): filter (no-op if NULL)
 * @limit_zero: boolean
 */
void nostr_filter_set_limit_zero(NostrFilter *filter, bool limit_zero);

/* Optional legacy aliases (temporary): map old to new if enabled */
#ifdef NOSTR_ENABLE_LEGACY_ALIASES
#  define create_filter                          nostr_filter_new
#  define free_filter                            nostr_filter_free
#  define filter_matches                         nostr_filter_matches
#  define filter_match_ignoring_timestamp        nostr_filter_match_ignoring_timestamp
#  define create_filters                         nostr_filters_new
#  define filters_add                            nostr_filters_add
#  define free_filters                           nostr_filters_free
#  define filters_match                          nostr_filters_match
#  define filters_match_ignoring_timestamp       nostr_filters_match_ignoring_timestamp
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_FILTER_H__ */
