/* test_hsm_provider.c - Unit tests for HSM provider functionality
 *
 * Tests the GnHsmProvider interface and mock provider implementation.
 *
 * SPDX-License-Identifier: MIT
 */
#include <glib.h>
#include "../src/hsm_provider.h"
#include "../src/hsm_provider_mock.h"

/* Test fixture */
typedef struct {
  GnHsmProviderMock *mock;
  GnHsmManager *manager;
} HsmFixture;

static void
hsm_fixture_setup(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  fixture->mock = gn_hsm_provider_mock_new();
  g_assert_nonnull(fixture->mock);

  fixture->manager = gn_hsm_manager_get_default();
  g_assert_nonnull(fixture->manager);

  gn_hsm_manager_register_provider(fixture->manager, GN_HSM_PROVIDER(fixture->mock));
}

static void
hsm_fixture_teardown(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  if (fixture->mock) {
    gn_hsm_manager_unregister_provider(fixture->manager, GN_HSM_PROVIDER(fixture->mock));
    g_object_unref(fixture->mock);
  }
}

/* Test mock provider initialization */
static void
test_mock_provider_init(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  g_assert_cmpstr(gn_hsm_provider_get_name(GN_HSM_PROVIDER(fixture->mock)), ==, "Mock HSM");
  g_assert_true(gn_hsm_provider_is_available(GN_HSM_PROVIDER(fixture->mock)));

  GError *error = NULL;
  gboolean result = gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_true(result);
  g_assert_no_error(error);
}

/* Test device detection */
static void
test_mock_detect_devices(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Initialize provider */
  GError *error = NULL;
  gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);

  /* Add a simulated device */
  gn_hsm_provider_mock_add_device(fixture->mock, 1, "Test Token", FALSE);

  /* Detect devices */
  GPtrArray *devices = gn_hsm_provider_detect_devices(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);
  g_assert_nonnull(devices);
  g_assert_cmpuint(devices->len, ==, 1);

  GnHsmDeviceInfo *info = g_ptr_array_index(devices, 0);
  g_assert_cmpuint(info->slot_id, ==, 1);
  g_assert_cmpstr(info->label, ==, "Test Token");
  g_assert_false(info->needs_pin);

  g_ptr_array_unref(devices);
}

/* Test key generation */
static void
test_mock_generate_key(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Initialize and add device */
  GError *error = NULL;
  gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);

  gn_hsm_provider_mock_add_device(fixture->mock, 1, "Test Token", FALSE);

  /* Generate a key */
  GnHsmKeyInfo *key = gn_hsm_provider_generate_key(GN_HSM_PROVIDER(fixture->mock),
                                                    1, "My Nostr Key",
                                                    GN_HSM_KEY_TYPE_SECP256K1,
                                                    &error);
  g_assert_no_error(error);
  g_assert_nonnull(key);
  g_assert_nonnull(key->key_id);
  g_assert_cmpstr(key->label, ==, "My Nostr Key");
  g_assert_nonnull(key->npub);
  g_assert_true(g_str_has_prefix(key->npub, "npub1"));
  g_assert_nonnull(key->pubkey_hex);
  g_assert_cmpuint(strlen(key->pubkey_hex), ==, 64);

  gn_hsm_key_info_free(key);
}

/* Test key listing */
static void
test_mock_list_keys(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Initialize and add device */
  GError *error = NULL;
  gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);

  gn_hsm_provider_mock_add_device(fixture->mock, 1, "Test Token", FALSE);

  /* Generate two keys */
  GnHsmKeyInfo *key1 = gn_hsm_provider_generate_key(GN_HSM_PROVIDER(fixture->mock),
                                                     1, "Key 1",
                                                     GN_HSM_KEY_TYPE_SECP256K1,
                                                     &error);
  g_assert_no_error(error);
  gn_hsm_key_info_free(key1);

  GnHsmKeyInfo *key2 = gn_hsm_provider_generate_key(GN_HSM_PROVIDER(fixture->mock),
                                                     1, "Key 2",
                                                     GN_HSM_KEY_TYPE_SECP256K1,
                                                     &error);
  g_assert_no_error(error);
  gn_hsm_key_info_free(key2);

  /* List keys */
  GPtrArray *keys = gn_hsm_provider_list_keys(GN_HSM_PROVIDER(fixture->mock), 1, &error);
  g_assert_no_error(error);
  g_assert_nonnull(keys);
  g_assert_cmpuint(keys->len, ==, 2);

  g_ptr_array_unref(keys);
}

/* Test PIN authentication */
static void
test_mock_pin_authentication(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Initialize and add device with PIN */
  GError *error = NULL;
  gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);

  gn_hsm_provider_mock_add_device(fixture->mock, 1, "Secure Token", TRUE);
  gn_hsm_provider_mock_set_pin(fixture->mock, 1, "1234");

  /* Try to list keys without login - should fail */
  GPtrArray *keys = gn_hsm_provider_list_keys(GN_HSM_PROVIDER(fixture->mock), 1, &error);
  g_assert_null(keys);
  g_assert_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED);
  g_clear_error(&error);

  /* Login with wrong PIN - should fail */
  gboolean logged_in = gn_hsm_provider_login(GN_HSM_PROVIDER(fixture->mock), 1, "wrong", &error);
  g_assert_false(logged_in);
  g_assert_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_INCORRECT);
  g_clear_error(&error);

  /* Login with correct PIN - should succeed */
  logged_in = gn_hsm_provider_login(GN_HSM_PROVIDER(fixture->mock), 1, "1234", &error);
  g_assert_true(logged_in);
  g_assert_no_error(error);

  /* Now listing keys should work */
  keys = gn_hsm_provider_list_keys(GN_HSM_PROVIDER(fixture->mock), 1, &error);
  g_assert_no_error(error);
  g_assert_nonnull(keys);
  g_ptr_array_unref(keys);

  /* Logout */
  gn_hsm_provider_logout(GN_HSM_PROVIDER(fixture->mock), 1);
}

/* Test simulated errors */
static void
test_mock_simulated_error(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Initialize */
  GError *error = NULL;
  gn_hsm_provider_init(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);

  /* Simulate an error */
  gn_hsm_provider_mock_simulate_error(fixture->mock, GN_HSM_ERROR_DEVICE_ERROR);

  /* Next operation should fail */
  GPtrArray *devices = gn_hsm_provider_detect_devices(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_null(devices);
  g_assert_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR);
  g_clear_error(&error);

  /* Subsequent operations should work */
  gn_hsm_provider_mock_add_device(fixture->mock, 1, "Test", FALSE);
  devices = gn_hsm_provider_detect_devices(GN_HSM_PROVIDER(fixture->mock), &error);
  g_assert_no_error(error);
  g_assert_nonnull(devices);
  g_ptr_array_unref(devices);
}

/* Test manager provider registration */
static void
test_manager_providers(HsmFixture *fixture, gconstpointer user_data)
{
  (void)user_data;

  /* Check provider is registered */
  GList *providers = gn_hsm_manager_get_providers(fixture->manager);
  g_assert_nonnull(providers);
  g_assert_nonnull(g_list_find(providers, fixture->mock));

  /* Find by name */
  GnHsmProvider *found = gn_hsm_manager_get_provider_by_name(fixture->manager, "Mock HSM");
  g_assert_true(found == GN_HSM_PROVIDER(fixture->mock));

  /* Get available providers */
  GList *available = gn_hsm_manager_get_available_providers(fixture->manager);
  g_assert_nonnull(available);
  g_list_free(available);
}

/* Test device info copy/free */
static void
test_device_info_copy(void)
{
  GnHsmDeviceInfo info = {
    .slot_id = 42,
    .label = g_strdup("Test Label"),
    .manufacturer = g_strdup("Test Mfg"),
    .model = g_strdup("Test Model"),
    .serial = g_strdup("12345"),
    .flags = 0x1234,
    .is_token_present = TRUE,
    .is_initialized = TRUE,
    .needs_pin = FALSE
  };

  GnHsmDeviceInfo *copy = gn_hsm_device_info_copy(&info);
  g_assert_nonnull(copy);
  g_assert_cmpuint(copy->slot_id, ==, 42);
  g_assert_cmpstr(copy->label, ==, "Test Label");
  g_assert_cmpstr(copy->manufacturer, ==, "Test Mfg");
  g_assert_cmpstr(copy->serial, ==, "12345");
  g_assert_true(copy->is_token_present);

  /* Free both */
  g_free(info.label);
  g_free(info.manufacturer);
  g_free(info.model);
  g_free(info.serial);
  gn_hsm_device_info_free(copy);
}

/* Test key info copy/free */
static void
test_key_info_copy(void)
{
  GnHsmKeyInfo info = {
    .key_id = g_strdup("key123"),
    .label = g_strdup("My Key"),
    .npub = g_strdup("npub1abc..."),
    .pubkey_hex = g_strdup("abcd1234..."),
    .key_type = GN_HSM_KEY_TYPE_SECP256K1,
    .created_at = g_strdup("2024-01-01T00:00:00Z"),
    .slot_id = 1,
    .can_sign = TRUE,
    .is_extractable = FALSE
  };

  GnHsmKeyInfo *copy = gn_hsm_key_info_copy(&info);
  g_assert_nonnull(copy);
  g_assert_cmpstr(copy->key_id, ==, "key123");
  g_assert_cmpstr(copy->label, ==, "My Key");
  g_assert_cmpstr(copy->npub, ==, "npub1abc...");
  g_assert_cmpint(copy->key_type, ==, GN_HSM_KEY_TYPE_SECP256K1);
  g_assert_true(copy->can_sign);

  /* Free both */
  g_free(info.key_id);
  g_free(info.label);
  g_free(info.npub);
  g_free(info.pubkey_hex);
  g_free(info.created_at);
  gn_hsm_key_info_free(copy);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  /* Info structure tests (no fixture needed) */
  g_test_add_func("/hsm/device_info/copy", test_device_info_copy);
  g_test_add_func("/hsm/key_info/copy", test_key_info_copy);

  /* Mock provider tests */
  g_test_add("/hsm/mock/init", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_provider_init, hsm_fixture_teardown);
  g_test_add("/hsm/mock/detect_devices", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_detect_devices, hsm_fixture_teardown);
  g_test_add("/hsm/mock/generate_key", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_generate_key, hsm_fixture_teardown);
  g_test_add("/hsm/mock/list_keys", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_list_keys, hsm_fixture_teardown);
  g_test_add("/hsm/mock/pin_authentication", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_pin_authentication, hsm_fixture_teardown);
  g_test_add("/hsm/mock/simulated_error", HsmFixture, NULL,
             hsm_fixture_setup, test_mock_simulated_error, hsm_fixture_teardown);

  /* Manager tests */
  g_test_add("/hsm/manager/providers", HsmFixture, NULL,
             hsm_fixture_setup, test_manager_providers, hsm_fixture_teardown);

  return g_test_run();
}
