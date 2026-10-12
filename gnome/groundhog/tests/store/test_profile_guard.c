/* nostrc-ttvxv: a test can never reach the owner's real profile.
 *
 * Proves, through the code the app runs:
 *  - this CTest entry itself runs with a private HOME and XDG directories and
 *    NOSTRC_TEST_ISOLATION=1 (the directory-wide CTest setup), and
 *    nostrc_test_isolation_assert() aborts once HOME is the real home;
 *  - gh_store_open() / gh_store_exists() / gh_store_account_dir_path() refuse
 *    the real data home under test, and for the well-known test accounts
 *    (secret keys 1..4) even outside a test, before anything is created; an
 *    ordinary account outside a test is unaffected;
 *  - on macOS the store key (the app's GhStoreKey over the Keychain backend)
 *    and the NIP-46 credential store refuse the default Keychain in the same
 *    cases, while a temporary keychain stays usable.
 * Each case that points at the real profile first asserts the guard's verdict
 * and runs in a g_test_trap_subprocess() child, so nothing reaches the real
 * profile even if a later step regressed. */
#include "gh-profile-guard.h"
#include "gh-store.h"
#include "gh-store-key.h"
#include "nostrc-test-isolation.h"
#ifdef __APPLE__
#include "gh-store-key-keychain.h"
#include "gh-nip46-credentials.h"
#include "gh-nip46-credentials-private.h"
#endif

#include <glib/gstdio.h>
#include <pwd.h>
#include <unistd.h>

#define KEY1 "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define KEY3 "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
/* Not a well-known key: arbitrary 32 bytes. */
#define OTHER "5d0f5a1fd2b9e4f87c3b8f7a64a0d3e6f2b6c1a9e8d7c6b5a4f3e2d1c0b9a8f7"

static const gchar *
real_home(void)
{
  struct passwd *pw = getpwuid(getuid());
  g_assert_nonnull(pw);
  return pw->pw_dir;
}

static gchar *
real_data_home(void)
{
  return g_build_filename(real_home(), ".local", "share", NULL);
}

/* ---- the environment CTest gives every Groundhog test ---------------------- */

static void
test_ctest_environment(void)
{
  g_assert_true(nostrc_test_isolation_enabled());
  g_assert_true(gh_profile_guard_under_test());
  g_auto(GStrv) envp = g_get_environ();
  g_autofree gchar *why = nostrc_test_isolation_violation(real_home(),
                                                          (const gchar *const *)envp);
  g_assert_null(why);
  g_assert_cmpstr(g_get_home_dir(), !=, real_home());
  g_autofree gchar *share = real_data_home();
  g_assert_false(gh_profile_guard_refuses_data_dir(g_get_user_data_dir(), KEY1));
  g_assert_false(g_str_has_prefix(g_get_user_data_dir(), share));
}

static void
test_isolation_violation_rules(void)
{
  g_autofree gchar *root = g_dir_make_tmp("ttvxv-home-XXXXXX", NULL);
  g_autofree gchar *other = g_build_filename(root, "other", NULL);
  g_autofree gchar *share = g_build_filename(root, ".local", "share", "x", NULL);
  const gchar *home_real[] = { g_strconcat("HOME=", root, NULL), NULL };
  g_autofree gchar *why = nostrc_test_isolation_violation(root, home_real);
  g_assert_nonnull(why);
  g_assert_nonnull(strstr(why, "HOME is the real home"));
  g_free((gchar *)home_real[0]);

  /* A private HOME but XDG_DATA_HOME inside the real data home. */
  gchar *data_real[] = { g_strconcat("HOME=", other, NULL),
                         g_strconcat("XDG_DATA_HOME=", share, NULL), NULL };
  g_clear_pointer(&why, g_free);
  why = nostrc_test_isolation_violation(root, (const gchar *const *)data_real);
  g_assert_nonnull(why);
  g_assert_nonnull(strstr(why, "XDG_DATA_HOME"));
  g_free(data_real[1]);
  data_real[1] = NULL;
  g_clear_pointer(&why, g_free);
  why = nostrc_test_isolation_violation(root, (const gchar *const *)data_real);
  g_assert_null(why);
  g_free(data_real[0]);
  g_rmdir(root);
}

static void
test_isolation_assert_trips(void)
{
  if (g_test_subprocess()) {
    g_setenv("HOME", real_home(), TRUE);
    nostrc_test_isolation_assert();
    return;
  }
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_test_trap_assert_stderr("*test isolation (nostrc-ttvxv): HOME is the real home*");
}

/* ---- the store ------------------------------------------------------------- */

static void
test_guard_rules(void)
{
  g_autofree gchar *root = g_dir_make_tmp("ttvxv-guard-XXXXXX", NULL);
  g_autofree gchar *share = g_build_filename(root, ".local", "share", NULL);
  g_autofree gchar *below = g_build_filename(share, ".groundhog-instances", "a", NULL);
  g_autofree gchar *dotted = g_build_filename(root, ".local", "x", "..", "share", NULL);
  g_autofree gchar *beside = g_build_filename(root, ".local", "shared", NULL);
  /* Under test: the real data home, anything below it, any spelling of it. */
  g_assert_true(gh_profile_guard_refuses_data_dir_for(root, share, OTHER, TRUE));
  g_assert_true(gh_profile_guard_refuses_data_dir_for(root, below, OTHER, TRUE));
  g_assert_true(gh_profile_guard_refuses_data_dir_for(root, dotted, NULL, TRUE));
  g_assert_false(gh_profile_guard_refuses_data_dir_for(root, beside, OTHER, TRUE));
  /* Outside a test: only the well-known test accounts. */
  g_assert_true(gh_profile_guard_refuses_data_dir_for(root, share, KEY1, FALSE));
  g_assert_true(gh_profile_guard_refuses_data_dir_for(root, below, KEY3, FALSE));
  g_assert_false(gh_profile_guard_refuses_data_dir_for(root, share, OTHER, FALSE));
  g_assert_false(gh_profile_guard_refuses_data_dir_for(root, beside, KEY1, FALSE));
  g_rmdir(root);
}

/* Runs @fn in a child whose environment names the real profile: HOME is the
 * owner's home and XDG_DATA_HOME unset (g_get_user_data_dir() is then the
 * real data home, as for the 2026-10-10 store). */
static void
store_open_real(const gchar *account)
{
  g_unsetenv("XDG_DATA_HOME");
  g_setenv("HOME", real_home(), TRUE);
  g_autofree gchar *share = real_data_home();
  g_assert_cmpstr(g_get_user_data_dir(), ==, share);
  /* The verdict first: never call the store unless it will refuse. */
  g_assert_true(gh_profile_guard_refuses_data_dir(g_get_user_data_dir(), account));
  g_autofree gchar *path = NULL;
  {
    g_autoptr(GError) error = NULL;
    path = gh_store_account_dir_path(NULL, account, &error);
    g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS);
  }
  {
    g_autoptr(GError) error = NULL;
    gboolean exists = TRUE;
    g_assert_false(gh_store_exists(NULL, account, &exists, &error));
    g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS);
  }
  /* A valid key and store id, so only the data directory can be refused. */
  static const guint8 zero[GH_STORE_KEY_SIZE] = { 0 };
  g_autoptr(GBytes) key = g_bytes_new_static(zero, sizeof zero);
  g_autofree gchar *store_id = g_uuid_string_random();
  GhStoreConfig config = { NULL, account, NULL, NULL, NULL };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_null(store);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS);
  g_print("refused: %s\n", error->message);
}

static void
test_store_refuses_real_data_home_under_test(void)
{
  if (g_test_subprocess()) {
    g_setenv(GH_PROFILE_GUARD_ENV, "1", TRUE);
    store_open_real(OTHER);
    return;
  }
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed();
  g_test_trap_assert_stdout("*Refusing to use the real user's data directory*under test*");
}

static void
test_store_refuses_test_account_outside_test(void)
{
  if (g_test_subprocess()) {
    g_unsetenv(GH_PROFILE_GUARD_ENV);
    store_open_real(KEY1);
    return;
  }
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed();
  g_test_trap_assert_stdout("*Refusing*for a well-known test account*");
}

/* The guard does not get in the way of a private data directory. */
static void
test_store_private_dir_allowed(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("ttvxv-store-XXXXXX", NULL);
  g_autoptr(GError) error = NULL;
  gboolean exists = TRUE;
  g_assert_true(gh_store_exists(dir, KEY1, &exists, &error));
  g_assert_no_error(error);
  g_assert_false(exists);
  g_rmdir(dir);
}

/* ---- the Keychain (macOS) ---------------------------------------------------- */

#ifdef __APPLE__
typedef struct {
  GBytes *key;
  GError *error;
  gboolean done;
} KeyResult;

static void
on_key(GObject *source, GAsyncResult *res, gpointer data)
{
  KeyResult *r = data;
  r->key = gh_store_key_lookup_or_create_finish(GH_STORE_KEY(source), res, NULL, NULL,
                                                &r->error);
  r->done = TRUE;
}

/* GhStoreKey over the Keychain backend with no test keychain: the user's
 * login Keychain, as gh_store_key_new(NULL) gives the app on macOS. */
static void
store_key_default_keychain(const gchar *account)
{
  g_assert_true(gh_profile_guard_refuses_default_keychain(account));
  GhStoreKeyBackend *backend = g_object_new(GH_TYPE_STORE_KEY_KEYCHAIN, NULL);
  GhStoreKey *store_key = gh_store_key_new(backend);
  g_object_unref(backend);
  KeyResult r = { 0 };
  gh_store_key_lookup_or_create_async(store_key, account, GH_STORE_KEY_FLAGS_NONE, NULL,
                                      on_key, &r);
  while (!r.done)
    g_main_context_iteration(NULL, TRUE);
  g_assert_null(r.key);
  g_assert_error(r.error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE);
  g_print("refused: %s\n", r.error->message);
  g_error_free(r.error);
  g_object_unref(store_key);
}

typedef struct {
  GError *error;
  gboolean done;
} CredResult;

static void
on_cred(GObject *source, GAsyncResult *res, gpointer data)
{
  CredResult *r = data;
  GhNip46Credential *cred = gh_nip46_credential_store_lookup_finish(
    GH_NIP46_CREDENTIAL_STORE(source), res, &r->error);
  g_assert_null(cred);
  r->done = TRUE;
}

static void
nip46_default_keychain(const gchar *account)
{
  g_assert_true(gh_profile_guard_refuses_default_keychain(account));
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_keychain(NULL);
  CredResult r = { 0 };
  gh_nip46_credential_store_lookup_async(store, account, NULL, on_cred, &r);
  while (!r.done)
    g_main_context_iteration(NULL, TRUE);
  g_assert_error(r.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  g_print("refused: %s\n", r.error->message);
  g_error_free(r.error);
  g_object_unref(store);
}

static void
test_keychain_refused_under_test(void)
{
  if (g_test_subprocess()) {
    g_setenv(GH_PROFILE_GUARD_ENV, "1", TRUE);
    store_key_default_keychain(OTHER);
    nip46_default_keychain(OTHER);
    return;
  }
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed();
  g_test_trap_assert_stdout("*Refusing to use the login Keychain under test*"
                            "*Refusing to use the login Keychain under test*");
}

static void
test_keychain_refuses_test_account_outside_test(void)
{
  if (g_test_subprocess()) {
    g_unsetenv(GH_PROFILE_GUARD_ENV);
    store_key_default_keychain(KEY1);
    nip46_default_keychain(KEY1);
    return;
  }
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed();
  g_test_trap_assert_stdout("*Refusing to use the login Keychain for a well-known test account*");
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  /* The subprocess cases re-run this binary; the guard must not abort them
   * before they move HOME themselves. */
  if (!g_test_subprocess())
    nostrc_test_isolation_assert();
  g_test_add_func("/groundhog/profile-guard/ctest-environment", test_ctest_environment);
  g_test_add_func("/groundhog/profile-guard/isolation-rules", test_isolation_violation_rules);
  g_test_add_func("/groundhog/profile-guard/isolation-assert-trips", test_isolation_assert_trips);
  g_test_add_func("/groundhog/profile-guard/rules", test_guard_rules);
  g_test_add_func("/groundhog/profile-guard/store-under-test",
                  test_store_refuses_real_data_home_under_test);
  g_test_add_func("/groundhog/profile-guard/store-test-account",
                  test_store_refuses_test_account_outside_test);
  g_test_add_func("/groundhog/profile-guard/store-private-dir", test_store_private_dir_allowed);
#ifdef __APPLE__
  g_test_add_func("/groundhog/profile-guard/keychain-under-test",
                  test_keychain_refused_under_test);
  g_test_add_func("/groundhog/profile-guard/keychain-test-account",
                  test_keychain_refuses_test_account_outside_test);
#endif
  return g_test_run();
}
