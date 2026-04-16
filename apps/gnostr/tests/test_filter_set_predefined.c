/* test_filter_set_predefined.c — Unit tests for the predefined filter
 * set builders and manager wiring.
 *
 * SPDX-License-Identifier: MIT
 *
 * Only the pure builders + `install` path are exercised here. The
 * refresh_from_services() path touches process-wide singletons (follow
 * list cache, bookmarks, mute list) which makes it poorly suited to
 * isolated unit tests.
 *
 * nostrc-yg8j.3
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "model/gnostr-filter-set.h"
#include "model/gnostr-filter-set-manager.h"
#include "model/gnostr-filter-set-predefined.h"

/* ----- Helpers ----- */

static const gchar *follows_a[] = {
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
  NULL
};

static const gchar *follows_b[] = {
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f",
  "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f",
  NULL
};

static const gchar *bookmark_ids[] = {
  "aaaabbbbccccddddeeeeffff00001111222233334444555566667777aaaabbbb",
  "bbbbccccddddeeeeffff00001111222233334444555566667777888899990000",
  NULL
};

static const gchar *muted[] = {
  "deadbeef00000000000000000000000000000000000000000000000000000000",
  NULL
};

/* ----- Tests ----- */

static void
test_build_following(void)
{
  g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_predefined_build_following(follows_a);
  g_assert_nonnull(fs);
  g_assert_cmpstr(gnostr_filter_set_get_id(fs),   ==, GNOSTR_FILTER_SET_ID_FOLLOWING);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs), ==, "Following");
  g_assert_cmpint(gnostr_filter_set_get_source(fs),
                  ==, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);

  /* Authors populated from input */
  const gchar * const *a = gnostr_filter_set_get_authors(fs);
  g_assert_nonnull(a);
  g_assert_cmpstr(a[0], ==, follows_a[0]);
  g_assert_cmpstr(a[1], ==, follows_a[1]);
  g_assert_null(a[2]);

  /* Kinds fixed to [1, 6] */
  gsize nk = 0;
  const gint *k = gnostr_filter_set_get_kinds(fs, &nk);
  g_assert_cmpuint(nk, ==, 2);
  g_assert_cmpint(k[0], ==, 1);
  g_assert_cmpint(k[1], ==, 6);

  /* Not empty */
  g_assert_false(gnostr_filter_set_is_empty(fs));
}

static void
test_build_following_empty(void)
{
  g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_predefined_build_following(NULL);
  g_assert_nonnull(fs);
  /* Empty authors but kinds are set — still not empty as per is_empty */
  g_assert_null(gnostr_filter_set_get_authors(fs));
  g_assert_false(gnostr_filter_set_is_empty(fs)); /* kinds keeps it non-empty */
}

static void
test_build_bookmarks(void)
{
  g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_predefined_build_bookmarks(bookmark_ids);
  g_assert_nonnull(fs);
  g_assert_cmpstr(gnostr_filter_set_get_id(fs),   ==, GNOSTR_FILTER_SET_ID_BOOKMARKS);
  g_assert_cmpstr(gnostr_filter_set_get_name(fs), ==, "Bookmarks");

  const gchar * const *ids = gnostr_filter_set_get_ids(fs);
  g_assert_nonnull(ids);
  g_assert_cmpstr(ids[0], ==, bookmark_ids[0]);
  g_assert_cmpstr(ids[1], ==, bookmark_ids[1]);
  g_assert_null(ids[2]);

  /* No kind restriction */
  gsize nk = 99;
  g_assert_null(gnostr_filter_set_get_kinds(fs, &nk));
  g_assert_cmpuint(nk, ==, 0);
}

static void
test_build_bookmarks_empty(void)
{
  g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_predefined_build_bookmarks(NULL);
  g_assert_nonnull(fs);
  g_assert_null(gnostr_filter_set_get_ids(fs));
  /* Empty ids + no kinds + no other criteria -> empty */
  g_assert_true(gnostr_filter_set_is_empty(fs));
}

static void
test_build_muted_inverse(void)
{
  g_autoptr(GnostrFilterSet) fs =
      gnostr_filter_set_predefined_build_muted_inverse(muted);
  g_assert_nonnull(fs);
  g_assert_cmpstr(gnostr_filter_set_get_id(fs),
                  ==, GNOSTR_FILTER_SET_ID_MUTED_INVERSE);

  /* excluded_authors populated */
  const gchar * const *ex = gnostr_filter_set_get_excluded_authors(fs);
  g_assert_nonnull(ex);
  g_assert_cmpstr(ex[0], ==, muted[0]);
  g_assert_null(ex[1]);

  /* No authors constraint — the feed is "global minus muted" */
  g_assert_null(gnostr_filter_set_get_authors(fs));

  /* Kinds fixed to [1] */
  gsize nk = 0;
  const gint *k = gnostr_filter_set_get_kinds(fs, &nk);
  g_assert_cmpuint(nk, ==, 1);
  g_assert_cmpint(k[0], ==, 1);
}

static void
test_install_adds_three(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();

  gnostr_filter_set_predefined_install(mgr, follows_a, bookmark_ids, muted);

  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 3);
  g_assert_true(gnostr_filter_set_manager_contains(mgr, GNOSTR_FILTER_SET_ID_FOLLOWING));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, GNOSTR_FILTER_SET_ID_BOOKMARKS));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, GNOSTR_FILTER_SET_ID_MUTED_INVERSE));

  /* All three are predefined */
  const char *ids[] = {
      GNOSTR_FILTER_SET_ID_FOLLOWING,
      GNOSTR_FILTER_SET_ID_BOOKMARKS,
      GNOSTR_FILTER_SET_ID_MUTED_INVERSE
  };
  for (gsize i = 0; i < G_N_ELEMENTS(ids); i++) {
    GnostrFilterSet *fs = gnostr_filter_set_manager_get(mgr, ids[i]);
    g_assert_cmpint(gnostr_filter_set_get_source(fs),
                    ==, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);
  }
}

static void
test_install_updates_existing(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();

  /* First install — Following has 2 pubkeys */
  gnostr_filter_set_predefined_install(mgr, follows_a, NULL, NULL);
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 3);

  /* Second install — Following now has 3 pubkeys, Bookmarks gets ids */
  gnostr_filter_set_predefined_install(mgr, follows_b, bookmark_ids, muted);

  /* Count is still 3 (updates, not additions) */
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 3);

  /* Following authors reflect the new list */
  GnostrFilterSet *follow = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_FOLLOWING);
  const gchar * const *a = gnostr_filter_set_get_authors(follow);
  g_assert_nonnull(a);
  g_assert_cmpstr(a[0], ==, follows_b[0]);
  g_assert_cmpstr(a[2], ==, follows_b[2]);
  g_assert_null(a[3]);

  /* Bookmarks ids populated */
  GnostrFilterSet *bm = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_BOOKMARKS);
  const gchar * const *ids = gnostr_filter_set_get_ids(bm);
  g_assert_nonnull(ids);
  g_assert_cmpstr(ids[0], ==, bookmark_ids[0]);

  /* Muted-inverse excluded_authors populated */
  GnostrFilterSet *mi = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_MUTED_INVERSE);
  const gchar * const *ex = gnostr_filter_set_get_excluded_authors(mi);
  g_assert_nonnull(ex);
  g_assert_cmpstr(ex[0], ==, muted[0]);
}

static void
test_install_all_null_lists(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  gnostr_filter_set_predefined_install(mgr, NULL, NULL, NULL);

  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 3);
  /* All three exist but carry empty lists */
  GnostrFilterSet *follow = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_FOLLOWING);
  g_assert_null(gnostr_filter_set_get_authors(follow));

  GnostrFilterSet *bm = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_BOOKMARKS);
  g_assert_null(gnostr_filter_set_get_ids(bm));

  GnostrFilterSet *mi = gnostr_filter_set_manager_get(
      mgr, GNOSTR_FILTER_SET_ID_MUTED_INVERSE);
  g_assert_null(gnostr_filter_set_get_excluded_authors(mi));
}

static void
test_install_predefined_not_removable(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  gnostr_filter_set_predefined_install(mgr, follows_a, bookmark_ids, muted);

  /* Manager must refuse to remove predefined entries. */
  g_assert_false(gnostr_filter_set_manager_remove(mgr, GNOSTR_FILTER_SET_ID_FOLLOWING));
  g_assert_false(gnostr_filter_set_manager_remove(mgr, GNOSTR_FILTER_SET_ID_BOOKMARKS));
  g_assert_false(gnostr_filter_set_manager_remove(mgr, GNOSTR_FILTER_SET_ID_MUTED_INVERSE));
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 3);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/filter-set-predefined/build-following",           test_build_following);
  g_test_add_func("/filter-set-predefined/build-following-empty",     test_build_following_empty);
  g_test_add_func("/filter-set-predefined/build-bookmarks",           test_build_bookmarks);
  g_test_add_func("/filter-set-predefined/build-bookmarks-empty",     test_build_bookmarks_empty);
  g_test_add_func("/filter-set-predefined/build-muted-inverse",       test_build_muted_inverse);
  g_test_add_func("/filter-set-predefined/install-adds-three",        test_install_adds_three);
  g_test_add_func("/filter-set-predefined/install-updates-existing",  test_install_updates_existing);
  g_test_add_func("/filter-set-predefined/install-all-null-lists",    test_install_all_null_lists);
  g_test_add_func("/filter-set-predefined/predefined-not-removable",  test_install_predefined_not_removable);

  return g_test_run();
}
