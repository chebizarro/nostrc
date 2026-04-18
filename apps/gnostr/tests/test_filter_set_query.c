/* test_filter_set_query.c — Unit tests for
 * gnostr_filter_set_to_timeline_query().
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers:
 *   - NULL / empty filter set → global query fallback
 *   - Authors + kinds passthrough
 *   - Default kinds when unspecified ([1, 6])
 *   - Hashtag single pass-through
 *   - Multi-hashtag → all tags forwarded as a single `#t` array
 *     (NIP-01 OR semantics; nostrc-yg8j.7)
 *   - since / until / limit pass-through
 *   - ids mapped to top-level NIP-01 "ids" (nostrc-ch2v)
 *   - excluded_authors dropped
 *   - Combined full spec
 *
 * nostrc-yg8j.4
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/model/gnostr-filter-set.h"
#include "../src/model/gnostr-filter-set-query.h"

/* ----- helpers ----- */

static const gchar *kAuthors[] = {
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
  NULL
};

static const gchar *kIds[] = {
  "aaaabbbbccccddddeeeeffff00001111222233334444555566667777aaaabbbb",
  NULL
};

static const gchar *kExcluded[] = {
  "deadbeef00000000000000000000000000000000000000000000000000000000",
  NULL
};

/* Returns TRUE if @q has exactly the expected default kinds [1,6]. */
static gboolean
has_default_kinds(GNostrTimelineQuery *q)
{
  return q && q->n_kinds == 2 && q->kinds[0] == 1 && q->kinds[1] == 6;
}

/* ----- tests ----- */

static void
test_null_input_returns_global(void)
{
  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(NULL);
  g_assert_nonnull(q);
  g_assert_true(has_default_kinds(q));
  g_assert_cmpuint(q->n_authors, ==, 0);
  g_assert_null(q->search);
  g_assert_null(q->hashtag);
  gnostr_timeline_query_free(q);
}

static void
test_empty_filter_set_returns_global(void)
{
  /* Metadata-only set is still "empty" per gnostr_filter_set_is_empty(). */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Blank");
  g_assert_true(gnostr_filter_set_is_empty(fs));

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_true(has_default_kinds(q));
  g_assert_cmpuint(q->n_authors, ==, 0);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_authors_passthrough(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Follows");
  gnostr_filter_set_set_authors(fs, kAuthors);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_authors, ==, 2);
  g_assert_cmpstr(q->authors[0], ==, kAuthors[0]);
  g_assert_cmpstr(q->authors[1], ==, kAuthors[1]);
  /* Kinds default to [1, 6] when none specified. */
  g_assert_true(has_default_kinds(q));

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_kinds_passthrough(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Articles + notes");
  static const gint kinds[] = { 1, 30023 };
  gnostr_filter_set_set_kinds(fs, kinds, G_N_ELEMENTS(kinds));

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_kinds, ==, 2);
  g_assert_cmpint(q->kinds[0], ==, 1);
  g_assert_cmpint(q->kinds[1], ==, 30023);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_single_hashtag(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("#bitcoin");
  static const gchar *tags[] = { "bitcoin", NULL };
  gnostr_filter_set_set_hashtags(fs, tags);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  /* Single-tag path: still populates q->hashtag for legacy readers,
   * and the multi-tag list carries the same lone entry. */
  g_assert_cmpstr(q->hashtag, ==, "bitcoin");
  g_assert_cmpuint(q->n_hashtags, ==, 1);
  g_assert_cmpstr(q->hashtags[0], ==, "bitcoin");
  g_assert_true(has_default_kinds(q));

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_multi_hashtag_forwards_all(void)
{
  /* nostrc-yg8j.7 — Every non-empty tag from the filter set now flows
   * into q->hashtags; to_json() combines them into a single `#t` array
   * which NIP-01 treats as OR across values. No warning is emitted. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Multi");
  static const gchar *tags[] = { "bitcoin", "nostr", "zaps", NULL };
  gnostr_filter_set_set_hashtags(fs, tags);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_hashtags, ==, 3);
  g_assert_cmpstr(q->hashtags[0], ==, "bitcoin");
  g_assert_cmpstr(q->hashtags[1], ==, "nostr");
  g_assert_cmpstr(q->hashtags[2], ==, "zaps");
  /* Legacy single-field mirror points at the first entry so older
   * callers reading q->hashtag directly still see a sensible value. */
  g_assert_cmpstr(q->hashtag, ==, "bitcoin");

  /* JSON emission: a single `#t` array carrying all three tags. */
  const char *json = gnostr_timeline_query_to_json(q);
  g_assert_nonnull(json);
  g_assert_nonnull(strstr(json, "\"#t\":[\"bitcoin\",\"nostr\",\"zaps\"]"));

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_multi_hashtag_skips_empty(void)
{
  /* Defensive: empty-string entries inside hashtags[] must never end
   * up in the emitted filter — they would produce "\"\"" inside the
   * `#t` array and confuse relays. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Multi-with-holes");
  static const gchar *tags[] = { "bitcoin", "", "zaps", NULL };
  gnostr_filter_set_set_hashtags(fs, tags);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_hashtags, ==, 2);
  g_assert_cmpstr(q->hashtags[0], ==, "bitcoin");
  g_assert_cmpstr(q->hashtags[1], ==, "zaps");

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_time_and_limit_passthrough(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Window");
  gnostr_filter_set_set_since(fs, 1700000000);
  gnostr_filter_set_set_until(fs, 1800000000);
  gnostr_filter_set_set_limit(fs, 123);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpint((gint64)q->since, ==, 1700000000);
  g_assert_cmpint((gint64)q->until, ==, 1800000000);
  g_assert_cmpuint(q->limit, ==, 123);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_ids_only_maps_to_query_ids(void)
{
  /* nostrc-ch2v: A filter set whose only criterion is `ids` now maps
   * to the query's top-level `ids` field which emits NIP-01 "ids":[].
   * This enables bookmark-style feeds. The `event_ids` (#e) field
   * must remain empty — it has different semantics. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Bookmarks-only");
  gnostr_filter_set_set_ids(fs, kIds);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_true(has_default_kinds(q));
  g_assert_cmpuint(q->n_event_ids, ==, 0);
  g_assert_cmpuint(q->n_authors, ==, 0);
  /* The top-level ids field must be populated. */
  g_assert_cmpuint(q->n_ids, ==, 1);
  g_assert_cmpstr(q->ids[0], ==, kIds[0]);

  /* Verify JSON emission includes "ids":[...] */
  const char *json = gnostr_timeline_query_to_json(q);
  g_assert_nonnull(json);
  g_assert_nonnull(strstr(json, "\"ids\":"));
  g_assert_null(strstr(json, "\"#e\":"));

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_ids_with_authors(void)
{
  /* nostrc-ch2v: Verify ids + authors can coexist — ids goes to the
   * top-level "ids" field, authors goes to "authors", and the #e tag
   * filter remains unused. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Bookmarks");
  gnostr_filter_set_set_ids(fs, kIds);
  gnostr_filter_set_set_authors(fs, kAuthors);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_event_ids, ==, 0);
  g_assert_cmpuint(q->n_ids, ==, 1);
  g_assert_cmpstr(q->ids[0], ==, kIds[0]);
  g_assert_cmpuint(q->n_authors, ==, 2);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_excluded_authors_dropped(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Muted inverse");
  gnostr_filter_set_set_excluded_authors(fs, kExcluded);
  /* Add authors too so is_empty() is FALSE. */
  gnostr_filter_set_set_authors(fs, kAuthors);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  /* excluded_authors is not a Nostr filter field. The query has the two
   * included authors and nothing else — we just verify no crash and no
   * bogus author entry slipped in. */
  g_assert_cmpuint(q->n_authors, ==, 2);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_full_spec_combined(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Combo");
  static const gint kinds[] = { 1 };
  static const gchar *tags[] = { "gnostr", NULL };
  gnostr_filter_set_set_authors(fs, kAuthors);
  gnostr_filter_set_set_kinds(fs, kinds, G_N_ELEMENTS(kinds));
  gnostr_filter_set_set_hashtags(fs, tags);
  gnostr_filter_set_set_since(fs, 1700000000);
  gnostr_filter_set_set_until(fs, 1800000000);
  gnostr_filter_set_set_limit(fs, 50);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_kinds, ==, 1);
  g_assert_cmpint(q->kinds[0], ==, 1);
  g_assert_cmpuint(q->n_authors, ==, 2);
  g_assert_cmpstr(q->hashtag, ==, "gnostr");
  g_assert_cmpint((gint64)q->since, ==, 1700000000);
  g_assert_cmpint((gint64)q->until, ==, 1800000000);
  g_assert_cmpuint(q->limit, ==, 50);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_empty_author_string_skipped(void)
{
  /* Defensive: make sure empty strings in the authors array don't slip
   * into the query. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Holes");
  static const gchar *authors[] = {
    "",
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
    NULL
  };
  gnostr_filter_set_set_authors(fs, authors);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpuint(q->n_authors, ==, 1);
  g_assert_cmpstr(q->authors[0], ==, authors[1]);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

/* ----- main ----- */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/filter-set-query/null-input",          test_null_input_returns_global);
  g_test_add_func("/filter-set-query/empty",               test_empty_filter_set_returns_global);
  g_test_add_func("/filter-set-query/authors",             test_authors_passthrough);
  g_test_add_func("/filter-set-query/kinds",               test_kinds_passthrough);
  g_test_add_func("/filter-set-query/hashtag/single",        test_single_hashtag);
  g_test_add_func("/filter-set-query/hashtag/multi-forwards", test_multi_hashtag_forwards_all);
  g_test_add_func("/filter-set-query/hashtag/multi-skip-empty",
                                                              test_multi_hashtag_skips_empty);
  g_test_add_func("/filter-set-query/time-and-limit",      test_time_and_limit_passthrough);
  g_test_add_func("/filter-set-query/ids-maps-to-query",   test_ids_only_maps_to_query_ids);
  g_test_add_func("/filter-set-query/ids-with-authors",     test_ids_with_authors);
  g_test_add_func("/filter-set-query/excluded-dropped",    test_excluded_authors_dropped);
  g_test_add_func("/filter-set-query/full-spec",           test_full_spec_combined);
  g_test_add_func("/filter-set-query/skip-empty-author",   test_empty_author_string_skipped);

  return g_test_run();
}
