/* test_filter_set_manager.c — Unit tests for GnostrFilterSetManager.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.2
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <locale.h>
#include <string.h>

#include "model/gnostr-filter-set.h"
#include "model/gnostr-filter-set-manager.h"

/* ------------------------------------------------------------------------
 * Temp-path helper: each test case creates a private dir so file-backed
 * tests are isolated even under parallel ctest runs.
 * ------------------------------------------------------------------------ */

typedef struct {
  gchar *dir;
  gchar *path;
} TestPath;

static void
test_path_init(TestPath *tp, const char *label)
{
  g_autofree gchar *tmpl = g_strdup_printf("gnostr-fsm-%s-XXXXXX", label);
  tp->dir = g_dir_make_tmp(tmpl, NULL);
  g_assert_nonnull(tp->dir);
  tp->path = g_build_filename(tp->dir, "filter-sets.json", NULL);
}

static void
test_path_clear(TestPath *tp)
{
  if (tp->path && g_file_test(tp->path, G_FILE_TEST_EXISTS))
    g_unlink(tp->path);
  if (tp->dir) {
    g_rmdir(tp->dir);
  }
  g_clear_pointer(&tp->path, g_free);
  g_clear_pointer(&tp->dir, g_free);
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void
test_construction(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_assert_nonnull(mgr);
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 0);
  g_assert_nonnull(gnostr_filter_set_manager_get_model(mgr));
}

static void
test_install_defaults(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  gnostr_filter_set_manager_install_defaults(mgr);

  /* Four built-in sets */
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 4);

  /* All predefined */
  const char *predef_ids[] = {
      "predefined-global", "predefined-follows",
      "predefined-mentions", "predefined-media"
  };
  for (gsize i = 0; i < G_N_ELEMENTS(predef_ids); i++) {
    GnostrFilterSet *fs = gnostr_filter_set_manager_get(mgr, predef_ids[i]);
    g_assert_nonnull(fs);
    g_assert_cmpint(gnostr_filter_set_get_source(fs),
                    ==, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);
  }

  /* Idempotent */
  gnostr_filter_set_manager_install_defaults(mgr);
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 4);
}

static void
test_add_get_contains(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new_with_name("Custom");
  gnostr_filter_set_set_id(fs, "custom-1");

  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 1);
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "custom-1"));

  GnostrFilterSet *got = gnostr_filter_set_manager_get(mgr, "custom-1");
  g_assert_true(got == fs);
  g_assert_cmpstr(gnostr_filter_set_get_name(got), ==, "Custom");
  g_assert_null(gnostr_filter_set_manager_get(mgr, "missing"));
}

static void
test_add_auto_id(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new();
  /* Passing NULL/"" to the id setter regenerates a fresh id. */
  gnostr_filter_set_set_id(fs, NULL);

  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));
  const gchar *id = gnostr_filter_set_get_id(fs);
  g_assert_nonnull(id);
  g_assert_cmpuint(strlen(id), >, 0);
  g_assert_true(gnostr_filter_set_manager_contains(mgr, id));
}

static void
test_add_rejects_duplicate_id(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) a = gnostr_filter_set_new();
  g_autoptr(GnostrFilterSet) b = gnostr_filter_set_new();
  gnostr_filter_set_set_id(a, "dup-id");
  gnostr_filter_set_set_id(b, "dup-id");
  g_assert_true (gnostr_filter_set_manager_add(mgr, a));
  g_assert_false(gnostr_filter_set_manager_add(mgr, b));
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 1);
}

static void
test_update_in_place(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new_with_name("Initial");
  gnostr_filter_set_set_id(fs, "updatable");
  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));

  /* Prepare an updated copy */
  g_autoptr(GnostrFilterSet) upd = gnostr_filter_set_clone(fs);
  gnostr_filter_set_set_name(upd, "Renamed");
  gnostr_filter_set_set_description(upd, "After update");

  g_assert_true(gnostr_filter_set_manager_update(mgr, upd));

  /* The stored object should now reflect the update */
  GnostrFilterSet *got = gnostr_filter_set_manager_get(mgr, "updatable");
  g_assert_nonnull(got);
  g_assert_cmpstr(gnostr_filter_set_get_name(got), ==, "Renamed");
  g_assert_cmpstr(gnostr_filter_set_get_description(got), ==, "After update");
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 1);
}

static void
test_update_rejects_unknown(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new_with_name("Ghost");
  gnostr_filter_set_set_id(fs, "no-such-id");
  g_assert_false(gnostr_filter_set_manager_update(mgr, fs));
}

static void
test_remove_custom(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new_with_name("To-remove");
  gnostr_filter_set_set_id(fs, "removable");
  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 1);

  g_assert_true(gnostr_filter_set_manager_remove(mgr, "removable"));
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 0);
  g_assert_false(gnostr_filter_set_manager_contains(mgr, "removable"));

  /* Second remove is a no-op */
  g_assert_false(gnostr_filter_set_manager_remove(mgr, "removable"));
}

static void
test_remove_predefined_rejected(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  gnostr_filter_set_manager_install_defaults(mgr);

  g_assert_true(gnostr_filter_set_manager_contains(mgr, "predefined-global"));
  g_assert_false(gnostr_filter_set_manager_remove(mgr, "predefined-global"));
  /* Still present */
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "predefined-global"));
}

static void
test_load_missing_installs_defaults(void)
{
  TestPath tp;
  test_path_init(&tp, "load-missing");

  g_autoptr(GnostrFilterSetManager) mgr =
      gnostr_filter_set_manager_new_for_path(tp.path);

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_manager_load(mgr, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 4);

  test_path_clear(&tp);
}

static void
test_save_and_reload_roundtrip(void)
{
  TestPath tp;
  test_path_init(&tp, "roundtrip");

  /* First manager: install defaults + add two customs + save */
  {
    g_autoptr(GnostrFilterSetManager) mgr =
        gnostr_filter_set_manager_new_for_path(tp.path);
    g_autoptr(GError) err = NULL;
    g_assert_true(gnostr_filter_set_manager_load(mgr, &err));

    g_autoptr(GnostrFilterSet) a = gnostr_filter_set_new_with_name("My hashtag");
    gnostr_filter_set_set_id(a, "custom-a");
    const char *tags[] = { "bitcoin", "nostr", NULL };
    gnostr_filter_set_set_hashtags(a, tags);

    g_autoptr(GnostrFilterSet) b = gnostr_filter_set_new_with_name("Friends");
    gnostr_filter_set_set_id(b, "custom-b");
    const char *authors[] = { "abc123", "def456", NULL };
    gnostr_filter_set_set_authors(b, authors);
    const gint kinds[] = { 1, 6 };
    gnostr_filter_set_set_kinds(b, kinds, G_N_ELEMENTS(kinds));

    g_assert_true(gnostr_filter_set_manager_add(mgr, a));
    g_assert_true(gnostr_filter_set_manager_add(mgr, b));

    g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 6); /* 4 + 2 */
    g_assert_true(gnostr_filter_set_manager_save(mgr, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_test(tp.path, G_FILE_TEST_EXISTS));
  }

  /* Second manager: reload and check customs were persisted. Predefined
   * sets should be reinstalled fresh (not duplicated). */
  {
    g_autoptr(GnostrFilterSetManager) mgr2 =
        gnostr_filter_set_manager_new_for_path(tp.path);
    g_autoptr(GError) err = NULL;
    g_assert_true(gnostr_filter_set_manager_load(mgr2, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(gnostr_filter_set_manager_count(mgr2), ==, 6);

    GnostrFilterSet *a = gnostr_filter_set_manager_get(mgr2, "custom-a");
    g_assert_nonnull(a);
    g_assert_cmpstr(gnostr_filter_set_get_name(a), ==, "My hashtag");
    g_assert_cmpint(gnostr_filter_set_get_source(a),
                    ==, GNOSTR_FILTER_SET_SOURCE_CUSTOM);
    const gchar * const *tags = gnostr_filter_set_get_hashtags(a);
    g_assert_nonnull(tags);
    g_assert_cmpstr(tags[0], ==, "bitcoin");
    g_assert_cmpstr(tags[1], ==, "nostr");

    GnostrFilterSet *b = gnostr_filter_set_manager_get(mgr2, "custom-b");
    g_assert_nonnull(b);
    gsize nk = 0;
    const gint *kinds = gnostr_filter_set_get_kinds(b, &nk);
    g_assert_cmpuint(nk, ==, 2);
    g_assert_cmpint(kinds[0], ==, 1);
    g_assert_cmpint(kinds[1], ==, 6);

    /* Predefined still present */
    g_assert_true(gnostr_filter_set_manager_contains(mgr2, "predefined-global"));
  }

  test_path_clear(&tp);
}

static void
test_save_omits_predefined(void)
{
  TestPath tp;
  test_path_init(&tp, "omits-predef");

  g_autoptr(GnostrFilterSetManager) mgr =
      gnostr_filter_set_manager_new_for_path(tp.path);
  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_manager_load(mgr, &err));
  g_assert_true(gnostr_filter_set_manager_save(mgr, &err));
  g_assert_no_error(err);

  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(tp.path, &contents, NULL, NULL));
  /* None of the predefined ids should appear in the persisted file */
  g_assert_null(strstr(contents, "predefined-global"));
  g_assert_null(strstr(contents, "predefined-follows"));

  test_path_clear(&tp);
}

static void
test_load_rejects_invalid_json(void)
{
  TestPath tp;
  test_path_init(&tp, "invalid");
  g_assert_true(g_file_set_contents(tp.path, "not json", -1, NULL));

  g_autoptr(GnostrFilterSetManager) mgr =
      gnostr_filter_set_manager_new_for_path(tp.path);

  g_autoptr(GError) err = NULL;
  g_assert_false(gnostr_filter_set_manager_load(mgr, &err));
  g_assert_nonnull(err);

  test_path_clear(&tp);
}

static void
test_load_skips_invalid_entries(void)
{
  TestPath tp;
  test_path_init(&tp, "mixed-valid");

  /* Three entries:
   *   - valid custom
   *   - non-object (should be skipped silently)
   *   - duplicate of a predefined id (should also be skipped)
   */
  const char *payload =
      "{\n"
      "  \"version\": 1,\n"
      "  \"filter_sets\": [\n"
      "    {\"id\":\"custom-good\", \"name\":\"Good\"},\n"
      "    42,\n"
      "    {\"id\":\"predefined-global\", \"name\":\"Collides\"}\n"
      "  ]\n"
      "}\n";
  g_assert_true(g_file_set_contents(tp.path, payload, -1, NULL));

  g_autoptr(GnostrFilterSetManager) mgr =
      gnostr_filter_set_manager_new_for_path(tp.path);
  g_autoptr(GError) err = NULL;

  /* Silence expected g_warning from the duplicate-id skip so the test
   * output stays clean. */
  g_test_expect_message("gnostr-filter-set-manager",
                        G_LOG_LEVEL_WARNING,
                        "*duplicate*");
  g_assert_true(gnostr_filter_set_manager_load(mgr, &err));
  g_test_assert_expected_messages();
  g_assert_no_error(err);

  /* Defaults (4) + 1 good custom = 5 */
  g_assert_cmpuint(gnostr_filter_set_manager_count(mgr), ==, 5);
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "custom-good"));
  /* The predefined kept its original name, not the colliding one */
  GnostrFilterSet *global = gnostr_filter_set_manager_get(mgr, "predefined-global");
  g_assert_nonnull(global);
  g_assert_cmpstr(gnostr_filter_set_get_name(global), ==, "Global");

  test_path_clear(&tp);
}

static void
test_get_model_observable(void)
{
  g_autoptr(GnostrFilterSetManager) mgr = gnostr_filter_set_manager_new();
  GListModel *model = gnostr_filter_set_manager_get_model(mgr);
  g_assert_nonnull(model);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 0);

  g_autoptr(GnostrFilterSet) fs = gnostr_filter_set_new_with_name("One");
  gnostr_filter_set_set_id(fs, "m-1");
  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));

  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 1);
  g_autoptr(GnostrFilterSet) got =
      (GnostrFilterSet *)g_list_model_get_item(model, 0);
  g_assert_nonnull(got);
  g_assert_cmpstr(gnostr_filter_set_get_id(got), ==, "m-1");
}

/* ------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
  setlocale(LC_ALL, "C");
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/filter-set-manager/construction",             test_construction);
  g_test_add_func("/filter-set-manager/install-defaults",         test_install_defaults);
  g_test_add_func("/filter-set-manager/add-get-contains",         test_add_get_contains);
  g_test_add_func("/filter-set-manager/add-auto-id",              test_add_auto_id);
  g_test_add_func("/filter-set-manager/add-rejects-duplicate",    test_add_rejects_duplicate_id);
  g_test_add_func("/filter-set-manager/update-in-place",          test_update_in_place);
  g_test_add_func("/filter-set-manager/update-rejects-unknown",   test_update_rejects_unknown);
  g_test_add_func("/filter-set-manager/remove-custom",            test_remove_custom);
  g_test_add_func("/filter-set-manager/remove-predefined",        test_remove_predefined_rejected);
  g_test_add_func("/filter-set-manager/load-missing-defaults",    test_load_missing_installs_defaults);
  g_test_add_func("/filter-set-manager/save-reload",              test_save_and_reload_roundtrip);
  g_test_add_func("/filter-set-manager/save-omits-predefined",    test_save_omits_predefined);
  g_test_add_func("/filter-set-manager/load-invalid-json",        test_load_rejects_invalid_json);
  g_test_add_func("/filter-set-manager/load-skip-invalid-entry",  test_load_skips_invalid_entries);
  g_test_add_func("/filter-set-manager/get-model-observable",     test_get_model_observable);

  return g_test_run();
}
