/* hsm_provider_mock.c - Mock HSM provider implementation
 *
 * Software-based mock HSM for testing without hardware.
 * Uses libsecp256k1 for actual cryptographic operations.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hsm_provider_mock.h"
#include <string.h>
#include <time.h>

/* Include libnostr for crypto operations */
#include <nostr_nip19.h>
#include <nostr_keys.h>
#include <keys.h>       /* nostr_key_generate_private() - no GObject equivalent */
#include <nostr-event.h>

/* Mock device state */
typedef struct {
  guint64 slot_id;
  gchar *label;
  gchar *pin;
  gboolean needs_pin;
  gboolean is_logged_in;
  GHashTable *keys; /* key_id -> MockKey */
} MockDevice;

/* Mock key state */
typedef struct {
  gchar *key_id;
  gchar *label;
  guint8 private_key[32];
  guint8 public_key[32];
  gchar *npub;
  gchar *pubkey_hex;
  GnHsmKeyType key_type;
  gint64 created_at;
} MockKey;

struct _GnHsmProviderMock {
  GObject parent_instance;

  gboolean initialized;
  GHashTable *devices; /* slot_id -> MockDevice */
  GnHsmError simulated_error;
  gboolean has_simulated_error;
  guint operation_count;
  GMutex lock;
};

static void gn_hsm_provider_mock_iface_init(GnHsmProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnHsmProviderMock, gn_hsm_provider_mock, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_HSM_PROVIDER,
                                              gn_hsm_provider_mock_iface_init))

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void
mock_key_free(MockKey *key)
{
  if (!key)
    return;
  g_free(key->key_id);
  g_free(key->label);
  g_free(key->npub);
  g_free(key->pubkey_hex);
  /* Securely clear private key */
  memset(key->private_key, 0, sizeof(key->private_key));
  g_free(key);
}

static void
mock_device_free(MockDevice *dev)
{
  if (!dev)
    return;
  g_free(dev->label);
  g_free(dev->pin);
  if (dev->keys)
    g_hash_table_unref(dev->keys);
  g_free(dev);
}

static gchar *
generate_key_id(void)
{
  guint8 random_bytes[8];
  for (int i = 0; i < 8; i++) {
    random_bytes[i] = g_random_int_range(0, 256);
  }
  return g_base64_encode(random_bytes, 8);
}

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
  static const gchar hex_chars[] = "0123456789abcdef";
  gchar *result = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    result[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
    result[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
  }
  result[len * 2] = '\0';
  return result;
}

static gboolean
hex_to_bytes(const gchar *hex, guint8 *out, gsize expected_len)
{
  gsize hex_len = strlen(hex);
  if (hex_len != expected_len * 2)
    return FALSE;

  for (gsize i = 0; i < expected_len; i++) {
    gchar high = hex[i * 2];
    gchar low = hex[i * 2 + 1];
    guint8 val = 0;

    if (high >= '0' && high <= '9')
      val = (high - '0') << 4;
    else if (high >= 'a' && high <= 'f')
      val = (high - 'a' + 10) << 4;
    else if (high >= 'A' && high <= 'F')
      val = (high - 'A' + 10) << 4;
    else
      return FALSE;

    if (low >= '0' && low <= '9')
      val |= low - '0';
    else if (low >= 'a' && low <= 'f')
      val |= low - 'a' + 10;
    else if (low >= 'A' && low <= 'F')
      val |= low - 'A' + 10;
    else
      return FALSE;

    out[i] = val;
  }
  return TRUE;
}

/* ============================================================================
 * Provider Interface Implementation
 * ============================================================================ */

static const gchar *
mock_get_name(GnHsmProvider *provider)
{
  (void)provider;
  return "Mock HSM";
}

static gboolean
mock_is_available(GnHsmProvider *provider)
{
  (void)provider;
  /* Mock is always available */
  return TRUE;
}

static gboolean
mock_init_provider(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);
  (void)error;

  g_mutex_lock(&self->lock);
  self->initialized = TRUE;
  g_mutex_unlock(&self->lock);

  g_message("Mock HSM provider initialized");
  return TRUE;
}

static void
mock_shutdown_provider(GnHsmProvider *provider)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->initialized = FALSE;
  g_mutex_unlock(&self->lock);

  g_message("Mock HSM provider shut down");
}

static GPtrArray *
mock_detect_devices(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in detect_devices");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GPtrArray *devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hsm_device_info_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->devices);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MockDevice *dev = (MockDevice *)value;
    GnHsmDeviceInfo *info = g_new0(GnHsmDeviceInfo, 1);

    info->slot_id = dev->slot_id;
    info->label = g_strdup(dev->label);
    info->manufacturer = g_strdup("Mock Manufacturer");
    info->model = g_strdup("Mock HSM v1.0");
    info->serial = g_strdup_printf("MOCK%04" G_GUINT64_FORMAT, dev->slot_id);
    info->flags = 0;
    info->is_token_present = TRUE;
    info->is_initialized = TRUE;
    info->needs_pin = dev->needs_pin;

    g_ptr_array_add(devices, info);
  }

  g_mutex_unlock(&self->lock);
  return devices;
}

static GPtrArray *
mock_list_keys(GnHsmProvider *provider, guint64 slot_id, GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in list_keys");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GPtrArray *keys = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hsm_key_info_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, dev->keys);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MockKey *mkey = (MockKey *)value;
    GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);

    info->key_id = g_strdup(mkey->key_id);
    info->label = g_strdup(mkey->label);
    info->npub = g_strdup(mkey->npub);
    info->pubkey_hex = g_strdup(mkey->pubkey_hex);
    info->key_type = mkey->key_type;
    info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, mkey->created_at);
    info->slot_id = slot_id;
    info->can_sign = TRUE;
    info->is_extractable = FALSE;

    g_ptr_array_add(keys, info);
  }

  g_mutex_unlock(&self->lock);
  return keys;
}

static GnHsmKeyInfo *
mock_get_public_key(GnHsmProvider *provider,
                    guint64 slot_id,
                    const gchar *key_id,
                    GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in get_public_key");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockKey *mkey = g_hash_table_lookup(dev->keys, key_id);
  if (!mkey) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Key '%s' not found in slot %" G_GUINT64_FORMAT, key_id, slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
  info->key_id = g_strdup(mkey->key_id);
  info->label = g_strdup(mkey->label);
  info->npub = g_strdup(mkey->npub);
  info->pubkey_hex = g_strdup(mkey->pubkey_hex);
  info->key_type = mkey->key_type;
  info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, mkey->created_at);
  info->slot_id = slot_id;
  info->can_sign = TRUE;
  info->is_extractable = FALSE;

  g_mutex_unlock(&self->lock);
  return info;
}

static gboolean
mock_sign_hash(GnHsmProvider *provider,
               guint64 slot_id,
               const gchar *key_id,
               const guint8 *hash,
               gsize hash_len,
               guint8 *signature,
               gsize *signature_len,
               GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  if (hash_len != 32) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Hash must be 32 bytes, got %" G_GSIZE_FORMAT, hash_len);
    return FALSE;
  }

  if (*signature_len < 64) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Signature buffer too small (need 64, got %" G_GSIZE_FORMAT ")",
                *signature_len);
    return FALSE;
  }

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in sign_hash");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for signing");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  MockKey *mkey = g_hash_table_lookup(dev->keys, key_id);
  if (!mkey) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Key '%s' not found", key_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Use libnostr to sign via nostr_event_sign
   * We create a temporary event, set its id to the hash, and sign it.
   * Then extract the signature bytes from the resulting sig hex.
   */
  NostrEvent *temp_event = nostr_event_new();
  if (!temp_event) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to create temporary event for signing");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Set up event with minimal fields for signing */
  nostr_event_set_pubkey(temp_event, mkey->pubkey_hex);
  nostr_event_set_kind(temp_event, 1);
  nostr_event_set_created_at(temp_event, time(NULL));
  nostr_event_set_content(temp_event, "");

  /* Set the event id to our hash (this is what will be signed) */
  gchar *hash_hex = bytes_to_hex(hash, 32);
  temp_event->id = g_strdup(hash_hex);
  g_free(hash_hex);

  /* Sign with the private key */
  gchar *sk_hex = bytes_to_hex(mkey->private_key, 32);
  int sign_result = nostr_event_sign(temp_event, sk_hex);

  /* Securely clear the private key hex */
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);

  if (sign_result != 0) {
    nostr_event_free(temp_event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Signing failed with code %d", sign_result);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Extract signature */
  const char *sig_hex = nostr_event_get_sig(temp_event);
  if (!sig_hex || !hex_to_bytes(sig_hex, signature, 64)) {
    nostr_event_free(temp_event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to decode signature");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  nostr_event_free(temp_event);
  *signature_len = 64;

  g_mutex_unlock(&self->lock);
  return TRUE;
}

static gchar *
mock_sign_event(GnHsmProvider *provider,
                guint64 slot_id,
                const gchar *key_id,
                const gchar *event_json,
                GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in sign_event");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for signing");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockKey *mkey = g_hash_table_lookup(dev->keys, key_id);
  if (!mkey) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Key '%s' not found", key_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Use libnostr to sign the event */
  /* Parse the event JSON */
  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to create event for signing");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  if (!nostr_event_deserialize_compact(event, event_json, NULL)) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to parse event JSON");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Sign with the private key */
  gchar *sk_hex = bytes_to_hex(mkey->private_key, 32);
  int sign_result = nostr_event_sign(event, sk_hex);

  /* Securely clear the private key hex */
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);

  if (sign_result != 0) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Event signing failed with code %d", sign_result);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Serialize back to JSON */
  gchar *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);

  if (!signed_json) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to serialize signed event");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  g_mutex_unlock(&self->lock);
  return signed_json;
}

static GnHsmKeyInfo *
mock_generate_key(GnHsmProvider *provider,
                  guint64 slot_id,
                  const gchar *label,
                  GnHsmKeyType key_type,
                  GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  if (key_type != GN_HSM_KEY_TYPE_SECP256K1) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Only secp256k1 keys are supported");
    return NULL;
  }

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in generate_key");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for key generation");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Generate a new keypair using libnostr */
  gchar *sk_hex = nostr_key_generate_private();
  if (!sk_hex) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                "Failed to generate key");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GNostrKeys *keys = gnostr_keys_new_from_hex(sk_hex, NULL);
  if (!keys) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                "Failed to derive public key");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  const gchar *pk_hex = gnostr_keys_get_pubkey(keys);

  /* Create mock key */
  MockKey *mkey = g_new0(MockKey, 1);
  mkey->key_id = generate_key_id();
  mkey->label = g_strdup(label);
  mkey->key_type = key_type;
  mkey->created_at = time(NULL);

  /* Store keys */
  hex_to_bytes(sk_hex, mkey->private_key, 32);
  hex_to_bytes(pk_hex, mkey->public_key, 32);
  mkey->pubkey_hex = g_strdup(pk_hex);

  /* Generate npub */
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(pk_hex, NULL);
  if (nip19) {
    mkey->npub = g_strdup(gnostr_nip19_get_bech32(nip19));
    g_object_unref(nip19);
  } else {
    mkey->npub = g_strdup_printf("npub1%s", pk_hex); /* Fallback */
  }

  /* Clear sensitive data */
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);
  g_object_unref(keys);

  /* Store in device */
  g_hash_table_insert(dev->keys, g_strdup(mkey->key_id), mkey);

  /* Create return info */
  GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
  info->key_id = g_strdup(mkey->key_id);
  info->label = g_strdup(mkey->label);
  info->npub = g_strdup(mkey->npub);
  info->pubkey_hex = g_strdup(mkey->pubkey_hex);
  info->key_type = mkey->key_type;
  info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, mkey->created_at);
  info->slot_id = slot_id;
  info->can_sign = TRUE;
  info->is_extractable = FALSE;

  g_mutex_unlock(&self->lock);
  return info;
}

static gboolean
mock_import_key(GnHsmProvider *provider,
                guint64 slot_id,
                const gchar *label,
                const guint8 *private_key,
                gsize key_len,
                GnHsmKeyInfo **out_info,
                GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  if (key_len != 32) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Private key must be 32 bytes");
    return FALSE;
  }

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in import_key");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for key import");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Derive public key */
  gchar *sk_hex = bytes_to_hex(private_key, 32);
  GNostrKeys *keys = gnostr_keys_new_from_hex(sk_hex, NULL);

  if (!keys) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to derive public key");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  const gchar *pk_hex = gnostr_keys_get_pubkey(keys);

  /* Create mock key */
  MockKey *mkey = g_new0(MockKey, 1);
  mkey->key_id = generate_key_id();
  mkey->label = g_strdup(label);
  mkey->key_type = GN_HSM_KEY_TYPE_SECP256K1;
  mkey->created_at = time(NULL);

  memcpy(mkey->private_key, private_key, 32);
  hex_to_bytes(pk_hex, mkey->public_key, 32);
  mkey->pubkey_hex = g_strdup(pk_hex);

  /* Generate npub */
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(pk_hex, NULL);
  if (nip19) {
    mkey->npub = g_strdup(gnostr_nip19_get_bech32(nip19));
    g_object_unref(nip19);
  } else {
    mkey->npub = g_strdup_printf("npub1%s", pk_hex);
  }

  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);
  g_object_unref(keys);

  /* Store in device */
  g_hash_table_insert(dev->keys, g_strdup(mkey->key_id), mkey);

  /* Create return info if requested */
  if (out_info) {
    GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
    info->key_id = g_strdup(mkey->key_id);
    info->label = g_strdup(mkey->label);
    info->npub = g_strdup(mkey->npub);
    info->pubkey_hex = g_strdup(mkey->pubkey_hex);
    info->key_type = mkey->key_type;
    info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, mkey->created_at);
    info->slot_id = slot_id;
    info->can_sign = TRUE;
    info->is_extractable = FALSE;
    *out_info = info;
  }

  g_mutex_unlock(&self->lock);
  return TRUE;
}

static gboolean
mock_delete_key(GnHsmProvider *provider,
                guint64 slot_id,
                const gchar *key_id,
                GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in delete_key");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  if (dev->needs_pin && !dev->is_logged_in) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_REQUIRED,
                "Login required for key deletion");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  if (!g_hash_table_remove(dev->keys, key_id)) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Key '%s' not found", key_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  g_mutex_unlock(&self->lock);
  return TRUE;
}

static gboolean
mock_login(GnHsmProvider *provider, guint64 slot_id, const gchar *pin, GError **error)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  if (self->has_simulated_error) {
    self->has_simulated_error = FALSE;
    g_set_error(error, GN_HSM_ERROR, self->simulated_error,
                "Simulated error in login");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (!dev) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Device slot %" G_GUINT64_FORMAT " not found", slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  if (!dev->needs_pin) {
    dev->is_logged_in = TRUE;
    g_mutex_unlock(&self->lock);
    return TRUE;
  }

  if (!dev->pin || !pin || g_strcmp0(dev->pin, pin) != 0) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_PIN_INCORRECT,
                "Incorrect PIN");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  dev->is_logged_in = TRUE;
  g_mutex_unlock(&self->lock);
  return TRUE;
}

static void
mock_logout(GnHsmProvider *provider, guint64 slot_id)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(provider);

  g_mutex_lock(&self->lock);
  self->operation_count++;

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (dev) {
    dev->is_logged_in = FALSE;
  }

  g_mutex_unlock(&self->lock);
}

/* ============================================================================
 * Interface Setup
 * ============================================================================ */

static void
gn_hsm_provider_mock_iface_init(GnHsmProviderInterface *iface)
{
  iface->get_name = mock_get_name;
  iface->is_available = mock_is_available;
  iface->init_provider = mock_init_provider;
  iface->shutdown_provider = mock_shutdown_provider;
  iface->detect_devices = mock_detect_devices;
  iface->list_keys = mock_list_keys;
  iface->get_public_key = mock_get_public_key;
  iface->sign_hash = mock_sign_hash;
  iface->sign_event = mock_sign_event;
  iface->generate_key = mock_generate_key;
  iface->import_key = mock_import_key;
  iface->delete_key = mock_delete_key;
  iface->login = mock_login;
  iface->logout = mock_logout;
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gn_hsm_provider_mock_finalize(GObject *object)
{
  GnHsmProviderMock *self = GN_HSM_PROVIDER_MOCK(object);

  g_mutex_lock(&self->lock);
  g_hash_table_unref(self->devices);
  self->devices = NULL;
  g_mutex_unlock(&self->lock);

  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hsm_provider_mock_parent_class)->finalize(object);
}

static void
gn_hsm_provider_mock_class_init(GnHsmProviderMockClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hsm_provider_mock_finalize;
}

static void
gn_hsm_provider_mock_init(GnHsmProviderMock *self)
{
  g_mutex_init(&self->lock);
  self->initialized = FALSE;
  self->devices = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                        NULL, (GDestroyNotify)mock_device_free);
  self->has_simulated_error = FALSE;
  self->operation_count = 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnHsmProviderMock *
gn_hsm_provider_mock_new(void)
{
  return g_object_new(GN_TYPE_HSM_PROVIDER_MOCK, NULL);
}

void
gn_hsm_provider_mock_add_device(GnHsmProviderMock *self,
                                guint64 slot_id,
                                const gchar *label,
                                gboolean needs_pin)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));
  g_return_if_fail(label != NULL);

  g_mutex_lock(&self->lock);

  MockDevice *dev = g_new0(MockDevice, 1);
  dev->slot_id = slot_id;
  dev->label = g_strdup(label);
  dev->needs_pin = needs_pin;
  dev->is_logged_in = !needs_pin;
  dev->keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)mock_key_free);

  g_hash_table_insert(self->devices, GUINT_TO_POINTER((guint)slot_id), dev);

  g_mutex_unlock(&self->lock);
}

void
gn_hsm_provider_mock_remove_device(GnHsmProviderMock *self, guint64 slot_id)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));

  g_mutex_lock(&self->lock);
  g_hash_table_remove(self->devices, GUINT_TO_POINTER((guint)slot_id));
  g_mutex_unlock(&self->lock);
}

void
gn_hsm_provider_mock_set_pin(GnHsmProviderMock *self,
                             guint64 slot_id,
                             const gchar *pin)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));

  g_mutex_lock(&self->lock);

  MockDevice *dev = g_hash_table_lookup(self->devices,
                                        GUINT_TO_POINTER((guint)slot_id));
  if (dev) {
    g_free(dev->pin);
    dev->pin = g_strdup(pin);
  }

  g_mutex_unlock(&self->lock);
}

void
gn_hsm_provider_mock_simulate_error(GnHsmProviderMock *self, GnHsmError error_code)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));

  g_mutex_lock(&self->lock);
  self->simulated_error = error_code;
  self->has_simulated_error = TRUE;
  g_mutex_unlock(&self->lock);
}

void
gn_hsm_provider_mock_clear_simulated_error(GnHsmProviderMock *self)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));

  g_mutex_lock(&self->lock);
  self->has_simulated_error = FALSE;
  g_mutex_unlock(&self->lock);
}

guint
gn_hsm_provider_mock_get_operation_count(GnHsmProviderMock *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_MOCK(self), 0);

  g_mutex_lock(&self->lock);
  guint count = self->operation_count;
  g_mutex_unlock(&self->lock);

  return count;
}

void
gn_hsm_provider_mock_reset_operation_count(GnHsmProviderMock *self)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_MOCK(self));

  g_mutex_lock(&self->lock);
  self->operation_count = 0;
  g_mutex_unlock(&self->lock);
}
