#ifndef __NOSTR_FILTER_H__
#define __NOSTR_FILTER_H__

/* Public header: standardized Nostr filter APIs. */

#include <stddef.h>
#include <stdbool.h>
#include "nostr-timestamp.h"
#include "nostr-tag.h"
#include "string_array.h"
#include "int_array.h"
/* Note: do NOT include this header recursively. */

#ifdef __cplusplus
extern "C" {
#endif

/* Provide canonical type names (forward matches nostr-event.h) */
typedef struct _NostrEvent NostrEvent; /* forward */

typedef struct NostrFilter {
    StringArray ids;
    IntArray kinds;
    StringArray authors;
    NostrTags *tags;
    NostrTimestamp since;
    NostrTimestamp until;
    int limit;
    char *search;
    bool limit_zero;
    StringArray relays;       /* nostrc-57j: relay URLs for relay-aware filtering */
} NostrFilter;

typedef struct NostrFilters {
    NostrFilter *filters;
    size_t count;
    size_t capacity;
} NostrFilters;

/* Constructors / matchers */
NostrFilter  *nostr_filter_new(void);
void          nostr_filter_free(NostrFilter *filter);
/**
 * nostr_filter_clear:
 * @filter: (nullable): filter to clear
 *
 * Frees all heap-allocated contents of @filter (ids, kinds, authors, tags, search)
 * without freeing the struct itself. Safe to use for stack-allocated filters
 * (e.g., results of `nostr_nip01_filter_build()` when the destination is a
 * stack variable). After this call, the filter is reset to an empty state.
 *
 * Notes:
 * - This function is also safe to call on a zeroed filter (e.g., after
 *   `nostr_filters_add()` which zeros the source to prevent double-free).
 */
void          nostr_filter_clear(NostrFilter *filter);
bool          nostr_filter_matches(NostrFilter *filter, NostrEvent *event);
bool          nostr_filter_match_ignoring_timestamp(NostrFilter *filter, NostrEvent *event);

NostrFilters *nostr_filters_new(void);
/**
 * nostr_filters_add:
 * @filters: (not nullable): destination vector
 * @filter: (inout) (transfer full): source filter whose contents will be MOVED
 *
 * Appends a filter to @filters by moving its contents into the internal array
 * slot using shallow copy. The source @filter is then zeroed with memset to
 * prevent accidental double-free by the caller.
 *
 * Ownership: @filters takes full ownership of the filter contents. Caller must
 * not use or free internal members of @filter after this call. It is safe to
 * call `nostr_filter_clear(filter)` afterwards; it will be a no-op.
 *
 * Returns: %true on success, %false on failure.
 */
bool          nostr_filters_add(NostrFilters *filters, NostrFilter *filter);
void          nostr_filters_free(NostrFilters *filters);
bool          nostr_filters_match(NostrFilters *filters, NostrEvent *event);
bool          nostr_filters_match_ignoring_timestamp(NostrFilters *filters, NostrEvent *event);

/* Deep-copy for boxed */
NostrFilter  *nostr_filter_copy(const NostrFilter *filter);

#ifdef __GI_SCANNER__
#include <glib-object.h>
GType nostr_filter_get_type(void);
#define NOSTR_TYPE_FILTER (nostr_filter_get_type())
#endif

/* GLib autoptr cleanup functions (available when building with GLib) */
#if defined(NOSTR_HAVE_GLIB) || defined(__GI_SCANNER__)
#include <glib-object.h>
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrFilter, nostr_filter_free)
/* NostrFilterBuilder cleanup declared after builder type definition below */
#endif

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
NostrTimestamp nostr_filter_get_since(const NostrFilter *filter);
/**
 * nostr_filter_set_since:
 * @filter: (nullable): filter (no-op if NULL)
 * @since: timestamp
 */
void nostr_filter_set_since(NostrFilter *filter, NostrTimestamp since);

/**
 * nostr_filter_get_until:
 * @filter: (nullable): filter
 *
 * Returns: until timestamp
 */
NostrTimestamp nostr_filter_get_until(const NostrFilter *filter);
/**
 * nostr_filter_set_until:
 * @filter: (nullable): filter (no-op if NULL)
 * @until: timestamp
 */
void nostr_filter_set_until(NostrFilter *filter, NostrTimestamp until);

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

/* GI-friendly helpers (migrated from nostr-filter-wrap.h) */
/* ids */
size_t        nostr_filter_ids_len(const NostrFilter *filter);
const char   *nostr_filter_ids_get(const NostrFilter *filter, size_t index);

/* kinds */
size_t        nostr_filter_kinds_len(const NostrFilter *filter);
int           nostr_filter_kinds_get(const NostrFilter *filter, size_t index);

/* authors */
size_t        nostr_filter_authors_len(const NostrFilter *filter);
const char   *nostr_filter_authors_get(const NostrFilter *filter, size_t index);

/* timestamps as int64 for GI */
int64_t       nostr_filter_get_since_i64(const NostrFilter *filter);
void          nostr_filter_set_since_i64(NostrFilter *filter, int64_t since);
int64_t       nostr_filter_get_until_i64(const NostrFilter *filter);
void          nostr_filter_set_until_i64(NostrFilter *filter, int64_t until);

/* tags 2D array-like accessors */
size_t        nostr_filter_tags_len(const NostrFilter *filter);
size_t        nostr_filter_tag_len(const NostrFilter *filter, size_t tag_index);
const char   *nostr_filter_tag_get(const NostrFilter *filter, size_t tag_index, size_t item_index);

/* relays */
const StringArray *nostr_filter_get_relays(const NostrFilter *filter);
void nostr_filter_set_relays(NostrFilter *filter, const char *const *relays, size_t count);
size_t        nostr_filter_relays_len(const NostrFilter *filter);
const char   *nostr_filter_relays_get(const NostrFilter *filter, size_t index);

/* mutating helpers */
void          nostr_filter_add_id(NostrFilter *filter, const char *id);
void          nostr_filter_add_kind(NostrFilter *filter, int kind);
void          nostr_filter_add_author(NostrFilter *filter, const char *author);
void          nostr_filter_add_relay(NostrFilter *filter, const char *relay);
void          nostr_filter_tags_append(NostrFilter *filter, const char *key, const char *value, const char *relay);

/* Compact fast-path JSON (de)serializers */
char         *nostr_filter_serialize_compact(const NostrFilter *filter);
typedef struct NostrJsonErrorInfo NostrJsonErrorInfo;
int           nostr_filter_deserialize_compact(NostrFilter *filter, const char *json,
                                                NostrJsonErrorInfo *err_out);

/* Backend-abstracted wrappers (prefer compact, fallback to backend) */
int           nostr_filter_deserialize(NostrFilter *filter, const char *json);

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

/* ============================================================================
 * NostrFilterBuilder - Fluent builder pattern for NostrFilter
 * ============================================================================
 */

/**
 * NostrFilterBuilder:
 *
 * Opaque builder type for constructing NostrFilter objects using a fluent API.
 * All builder methods return the builder pointer for chaining.
 *
 * Example usage:
 * ```c
 * NostrFilter *filter = nostr_filter_builder_build(
 *     nostr_filter_builder_kinds(
 *         nostr_filter_builder_authors(
 *             nostr_filter_builder_new(),
 *             "pubkey1", "pubkey2", NULL),
 *         1, 6, -1));
 * ```
 */
typedef struct NostrFilterBuilder NostrFilterBuilder;

/**
 * nostr_filter_builder_new:
 *
 * Creates a new filter builder with default values.
 *
 * Returns: (transfer full): A new NostrFilterBuilder, or NULL on allocation failure.
 *          Free with nostr_filter_builder_free() if not calling build().
 */
NostrFilterBuilder *nostr_filter_builder_new(void);

/**
 * nostr_filter_builder_ids:
 * @builder: The builder instance
 * @...: NULL-terminated list of event ID strings
 *
 * Sets the event IDs to filter. Pass strings followed by NULL terminator.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_ids(NostrFilterBuilder *builder, ...);

/**
 * nostr_filter_builder_authors:
 * @builder: The builder instance
 * @...: NULL-terminated list of author pubkey strings
 *
 * Sets the author pubkeys to filter. Pass strings followed by NULL terminator.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_authors(NostrFilterBuilder *builder, ...);

/**
 * nostr_filter_builder_kinds:
 * @builder: The builder instance
 * @...: -1 terminated list of event kind integers
 *
 * Sets the event kinds to filter. Pass integers followed by -1 terminator.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_kinds(NostrFilterBuilder *builder, ...);

/**
 * nostr_filter_builder_since:
 * @builder: The builder instance
 * @timestamp: Unix timestamp for "since" filter
 *
 * Sets the minimum timestamp for matching events.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_since(NostrFilterBuilder *builder, int64_t timestamp);

/**
 * nostr_filter_builder_until:
 * @builder: The builder instance
 * @timestamp: Unix timestamp for "until" filter
 *
 * Sets the maximum timestamp for matching events.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_until(NostrFilterBuilder *builder, int64_t timestamp);

/**
 * nostr_filter_builder_limit:
 * @builder: The builder instance
 * @limit: Maximum number of events to return
 *
 * Sets the limit on number of events returned.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_limit(NostrFilterBuilder *builder, unsigned int limit);

/**
 * nostr_filter_builder_tag:
 * @builder: The builder instance
 * @key: Tag key (e.g., "e", "p")
 * @value: Tag value to match
 *
 * Adds a tag filter requirement. Can be called multiple times
 * for the same key to add multiple allowed values.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_tag(NostrFilterBuilder *builder, const char *key, const char *value);

/**
 * nostr_filter_builder_relays:
 * @builder: The builder instance
 * @...: NULL-terminated list of relay URL strings
 *
 * Sets the relay URLs to filter. Pass strings followed by NULL terminator.
 *
 * Returns: (transfer none): The same builder for chaining
 */
NostrFilterBuilder *nostr_filter_builder_relays(NostrFilterBuilder *builder, ...);

/**
 * nostr_filter_builder_build:
 * @builder: (transfer full): The builder instance
 *
 * Constructs the NostrFilter from the builder and frees the builder.
 * After calling this, the builder pointer is no longer valid.
 *
 * Returns: (transfer full): A new NostrFilter, or NULL on failure.
 */
NostrFilter *nostr_filter_builder_build(NostrFilterBuilder *builder);

/**
 * nostr_filter_builder_free:
 * @builder: (nullable): The builder to free
 *
 * Frees a builder without constructing a filter.
 * Use this if you need to abort filter construction.
 */
void nostr_filter_builder_free(NostrFilterBuilder *builder);

/* GLib autoptr cleanup for NostrFilterBuilder */
#if defined(NOSTR_HAVE_GLIB) || defined(__GI_SCANNER__)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrFilterBuilder, nostr_filter_builder_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_FILTER_H__ */
