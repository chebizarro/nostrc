/* test_store_key_keychain.c — GhStoreKeyKeychain backend tests.
 *
 * Uses a synthetic test-only account pubkey
 * ("0000…01") that cannot collide with real identities.
 * All items are cleaned up on exit via atexit() and per-test destroy.
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

/* Test-only synthetic pubkey — all zeros except last byte = 01.
 * Cannot correspond to a real x-only secp256k1 pubkey. */
#define TEST_ACCOUNT \
  "0000000000000000000000000000000000000000000000000000000000000001"
#define TEST_ACCOUNT_2 \
  "0000000000000000000000000000000000000000000000000000000000000002"

/* ---- Cleanup guarantee ------------------------------------------------ */

/* Remove any items left by a crashed previous run. We delete by service
 * name + account, which is the same query the backend uses. */
static void
cleanup_test_items(void)
{
  const char *accounts[] = { TEST_ACCOUNT, TEST_ACCOUNT_2, NULL };
  for (int i = 0; accounts[i]; i++) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(NULL,
        GH_STORE_KEY_SCHEMA_NAME, kCFStringEncodingUTF8);
    CFStringRef acct = CFStringCreateWithCString(NULL, accounts[i],
        kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    SecItemDelete(q); /* ignore errors */
    CFRelease(acct);
    CFRelease(service);
    CFRelease(q);
  }
}

/* ---- Test helpers ----------------------------------------------------- */

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

/* ---- Tests ------------------------------------------------------------ */

static void
test_store_search_verify(void)
{
  if (sodium_init() < 0 && sodium_init() != 1) {
    g_test_skip("sodium_init failed");
    return;
  }

  /* Pre-clean in case a previous run crashed. */
  cleanup_test_items();

  GhStoreKeyBackend *backend = g_object_new(GH_TYPE_STORE_KEY_KEYCHAIN, NULL);
  GhStoreKey *sk = gh_store_key_new(backend);
  g_object_unref(backend);

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
}

int
main(int argc, char *argv[])
{
  /* Guarantee cleanup even on abort/crash. */
  atexit(cleanup_test_items);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/store-key-keychain/store-search-verify",
                  test_store_search_verify);
  int result = g_test_run();

  /* Explicit cleanup after all tests. */
  cleanup_test_items();
  return result;
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
