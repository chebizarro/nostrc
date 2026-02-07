#ifndef GNOSTR_KEYS_H
#define GNOSTR_KEYS_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

/* Define GNostrKeys GObject wrapper */
/* Prefixed with G to avoid clashing with core NostrKeys functions */
#define GNOSTR_TYPE_KEYS (gnostr_keys_get_type())
G_DECLARE_FINAL_TYPE(GNostrKeys, gnostr_keys, GNOSTR, KEYS, GObject)

/**
 * GNostrKeys:
 *
 * A GObject wrapper for Nostr key operations including key generation,
 * import, public key derivation, signing, and encryption/decryption.
 *
 * ## Signals
 *
 * - #GNostrKeys::key-generated - Emitted after a new keypair has been generated
 * - #GNostrKeys::key-imported - Emitted after a key has been successfully imported
 * - #GNostrKeys::signed - Emitted after a signing operation completes
 * - #GNostrKeys::encrypted - Emitted after an encryption operation completes
 * - #GNostrKeys::decrypted - Emitted after a decryption operation completes
 *
 * ## Properties
 *
 * - #GNostrKeys:pubkey - The public key in hex format (read-only after creation)
 * - #GNostrKeys:has-private-key - Whether a private key is loaded (read-only)
 *
 * ## Security Notes
 *
 * The private key is stored in a secure buffer with best-effort memory locking
 * and explicit wiping on destruction. Never expose the private key directly.
 *
 * Since: 0.1
 */

/**
 * gnostr_keys_new:
 *
 * Creates a new GNostrKeys instance with a freshly generated keypair.
 *
 * Returns: (transfer full): a new #GNostrKeys with a generated keypair
 */
GNostrKeys *gnostr_keys_new(void);

/**
 * gnostr_keys_new_from_hex:
 * @privkey_hex: hex-encoded private key (64 characters)
 * @error: (nullable): return location for a #GError
 *
 * Creates a new GNostrKeys from a hex-encoded private key.
 *
 * Returns: (transfer full) (nullable): a new #GNostrKeys, or %NULL on error
 */
GNostrKeys *gnostr_keys_new_from_hex(const gchar *privkey_hex, GError **error);

/**
 * gnostr_keys_new_from_nsec:
 * @nsec: bech32-encoded private key (nsec1...)
 * @error: (nullable): return location for a #GError
 *
 * Creates a new GNostrKeys from an nsec-encoded private key (NIP-19).
 *
 * Returns: (transfer full) (nullable): a new #GNostrKeys, or %NULL on error
 */
GNostrKeys *gnostr_keys_new_from_nsec(const gchar *nsec, GError **error);

/**
 * gnostr_keys_new_pubkey_only:
 * @pubkey_hex: hex-encoded public key (64 characters)
 * @error: (nullable): return location for a #GError
 *
 * Creates a new GNostrKeys with only a public key (no private key).
 * This instance can only verify signatures and encrypt to, but cannot sign.
 *
 * Returns: (transfer full) (nullable): a new #GNostrKeys, or %NULL on error
 */
GNostrKeys *gnostr_keys_new_pubkey_only(const gchar *pubkey_hex, GError **error);

/* Property accessors */

/**
 * gnostr_keys_get_pubkey:
 * @self: a #GNostrKeys
 *
 * Gets the public key in hex format.
 *
 * Returns: (transfer none) (nullable): the public key as a 64-character hex string
 */
const gchar *gnostr_keys_get_pubkey(GNostrKeys *self);

/**
 * gnostr_keys_get_npub:
 * @self: a #GNostrKeys
 *
 * Gets the public key in npub (NIP-19 bech32) format.
 *
 * Returns: (transfer full) (nullable): the public key as npub string, free with g_free()
 */
gchar *gnostr_keys_get_npub(GNostrKeys *self);

/**
 * gnostr_keys_has_private_key:
 * @self: a #GNostrKeys
 *
 * Checks whether this instance has a private key loaded.
 *
 * Returns: %TRUE if a private key is available, %FALSE otherwise
 */
gboolean gnostr_keys_has_private_key(GNostrKeys *self);

/* Signing operations */

/**
 * gnostr_keys_sign:
 * @self: a #GNostrKeys
 * @message: the message to sign (typically an event ID)
 * @error: (nullable): return location for a #GError
 *
 * Signs a message using Schnorr signatures (BIP-340).
 * Requires a private key to be loaded.
 * Emits the "signed" signal on success.
 *
 * Returns: (transfer full) (nullable): the signature as a 128-character hex string
 */
gchar *gnostr_keys_sign(GNostrKeys *self, const gchar *message, GError **error);

/**
 * gnostr_keys_verify:
 * @self: a #GNostrKeys
 * @message: the message that was signed
 * @signature: the signature to verify (128-character hex string)
 * @error: (nullable): return location for a #GError
 *
 * Verifies a Schnorr signature against this key's public key.
 *
 * Returns: %TRUE if signature is valid, %FALSE otherwise
 */
gboolean gnostr_keys_verify(GNostrKeys *self, const gchar *message, const gchar *signature, GError **error);

/* NIP-04 Encryption/Decryption */

/**
 * gnostr_keys_nip04_encrypt:
 * @self: a #GNostrKeys
 * @plaintext: the plaintext message to encrypt
 * @recipient_pubkey: the recipient's public key (64-character hex)
 * @error: (nullable): return location for a #GError
 *
 * Encrypts a message using NIP-04 (deprecated, use NIP-44 for new code).
 * Requires a private key to be loaded.
 * Emits the "encrypted" signal on success.
 *
 * Returns: (transfer full) (nullable): the encrypted content in NIP-04 format
 */
gchar *gnostr_keys_nip04_encrypt(GNostrKeys *self,
                                  const gchar *plaintext,
                                  const gchar *recipient_pubkey,
                                  GError **error);

/**
 * gnostr_keys_nip04_decrypt:
 * @self: a #GNostrKeys
 * @ciphertext: the NIP-04 encrypted content
 * @sender_pubkey: the sender's public key (64-character hex)
 * @error: (nullable): return location for a #GError
 *
 * Decrypts a NIP-04 encrypted message.
 * Requires a private key to be loaded.
 * Emits the "decrypted" signal on success.
 *
 * Returns: (transfer full) (nullable): the decrypted plaintext
 */
gchar *gnostr_keys_nip04_decrypt(GNostrKeys *self,
                                  const gchar *ciphertext,
                                  const gchar *sender_pubkey,
                                  GError **error);

/* NIP-44 Encryption/Decryption */

/**
 * gnostr_keys_nip44_encrypt:
 * @self: a #GNostrKeys
 * @plaintext: the plaintext message to encrypt
 * @recipient_pubkey: the recipient's public key (64-character hex)
 * @error: (nullable): return location for a #GError
 *
 * Encrypts a message using NIP-44 v2 (recommended for new code).
 * Requires a private key to be loaded.
 * Emits the "encrypted" signal on success.
 *
 * Returns: (transfer full) (nullable): the encrypted content in NIP-44 base64 format
 */
gchar *gnostr_keys_nip44_encrypt(GNostrKeys *self,
                                  const gchar *plaintext,
                                  const gchar *recipient_pubkey,
                                  GError **error);

/**
 * gnostr_keys_nip44_decrypt:
 * @self: a #GNostrKeys
 * @ciphertext: the NIP-44 encrypted content (base64)
 * @sender_pubkey: the sender's public key (64-character hex)
 * @error: (nullable): return location for a #GError
 *
 * Decrypts a NIP-44 encrypted message.
 * Requires a private key to be loaded.
 * Emits the "decrypted" signal on success.
 *
 * Returns: (transfer full) (nullable): the decrypted plaintext
 */
gchar *gnostr_keys_nip44_decrypt(GNostrKeys *self,
                                  const gchar *ciphertext,
                                  const gchar *sender_pubkey,
                                  GError **error);

/* Utility functions */

/**
 * gnostr_keys_is_valid_pubkey:
 * @pubkey_hex: a potential public key string
 *
 * Validates whether a string is a valid Nostr public key.
 *
 * Returns: %TRUE if valid, %FALSE otherwise
 */
gboolean gnostr_keys_is_valid_pubkey(const gchar *pubkey_hex);

/**
 * gnostr_keys_generate_new:
 * @self: a #GNostrKeys
 * @error: (nullable): return location for a #GError
 *
 * Generates a new keypair, replacing any existing key.
 * Emits the "key-generated" signal on success.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_keys_generate_new(GNostrKeys *self, GError **error);

G_END_DECLS

#endif /* GNOSTR_KEYS_H */
