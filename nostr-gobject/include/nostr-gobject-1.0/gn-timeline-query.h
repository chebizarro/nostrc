/**
 * GNostrTimelineQuery - Filter specification for timeline views
 *
 * An immutable filter specification that defines what notes to display
 * in a timeline. Supports kinds, authors, time ranges, and search.
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#ifndef GNOSTR_TIMELINE_QUERY_H
#define GNOSTR_TIMELINE_QUERY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct _GNostrTimelineQuery GNostrTimelineQuery;
typedef struct _GNostrTimelineQueryBuilder GNostrTimelineQueryBuilder;

/**
 * GNostrTimelineQuery:
 *
 * Immutable filter specification for timeline queries.
 * Create using constructors or builder pattern.
 */
struct _GNostrTimelineQuery {
  gint *kinds;              /* Array of event kinds (1=note, 6=repost, etc.) */
  gsize n_kinds;
  char **authors;           /* Array of pubkey hex strings (NULL = all authors) */
  gsize n_authors;
  char **event_ids;         /* Array of event ID hex strings for #e tag filter */
  gsize n_event_ids;
  gint64 since;             /* Unix timestamp lower bound (0 = no limit) */
  gint64 until;             /* Unix timestamp upper bound (0 = no limit) */
  guint limit;              /* Max items per query page (default: 50) */
  char *search;             /* Full-text search query (NULL = none) */
  gboolean include_replies; /* Whether to include reply notes */
  char *hashtag;            /* Filter by hashtag (NULL = none) */

  /* Internal: cached JSON representation */
  char *_cached_json;
  guint _hash;
};

/* ============== Constructors ============== */

/**
 * gnostr_timeline_query_new_global:
 *
 * Create a query for the global timeline (kinds 1 and 6, all authors).
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_global(void);

/**
 * gnostr_timeline_query_new_for_author:
 * @pubkey: Hex-encoded public key
 *
 * Create a query for a single author's timeline.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_for_author(const char *pubkey);

/**
 * gnostr_timeline_query_new_for_authors:
 * @pubkeys: Array of hex-encoded public keys
 * @n: Number of pubkeys
 *
 * Create a query for multiple authors (e.g., a user list).
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_for_authors(const char **pubkeys, gsize n);

/**
 * gnostr_timeline_query_new_for_search:
 * @search_query: Full-text search query
 *
 * Create a query for full-text search results.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_for_search(const char *search_query);

/**
 * gnostr_timeline_query_new_for_hashtag:
 * @hashtag: Hashtag without the # prefix
 *
 * Create a query for notes with a specific hashtag.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_for_hashtag(const char *hashtag);

/**
 * gnostr_timeline_query_new_thread:
 * @root_event_id: Hex-encoded event ID of the thread root
 *
 * Create a query for a thread view.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_new_thread(const char *root_event_id);

/* ============== Builder Pattern ============== */

/**
 * gnostr_timeline_query_builder_new:
 *
 * Create a new query builder for complex queries.
 *
 * Returns: (transfer full): A new builder, free with gnostr_timeline_query_builder_free()
 */
GNostrTimelineQueryBuilder *gnostr_timeline_query_builder_new(void);

void gnostr_timeline_query_builder_add_kind(GNostrTimelineQueryBuilder *builder, gint kind);
void gnostr_timeline_query_builder_add_author(GNostrTimelineQueryBuilder *builder, const char *pubkey);
void gnostr_timeline_query_builder_add_event_id(GNostrTimelineQueryBuilder *builder, const char *event_id);
void gnostr_timeline_query_builder_set_since(GNostrTimelineQueryBuilder *builder, gint64 since);
void gnostr_timeline_query_builder_set_until(GNostrTimelineQueryBuilder *builder, gint64 until);
void gnostr_timeline_query_builder_set_limit(GNostrTimelineQueryBuilder *builder, guint limit);
void gnostr_timeline_query_builder_set_search(GNostrTimelineQueryBuilder *builder, const char *search);
void gnostr_timeline_query_builder_set_include_replies(GNostrTimelineQueryBuilder *builder, gboolean include);
void gnostr_timeline_query_builder_set_hashtag(GNostrTimelineQueryBuilder *builder, const char *hashtag);

/**
 * gnostr_timeline_query_builder_build:
 * @builder: The builder
 *
 * Build the query and free the builder.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_builder_build(GNostrTimelineQueryBuilder *builder);

void gnostr_timeline_query_builder_free(GNostrTimelineQueryBuilder *builder);

/* ============== Query Operations ============== */

/**
 * gnostr_timeline_query_to_json:
 * @query: The query
 *
 * Convert query to NostrDB filter JSON.
 *
 * Returns: (transfer none): JSON string (owned by query, do not free)
 */
const char *gnostr_timeline_query_to_json(GNostrTimelineQuery *query);

/**
 * gnostr_timeline_query_to_json_with_until:
 * @query: The query
 * @until: Override until timestamp for pagination
 *
 * Convert query to NostrDB filter JSON with custom until.
 *
 * Returns: (transfer full): JSON string, caller must g_free()
 */
char *gnostr_timeline_query_to_json_with_until(GNostrTimelineQuery *query, gint64 until);

/**
 * gnostr_timeline_query_hash:
 * @query: The query
 *
 * Get hash value for caching.
 *
 * Returns: Hash value
 */
guint gnostr_timeline_query_hash(GNostrTimelineQuery *query);

/**
 * gnostr_timeline_query_equal:
 * @a: First query
 * @b: Second query
 *
 * Check if two queries are equal.
 *
 * Returns: TRUE if equal
 */
gboolean gnostr_timeline_query_equal(GNostrTimelineQuery *a, GNostrTimelineQuery *b);

/**
 * gnostr_timeline_query_copy:
 * @query: The query to copy
 *
 * Create a deep copy of a query.
 *
 * Returns: (transfer full): A new query, free with gnostr_timeline_query_free()
 */
GNostrTimelineQuery *gnostr_timeline_query_copy(GNostrTimelineQuery *query);

/**
 * gnostr_timeline_query_free:
 * @query: The query to free
 *
 * Free a query and all its resources.
 */
void gnostr_timeline_query_free(GNostrTimelineQuery *query);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_QUERY_H */
