/* test_store_key_keychain.c — GhStoreKeyKeychain backend tests.
 *
 * All operations are scoped to a temporary file-based keychain created
 * with SecKeychainCreate in a mkdtemp directory.  The login keychain is
 * never touched.
 *
 * Tests:
 *   KC-MAC-1: store + search + verify secret
 *   KC-MAC-2: clear + verify gone
 *   KC-MAC-3: search with no items returns empty array
 */

#ifdef __APPLE__

#include "gh-store-key.h"
#include "gh-store-key-keychain.h"
#include <Security/Security.h>
#include <sodium.h>
#include <glib.h>
#include <unistd.h>

/* ---- Temporary keychain management -------------------------------------- */

static SecKeychainRef temp_keychain = NULL;
static char           temp_dir[256] = { 0 };
static char           temp_kc_path[512] = { 0 };

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Create a temporary file-based keychain. */
static gboolean
create_temp_keychain(void)
{
  /* mkdtemp for the keychain file. */
  g_snprintf(temp_dir, sizeof(temp_dir), "%s/kc_test_XXXXXX",
             g_get_tmp_dir());
  if (!mkdtemp(temp_dir)) {
    g_printerr("mkdtemp failed: %s\n", g_strerror(errno));
    return FALSE;
  }

  g_snprintf(temp_kc_path, sizeof(temp_kc_path), "%s/test.keychain", temp_dir);

  OSStatus st = SecKeychainCreate(temp_kc_path, /* pathName */
                                  4, "test",    /* password */
                                  FALSE,        /* promptUser */
                                  NULL,         /* initialAccess */
                                  &temp_keychain);
  if (st != errSecSuccess) {
    g_printerr("SecKeychainCreate failed: %d\n", (int)st);
    return FALSE;
  }

  /* Unlock the temporary keychain so SecItemAdd doesn't prompt. */
  st = SecKeychainUnlock(temp_keychain, 4, "test", TRUE);
  if (st != errSecSuccess)
    g_printerr("SecKeychainUnlock warning: %d\n", (int)st);

  return TRUE;
}

/* Remove the temporary keychain and its directory. */
static void
destroy_temp_keychain(void)
{
  if (temp_keychain) {
    SecKeychainDelete(temp_keychain);
    CFRelease(temp_keychain);
    temp_keychain = NULL;
  }
  if (temp_kc_path[0]) {
    unlink(temp_kc_path);
    /* macOS may also create <path>-db */
    char dbpath[600];
    g_snprintf(dbpath, sizeof(dbpath), "%s-db", temp_kc_path);
    unlink(dbpath);
    temp_kc_path[0] = '\0';
  }
  if (temp_dir[0]) {
    rmdir(temp_dir);
    temp_dir[0] = '\0';
  }
}

#pragma clang diagnostic pop

/* ---- Test helpers ------------------------------------------------------- */

static GMainLoop *loop;

typedef struct {
  GBytes *key;
  gchar *store_id;
  gboolean created;
  GError *error;
} LookupResult;

static void
on_lookup_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  LookupResult *r = user_data;
  GhStoreKey *sk = GH_STORE_KEY(source);
  r->key = gh_store_key_lookup_or_create_finish(sk, res, &r->store_id,
                                                 &r->created, &r->error);
  g_main_loop_quit(loop);
}

static void
on_destroy_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GhStoreKey *sk = GH_STORE_KEY(source);
  GError **err = user_data;
  gh_store_key_destroy_finish(sk, res, err);
  g_main_loop_quit(loop);
}

static void
on_lookup_only_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  LookupResult *r = user_data;
  GhStoreKey *sk = GH_STORE_KEY(source);
  r->key = gh_store_key_lookup_finish(sk, res, &r->store_id, &r->error);
  g_main_loop_quit(loop);
}

/* ---- Tests -------------------------------------------------------------- */

#define TEST_ACCOUNT \
  "0000000000000000000000000000000000000000000000000000000000000001"
#define TEST_ACCOUNT_2 \
  "0000000000000000000000000000000000000000000000000000000000000002"

static void
test_store_search_verify(void)
{
  if (sodium_init() < 0 && sodium_init() != 1) {
    g_test_skip("sodium_init failed");
    return;
  }

  if (!create_temp_keychain()) {
    g_test_skip("could not create temporary keychain");
    return;
  }

  /* Create backend and inject our temporary keychain. */
  GhStoreKeyKeychain *kc_backend = g_object_new(GH_TYPE_STORE_KEY_KEYCHAIN, NULL);
  gh_store_key_keychain_set_keychain(kc_backend, temp_keychain);

  GhStoreKey *sk = gh_store_key_new(GH_STORE_KEY_BACKEND(kc_backend));
  g_object_unref(kc_backend);

  /* KC-MAC-1: lookup_or_create stores a new key and returns it. */
  LookupResult r1 = { 0 };
  loop = g_main_loop_new(NULL, FALSE);
  gh_store_key_lookup_or_create_async(sk, TEST_ACCOUNT,
      GH_STORE_KEY_FLAGS_NONE, NULL, on_lookup_done, &r1);
  g_main_loop_run(loop);
  g_assert_no_error(r1.error);
  g_assert_nonnull(r1.key);
  g_assert_true(r1.created);
  g_assert_nonnull(r1.store_id);
  g_assert_cmpuint(g_bytes_get_size(r1.key), ==, GH_STORE_KEY_SIZE);

  /* A second lookup returns the same key (KC-MAC-1 continued). */
  LookupResult r2 = { 0 };
  gh_store_key_lookup_async(sk, TEST_ACCOUNT,
      GH_STORE_KEY_FLAGS_NONE, NULL, on_lookup_only_done, &r2);
  g_main_loop_run(loop);
  g_assert_no_error(r2.error);
  g_assert_nonnull(r2.key);
  g_assert_true(g_bytes_equal(r1.key, r2.key));
  g_assert_cmpstr(r1.store_id, ==, r2.store_id);

  /* KC-MAC-2: destroy clears the key. */
  GError *derr = NULL;
  gh_store_key_destroy_async(sk, TEST_ACCOUNT,
      GH_STORE_KEY_FLAGS_NONE, NULL, on_destroy_done, &derr);
  g_main_loop_run(loop);
  g_assert_no_error(derr);

  /* After destroy, lookup returns NOT_FOUND. */
  LookupResult r3 = { 0 };
  gh_store_key_lookup_async(sk, TEST_ACCOUNT,
      GH_STORE_KEY_FLAGS_NONE, NULL, on_lookup_only_done, &r3);
  g_main_loop_run(loop);
  g_assert_null(r3.key);
  g_assert_error(r3.error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NOT_FOUND);

  /* KC-MAC-3: search with no items returns NOT_FOUND. */
  LookupResult r4 = { 0 };
  gh_store_key_lookup_async(sk, TEST_ACCOUNT_2,
      GH_STORE_KEY_FLAGS_NONE, NULL, on_lookup_only_done, &r4);
  g_main_loop_run(loop);
  g_assert_null(r4.key);
  g_assert_error(r4.error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NOT_FOUND);

  /* Cleanup */
  g_bytes_unref(r1.key);
  g_free(r1.store_id);
  g_bytes_unref(r2.key);
  g_free(r2.store_id);
  g_clear_error(&r3.error);
  g_clear_error(&r4.error);
  g_main_loop_unref(loop);
  g_object_unref(sk);

  destroy_temp_keychain();
}

/* KC-MAC-4: when the Keychain is unavailable (errSecNotAvailable / -60006,
 * e.g. no default keychain in a headless gate environment), every operation
 * must surface GH_STORE_KEY_ERROR_UNAVAILABLE, not a generic FAILED.
 *
 * This cannot be tested reliably from a GUI session (the login keychain is
 * always available); the mapping is exercised by the
 * /store-key-libsecret/kc4-no-session-bus and /store-key/kc4/bus-without-
 * secret-service tests in test_store_key.c / test_store_key_keyring.c, which
 * run in the pre-push gate's hermetic (no-GUI) environment. */

int
main(int argc, char *argv[])
{
  /* Safety net: clean up temp keychain even on abort. */
  atexit(destroy_temp_keychain);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/store-key-keychain/store-search-verify",
                  test_store_search_verify);
  return g_test_run();
}

#else /* !__APPLE__ */

#include <glib.h>

int
main(int argc, char *argv[])
{
  (void)argc; (void)argv;
  g_print("SKIP: Keychain tests only run on macOS\n");
  return 0;
}

#endif /* __APPLE__ */
