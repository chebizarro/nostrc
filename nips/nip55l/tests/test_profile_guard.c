/* test_profile_guard — the signer's key store is off limits under test
 * (nostrc-wqn0u).
 *
 * In the environment CTest gives every nip55l test (NOSTRC_TEST_ISOLATION=1,
 * private HOME / XDG_*), with no private test bus declared, the real
 * signer_ops code
 *   - refuses StoreKey and ClearKey (PERMISSION_DENIED) instead of writing to
 *     the login Keychain or the session's Secret Service;
 *   - finds no stored identity (the store is not read), while an environment
 *     key still works;
 *   - on a declared test bus does not refuse (libsecret builds): StoreKey then
 *     fails only because nothing listens at that address. */
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip55l/error.h"
#include "nostr/nip55l/signer_ops.h"

#define TEST_NOSTRC_TEST_BUS_ENV "NOSTRC_TEST_BUS_ADDRESS"
/* BIP-340 test vector 0: secret key 3, a well-known test key. */
#define TEST_SK_HEX "0000000000000000000000000000000000000000000000000000000000000003"

static void
no_private_bus(void)
{
  g_unsetenv(TEST_NOSTRC_TEST_BUS_ENV);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  g_unsetenv("NOSTR_SIGNER_NSEC");
}

static void
test_writes_refused(void)
{
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  no_private_bus();
#ifdef NIP55L_TEST_HAVE_STORE
  g_assert_cmpint(nostr_nip55l_store_key(TEST_SK_HEX, NULL), ==,
                  NOSTR_SIGNER_ERROR_PERMISSION_DENIED);
  g_assert_cmpint(nostr_nip55l_clear_key("profile-guard-test"), ==,
                  NOSTR_SIGNER_ERROR_PERMISSION_DENIED);
#else
  /* No key store compiled in: nothing to refuse, nothing stored. */
  g_assert_cmpint(nostr_nip55l_store_key(TEST_SK_HEX, NULL), ==, NOSTR_SIGNER_ERROR_NOT_FOUND);
#endif
}

static void
test_reads_see_no_store(void)
{
  no_private_bus();
  char *npub = NULL;
  g_assert_cmpint(nostr_nip55l_get_public_key(&npub), ==, NOSTR_SIGNER_ERROR_NOT_FOUND);
  g_assert_null(npub);
  char **npubs = NULL;
  int count = -1;
  int rc = nostr_nip55l_list_identities(&npubs, &count);
  g_assert_true(rc == 0 || rc == NOSTR_SIGNER_ERROR_NOT_FOUND);
  g_assert_cmpint(rc == 0 ? count : 0, ==, 0);
  free(npubs);

  /* The environment key is not the owner's store: it still signs. */
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", TEST_SK_HEX, TRUE);
  g_assert_cmpint(nostr_nip55l_get_public_key(&npub), ==, 0);
  g_assert_true(npub && strncmp(npub, "npub1", 5) == 0);
  free(npub);
  npubs = NULL;
  g_assert_cmpint(nostr_nip55l_list_identities(&npubs, &count), ==, 0);
  g_assert_cmpint(count, ==, 1);
  free(npubs[0]);
  free(npubs);
  g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
}

static void
test_private_bus_not_refused(void)
{
#ifdef NIP55L_TEST_HAVE_LIBSECRET_STORE
  no_private_bus();
  g_setenv(TEST_NOSTRC_TEST_BUS_ENV, "unix:path=/nonexistent-nostrc-bus", TRUE);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*StoreKey*");
  int rc = nostr_nip55l_store_key(TEST_SK_HEX, NULL);
  g_test_assert_expected_messages();
  g_assert_cmpint(rc, ==, NOSTR_SIGNER_ERROR_BACKEND);
  g_unsetenv(TEST_NOSTRC_TEST_BUS_ENV);
#else
  /* The Keychain has no private variant: under test it is always refused. */
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
#endif
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip55l/profile-guard/writes-refused", test_writes_refused);
  g_test_add_func("/nip55l/profile-guard/reads-see-no-store", test_reads_see_no_store);
  g_test_add_func("/nip55l/profile-guard/private-bus-not-refused", test_private_bus_not_refused);
  return g_test_run();
}
