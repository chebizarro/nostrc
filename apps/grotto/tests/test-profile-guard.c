/* test-profile-guard.c — Grotto refuses the owner's profile and key stores
 * under test (nostrc-wqn0u).
 *
 * Runs the real code (grotto_bunker_core) in the environment CTest gives
 * every Grotto test: NOSTRC_TEST_ISOLATION=1 and a private HOME / XDG_*.
 *
 *   /profile-guard/rules         the pure rules of nostrc-profile-guard.h
 *   /profile-guard/environment   this process really is isolated
 *   /profile-guard/secret-store  no Keychain / Secret Service reads, no
 *                                StoreKey / ClearKey to a signer on a
 *                                session bus that is not a private test bus;
 *                                the same call on a declared test bus is not
 *                                refused (it fails for its own reason)
 *   /profile-guard/startup       both Grotto programs abort at start-up, and
 *                                AccountsStore refuses its file, when the
 *                                XDG directories are the real profile
 *
 * The aborting cases run in a subprocess whose XDG_CONFIG_HOME names the
 * real ~/.config: the guard fires before anything there is opened. */
#include <glib.h>
#include <pwd.h>
#include <unistd.h>

#include "nostrc-test-isolation.h"
#include <nostrc-profile-guard.h>

#include "accounts_store.h"
#include "legacy_dirs.h"
#include "secret_store.h"

/* BIP-340 test vector 0 secret key (3): a well-known test key. */
#define TEST_SK_HEX "0000000000000000000000000000000000000000000000000000000000000003"
/* Any selector: the guard answers before a selector is looked at. */
#define TEST_NPUB "npub1profileguardtest"

static const gchar *
real_home(void)
{
  struct passwd *pw = getpwuid(getuid());
  g_assert_nonnull(pw);
  return pw->pw_dir;
}

static void
test_rules(void)
{
  const gchar *home = "/nonexistent-nostrc-home/owner";
  static const gchar *const refused[] = {
    "/nonexistent-nostrc-home/owner/.config",
    "/nonexistent-nostrc-home/owner/.config/grotto/accounts.ini",
    "/nonexistent-nostrc-home/owner/.local/share/grotto",
    "/nonexistent-nostrc-home/owner/.cache/gnostr/ndb",
    "/nonexistent-nostrc-home/owner/.local/state/x",
    "/nonexistent-nostrc-home/owner/Library/Keychains/login.keychain-db",
    "/nonexistent-nostrc-home/owner/build/../.config/grotto",
  };
  static const gchar *const allowed[] = {
    "/nonexistent-nostrc-home/owner/src/_build/test-homes/t/config",
    "/nonexistent-nostrc-home/owner/.configuration",
    "/nonexistent-nostrc-home/owner",
    "/tmp/grotto",
  };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++)
    g_assert_true(nostrc_profile_guard_is_real_profile_for(home, refused[i]));
  for (guint i = 0; i < G_N_ELEMENTS(allowed); i++)
    g_assert_false(nostrc_profile_guard_is_real_profile_for(home, allowed[i]));
  g_assert_false(nostrc_profile_guard_is_real_profile_for(NULL, refused[0]));

  /* Off test any bus; under test only the declared private one. */
  g_assert_true(nostrc_profile_guard_session_bus_allowed_for(FALSE, "unix:path=/a", NULL));
  g_assert_true(nostrc_profile_guard_session_bus_allowed_for(TRUE, "unix:path=/a", "unix:path=/a"));
  g_assert_false(nostrc_profile_guard_session_bus_allowed_for(TRUE, "unix:path=/a", "unix:path=/b"));
  g_assert_false(nostrc_profile_guard_session_bus_allowed_for(TRUE, "unix:path=/a", NULL));
  g_assert_false(nostrc_profile_guard_session_bus_allowed_for(TRUE, NULL, NULL));
  g_assert_false(nostrc_profile_guard_session_bus_allowed_for(TRUE, "", ""));

  g_assert_true(nostrc_profile_guard_settings_backend_allowed_for(TRUE, "GMemorySettingsBackend"));
  g_assert_true(nostrc_profile_guard_settings_backend_allowed_for(TRUE, "GKeyfileSettingsBackend"));
  g_assert_false(nostrc_profile_guard_settings_backend_allowed_for(TRUE, "DConfSettingsBackend"));
  g_assert_false(nostrc_profile_guard_settings_backend_allowed_for(TRUE, "GNextstepSettingsBackend"));
  g_assert_true(nostrc_profile_guard_settings_backend_allowed_for(FALSE, "DConfSettingsBackend"));
}

static void
test_environment(void)
{
  /* CTest set NOSTRC_TEST_ISOLATION and private directories for this test. */
  g_assert_true(nostrc_profile_guard_under_test());
  nostrc_test_isolation_assert();
  g_assert_null(nostrc_profile_guard_user_dirs_violation());
  g_assert_false(nostrc_profile_guard_refuses_path(g_get_user_config_dir()));
  g_autofree gchar *real_config = g_build_filename(real_home(), ".config", "grotto", NULL);
  g_assert_true(nostrc_profile_guard_refuses_path(real_config));
  g_assert_true(nostrc_profile_guard_refuses_default_keychain());
}

static void
test_secret_store(void)
{
  /* No private test bus here: whatever session bus there is, is not one. */
  g_unsetenv(NOSTRC_TEST_BUS_ADDRESS_ENV);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_assert_true(nostrc_profile_guard_refuses_session_keyring());

  /* Direct reads of the user's key store: refused, so nothing is listed. */
  g_assert_false(secret_store_is_available());
  g_autoptr(GPtrArray) listed = secret_store_list();
  g_assert_cmpuint(listed->len, ==, 0);
  g_assert_false(secret_store_has_secret(TEST_NPUB));
  g_autofree gchar *nsec = NULL;
  g_assert_cmpint(secret_store_get_secret(TEST_NPUB, &nsec), ==, SECRET_STORE_ERR_PERMISSION);
  g_assert_null(nsec);
  SecretStoreEntry *entry = NULL;
  g_assert_cmpint(secret_store_lookup_by_fingerprint("deadbeef", &entry), ==,
                  SECRET_STORE_ERR_PERMISSION);
  g_assert_null(entry);

  /* Key writes go to the signer daemon on the session bus: refused. */
  g_assert_cmpint(secret_store_add(TEST_SK_HEX, "test", FALSE), ==, SECRET_STORE_ERR_PERMISSION);
  g_assert_cmpint(secret_store_remove(TEST_NPUB), ==, SECRET_STORE_ERR_PERMISSION);
  g_autofree gchar *generated = NULL;
  g_assert_cmpint(secret_store_generate("test", FALSE, &generated), ==,
                  SECRET_STORE_ERR_PERMISSION);
  g_assert_null(generated);

  /* Control: declared a private test bus, the same StoreKey is not refused;
   * it fails because nothing listens at that address. */
  g_setenv(NOSTRC_TEST_BUS_ADDRESS_ENV, "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_assert_false(nostrc_profile_guard_refuses_session_keyring());
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*StoreKey (no session bus)*");
  g_assert_cmpint(secret_store_add(TEST_SK_HEX, "test", FALSE), ==, SECRET_STORE_ERR_BACKEND);
  g_test_assert_expected_messages();
  g_unsetenv(NOSTRC_TEST_BUS_ADDRESS_ENV);
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
}

static void
startup_child(void)
{
  grotto_legacy_dirs_migrate();
}

static void
accounts_child(void)
{
  AccountsStore *as = accounts_store_new();
  accounts_store_free(as);
}

static void
expect_refused_with_real_config(const gchar *child, const gchar *message)
{
  g_autofree gchar *real_config = g_build_filename(real_home(), ".config", NULL);
  g_auto(GStrv) envp = g_get_environ();
  envp = g_environ_setenv(envp, "XDG_CONFIG_HOME", real_config, TRUE);
  g_test_trap_subprocess_with_envp(child, (const gchar *const *)envp, 0,
                                   G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_test_trap_assert_stderr(message);
}

static void
test_startup(void)
{
  expect_refused_with_real_config("/profile-guard/startup/subprocess/legacy-dirs",
                                  "*grotto: the user config directory * is the real profile*");
  expect_refused_with_real_config("/profile-guard/startup/subprocess/accounts",
                                  "*accounts_store: * is the real profile; refusing it*");
  /* The same programs start in the private profile CTest gave them. */
  g_test_trap_subprocess("/profile-guard/startup/subprocess/legacy-dirs", 0,
                         G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed();
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/profile-guard/rules", test_rules);
  g_test_add_func("/profile-guard/environment", test_environment);
  g_test_add_func("/profile-guard/secret-store", test_secret_store);
  g_test_add_func("/profile-guard/startup", test_startup);
  g_test_add_func("/profile-guard/startup/subprocess/legacy-dirs", startup_child);
  g_test_add_func("/profile-guard/startup/subprocess/accounts", accounts_child);
  return g_test_run();
}
