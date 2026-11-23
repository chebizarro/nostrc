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

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_KEY_PROVIDER_SECP256K1_H */
