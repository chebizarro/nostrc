/* test_profile_guard — GNostr refuses the owner's profile under test
 * (nostrc-wqn0u).
 *
 * Runs the real code in the environment CTest gives every GNostr test:
 * NOSTRC_TEST_ISOLATION=1, a private HOME / XDG_* and the memory GSettings
 * backend.
 *
 *   /gnostr/profile-guard/ndb-dir    the NDB directory is the private cache
 *                                    one; one in the real ~/.cache aborts
 *   /gnostr/profile-guard/startup    gnostr's start-up check passes here and
 *                                    aborts when XDG_DATA_HOME is the real one
 *   /gnostr/profile-guard/keystore   the identity shim reads no login
 *                                    Keychain / session keyring off a
 *                                    private test bus
 *
 * The aborting cases run in a subprocess: the guard fires before anything in
 * the real profile is opened. */
#include <glib.h>
#include <pwd.h>
#include <unistd.h>

#include "nostrc-test-isolation.h"
#include <nostrc-profile-guard.h>

#include "util/gnostr_paths.h"
#include "util/keystore.h"

static const gchar *
real_home(void)
{
  struct passwd *pw = getpwuid(getuid());
  g_assert_nonnull(pw);
  return pw->pw_dir;
}

static void
test_ndb_dir(void)
{
  g_assert_true(nostrc_profile_guard_under_test());
  nostrc_test_isolation_assert();
  g_unsetenv("GNOSTR_DB_DIR");
  g_autofree gchar *dir = gnostr_get_db_dir();
  g_autofree gchar *expected = g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  g_assert_cmpstr(dir, ==, expected);

  g_autofree gchar *real_ndb = g_build_filename(real_home(), ".cache", "gnostr", "ndb", NULL);
  g_auto(GStrv) envp = g_get_environ();
  envp = g_environ_setenv(envp, "GNOSTR_DB_DIR", real_ndb, TRUE);
  g_test_trap_subprocess_with_envp("/gnostr/profile-guard/subprocess/ndb-dir",
                                   (const gchar *const *)envp, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_test_trap_assert_stderr("*the NDB directory * is the real profile; refusing it*");
}

static void
ndb_dir_child(void)
{
  g_free(gnostr_get_db_dir());
}

static void
test_startup(void)
{
  /* Private directories and the memory backend: start-up proceeds. */
  gnostr_assert_private_profile();

  g_autofree gchar *real_data = g_build_filename(real_home(), ".local", "share", NULL);
  g_auto(GStrv) envp = g_get_environ();
  envp = g_environ_setenv(envp, "XDG_DATA_HOME", real_data, TRUE);
  g_test_trap_subprocess_with_envp("/gnostr/profile-guard/subprocess/startup",
                                   (const gchar *const *)envp, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_test_trap_assert_stderr("*gnostr: the user data directory * is the real profile*");
}

static void
startup_child(void)
{
  gnostr_assert_private_profile();
}

static void
test_keystore(void)
{
#if defined(HAVE_MACOS_KEYCHAIN) || defined(HAVE_LIBSECRET)
  /* No private test bus: the session bus here is not one. */
  g_unsetenv(NOSTRC_TEST_BUS_ADDRESS_ENV);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_assert_false(gnostr_keystore_available());
  g_assert_false(gnostr_keystore_has_key("npub1profileguardtest"));
  GError *error = NULL;
  g_assert_null(gnostr_keystore_list_keys(&error));
  g_assert_error(error, GNOSTR_KEYSTORE_ERROR, GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED);
  g_clear_error(&error);
  g_assert_null(gnostr_keystore_list_legacy_keys(&error));
  g_assert_error(error, GNOSTR_KEYSTORE_ERROR, GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED);
  g_clear_error(&error);
#ifdef HAVE_LIBSECRET
  /* Control: on a declared test bus the lookup is not refused; it fails
   * because nothing listens at that address. */
  g_setenv(NOSTRC_TEST_BUS_ADDRESS_ENV, "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_assert_null(gnostr_keystore_list_legacy_keys(&error));
  g_assert_nonnull(error);
  g_assert_false(g_error_matches(error, GNOSTR_KEYSTORE_ERROR, GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED));
  g_clear_error(&error);
  g_unsetenv(NOSTRC_TEST_BUS_ADDRESS_ENV);
#endif
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
#else
  /* The stub shim holds no store to refuse. */
  g_assert_false(gnostr_keystore_available());
#endif
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/profile-guard/ndb-dir", test_ndb_dir);
  g_test_add_func("/gnostr/profile-guard/startup", test_startup);
  g_test_add_func("/gnostr/profile-guard/keystore", test_keystore);
  g_test_add_func("/gnostr/profile-guard/subprocess/ndb-dir", ndb_dir_child);
  g_test_add_func("/gnostr/profile-guard/subprocess/startup", startup_child);
  return g_test_run();
}
