/**
 * Keystore Unit Tests
 *
 * Tests for secure key storage functionality.
 * Note: These tests require a working keyring/keychain to pass.
 */

#include <glib.h>
#include <string.h>
#include "../src/util/keystore.h"

/* Test npub/nsec pair for testing (these are well-known test keys from NIP-19 test vectors) */
/* Private key: 67dea2ed018072d675f5415ecfaed7d2597555e202d85b3d65ea4e58d2d92ffa */
/* Public key: 7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e */
#define TEST_NPUB "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"
#define TEST_NSEC "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5"
#define TEST_LABEL "Test Key"

static void test_keystore_available(void) {
  gboolean available = gnostr_keystore_available();
  /* This test may pass or fail depending on platform */
  g_test_message("Keystore available: %s", available ? "yes" : "no");
#if defined(HAVE_LIBSECRET) || defined(HAVE_MACOS_KEYCHAIN)
  if (!available) {
    g_test_skip("Keystore backend compiled in but not available at runtime");
    return;
  }
  g_assert_true(available);
#else
  g_assert_false(available);
#endif
}

static void test_keystore_store_invalid_npub(void) {
  GError *error = NULL;

  /* Test with invalid npub */
  gboolean result = gnostr_keystore_store_key("invalid", TEST_NSEC, NULL, &error);
  g_assert_false(result);
  g_assert_nonnull(error);
  g_assert_cmpint(error->code, ==, GNOSTR_KEYSTORE_ERROR_INVALID_KEY);
  g_clear_error(&error);

  /* Test with NULL npub */
  result = gnostr_keystore_store_key(NULL, TEST_NSEC, NULL, &error);
  g_assert_false(result);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

static void test_keystore_store_invalid_nsec(void) {
  GError *error = NULL;

  /* Test with invalid nsec */
  gboolean result = gnostr_keystore_store_key(TEST_NPUB, "invalid", NULL, &error);
  g_assert_false(result);
  g_assert_nonnull(error);
  g_assert_cmpint(error->code, ==, GNOSTR_KEYSTORE_ERROR_INVALID_KEY);
  g_clear_error(&error);

  /* Test with NULL nsec */
  result = gnostr_keystore_store_key(TEST_NPUB, NULL, NULL, &error);
  g_assert_false(result);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

static void test_keystore_retrieve_not_found(void) {
  if (!gnostr_keystore_available()) {
    g_test_skip("Keystore not available");
    return;
  }

  GError *error = NULL;

  /* Try to retrieve a key that doesn't exist */
  char *nsec = gnostr_keystore_retrieve_key(
      "npub1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", &error);

  g_assert_null(nsec);
  g_assert_nonnull(error);
  g_assert_cmpint(error->code, ==, GNOSTR_KEYSTORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
}

static void test_keystore_roundtrip(void) {
  if (!gnostr_keystore_available()) {
    g_test_skip("Keystore not available");
    return;
  }

  GError *error = NULL;

  /* Store a key */
  gboolean stored = gnostr_keystore_store_key(TEST_NPUB, TEST_NSEC, TEST_LABEL, &error);
  if (!stored) {
    g_test_message("Store failed: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    g_test_skip("Could not store key (keyring locked?)");
    return;
  }

  /* Verify it exists */
  g_assert_true(gnostr_keystore_has_key(TEST_NPUB));

  /* Retrieve the key */
  char *retrieved = gnostr_keystore_retrieve_key(TEST_NPUB, &error);
  g_assert_no_error(error);
  g_assert_nonnull(retrieved);
  g_assert_cmpstr(retrieved, ==, TEST_NSEC);

  /* Securely clear and free */
  memset(retrieved, 0, strlen(retrieved));
  g_free(retrieved);

  /* Delete the key */
  gboolean deleted = gnostr_keystore_delete_key(TEST_NPUB, &error);
  g_assert_no_error(error);
  g_assert_true(deleted);

  /* Verify it's gone */
  g_assert_false(gnostr_keystore_has_key(TEST_NPUB));
}

static void test_keystore_list_keys(void) {
  if (!gnostr_keystore_available()) {
    g_test_skip("Keystore not available");
    return;
  }

  GError *error = NULL;

  /* Store a test key */
  gboolean stored = gnostr_keystore_store_key(TEST_NPUB, TEST_NSEC, TEST_LABEL, &error);
  if (!stored) {
    g_clear_error(&error);
    g_test_skip("Could not store key");
    return;
  }

  /* List all keys */
  GList *keys = gnostr_keystore_list_keys(&error);
  g_assert_no_error(error);

  /* Find our test key */
  gboolean found = FALSE;
  for (GList *l = keys; l != NULL; l = l->next) {
    GnostrKeyInfo *info = (GnostrKeyInfo *)l->data;
    if (g_strcmp0(info->npub, TEST_NPUB) == 0) {
      found = TRUE;
      g_assert_cmpstr(info->label, !=, NULL);
      break;
    }
  }
  g_assert_true(found);

  g_list_free_full(keys, (GDestroyNotify)gnostr_key_info_free);

  /* Cleanup */
  gnostr_keystore_delete_key(TEST_NPUB, NULL);
}

static void test_key_info_copy(void) {
  GnostrKeyInfo original = {
    .npub = g_strdup(TEST_NPUB),
    .label = g_strdup(TEST_LABEL),
    .created_at = 1234567890
  };

  GnostrKeyInfo *copy = gnostr_key_info_copy(&original);
  g_assert_nonnull(copy);
  g_assert_cmpstr(copy->npub, ==, original.npub);
  g_assert_cmpstr(copy->label, ==, original.label);
  g_assert_cmpint(copy->created_at, ==, original.created_at);

  /* Verify they're different pointers */
  g_assert_true(copy->npub != original.npub);
  g_assert_true(copy->label != original.label);

  g_free(original.npub);
  g_free(original.label);
  gnostr_key_info_free(copy);
}

static void test_key_info_copy_null(void) {
  GnostrKeyInfo *copy = gnostr_key_info_copy(NULL);
  g_assert_null(copy);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/keystore/available", test_keystore_available);
  g_test_add_func("/keystore/store_invalid_npub", test_keystore_store_invalid_npub);
  g_test_add_func("/keystore/store_invalid_nsec", test_keystore_store_invalid_nsec);
  g_test_add_func("/keystore/retrieve_not_found", test_keystore_retrieve_not_found);
  g_test_add_func("/keystore/roundtrip", test_keystore_roundtrip);
  g_test_add_func("/keystore/list_keys", test_keystore_list_keys);
  g_test_add_func("/keystore/key_info_copy", test_key_info_copy);
  g_test_add_func("/keystore/key_info_copy_null", test_key_info_copy_null);

  return g_test_run();
}
