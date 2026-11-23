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
