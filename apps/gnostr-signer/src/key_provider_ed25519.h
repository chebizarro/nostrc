/* key_provider_ed25519.h - Ed25519 key provider for future NIP compatibility
 *
 * Implementation of GnKeyProvider for Ed25519 elliptic curve keys.
 * Ed25519 is not currently used by Nostr, but this provider enables
 * future NIP compatibility and cross-protocol key support.
 *
 * Features:
 * - Ed25519 signatures (RFC 8032)
 * - 32-byte private keys, 32-byte public keys
 * - 64-byte signatures
 * - Deterministic signatures (no randomness needed per signature)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPS_GNOSTR_SIGNER_KEY_PROVIDER_ED25519_H
#define APPS_GNOSTR_SIGNER_KEY_PROVIDER_ED25519_H

#include "key_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_KEY_PROVIDER_ED25519 (gn_key_provider_ed25519_get_type())

G_DECLARE_FINAL_TYPE(GnKeyProviderEd25519, gn_key_provider_ed25519,
                     GN, KEY_PROVIDER_ED25519, GObject)

/* Key sizes for Ed25519 */
#define GN_ED25519_PRIVATE_KEY_SIZE  32
#define GN_ED25519_PUBLIC_KEY_SIZE   32
#define GN_ED25519_SIGNATURE_SIZE    64
#define GN_ED25519_SEED_SIZE         32  /* Seed for deterministic key derivation */

/**
 * gn_key_provider_ed25519_new:
 *
 * Creates a new Ed25519 key provider instance.
 *
 * Returns: (transfer full): A new #GnKeyProviderEd25519
 */
GnKeyProviderEd25519 *gn_key_provider_ed25519_new(void);

/**
 * gn_key_provider_ed25519_get_default:
 *
 * Gets the singleton Ed25519 key provider instance.
 *
 * Returns: (transfer none): The default #GnKeyProviderEd25519
 */
GnKeyProviderEd25519 *gn_key_provider_ed25519_get_default(void);

/**
 * gn_key_provider_ed25519_register:
 *
 * Registers the Ed25519 provider in the global provider registry.
 * This should be called during application initialization.
 */
void gn_key_provider_ed25519_register(void);

/**
 * gn_key_provider_ed25519_is_available:
 *
 * Checks if Ed25519 support is available (library compiled with support).
 *
 * Returns: %TRUE if Ed25519 operations are available
 */
gboolean gn_key_provider_ed25519_is_available(void);

/* ============================================================================
 * Utility functions for direct Ed25519 operations
 * ============================================================================ */

/**
 * gn_ed25519_derive_pubkey_hex:
 * @private_key_hex: Private key/seed as 64-character hex string
 * @public_key_hex_out: (out) (transfer full): Output public key hex (64 chars)
 * @error: (out) (optional): Return location for error
 *
 * Derives an Ed25519 public key from a private key (hex format).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_ed25519_derive_pubkey_hex(const gchar  *private_key_hex,
                                      gchar       **public_key_hex_out,
                                      GError      **error);

/**
 * gn_ed25519_sign_hash_hex:
 * @private_key_hex: Private key as 64-character hex string
 * @hash_hex: Message hash as 64-character hex string
 * @signature_hex_out: (out) (transfer full): Output signature hex (128 chars)
 * @error: (out) (optional): Return location for error
 *
 * Signs a message hash with Ed25519.
 *
 * Note: Ed25519 typically signs the full message, not just a hash.
 * This function is provided for API compatibility.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_ed25519_sign_hash_hex(const gchar  *private_key_hex,
                                  const gchar  *hash_hex,
                                  gchar       **signature_hex_out,
                                  GError      **error);

/**
 * gn_ed25519_verify_hex:
 * @public_key_hex: Public key as 64-character hex string
 * @hash_hex: Message hash as 64-character hex string
 * @signature_hex: Signature as 128-character hex string
 * @error: (out) (optional): Return location for error
 *
 * Verifies an Ed25519 signature.
 *
 * Returns: %TRUE if valid, %FALSE if invalid or on error
 */
gboolean gn_ed25519_verify_hex(const gchar  *public_key_hex,
                               const gchar  *hash_hex,
                               const gchar  *signature_hex,
                               GError      **error);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_KEY_PROVIDER_ED25519_H */
