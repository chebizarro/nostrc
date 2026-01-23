/* key_provider.h - Abstract interface for cryptographic key operations
 *
 * This module defines a GInterface for cryptographic key providers,
 * allowing the signer to support multiple key types (secp256k1, ed25519, etc.)
 *
 * Current Nostr uses secp256k1, but this architecture supports future NIPs
 * that may introduce additional key types.
 *
 * Key operations:
 * - Sign: Create a signature for a message hash
 * - Verify: Verify a signature against a message hash
 * - Derive public key from private key
 * - Key type identification and metadata
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define GN_TYPE_KEY_PROVIDER (gn_key_provider_get_type())

G_DECLARE_INTERFACE(GnKeyProvider, gn_key_provider, GN, KEY_PROVIDER, GObject)

/**
 * GnKeyType:
 * @GN_KEY_TYPE_UNKNOWN: Unknown or invalid key type
 * @GN_KEY_TYPE_SECP256K1: secp256k1 elliptic curve (Nostr standard)
 * @GN_KEY_TYPE_ED25519: Ed25519 Edwards curve (potential future NIP)
 *
 * Supported cryptographic key types.
 */
typedef enum {
  GN_KEY_TYPE_UNKNOWN = 0,
  GN_KEY_TYPE_SECP256K1,
  GN_KEY_TYPE_ED25519
} GnKeyType;

/**
 * GnKeyProviderError:
 * @GN_KEY_PROVIDER_ERROR_INVALID_KEY: Invalid key format or data
 * @GN_KEY_PROVIDER_ERROR_SIGNING_FAILED: Signing operation failed
 * @GN_KEY_PROVIDER_ERROR_VERIFICATION_FAILED: Verification operation failed
 * @GN_KEY_PROVIDER_ERROR_UNSUPPORTED: Operation not supported by this provider
 * @GN_KEY_PROVIDER_ERROR_INTERNAL: Internal error
 *
 * Error codes for key provider operations.
 */
typedef enum {
  GN_KEY_PROVIDER_ERROR_INVALID_KEY,
  GN_KEY_PROVIDER_ERROR_SIGNING_FAILED,
  GN_KEY_PROVIDER_ERROR_VERIFICATION_FAILED,
  GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
  GN_KEY_PROVIDER_ERROR_INTERNAL
} GnKeyProviderError;

#define GN_KEY_PROVIDER_ERROR (gn_key_provider_error_quark())
GQuark gn_key_provider_error_quark(void);

/**
 * GnKeyProviderInterface:
 * @parent: Parent interface
 * @get_key_type: Returns the key type this provider handles
 * @get_key_type_name: Returns a human-readable name for the key type
 * @get_private_key_size: Returns the private key size in bytes
 * @get_public_key_size: Returns the public key size in bytes
 * @get_signature_size: Returns the signature size in bytes
 * @derive_public_key: Derives public key from private key
 * @sign: Signs a message hash with the private key
 * @verify: Verifies a signature against a message hash and public key
 * @generate_private_key: Generates a new random private key
 * @is_valid_private_key: Validates a private key
 * @is_valid_public_key: Validates a public key
 *
 * The virtual function table for #GnKeyProvider.
 */
struct _GnKeyProviderInterface {
  GTypeInterface parent;

  /* Key type information */
  GnKeyType     (*get_key_type)         (GnKeyProvider *self);
  const gchar * (*get_key_type_name)    (GnKeyProvider *self);
  gsize         (*get_private_key_size) (GnKeyProvider *self);
  gsize         (*get_public_key_size)  (GnKeyProvider *self);
  gsize         (*get_signature_size)   (GnKeyProvider *self);

  /* Key derivation */
  gboolean      (*derive_public_key)    (GnKeyProvider  *self,
                                         const guint8   *private_key,
                                         gsize           private_key_len,
                                         guint8         *public_key_out,
                                         gsize          *public_key_len_out,
                                         GError        **error);

  /* Signing and verification */
  gboolean      (*sign)                 (GnKeyProvider  *self,
                                         const guint8   *private_key,
                                         gsize           private_key_len,
                                         const guint8   *message_hash,
                                         gsize           hash_len,
                                         guint8         *signature_out,
                                         gsize          *signature_len_out,
                                         GError        **error);

  gboolean      (*verify)               (GnKeyProvider  *self,
                                         const guint8   *public_key,
                                         gsize           public_key_len,
                                         const guint8   *message_hash,
                                         gsize           hash_len,
                                         const guint8   *signature,
                                         gsize           signature_len,
                                         GError        **error);

  /* Key generation and validation */
  gboolean      (*generate_private_key) (GnKeyProvider  *self,
                                         guint8         *private_key_out,
                                         gsize          *private_key_len_out,
                                         GError        **error);

  gboolean      (*is_valid_private_key) (GnKeyProvider  *self,
                                         const guint8   *private_key,
                                         gsize           private_key_len);

  gboolean      (*is_valid_public_key)  (GnKeyProvider  *self,
                                         const guint8   *public_key,
                                         gsize           public_key_len);
};

/* ============================================================================
 * GnKeyProvider interface methods
 * ============================================================================ */

/**
 * gn_key_provider_get_key_type:
 * @self: A #GnKeyProvider
 *
 * Gets the key type this provider handles.
 *
 * Returns: The #GnKeyType
 */
GnKeyType gn_key_provider_get_key_type(GnKeyProvider *self);

/**
 * gn_key_provider_get_key_type_name:
 * @self: A #GnKeyProvider
 *
 * Gets a human-readable name for the key type.
 *
 * Returns: (transfer none): The key type name (e.g., "secp256k1", "ed25519")
 */
const gchar *gn_key_provider_get_key_type_name(GnKeyProvider *self);

/**
 * gn_key_provider_get_private_key_size:
 * @self: A #GnKeyProvider
 *
 * Gets the expected private key size in bytes.
 *
 * Returns: Private key size in bytes
 */
gsize gn_key_provider_get_private_key_size(GnKeyProvider *self);

/**
 * gn_key_provider_get_public_key_size:
 * @self: A #GnKeyProvider
 *
 * Gets the expected public key size in bytes.
 *
 * Returns: Public key size in bytes
 */
gsize gn_key_provider_get_public_key_size(GnKeyProvider *self);

/**
 * gn_key_provider_get_signature_size:
 * @self: A #GnKeyProvider
 *
 * Gets the expected signature size in bytes.
 *
 * Returns: Signature size in bytes
 */
gsize gn_key_provider_get_signature_size(GnKeyProvider *self);

/**
 * gn_key_provider_derive_public_key:
 * @self: A #GnKeyProvider
 * @private_key: The private key bytes
 * @private_key_len: Length of private key
 * @public_key_out: (out) (array length=public_key_len_out): Output buffer for public key
 * @public_key_len_out: (out): Length of output public key
 * @error: (out) (optional): Return location for error
 *
 * Derives the public key from a private key.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_key_provider_derive_public_key(GnKeyProvider  *self,
                                           const guint8   *private_key,
                                           gsize           private_key_len,
                                           guint8         *public_key_out,
                                           gsize          *public_key_len_out,
                                           GError        **error);

/**
 * gn_key_provider_sign:
 * @self: A #GnKeyProvider
 * @private_key: The private key bytes
 * @private_key_len: Length of private key
 * @message_hash: The message hash to sign (typically SHA-256)
 * @hash_len: Length of message hash
 * @signature_out: (out) (array length=signature_len_out): Output buffer for signature
 * @signature_len_out: (out): Length of output signature
 * @error: (out) (optional): Return location for error
 *
 * Signs a message hash with the private key.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_key_provider_sign(GnKeyProvider  *self,
                              const guint8   *private_key,
                              gsize           private_key_len,
                              const guint8   *message_hash,
                              gsize           hash_len,
                              guint8         *signature_out,
                              gsize          *signature_len_out,
                              GError        **error);

/**
 * gn_key_provider_verify:
 * @self: A #GnKeyProvider
 * @public_key: The public key bytes
 * @public_key_len: Length of public key
 * @message_hash: The message hash that was signed
 * @hash_len: Length of message hash
 * @signature: The signature to verify
 * @signature_len: Length of signature
 * @error: (out) (optional): Return location for error
 *
 * Verifies a signature against a message hash and public key.
 *
 * Returns: %TRUE if signature is valid, %FALSE if invalid or on error
 */
gboolean gn_key_provider_verify(GnKeyProvider  *self,
                                const guint8   *public_key,
                                gsize           public_key_len,
                                const guint8   *message_hash,
                                gsize           hash_len,
                                const guint8   *signature,
                                gsize           signature_len,
                                GError        **error);

/**
 * gn_key_provider_generate_private_key:
 * @self: A #GnKeyProvider
 * @private_key_out: (out) (array length=private_key_len_out): Output buffer for private key
 * @private_key_len_out: (out): Length of output private key
 * @error: (out) (optional): Return location for error
 *
 * Generates a new random private key.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_key_provider_generate_private_key(GnKeyProvider  *self,
                                              guint8         *private_key_out,
                                              gsize          *private_key_len_out,
                                              GError        **error);

/**
 * gn_key_provider_is_valid_private_key:
 * @self: A #GnKeyProvider
 * @private_key: The private key bytes to validate
 * @private_key_len: Length of private key
 *
 * Validates a private key.
 *
 * Returns: %TRUE if the key is valid, %FALSE otherwise
 */
gboolean gn_key_provider_is_valid_private_key(GnKeyProvider *self,
                                              const guint8  *private_key,
                                              gsize          private_key_len);

/**
 * gn_key_provider_is_valid_public_key:
 * @self: A #GnKeyProvider
 * @public_key: The public key bytes to validate
 * @public_key_len: Length of public key
 *
 * Validates a public key.
 *
 * Returns: %TRUE if the key is valid, %FALSE otherwise
 */
gboolean gn_key_provider_is_valid_public_key(GnKeyProvider *self,
                                             const guint8  *public_key,
                                             gsize          public_key_len);

/* ============================================================================
 * Key type utilities
 * ============================================================================ */

/**
 * gn_key_type_to_string:
 * @type: A #GnKeyType
 *
 * Converts a key type enum to its string identifier.
 *
 * Returns: (transfer none): String identifier (e.g., "secp256k1")
 */
const gchar *gn_key_type_to_string(GnKeyType type);

/**
 * gn_key_type_from_string:
 * @str: String identifier (e.g., "secp256k1", "ed25519")
 *
 * Parses a key type string to enum.
 *
 * Returns: The #GnKeyType, or %GN_KEY_TYPE_UNKNOWN if not recognized
 */
GnKeyType gn_key_type_from_string(const gchar *str);

/**
 * gn_key_type_detect_from_key:
 * @key_data: Key data (private or public)
 * @key_len: Length of key data
 *
 * Attempts to detect the key type from key data based on length and format.
 * This is a heuristic and may not always be accurate.
 *
 * Returns: The detected #GnKeyType, or %GN_KEY_TYPE_UNKNOWN
 */
GnKeyType gn_key_type_detect_from_key(const guint8 *key_data, gsize key_len);

/* ============================================================================
 * Provider registry
 * ============================================================================ */

/**
 * gn_key_provider_get_for_type:
 * @type: The #GnKeyType to get a provider for
 *
 * Gets the registered key provider for a specific type.
 *
 * Returns: (transfer none) (nullable): The provider, or %NULL if not registered
 */
GnKeyProvider *gn_key_provider_get_for_type(GnKeyType type);

/**
 * gn_key_provider_get_default:
 *
 * Gets the default key provider (secp256k1 for current Nostr).
 *
 * Returns: (transfer none): The default provider
 */
GnKeyProvider *gn_key_provider_get_default(void);

/**
 * gn_key_provider_register:
 * @type: The #GnKeyType this provider handles
 * @provider: The provider instance to register
 *
 * Registers a key provider for a specific type. The provider registry
 * takes a reference to the provider.
 */
void gn_key_provider_register(GnKeyType type, GnKeyProvider *provider);

/**
 * gn_key_provider_list_available:
 *
 * Lists all available (registered) key types.
 *
 * Returns: (transfer container) (element-type GnKeyType): Array of available types
 */
GArray *gn_key_provider_list_available(void);

/**
 * gn_key_providers_init:
 *
 * Initialize and register all built-in key providers.
 * This should be called once during application startup.
 * Safe to call multiple times (subsequent calls are no-ops).
 */
void gn_key_providers_init(void);

G_END_DECLS
