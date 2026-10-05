/* W31 R2: the pre-rename per-user directories move to "grotto" once. */
#include "legacy_dirs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

#define OLD_NAME "gnostr" "-signer"

static gchar *
make_base(void)
{
  g_autoptr(GError) error = NULL;
  gchar *base = g_dir_make_tmp("grotto-legacy-XXXXXX", &error);
  g_assert_no_error(error);
  return base;
}

static void
put(const gchar *base, const gchar *dir, const gchar *file, const gchar *text)
{
  g_autofree gchar *d = g_build_filename(base, dir, NULL);
  g_assert_cmpint(g_mkdir_with_parents(d, 0700), ==, 0);
  g_autofree gchar *f = g_build_filename(d, file, NULL);
  g_assert_true(g_file_set_contents(f, text, -1, NULL));
}

static gchar *
get(const gchar *base, const gchar *dir, const gchar *file)
{
  g_autofree gchar *f = g_build_filename(base, dir, file, NULL);
  gchar *text = NULL;
  g_file_get_contents(f, &text, NULL, NULL);
  return text;
}

static void
cleanup(const gchar *base)
{
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", base);
  g_assert_cmpint(system(cmd), ==, 0);
}

static void
test_old_only(void)
{
  g_autofree gchar *base = make_base();
  put(base, OLD_NAME, "accounts.ini", "[a]\n");
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_MOVED);
  g_autofree gchar *moved = get(base, "grotto", "accounts.ini");
  g_assert_cmpstr(moved, ==, "[a]\n");
  g_autofree gchar *old_dir = g_build_filename(base, OLD_NAME, NULL);
  g_assert_false(g_file_test(old_dir, G_FILE_TEST_EXISTS));
  /* A second start has nothing left to do. */
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_NOTHING);
  cleanup(base);
}

static void
test_new_only(void)
{
  g_autofree gchar *base = make_base();
  put(base, "grotto", "accounts.ini", "[new]\n");
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_NOTHING);
  g_autofree gchar *kept = get(base, "grotto", "accounts.ini");
  g_assert_cmpstr(kept, ==, "[new]\n");
  cleanup(base);
}

static void
test_both(void)
{
  g_autofree gchar *base = make_base();
  put(base, OLD_NAME, "accounts.ini", "[old]\n");
  put(base, "grotto", "accounts.ini", "[new]\n");
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_KEPT);
  g_autofree gchar *kept = get(base, "grotto", "accounts.ini");
  g_assert_cmpstr(kept, ==, "[new]\n");
  g_autofree gchar *old = get(base, OLD_NAME, "accounts.ini");
  g_assert_cmpstr(old, ==, "[old]\n"); /* never deleted, never merged */
  cleanup(base);
}

static void
test_neither(void)
{
  g_autofree gchar *base = make_base();
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_NOTHING);
  g_autofree gchar *new_dir = g_build_filename(base, "grotto", NULL);
  g_assert_false(g_file_test(new_dir, G_FILE_TEST_EXISTS)); /* nothing created */
  cleanup(base);
}

static void
test_symlink_not_followed(void)
{
  g_autofree gchar *base = make_base();
  put(base, "elsewhere", "accounts.ini", "[target]\n");
  g_autofree gchar *target = g_build_filename(base, "elsewhere", NULL);
  g_autofree gchar *link = g_build_filename(base, OLD_NAME, NULL);
  g_assert_cmpint(symlink(target, link), ==, 0);
  g_assert_cmpint(grotto_legacy_dir_migrate_in(base), ==, GROTTO_LEGACY_DIR_NOTHING);
  g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
  cleanup(base);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/grotto/legacy-dirs/old-only", test_old_only);
  g_test_add_func("/grotto/legacy-dirs/new-only", test_new_only);
  g_test_add_func("/grotto/legacy-dirs/both", test_both);
  g_test_add_func("/grotto/legacy-dirs/neither", test_neither);
  g_test_add_func("/grotto/legacy-dirs/symlink-not-followed", test_symlink_not_followed);
  return g_test_run();
}
