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

/* ============================================================================
 * Utility functions for direct Ed25519 operations
 * ============================================================================ */

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_KEY_PROVIDER_ED25519_H */
