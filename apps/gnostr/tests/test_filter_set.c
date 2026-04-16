/* test_filter_set.c — Unit tests for GnostrFilterSet.
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers:
 *   - Construction defaults + set/get round-trips
 *   - Deep clone independence
 *   - Structural equality
 *   - JSON serialization round-trip (empty + full)
 *   - GVariant serialization round-trip (empty + full)
 *   - Invalid input handling
 *
 * nostrc-yg8j.1
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/model/gnostr-filter-set.h"

/* ----- helpers ----- */

static const gchar *test_authors[] = {
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
  NULL
};

static const gchar *test_hashtags[] = { "bitcoin", "nostr", "zaps", NULL };

static const gint test_kinds[] = { 1, 6, 30023 };

static const gchar *test_ids[] = {
  "aaaabbbbccccddddeeeeffff00001111222233334444555566667777aaaabbbb",
  "bbbbccccddddeeeeffff00001111222233334444555566667777888899990000",
  NULL
};

static const gchar *test_excluded[] = {
  "deadbeef00000000000000000000000000000000000000000000000000000000",
  NULL
};

static GnostrFilterSet *
make_populated_filter_set(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Following");
  gnostr_filter_set_set_id         (fs, "fs-test-001");
  gnostr_filter_set_set_description(fs, "My followed authors");
  gnostr_filter_set_set_icon       (fs, "user-home-symbolic");
  gnostr_filter_set_set_color      (fs, "#3584e4");
  gnostr_filter_set_set_source     (fs, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);

  gnostr_filter_set_set_authors         (fs, test_authors);
  gnostr_filter_set_set_hashtags        (fs, test_hashtags);
  gnostr_filter_set_set_kinds           (fs, test_kinds, G_N_ELEMENTS(test_kinds));
  gnostr_filter_set_set_ids             (fs, test_ids);
  gnostr_filter_set_set_excluded_authors(fs, test_excluded);
  gnostr_filter_set_set_since           (fs, 1700000000);
  gnostr_filter_set_set_until           (fs, 1800000000);
  gnostr_filter_set_set_limit           (fs, 200);
  return fs;
}

/* ----- tests ----- */

static void
test_construction_defaults(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  g_assert_nonnull(fs);

  /* Auto-generated id is non-empty */
  const gchar *id = gnostr_filter_set_get_id(fs);
  g_assert_nonnull(id);
  g_assert_cmpuint(strlen(id), >, 0);

  /* Other fields NULL / 0 by default */
  g_assert_null(gnostr_filter_set_get_name(fs));
  g_assert_null(gnostr_filter_set_get_description(fs));
  g_assert_null(gnostr_filter_set_get_icon(fs));
  g_assert_null(gnostr_filter_set_get_color(fs));
  g_assert_cmpint(gnostr_filter_set_get_source(fs), ==, GNOSTR_FILTER_SET_SOURCE_CUSTOM);
  g_assert_null(gnostr_filter_set_get_authors(fs));
  g_assert_null(gnostr_filter_set_get_hashtags(fs));
  g_assert_null(gnostr_filter_set_get_ids(fs));
  g_assert_null(gnostr_filter_set_get_excluded_authors(fs));
  gsize n = 42;
  g_assert_null(gnostr_filter_set_get_kinds(fs, &n));
  g_assert_cmpuint(n, ==, 0);
  g_assert_cmpint(gnostr_filter_set_get_since(fs), ==, 0);
  g_assert_cmpint(gnostr_filter_set_get_until(fs), ==, 0);
  g_assert_cmpint(gnostr_filter_set_get_limit(fs), ==, 0);

  g_assert_true(gnostr_filter_set_is_empty(fs));

  g_object_unref(fs);
}

static void
test_new_with_name(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name("Bookmarks");
  g_assert_cmpstr(gnostr_filter_set_get_name(fs), ==, "Bookmarks");
  g_object_unref(fs);
}

static void
test_id_setter_regenerates_on_null(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  const gchar *first = g_strdup(gnostr_filter_set_get_id(fs));

  gnostr_filter_set_set_id(fs, NULL);
  const gchar *second = gnostr_filter_set_get_id(fs);
  g_assert_nonnull(second);
  g_assert_cmpuint(strlen(second), >, 0);
  /* Different from the original auto-generated id (high probability). */
  g_assert_cmpstr(first, !=, second);

  gnostr_filter_set_set_id(fs, "custom-id");
  g_assert_cmpstr(gnostr_filter_set_get_id(fs), ==, "custom-id");

  g_free((gchar *)first);
  g_object_unref(fs);
}

static void
test_is_empty(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  g_assert_true(gnostr_filter_set_is_empty(fs));

  /* Metadata alone does not make the filter non-empty */
  gnostr_filter_set_set_name(fs, "Hello");
  g_assert_true(gnostr_filter_set_is_empty(fs));

  gnostr_filter_set_set_limit(fs, 50);
  g_assert_false(gnostr_filter_set_is_empty(fs));

  gnostr_filter_set_set_limit(fs, 0);
  g_assert_true(gnostr_filter_set_is_empty(fs));

  g_object_unref(fs);
}

static void
test_setters_roundtrip(void)
{
  GnostrFilterSet *fs = make_populated_filter_set();

  g_assert_cmpstr(gnostr_filter_set_get_id(fs),          ==, "fs-test-001");
  g_assert_cmpstr(gnostr_filter_set_get_name(fs),        ==, "Following");
  g_assert_cmpstr(gnostr_filter_set_get_description(fs), ==, "My followed authors");
  g_assert_cmpstr(gnostr_filter_set_get_icon(fs),        ==, "user-home-symbolic");
  g_assert_cmpstr(gnostr_filter_set_get_color(fs),       ==, "#3584e4");
  g_assert_cmpint(gnostr_filter_set_get_source(fs),      ==, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);

  const gchar * const *a = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(a);
  g_assert_cmpstr(a[0], ==, test_authors[0]);
  g_assert_cmpstr(a[1], ==, test_authors[1]);
  g_assert_null(a[2]);

  const gchar * const *h = gnostr_filter_set_get_hashtags(fs);
  g_assert_nonnull(h);
  g_assert_cmpstr(h[0], ==, "bitcoin");
  g_assert_cmpstr(h[2], ==, "zaps");
  g_assert_null(h[3]);

  const gchar * const *ids = gnostr_filter_set_get_ids(fs);
  g_assert_nonnull(ids);
  g_assert_cmpstr(ids[0], ==, test_ids[0]);
  g_assert_cmpstr(ids[1], ==, test_ids[1]);
  g_assert_null(ids[2]);

  const gchar * const *excl = gnostr_filter_set_get_excluded_authors(fs);
  g_assert_nonnull(excl);
  g_assert_cmpstr(excl[0], ==, test_excluded[0]);
  g_assert_null(excl[1]);

  gsize nk = 0;
  const gint *k = gnostr_filter_set_get_kinds(fs, &nk);
  g_assert_nonnull(k);
  g_assert_cmpuint(nk, ==, 3);
  g_assert_cmpint(k[0], ==, 1);
  g_assert_cmpint(k[1], ==, 6);
  g_assert_cmpint(k[2], ==, 30023);

  g_assert_cmpint(gnostr_filter_set_get_since(fs), ==, 1700000000);
  g_assert_cmpint(gnostr_filter_set_get_until(fs), ==, 1800000000);
  g_assert_cmpint(gnostr_filter_set_get_limit(fs), ==, 200);

  g_assert_false(gnostr_filter_set_is_empty(fs));

  g_object_unref(fs);
}

static void
test_ids_and_excluded_affect_is_empty(void)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  g_assert_true(gnostr_filter_set_is_empty(fs));

  /* ids alone should make is_empty FALSE */
  gnostr_filter_set_set_ids(fs, test_ids);
  g_assert_false(gnostr_filter_set_is_empty(fs));
  gnostr_filter_set_set_ids(fs, NULL);
  g_assert_true(gnostr_filter_set_is_empty(fs));

  /* excluded_authors alone likewise */
  gnostr_filter_set_set_excluded_authors(fs, test_excluded);
  g_assert_false(gnostr_filter_set_is_empty(fs));
  gnostr_filter_set_set_excluded_authors(fs, NULL);
  g_assert_true(gnostr_filter_set_is_empty(fs));

  g_object_unref(fs);
}

static void
test_setters_clear_with_null(void)
{
  GnostrFilterSet *fs = make_populated_filter_set();

  gnostr_filter_set_set_authors(fs, NULL);
  g_assert_null(gnostr_filter_set_get_authors(fs));

  gnostr_filter_set_set_hashtags(fs, NULL);
  g_assert_null(gnostr_filter_set_get_hashtags(fs));

  gnostr_filter_set_set_kinds(fs, NULL, 0);
  gsize n = 99;
  g_assert_null(gnostr_filter_set_get_kinds(fs, &n));
  g_assert_cmpuint(n, ==, 0);

  gnostr_filter_set_set_description(fs, NULL);
  g_assert_null(gnostr_filter_set_get_description(fs));

  gnostr_filter_set_set_description(fs, "");
  g_assert_null(gnostr_filter_set_get_description(fs));

  g_object_unref(fs);
}

static void
test_clone_deep_copy(void)
{
  GnostrFilterSet *a = make_populated_filter_set();
  GnostrFilterSet *b = gnostr_filter_set_clone(a);

  g_assert_true(gnostr_filter_set_equal(a, b));

  /* Mutating clone must not affect original. */
  gnostr_filter_set_set_name(b, "Changed");
  g_assert_cmpstr(gnostr_filter_set_get_name(a), ==, "Following");
  g_assert_cmpstr(gnostr_filter_set_get_name(b), ==, "Changed");
  g_assert_false(gnostr_filter_set_equal(a, b));

  /* Same for string arrays. */
  const gchar *new_hashtags[] = { "changed", NULL };
  gnostr_filter_set_set_hashtags(b, new_hashtags);
  const gchar * const *orig_h = gnostr_filter_set_get_hashtags(a);
  g_assert_cmpstr(orig_h[0], ==, "bitcoin");

  g_object_unref(a);
  g_object_unref(b);
}

static void
test_equal_handles_null(void)
{
  g_assert_true (gnostr_filter_set_equal(NULL, NULL));
  GnostrFilterSet *a = gnostr_filter_set_new();
  g_assert_false(gnostr_filter_set_equal(a, NULL));
  g_assert_false(gnostr_filter_set_equal(NULL, a));
  g_assert_true (gnostr_filter_set_equal(a, a));
  g_object_unref(a);
}

static void
test_json_roundtrip_empty(void)
{
  GnostrFilterSet *a = gnostr_filter_set_new();
  gnostr_filter_set_set_id(a, "fs-empty");

  gchar *json = gnostr_filter_set_to_json(a);
  g_assert_nonnull(json);

  GError *error = NULL;
  GnostrFilterSet *b = gnostr_filter_set_new_from_json(json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(b);

  g_assert_true(gnostr_filter_set_equal(a, b));
  g_assert_true(gnostr_filter_set_is_empty(b));

  g_free(json);
  g_object_unref(a);
  g_object_unref(b);
}

static void
test_json_roundtrip_full(void)
{
  GnostrFilterSet *a = make_populated_filter_set();

  gchar *json = gnostr_filter_set_to_json(a);
  g_assert_nonnull(json);
  /* Sanity: emitted JSON mentions known fields. */
  g_assert_nonnull(strstr(json, "\"id\""));
  g_assert_nonnull(strstr(json, "\"Following\""));
  g_assert_nonnull(strstr(json, "\"bitcoin\""));
  g_assert_nonnull(strstr(json, "\"predefined\""));

  GError *error = NULL;
  GnostrFilterSet *b = gnostr_filter_set_new_from_json(json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(b);

  g_assert_true(gnostr_filter_set_equal(a, b));

  g_free(json);
  g_object_unref(a);
  g_object_unref(b);
}

static void
test_json_invalid_input(void)
{
  GError *error = NULL;

  /* Not JSON at all */
  GnostrFilterSet *fs = gnostr_filter_set_new_from_json("not json at all", &error);
  g_assert_null(fs);
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* JSON but not an object */
  fs = gnostr_filter_set_new_from_json("[1,2,3]", &error);
  g_assert_null(fs);
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* Empty object is valid (all fields optional) */
  fs = gnostr_filter_set_new_from_json("{}", &error);
  g_assert_no_error(error);
  g_assert_nonnull(fs);
  g_assert_true(gnostr_filter_set_is_empty(fs));
  g_object_unref(fs);
}

static void
test_variant_roundtrip_empty(void)
{
  GnostrFilterSet *a = gnostr_filter_set_new();
  gnostr_filter_set_set_id(a, "fs-variant-empty");

  GVariant *v = gnostr_filter_set_to_variant(a);
  g_assert_nonnull(v);
  v = g_variant_ref_sink(v);

  GnostrFilterSet *b = gnostr_filter_set_new_from_variant(v);
  g_assert_nonnull(b);
  g_assert_true(gnostr_filter_set_equal(a, b));

  g_variant_unref(v);
  g_object_unref(a);
  g_object_unref(b);
}

static void
test_variant_roundtrip_full(void)
{
  GnostrFilterSet *a = make_populated_filter_set();
  GVariant *v = gnostr_filter_set_to_variant(a);
  g_assert_nonnull(v);
  v = g_variant_ref_sink(v);
  g_assert_true(g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}")));

  GnostrFilterSet *b = gnostr_filter_set_new_from_variant(v);
  g_assert_nonnull(b);
  g_assert_true(gnostr_filter_set_equal(a, b));

  g_variant_unref(v);
  g_object_unref(a);
  g_object_unref(b);
}

static void
test_variant_wrong_type_rejected(void)
{
  GVariant *bad = g_variant_new_string("not a dict");
  bad = g_variant_ref_sink(bad);
  GnostrFilterSet *fs = gnostr_filter_set_new_from_variant(bad);
  g_assert_null(fs);
  g_variant_unref(bad);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/filter-set/construction-defaults",    test_construction_defaults);
  g_test_add_func("/filter-set/new-with-name",            test_new_with_name);
  g_test_add_func("/filter-set/id-null-regenerates",      test_id_setter_regenerates_on_null);
  g_test_add_func("/filter-set/is-empty",                 test_is_empty);
  g_test_add_func("/filter-set/ids-excluded-is-empty",    test_ids_and_excluded_affect_is_empty);
  g_test_add_func("/filter-set/setters-roundtrip",        test_setters_roundtrip);
  g_test_add_func("/filter-set/setters-clear-with-null",  test_setters_clear_with_null);
  g_test_add_func("/filter-set/clone-deep-copy",          test_clone_deep_copy);
  g_test_add_func("/filter-set/equal-null",               test_equal_handles_null);
  g_test_add_func("/filter-set/json-roundtrip-empty",     test_json_roundtrip_empty);
  g_test_add_func("/filter-set/json-roundtrip-full",      test_json_roundtrip_full);
  g_test_add_func("/filter-set/json-invalid-input",       test_json_invalid_input);
  g_test_add_func("/filter-set/variant-roundtrip-empty",  test_variant_roundtrip_empty);
  g_test_add_func("/filter-set/variant-roundtrip-full",   test_variant_roundtrip_full);
  g_test_add_func("/filter-set/variant-wrong-type",       test_variant_wrong_type_rejected);

  return g_test_run();
}
