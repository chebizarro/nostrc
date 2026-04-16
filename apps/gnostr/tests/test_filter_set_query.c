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
 *   - Multi-hashtag → first wins + g_warning()
 *   - since / until / limit pass-through
 *   - ids dropped (semantic mismatch with #e tag filter)
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
  g_assert_cmpstr(q->hashtag, ==, "bitcoin");
  g_assert_true(has_default_kinds(q));

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_multi_hashtag_first_wins_with_warning(void)
{
  /* We assert on the exact behaviour — first hashtag wins + a single
   * g_warning() is emitted — using g_test_expect_message so the test
   * runs in-process and doesn't hit the subprocess fatal-warnings
   * default. */
  g_test_expect_message("gnostr-filter-set-query",
                        G_LOG_LEVEL_WARNING,
                        "*timeline query only supports a single hashtag*");

  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Multi");
  static const gchar *tags[] = { "bitcoin", "nostr", "zaps", NULL };
  gnostr_filter_set_set_hashtags(fs, tags);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_cmpstr(q->hashtag, ==, "bitcoin");

  gnostr_timeline_query_free(q);
  g_object_unref(fs);

  g_test_assert_expected_messages();
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
test_ids_only_still_global(void)
{
  /* A filter set whose *only* populated criterion is `ids` still goes
   * through the builder path (is_empty() returns FALSE because `ids` is
   * populated), but the converter drops the ids field and no other
   * criteria are present, so the resulting query is effectively the
   * global feed: default kinds [1, 6], no authors, no #e tag filter.
   *
   * This pins the intended behaviour for bookmark-style filter sets:
   * they cannot be served by #GNostrTimelineQuery until a dedicated
   * dispatch path exists. If that changes, this test needs to be
   * revisited in lockstep. */
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Bookmarks-only");
  gnostr_filter_set_set_ids(fs, kIds);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  g_assert_true(has_default_kinds(q));
  g_assert_cmpuint(q->n_event_ids, ==, 0);
  g_assert_cmpuint(q->n_authors, ==, 0);

  gnostr_timeline_query_free(q);
  g_object_unref(fs);
}

static void
test_ids_dropped(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Bookmarks");
  gnostr_filter_set_set_ids(fs, kIds);
  /* Add authors too so is_empty() is FALSE. */
  gnostr_filter_set_set_authors(fs, kAuthors);

  GNostrTimelineQuery *q = gnostr_filter_set_to_timeline_query(fs);
  g_assert_nonnull(q);
  /* event_ids maps to #e tag filter — must stay empty when only `ids`
   * is populated, otherwise we'd silently change the query semantics. */
  g_assert_cmpuint(q->n_event_ids, ==, 0);

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
  g_test_add_func("/filter-set-query/hashtag/single",      test_single_hashtag);
  g_test_add_func("/filter-set-query/hashtag/multi-warns", test_multi_hashtag_first_wins_with_warning);
  g_test_add_func("/filter-set-query/time-and-limit",      test_time_and_limit_passthrough);
  g_test_add_func("/filter-set-query/ids-only-fallback",   test_ids_only_still_global);
  g_test_add_func("/filter-set-query/ids-dropped",         test_ids_dropped);
  g_test_add_func("/filter-set-query/excluded-dropped",    test_excluded_authors_dropped);
  g_test_add_func("/filter-set-query/full-spec",           test_full_spec_combined);
  g_test_add_func("/filter-set-query/skip-empty-author",   test_empty_author_string_skipped);

  return g_test_run();
}
