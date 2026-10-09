#include "gh-diagnostics.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gchar *
make_state_home(void)
{
  g_autoptr(GError) error = NULL;
  gchar *path = g_dir_make_tmp("groundhog-diagnostics-XXXXXX", &error);
  g_assert_no_error(error);
  return path;
}

static void
flush_idle(void)
{
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
}

static void
test_opt_in_and_clear(void)
{
  g_autofree gchar *home = make_state_home();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autofree gchar *directory = g_build_filename(home, "groundhog", "diagnostics", NULL);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, home);
  g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_FAILED);
  flush_idle();
  g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));

  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_FAILED);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_FAILED);
  gh_diagnostics_record(diagnostics, (GhDiagnosticComponent)99,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_FAILED);
  flush_idle();
  g_assert_true(g_file_test(directory, G_FILE_TEST_IS_DIR));
  struct stat st;
  g_assert_cmpint(g_stat(directory, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0700);
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  g_assert_nonnull(dir);
  const gchar *name = g_dir_read_name(dir);
  g_assert_nonnull(name);
  g_autofree gchar *file = g_build_filename(directory, name, NULL);
  g_assert_cmpint(g_stat(file, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_assert_null(g_dir_read_name(dir));
  g_autofree gchar *report = gh_diagnostics_snapshot(diagnostics);
  g_assert_nonnull(strstr(report, "relay\tconnect_failed\tfailed\t2"));
  g_assert_cmpuint(strlen(report), <=, 8000);
  g_assert_null(strstr(report, "99"));

  gh_diagnostics_free(diagnostics);
  diagnostics = gh_diagnostics_new(settings, home);
  g_autofree gchar *reloaded = gh_diagnostics_snapshot(diagnostics);
  g_assert_nonnull(strstr(reloaded, "relay\tconnect_failed\tfailed\t2"));
  g_settings_set_boolean(settings, "diagnostics-enabled", FALSE);
  g_assert_null(gh_diagnostics_get_delete_error(diagnostics));
  g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
  g_autofree gchar *cleared = gh_diagnostics_snapshot(diagnostics);
  g_assert_null(strstr(cleared, "connect_failed"));
  gh_diagnostics_free(diagnostics);
  g_rmdir(directory);
  g_autofree gchar *parent = g_build_filename(home, "groundhog", NULL);
  g_rmdir(parent);
  g_rmdir(home);
}

static void
test_memory_fallback(void)
{
  g_autofree gchar *home = make_state_home();
  g_autofree gchar *not_directory = g_build_filename(home, "not-a-directory", NULL);
  g_assert_true(g_file_set_contents(not_directory, "x", 1, NULL));
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, not_directory);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_UI,
                        GH_DIAGNOSTIC_EVENT_RENDER_FALLBACK, GH_DIAGNOSTIC_RESULT_FAILED);
  flush_idle();
  g_assert_nonnull(gh_diagnostics_get_save_error(diagnostics));
  g_autofree gchar *report = gh_diagnostics_snapshot(diagnostics);
  g_assert_nonnull(strstr(report, "ui\trender_fallback\tfailed\t1"));
  gh_diagnostics_free(diagnostics);
  g_remove(not_directory);
  g_rmdir(home);
}

static void
test_disable_cancels_pending_write(void)
{
  g_autofree gchar *home = make_state_home();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, home);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_NIP29,
                        GH_DIAGNOSTIC_EVENT_HISTORY_PARTIAL, GH_DIAGNOSTIC_RESULT_FAILED);
  g_settings_set_boolean(settings, "diagnostics-enabled", FALSE);
  flush_idle();
  g_autofree gchar *directory = g_build_filename(home, "groundhog", "diagnostics", NULL);
  g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));
  gh_diagnostics_free(diagnostics);
  g_rmdir(home);
}

static void
test_rotation(void)
{
  g_autofree gchar *home = make_state_home();
  g_autofree gchar *parent = g_build_filename(home, "groundhog", NULL);
  g_autofree gchar *directory = g_build_filename(parent, "diagnostics", NULL);
  g_assert_cmpint(g_mkdir(parent, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  for (gint i = 1; i <= 4; i++) {
    g_autoptr(GDateTime) date = g_date_time_add_days(now, -i);
    g_autofree gchar *day = g_date_time_format(date, "%Y-%m-%d");
    g_autofree gchar *name = g_strdup_printf("%s.tsv", day);
    g_autofree gchar *file = g_build_filename(directory, name, NULL);
    g_assert_true(g_file_set_contents(file, "schema\t1\n", -1, NULL));
    g_assert_cmpint(g_chmod(file, 0600), ==, 0);
  }
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, home);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_RETRY);
  flush_idle();
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  g_assert_nonnull(dir);
  guint files = 0;
  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) files++;
  g_assert_cmpuint(files, ==, 3);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_diagnostics_clear(diagnostics, &error));
  g_assert_no_error(error);
  gh_diagnostics_free(diagnostics);
  g_rmdir(directory);
  g_rmdir(parent);
  g_rmdir(home);
}

static void
test_delete_failure_is_visible(void)
{
  g_autofree gchar *home = make_state_home();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, home);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_PUBLISH_REJECTED, GH_DIAGNOSTIC_RESULT_FAILED);
  flush_idle();
  g_autofree gchar *directory = g_build_filename(home, "groundhog", "diagnostics", NULL);
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  g_assert_nonnull(dir);
  const gchar *name = g_dir_read_name(dir);
  g_assert_nonnull(name);
  g_autofree gchar *file = g_build_filename(directory, name, NULL);
  g_assert_cmpint(g_chmod(directory, 0500), ==, 0);
  g_settings_set_boolean(settings, "diagnostics-enabled", FALSE);
  g_assert_nonnull(gh_diagnostics_get_delete_error(diagnostics));
  g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
  g_assert_cmpint(g_chmod(directory, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_diagnostics_clear(diagnostics, &error));
  g_assert_no_error(error);
  gh_diagnostics_free(diagnostics);
  g_rmdir(directory);
  g_autofree gchar *parent = g_build_filename(home, "groundhog", NULL);
  g_rmdir(parent);
  g_rmdir(home);
}

static void
test_refuse_symlink(void)
{
  g_autofree gchar *home = make_state_home();
  g_autofree gchar *outside = make_state_home();
  g_autofree gchar *parent = g_build_filename(home, "groundhog", NULL);
  g_assert_cmpint(g_mkdir(parent, 0700), ==, 0);
  g_autofree gchar *link = g_build_filename(parent, "diagnostics", NULL);
  g_assert_cmpint(symlink(outside, link), ==, 0);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "diagnostics-enabled", TRUE);
  GhDiagnostics *diagnostics = gh_diagnostics_new(settings, home);
  gh_diagnostics_record(diagnostics, GH_DIAGNOSTIC_COMPONENT_RELAY,
                        GH_DIAGNOSTIC_EVENT_CONNECT_FAILED, GH_DIAGNOSTIC_RESULT_RETRY);
  flush_idle();
  g_assert_nonnull(gh_diagnostics_get_save_error(diagnostics));
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_diagnostics_clear(diagnostics, &error));
  g_assert_nonnull(error);
  gh_diagnostics_free(diagnostics);
  g_unlink(link);
  g_rmdir(parent);
  g_rmdir(outside);
  g_rmdir(home);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/diagnostics/opt-in-and-clear", test_opt_in_and_clear);
  g_test_add_func("/diagnostics/memory-fallback", test_memory_fallback);
  g_test_add_func("/diagnostics/disable-cancels-pending-write", test_disable_cancels_pending_write);
  g_test_add_func("/diagnostics/refuse-symlink", test_refuse_symlink);
  g_test_add_func("/diagnostics/delete-failure-is-visible", test_delete_failure_is_visible);
  g_test_add_func("/diagnostics/rotation", test_rotation);
  return g_test_run();
}
