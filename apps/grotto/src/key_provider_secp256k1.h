/* key_provider_secp256k1.h - secp256k1 key provider for Nostr
 *
 * Implementation of GnKeyProvider for secp256k1 elliptic curve keys,
 * which is the standard key type used by Nostr (NIP-01).
 *
 * Features:
 * - Schnorr signatures (BIP-340)
 * - X-only public keys (32 bytes)
 * - Integration with libsecp256k1
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPS_GNOSTR_SIGNER_KEY_PROVIDER_SECP256K1_H
#define APPS_GNOSTR_SIGNER_KEY_PROVIDER_SECP256K1_H

#include "key_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_KEY_PROVIDER_SECP256K1 (gn_key_provider_secp256k1_get_type())

G_DECLARE_FINAL_TYPE(GnKeyProviderSecp256k1, gn_key_provider_secp256k1,
                     GN, KEY_PROVIDER_SECP256K1, GObject)

/* Key sizes for secp256k1 (Nostr uses x-only/Schnorr format) */
#define GN_SECP256K1_PRIVATE_KEY_SIZE  32
#define GN_SECP256K1_PUBLIC_KEY_SIZE   32  /* x-only for Schnorr */
#define GN_SECP256K1_SIGNATURE_SIZE    64  /* Schnorr signature */

/**
 * gn_key_provider_secp256k1_new:
 *
 * Creates a new secp256k1 key provider instance.
 *
 * Returns: (transfer full): A new #GnKeyProviderSecp256k1
 */
GnKeyProviderSecp256k1 *gn_key_provider_secp256k1_new(void);

/**
 * gn_key_provider_secp256k1_get_default:
 *
 * Gets the singleton secp256k1 key provider instance.
 *
 * Returns: (transfer none): The default #GnKeyProviderSecp256k1
 */
GnKeyProviderSecp256k1 *gn_key_provider_secp256k1_get_default(void);

/**
 * gn_key_provider_secp256k1_register:
 *
 * Registers the secp256k1 provider in the global provider registry.
 * This should be called during application initialization.
 */
void gn_key_provider_secp256k1_register(void);

/* ============================================================================
 * Utility functions for direct secp256k1 operations
 * ============================================================================ */

/**
 * gn_secp256k1_derive_pubkey_hex:
 * @private_key_hex: Private key as 64-character hex string
 * @public_key_hex_out: (out) (transfer full): Output public key hex (64 chars)
 * @error: (out) (optional): Return location for error
 *
 * Derives an x-only public key from a private key (hex format).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_secp256k1_derive_pubkey_hex(const gchar  *private_key_hex,
                                        gchar       **public_key_hex_out,
                                        GError      **error);

/**
 * gn_secp256k1_sign_hash_hex:
 * @private_key_hex: Private key as 64-character hex string
 * @hash_hex: Message hash as 64-character hex string (SHA-256)
 * @signature_hex_out: (out) (transfer full): Output signature hex (128 chars)
 * @error: (out) (optional): Return location for error
 *
 * Signs a message hash with Schnorr signature (BIP-340).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_secp256k1_sign_hash_hex(const gchar  *private_key_hex,
                                    const gchar  *hash_hex,
                                    gchar       **signature_hex_out,
                                    GError      **error);

/**
 * gn_secp256k1_verify_hex:
 * @public_key_hex: Public key as 64-character hex string (x-only)
 * @hash_hex: Message hash as 64-character hex string
 * @signature_hex: Signature as 128-character hex string
 * @error: (out) (optional): Return location for error
 *
 * Verifies a Schnorr signature.
 *
 * Returns: %TRUE if valid, %FALSE if invalid or on error
 */
gboolean gn_secp256k1_verify_hex(const gchar  *public_key_hex,
                                 const gchar  *hash_hex,
                                 const gchar  *signature_hex,
                                 GError      **error);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_KEY_PROVIDER_SECP256K1_H */
