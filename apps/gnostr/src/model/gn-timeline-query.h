/**
 * GnTimelineQuery - Filter specification for timeline views
 *
 * An immutable filter specification that defines what notes to display
 * in a timeline. Supports kinds, authors, time ranges, and search.
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#ifndef GN_TIMELINE_QUERY_H
#define GN_TIMELINE_QUERY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct _GnTimelineQuery GnTimelineQuery;
typedef struct _GnTimelineQueryBuilder GnTimelineQueryBuilder;

/**
 * GnTimelineQuery:
 *
 * Immutable filter specification for timeline queries.
 * Create using constructors or builder pattern.
 */
struct _GnTimelineQuery {
  gint *kinds;              /* Array of event kinds (1=note, 6=repost, etc.) */
  gsize n_kinds;
  char **authors;           /* Array of pubkey hex strings (NULL = all authors) */
  gsize n_authors;
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
 * gn_timeline_query_new_global:
 *
 * Create a query for the global timeline (kinds 1 and 6, all authors).
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_global(void);

/**
 * gn_timeline_query_new_for_author:
 * @pubkey: Hex-encoded public key
 *
 * Create a query for a single author's timeline.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_for_author(const char *pubkey);

/**
 * gn_timeline_query_new_for_authors:
 * @pubkeys: Array of hex-encoded public keys
 * @n: Number of pubkeys
 *
 * Create a query for multiple authors (e.g., a user list).
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_for_authors(const char **pubkeys, gsize n);

/**
 * gn_timeline_query_new_for_search:
 * @search_query: Full-text search query
 *
 * Create a query for full-text search results.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_for_search(const char *search_query);

/**
 * gn_timeline_query_new_for_hashtag:
 * @hashtag: Hashtag without the # prefix
 *
 * Create a query for notes with a specific hashtag.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_for_hashtag(const char *hashtag);

/**
 * gn_timeline_query_new_thread:
 * @root_event_id: Hex-encoded event ID of the thread root
 *
 * Create a query for a thread view.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_new_thread(const char *root_event_id);

/* ============== Builder Pattern ============== */

/**
 * gn_timeline_query_builder_new:
 *
 * Create a new query builder for complex queries.
 *
 * Returns: (transfer full): A new builder, free with gn_timeline_query_builder_free()
 */
GnTimelineQueryBuilder *gn_timeline_query_builder_new(void);

void gn_timeline_query_builder_add_kind(GnTimelineQueryBuilder *builder, gint kind);
void gn_timeline_query_builder_add_author(GnTimelineQueryBuilder *builder, const char *pubkey);
void gn_timeline_query_builder_set_since(GnTimelineQueryBuilder *builder, gint64 since);
void gn_timeline_query_builder_set_until(GnTimelineQueryBuilder *builder, gint64 until);
void gn_timeline_query_builder_set_limit(GnTimelineQueryBuilder *builder, guint limit);
void gn_timeline_query_builder_set_search(GnTimelineQueryBuilder *builder, const char *search);
void gn_timeline_query_builder_set_include_replies(GnTimelineQueryBuilder *builder, gboolean include);
void gn_timeline_query_builder_set_hashtag(GnTimelineQueryBuilder *builder, const char *hashtag);

/**
 * gn_timeline_query_builder_build:
 * @builder: The builder
 *
 * Build the query and free the builder.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_builder_build(GnTimelineQueryBuilder *builder);

void gn_timeline_query_builder_free(GnTimelineQueryBuilder *builder);

/* ============== Query Operations ============== */

/**
 * gn_timeline_query_to_json:
 * @query: The query
 *
 * Convert query to NostrDB filter JSON.
 *
 * Returns: (transfer none): JSON string (owned by query, do not free)
 */
const char *gn_timeline_query_to_json(GnTimelineQuery *query);

/**
 * gn_timeline_query_to_json_with_until:
 * @query: The query
 * @until: Override until timestamp for pagination
 *
 * Convert query to NostrDB filter JSON with custom until.
 *
 * Returns: (transfer full): JSON string, caller must g_free()
 */
char *gn_timeline_query_to_json_with_until(GnTimelineQuery *query, gint64 until);

/**
 * gn_timeline_query_hash:
 * @query: The query
 *
 * Get hash value for caching.
 *
 * Returns: Hash value
 */
guint gn_timeline_query_hash(GnTimelineQuery *query);

/**
 * gn_timeline_query_equal:
 * @a: First query
 * @b: Second query
 *
 * Check if two queries are equal.
 *
 * Returns: TRUE if equal
 */
gboolean gn_timeline_query_equal(GnTimelineQuery *a, GnTimelineQuery *b);

/**
 * gn_timeline_query_copy:
 * @query: The query to copy
 *
 * Create a deep copy of a query.
 *
 * Returns: (transfer full): A new query, free with gn_timeline_query_free()
 */
GnTimelineQuery *gn_timeline_query_copy(GnTimelineQuery *query);

/**
 * gn_timeline_query_free:
 * @query: The query to free
 *
 * Free a query and all its resources.
 */
void gn_timeline_query_free(GnTimelineQuery *query);

G_END_DECLS

#endif /* GN_TIMELINE_QUERY_H */
