#ifndef GNOSTR_BIP39_H
#define GNOSTR_BIP39_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_BIP39 (gnostr_bip39_get_type())
G_DECLARE_FINAL_TYPE(GNostrBip39, gnostr_bip39, GNOSTR, BIP39, GObject)

/**
 * GNostrBip39:
 *
 * GObject wrapper for BIP-39 mnemonic seed phrase operations.
 *
 * Provides mnemonic generation, validation (with checksum verification),
 * and PBKDF2 seed derivation. Supports 12/15/18/21/24 word mnemonics
 * from the BIP-39 English wordlist.
 *
 * ## Properties
 *
 * - #GNostrBip39:mnemonic - The mnemonic phrase (read-only after generation/import)
 * - #GNostrBip39:word-count - Number of words in the mnemonic (read-only)
 * - #GNostrBip39:is-valid - Whether the current mnemonic passes BIP-39 validation (read-only)
 *
 * Since: 0.1
 */

/**
 * gnostr_bip39_new:
 *
 * Creates a new empty #GNostrBip39 instance (no mnemonic loaded).
 *
 * Returns: (transfer full): a new #GNostrBip39
 */
GNostrBip39 *gnostr_bip39_new(void);

/**
 * gnostr_bip39_generate:
 * @self: a #GNostrBip39
 * @word_count: number of words (12, 15, 18, 21, or 24)
 * @error: (nullable): return location for a #GError
 *
 * Generates a new BIP-39 mnemonic with the specified word count.
 * Updates the "mnemonic", "word-count", and "is-valid" properties.
 *
 * Returns: (transfer none) (nullable): the generated mnemonic string
 *   (owned by @self), or %NULL on error
 */
const gchar *gnostr_bip39_generate(GNostrBip39 *self,
                                    gint word_count,
                                    GError **error);

/**
 * gnostr_bip39_set_mnemonic:
 * @self: a #GNostrBip39
 * @mnemonic: the mnemonic phrase to import
 * @error: (nullable): return location for a #GError
 *
 * Imports an existing mnemonic phrase. The mnemonic is validated
 * against BIP-39 rules (wordlist, checksum).
 *
 * Returns: %TRUE if the mnemonic is valid and was imported, %FALSE on error
 */
gboolean gnostr_bip39_set_mnemonic(GNostrBip39 *self,
                                    const gchar *mnemonic,
                                    GError **error);

/**
 * gnostr_bip39_validate:
 * @mnemonic: a mnemonic phrase to validate
 *
 * Static validation: checks if a mnemonic string is a valid BIP-39 phrase
 * (correct word count, all words in the English wordlist, valid checksum).
 *
 * Returns: %TRUE if valid, %FALSE otherwise
 */
gboolean gnostr_bip39_validate(const gchar *mnemonic);

/**
 * gnostr_bip39_to_seed:
 * @self: a #GNostrBip39
 * @passphrase: (nullable): optional passphrase (NULL or "" for no passphrase)
 * @out_seed: (out) (array fixed-size=64) (transfer full): output 64-byte seed buffer.
 *   Caller must free with g_free() and should securely wipe before freeing.
 * @error: (nullable): return location for a #GError
 *
 * Derives a 64-byte seed from the mnemonic using PBKDF2-HMAC-SHA512
 * with 2048 iterations and salt "mnemonic" + passphrase.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_bip39_to_seed(GNostrBip39 *self,
                               const gchar *passphrase,
                               guint8 **out_seed,
                               GError **error);

/**
 * gnostr_bip39_to_keys:
 * @self: a #GNostrBip39
 * @passphrase: (nullable): optional passphrase for seed derivation
 * @error: (nullable): return location for a #GError
 *
 * Derives a Nostr keypair from the mnemonic using NIP-06 key derivation
 * (path m/44'/1237'/0'/0/0). Convenience method that combines
 * seed derivation + NIP-06 key derivation + GNostrKeys creation.
 *
 * Returns: (transfer full) (nullable): a new #GNostrKeys, or %NULL on error
 */
GObject *gnostr_bip39_to_keys(GNostrBip39 *self,
                               const gchar *passphrase,
                               GError **error);

/**
 * gnostr_bip39_get_mnemonic:
 * @self: a #GNostrBip39
 *
 * Gets the current mnemonic phrase.
 *
 * Returns: (transfer none) (nullable): the mnemonic string, or %NULL if not set
 */
const gchar *gnostr_bip39_get_mnemonic(GNostrBip39 *self);

/**
 * gnostr_bip39_get_word_count:
 * @self: a #GNostrBip39
 *
 * Gets the word count of the current mnemonic.
 *
 * Returns: the number of words, or 0 if no mnemonic is set
 */
gint gnostr_bip39_get_word_count(GNostrBip39 *self);

/**
 * gnostr_bip39_get_is_valid:
 * @self: a #GNostrBip39
 *
 * Whether the current mnemonic is valid per BIP-39.
 *
 * Returns: %TRUE if valid, %FALSE otherwise
 */
gboolean gnostr_bip39_get_is_valid(GNostrBip39 *self);

G_END_DECLS

#endif /* GNOSTR_BIP39_H */
