/* key_provider.c - Abstract interface for cryptographic key operations
 *
 * Implementation of the GnKeyProvider GInterface and provider registry.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "key_provider.h"
#include <string.h>

/* ============================================================================
 * Error quark
 * ============================================================================ */

G_DEFINE_QUARK(gn-key-provider-error-quark, gn_key_provider_error)

/* ============================================================================
 * GInterface implementation
 * ============================================================================ */

G_DEFINE_INTERFACE(GnKeyProvider, gn_key_provider, G_TYPE_OBJECT)

static void
gn_key_provider_default_init(GnKeyProviderInterface *iface)
{
  /* No default implementations - all methods must be provided by implementors */
  (void)iface;
}

/* ============================================================================
 * Interface method implementations (dispatch to vfuncs)
 * ============================================================================ */

GnKeyType
gn_key_provider_get_key_type(GnKeyProvider *self)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), GN_KEY_TYPE_UNKNOWN);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_key_type != NULL, GN_KEY_TYPE_UNKNOWN);

  return iface->get_key_type(self);
}

const gchar *
gn_key_provider_get_key_type_name(GnKeyProvider *self)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), "unknown");

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_key_type_name != NULL, "unknown");

  return iface->get_key_type_name(self);
}

gsize
gn_key_provider_get_private_key_size(GnKeyProvider *self)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), 0);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_private_key_size != NULL, 0);

  return iface->get_private_key_size(self);
}

gsize
gn_key_provider_get_public_key_size(GnKeyProvider *self)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), 0);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_public_key_size != NULL, 0);

  return iface->get_public_key_size(self);
}

gsize
gn_key_provider_get_signature_size(GnKeyProvider *self)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), 0);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->get_signature_size != NULL, 0);

  return iface->get_signature_size(self);
}

gboolean
gn_key_provider_derive_public_key(GnKeyProvider  *self,
                                  const guint8   *private_key,
                                  gsize           private_key_len,
                                  guint8         *public_key_out,
                                  gsize          *public_key_len_out,
                                  GError        **error)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(private_key != NULL, FALSE);
  g_return_val_if_fail(public_key_out != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->derive_public_key == NULL) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
                "derive_public_key not implemented for this provider");
    return FALSE;
  }

  return iface->derive_public_key(self, private_key, private_key_len,
                                  public_key_out, public_key_len_out, error);
}

gboolean
gn_key_provider_sign(GnKeyProvider  *self,
                     const guint8   *private_key,
                     gsize           private_key_len,
                     const guint8   *message_hash,
                     gsize           hash_len,
                     guint8         *signature_out,
                     gsize          *signature_len_out,
                     GError        **error)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(private_key != NULL, FALSE);
  g_return_val_if_fail(message_hash != NULL, FALSE);
  g_return_val_if_fail(signature_out != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->sign == NULL) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
                "sign not implemented for this provider");
    return FALSE;
  }

  return iface->sign(self, private_key, private_key_len,
                     message_hash, hash_len,
                     signature_out, signature_len_out, error);
}

gboolean
gn_key_provider_verify(GnKeyProvider  *self,
                       const guint8   *public_key,
                       gsize           public_key_len,
                       const guint8   *message_hash,
                       gsize           hash_len,
                       const guint8   *signature,
                       gsize           signature_len,
                       GError        **error)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(public_key != NULL, FALSE);
  g_return_val_if_fail(message_hash != NULL, FALSE);
  g_return_val_if_fail(signature != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->verify == NULL) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
                "verify not implemented for this provider");
    return FALSE;
  }

  return iface->verify(self, public_key, public_key_len,
                       message_hash, hash_len,
                       signature, signature_len, error);
}

gboolean
gn_key_provider_generate_private_key(GnKeyProvider  *self,
                                     guint8         *private_key_out,
                                     gsize          *private_key_len_out,
                                     GError        **error)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(private_key_out != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->generate_private_key == NULL) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
                "generate_private_key not implemented for this provider");
    return FALSE;
  }

  return iface->generate_private_key(self, private_key_out, private_key_len_out, error);
}

gboolean
gn_key_provider_is_valid_private_key(GnKeyProvider *self,
                                     const guint8  *private_key,
                                     gsize          private_key_len)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(private_key != NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->is_valid_private_key == NULL) {
    return FALSE;
  }

  return iface->is_valid_private_key(self, private_key, private_key_len);
}

gboolean
gn_key_provider_is_valid_public_key(GnKeyProvider *self,
                                    const guint8  *public_key,
                                    gsize          public_key_len)
{
  GnKeyProviderInterface *iface;

  g_return_val_if_fail(GN_IS_KEY_PROVIDER(self), FALSE);
  g_return_val_if_fail(public_key != NULL, FALSE);

  iface = GN_KEY_PROVIDER_GET_IFACE(self);
  if (iface->is_valid_public_key == NULL) {
    return FALSE;
  }

  return iface->is_valid_public_key(self, public_key, public_key_len);
}

/* ============================================================================
 * Key type utilities
 * ============================================================================ */

const gchar *
gn_key_type_to_string(GnKeyType type)
{
  switch (type) {
    case GN_KEY_TYPE_SECP256K1:
      return "secp256k1";
    case GN_KEY_TYPE_ED25519:
      return "ed25519";
    case GN_KEY_TYPE_UNKNOWN:
    default:
      return "unknown";
  }
}

GnKeyType
gn_key_type_from_string(const gchar *str)
{
  if (str == NULL) {
    return GN_KEY_TYPE_UNKNOWN;
  }

  if (g_ascii_strcasecmp(str, "secp256k1") == 0) {
    return GN_KEY_TYPE_SECP256K1;
  } else if (g_ascii_strcasecmp(str, "ed25519") == 0) {
    return GN_KEY_TYPE_ED25519;
  }

  return GN_KEY_TYPE_UNKNOWN;
}

GnKeyType
gn_key_type_detect_from_key(const guint8 *key_data, gsize key_len)
{
  if (key_data == NULL || key_len == 0) {
    return GN_KEY_TYPE_UNKNOWN;
  }

  /* Both secp256k1 and ed25519 use 32-byte private keys */
  if (key_len == 32) {
    /* Cannot definitively distinguish - default to secp256k1 for Nostr */
    return GN_KEY_TYPE_SECP256K1;
  }

  /* secp256k1 compressed public key is 33 bytes (02/03 prefix) */
  if (key_len == 33 && (key_data[0] == 0x02 || key_data[0] == 0x03)) {
    return GN_KEY_TYPE_SECP256K1;
  }

  /* secp256k1 uncompressed public key is 65 bytes (04 prefix) */
  if (key_len == 65 && key_data[0] == 0x04) {
    return GN_KEY_TYPE_SECP256K1;
  }

  /* secp256k1 x-only public key (Schnorr/BIP-340) is 32 bytes
   * ed25519 public key is also 32 bytes
   * For Nostr, assume secp256k1 x-only */
  if (key_len == 32) {
    return GN_KEY_TYPE_SECP256K1;
  }

  return GN_KEY_TYPE_UNKNOWN;
}

/* ============================================================================
 * Provider registry
 * ============================================================================ */

/* Thread-safe registry using GOnce for lazy initialization */
static GHashTable *provider_registry = NULL;
static GMutex registry_mutex;
static gboolean registry_initialized = FALSE;

static void
ensure_registry_initialized(void)
{
  g_mutex_lock(&registry_mutex);
  if (!registry_initialized) {
    provider_registry = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, g_object_unref);
    registry_initialized = TRUE;
  }
  g_mutex_unlock(&registry_mutex);
}

GnKeyProvider *
gn_key_provider_get_for_type(GnKeyType type)
{
  GnKeyProvider *provider = NULL;

  ensure_registry_initialized();

  g_mutex_lock(&registry_mutex);
  provider = g_hash_table_lookup(provider_registry, GINT_TO_POINTER(type));
  g_mutex_unlock(&registry_mutex);

  return provider;
}

GnKeyProvider *
gn_key_provider_get_default(void)
{
  /* Default to secp256k1 for current Nostr protocol */
  return gn_key_provider_get_for_type(GN_KEY_TYPE_SECP256K1);
}

void
gn_key_provider_register(GnKeyType type, GnKeyProvider *provider)
{
  g_return_if_fail(type != GN_KEY_TYPE_UNKNOWN);
  g_return_if_fail(GN_IS_KEY_PROVIDER(provider));

  ensure_registry_initialized();

  g_mutex_lock(&registry_mutex);
  g_hash_table_replace(provider_registry, GINT_TO_POINTER(type),
                       g_object_ref(provider));
  g_mutex_unlock(&registry_mutex);

  g_debug("key_provider: registered provider for %s", gn_key_type_to_string(type));
}

GArray *
gn_key_provider_list_available(void)
{
  GArray *types;
  GHashTableIter iter;
  gpointer key;

  ensure_registry_initialized();

  types = g_array_new(FALSE, FALSE, sizeof(GnKeyType));

  g_mutex_lock(&registry_mutex);
  g_hash_table_iter_init(&iter, provider_registry);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    GnKeyType type = GPOINTER_TO_INT(key);
    g_array_append_val(types, type);
  }
  g_mutex_unlock(&registry_mutex);

  return types;
}

/* ============================================================================
 * Initialization (called once at startup)
 * ============================================================================ */

/* Forward declarations - implemented in provider files */
extern void gn_key_provider_secp256k1_register(void);
extern void gn_key_provider_ed25519_register(void);

static gboolean providers_initialized = FALSE;

/**
 * gn_key_providers_init:
 *
 * Initialize and register all built-in key providers.
 * This should be called once during application startup.
 * Safe to call multiple times (subsequent calls are no-ops).
 */
void
gn_key_providers_init(void)
{
  if (providers_initialized) {
    return;
  }

  ensure_registry_initialized();

  /* Register secp256k1 provider (Nostr default) */
  gn_key_provider_secp256k1_register();

  /* Register ed25519 provider (for future NIP compatibility) */
  gn_key_provider_ed25519_register();

  providers_initialized = TRUE;

  g_debug("key_provider: initialized %u providers",
          g_hash_table_size(provider_registry));
}
