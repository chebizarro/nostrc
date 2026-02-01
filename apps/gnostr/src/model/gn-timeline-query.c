/**
 * GnTimelineQuery - Filter specification for timeline views
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#include "gn-timeline-query.h"
#include <string.h>
#include <stdio.h>

#define DEFAULT_LIMIT 50

/* Builder structure */
struct _GnTimelineQueryBuilder {
  GArray *kinds;
  GPtrArray *authors;
  gint64 since;
  gint64 until;
  guint limit;
  char *search;
  gboolean include_replies;
  char *hashtag;
};

/* ============== Internal Helpers ============== */

static GnTimelineQuery *query_alloc(void) {
  GnTimelineQuery *q = g_new0(GnTimelineQuery, 1);
  q->limit = DEFAULT_LIMIT;
  q->include_replies = TRUE;
  return q;
}

static void invalidate_cache(GnTimelineQuery *q) {
  g_clear_pointer(&q->_cached_json, g_free);
  q->_hash = 0;
}

/* ============== Constructors ============== */

GnTimelineQuery *gn_timeline_query_new_global(void) {
  GnTimelineQuery *q = query_alloc();
  
  /* Global timeline: kinds 1 (text note) and 6 (repost) */
  q->kinds = g_new(gint, 2);
  q->kinds[0] = 1;
  q->kinds[1] = 6;
  q->n_kinds = 2;
  
  return q;
}

GnTimelineQuery *gn_timeline_query_new_for_author(const char *pubkey) {
  g_return_val_if_fail(pubkey != NULL, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Kinds 1 and 6 */
  q->kinds = g_new(gint, 2);
  q->kinds[0] = 1;
  q->kinds[1] = 6;
  q->n_kinds = 2;
  
  /* Single author */
  q->authors = g_new(char *, 1);
  q->authors[0] = g_strdup(pubkey);
  q->n_authors = 1;
  
  return q;
}

GnTimelineQuery *gn_timeline_query_new_for_authors(const char **pubkeys, gsize n) {
  g_return_val_if_fail(pubkeys != NULL && n > 0, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Kinds 1 and 6 */
  q->kinds = g_new(gint, 2);
  q->kinds[0] = 1;
  q->kinds[1] = 6;
  q->n_kinds = 2;
  
  /* Multiple authors */
  q->authors = g_new(char *, n);
  for (gsize i = 0; i < n; i++) {
    q->authors[i] = g_strdup(pubkeys[i]);
  }
  q->n_authors = n;
  
  return q;
}

GnTimelineQuery *gn_timeline_query_new_for_search(const char *search_query) {
  g_return_val_if_fail(search_query != NULL, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Kinds 1 and 6 */
  q->kinds = g_new(gint, 2);
  q->kinds[0] = 1;
  q->kinds[1] = 6;
  q->n_kinds = 2;
  
  q->search = g_strdup(search_query);
  
  return q;
}

GnTimelineQuery *gn_timeline_query_new_for_hashtag(const char *hashtag) {
  g_return_val_if_fail(hashtag != NULL, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Kinds 1 and 6 */
  q->kinds = g_new(gint, 2);
  q->kinds[0] = 1;
  q->kinds[1] = 6;
  q->n_kinds = 2;
  
  q->hashtag = g_strdup(hashtag);
  
  return q;
}

GnTimelineQuery *gn_timeline_query_new_thread(const char *root_event_id) {
  g_return_val_if_fail(root_event_id != NULL, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Thread view: kind 1 only */
  q->kinds = g_new(gint, 1);
  q->kinds[0] = 1;
  q->n_kinds = 1;
  
  /* Include replies for thread view */
  q->include_replies = TRUE;
  
  /* nostrc-n63f: Thread queries use a tagged-reference filter convention.
   * The "e:" prefix in hashtag field signals to the query executor that this
   * is an event reference filter, not a hashtag. This allows reuse of existing
   * infrastructure without adding a dedicated event_ids field to the struct.
   * A future refactor could add proper #e/#p tag filter arrays. */
  q->hashtag = g_strdup_printf("e:%s", root_event_id);
  
  return q;
}

/* ============== Builder Pattern ============== */

GnTimelineQueryBuilder *gn_timeline_query_builder_new(void) {
  GnTimelineQueryBuilder *b = g_new0(GnTimelineQueryBuilder, 1);
  b->kinds = g_array_new(FALSE, FALSE, sizeof(gint));
  b->authors = g_ptr_array_new_with_free_func(g_free);
  b->limit = DEFAULT_LIMIT;
  b->include_replies = TRUE;
  return b;
}

void gn_timeline_query_builder_add_kind(GnTimelineQueryBuilder *builder, gint kind) {
  g_return_if_fail(builder != NULL);
  g_array_append_val(builder->kinds, kind);
}

void gn_timeline_query_builder_add_author(GnTimelineQueryBuilder *builder, const char *pubkey) {
  g_return_if_fail(builder != NULL && pubkey != NULL);
  g_ptr_array_add(builder->authors, g_strdup(pubkey));
}

void gn_timeline_query_builder_set_since(GnTimelineQueryBuilder *builder, gint64 since) {
  g_return_if_fail(builder != NULL);
  builder->since = since;
}

void gn_timeline_query_builder_set_until(GnTimelineQueryBuilder *builder, gint64 until) {
  g_return_if_fail(builder != NULL);
  builder->until = until;
}

void gn_timeline_query_builder_set_limit(GnTimelineQueryBuilder *builder, guint limit) {
  g_return_if_fail(builder != NULL);
  builder->limit = limit > 0 ? limit : DEFAULT_LIMIT;
}

void gn_timeline_query_builder_set_search(GnTimelineQueryBuilder *builder, const char *search) {
  g_return_if_fail(builder != NULL);
  g_free(builder->search);
  builder->search = g_strdup(search);
}

void gn_timeline_query_builder_set_include_replies(GnTimelineQueryBuilder *builder, gboolean include) {
  g_return_if_fail(builder != NULL);
  builder->include_replies = include;
}

void gn_timeline_query_builder_set_hashtag(GnTimelineQueryBuilder *builder, const char *hashtag) {
  g_return_if_fail(builder != NULL);
  g_free(builder->hashtag);
  builder->hashtag = g_strdup(hashtag);
}

GnTimelineQuery *gn_timeline_query_builder_build(GnTimelineQueryBuilder *builder) {
  g_return_val_if_fail(builder != NULL, NULL);
  
  GnTimelineQuery *q = query_alloc();
  
  /* Copy kinds */
  if (builder->kinds->len > 0) {
    q->n_kinds = builder->kinds->len;
    q->kinds = g_new(gint, q->n_kinds);
    memcpy(q->kinds, builder->kinds->data, q->n_kinds * sizeof(gint));
  }
  
  /* Copy authors */
  if (builder->authors->len > 0) {
    q->n_authors = builder->authors->len;
    q->authors = g_new(char *, q->n_authors);
    for (gsize i = 0; i < q->n_authors; i++) {
      q->authors[i] = g_strdup(g_ptr_array_index(builder->authors, i));
    }
  }
  
  q->since = builder->since;
  q->until = builder->until;
  q->limit = builder->limit;
  q->search = g_strdup(builder->search);
  q->include_replies = builder->include_replies;
  q->hashtag = g_strdup(builder->hashtag);
  
  gn_timeline_query_builder_free(builder);
  
  return q;
}

void gn_timeline_query_builder_free(GnTimelineQueryBuilder *builder) {
  if (!builder) return;
  
  g_array_free(builder->kinds, TRUE);
  g_ptr_array_free(builder->authors, TRUE);
  g_free(builder->search);
  g_free(builder->hashtag);
  g_free(builder);
}

/* ============== Query Operations ============== */

const char *gn_timeline_query_to_json(GnTimelineQuery *query) {
  g_return_val_if_fail(query != NULL, NULL);
  
  if (query->_cached_json) {
    return query->_cached_json;
  }
  
  GString *json = g_string_new("{");
  gboolean first = TRUE;
  
  /* Kinds */
  if (query->n_kinds > 0) {
    g_string_append(json, "\"kinds\":[");
    for (gsize i = 0; i < query->n_kinds; i++) {
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "%d", query->kinds[i]);
    }
    g_string_append_c(json, ']');
    first = FALSE;
  }
  
  /* Authors */
  if (query->n_authors > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append(json, "\"authors\":[");
    for (gsize i = 0; i < query->n_authors; i++) {
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "\"%s\"", query->authors[i]);
    }
    g_string_append_c(json, ']');
    first = FALSE;
  }
  
  /* Since */
  if (query->since > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append_printf(json, "\"since\":%lld", (long long)query->since);
    first = FALSE;
  }
  
  /* Until */
  if (query->until > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append_printf(json, "\"until\":%lld", (long long)query->until);
    first = FALSE;
  }
  
  /* Limit */
  if (!first) g_string_append_c(json, ',');
  g_string_append_printf(json, "\"limit\":%u", query->limit);
  
  /* Hashtag (as #t tag) */
  if (query->hashtag && !g_str_has_prefix(query->hashtag, "e:")) {
    g_string_append_printf(json, ",\"#t\":[\"%s\"]", query->hashtag);
  }
  
  g_string_append_c(json, '}');
  
  query->_cached_json = g_string_free(json, FALSE);
  return query->_cached_json;
}

char *gn_timeline_query_to_json_with_until(GnTimelineQuery *query, gint64 until) {
  g_return_val_if_fail(query != NULL, NULL);
  
  GString *json = g_string_new("{");
  gboolean first = TRUE;
  
  /* Kinds */
  if (query->n_kinds > 0) {
    g_string_append(json, "\"kinds\":[");
    for (gsize i = 0; i < query->n_kinds; i++) {
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "%d", query->kinds[i]);
    }
    g_string_append_c(json, ']');
    first = FALSE;
  }
  
  /* Authors */
  if (query->n_authors > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append(json, "\"authors\":[");
    for (gsize i = 0; i < query->n_authors; i++) {
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "\"%s\"", query->authors[i]);
    }
    g_string_append_c(json, ']');
    first = FALSE;
  }
  
  /* Since */
  if (query->since > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append_printf(json, "\"since\":%lld", (long long)query->since);
    first = FALSE;
  }
  
  /* Until - use provided value */
  if (until > 0) {
    if (!first) g_string_append_c(json, ',');
    g_string_append_printf(json, "\"until\":%lld", (long long)until);
    first = FALSE;
  }
  
  /* Limit */
  if (!first) g_string_append_c(json, ',');
  g_string_append_printf(json, "\"limit\":%u", query->limit);
  
  /* Hashtag */
  if (query->hashtag && !g_str_has_prefix(query->hashtag, "e:")) {
    g_string_append_printf(json, ",\"#t\":[\"%s\"]", query->hashtag);
  }
  
  g_string_append_c(json, '}');
  
  return g_string_free(json, FALSE);
}

guint gn_timeline_query_hash(GnTimelineQuery *query) {
  g_return_val_if_fail(query != NULL, 0);
  
  if (query->_hash != 0) {
    return query->_hash;
  }
  
  guint hash = 0;
  
  /* Hash kinds */
  for (gsize i = 0; i < query->n_kinds; i++) {
    hash = hash * 31 + (guint)query->kinds[i];
  }
  
  /* Hash authors */
  for (gsize i = 0; i < query->n_authors; i++) {
    hash = hash * 31 + g_str_hash(query->authors[i]);
  }
  
  /* Hash other fields */
  hash = hash * 31 + (guint)(query->since & 0xFFFFFFFF);
  hash = hash * 31 + (guint)(query->until & 0xFFFFFFFF);
  hash = hash * 31 + query->limit;
  hash = hash * 31 + (query->include_replies ? 1 : 0);
  
  if (query->search) {
    hash = hash * 31 + g_str_hash(query->search);
  }
  if (query->hashtag) {
    hash = hash * 31 + g_str_hash(query->hashtag);
  }
  
  query->_hash = hash;
  return hash;
}

gboolean gn_timeline_query_equal(GnTimelineQuery *a, GnTimelineQuery *b) {
  if (a == b) return TRUE;
  if (!a || !b) return FALSE;
  
  /* Quick hash check */
  if (gn_timeline_query_hash(a) != gn_timeline_query_hash(b)) {
    return FALSE;
  }
  
  /* Detailed comparison */
  if (a->n_kinds != b->n_kinds) return FALSE;
  for (gsize i = 0; i < a->n_kinds; i++) {
    if (a->kinds[i] != b->kinds[i]) return FALSE;
  }
  
  if (a->n_authors != b->n_authors) return FALSE;
  for (gsize i = 0; i < a->n_authors; i++) {
    if (g_strcmp0(a->authors[i], b->authors[i]) != 0) return FALSE;
  }
  
  if (a->since != b->since) return FALSE;
  if (a->until != b->until) return FALSE;
  if (a->limit != b->limit) return FALSE;
  if (a->include_replies != b->include_replies) return FALSE;
  if (g_strcmp0(a->search, b->search) != 0) return FALSE;
  if (g_strcmp0(a->hashtag, b->hashtag) != 0) return FALSE;
  
  return TRUE;
}

GnTimelineQuery *gn_timeline_query_copy(GnTimelineQuery *query) {
  g_return_val_if_fail(query != NULL, NULL);
  
  GnTimelineQuery *copy = query_alloc();
  
  if (query->n_kinds > 0) {
    copy->n_kinds = query->n_kinds;
    copy->kinds = g_new(gint, copy->n_kinds);
    memcpy(copy->kinds, query->kinds, copy->n_kinds * sizeof(gint));
  }
  
  if (query->n_authors > 0) {
    copy->n_authors = query->n_authors;
    copy->authors = g_new(char *, copy->n_authors);
    for (gsize i = 0; i < copy->n_authors; i++) {
      copy->authors[i] = g_strdup(query->authors[i]);
    }
  }
  
  copy->since = query->since;
  copy->until = query->until;
  copy->limit = query->limit;
  copy->search = g_strdup(query->search);
  copy->include_replies = query->include_replies;
  copy->hashtag = g_strdup(query->hashtag);
  
  return copy;
}

void gn_timeline_query_free(GnTimelineQuery *query) {
  if (!query) return;
  
  g_free(query->kinds);
  
  if (query->authors) {
    for (gsize i = 0; i < query->n_authors; i++) {
      g_free(query->authors[i]);
    }
    g_free(query->authors);
  }
  
  g_free(query->search);
  g_free(query->hashtag);
  g_free(query->_cached_json);
  g_free(query);
}
