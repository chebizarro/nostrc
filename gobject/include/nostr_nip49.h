#ifndef GNOSTR_NIP49_H
#define GNOSTR_NIP49_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_NIP49 (gnostr_nip49_get_type())
G_DECLARE_FINAL_TYPE(GNostrNip49, gnostr_nip49, GNOSTR, NIP49, GObject)

/**
 * GNostrNip49SecurityLevel:
 * @GNOSTR_NIP49_SECURITY_INSECURE: Client does not track whether key material
 *   was handled securely (RAM may have been swapped to disk)
 * @GNOSTR_NIP49_SECURITY_SECURE: Client has made best efforts to keep key
 *   material in secure memory (mlock/VirtualLock)
 * @GNOSTR_NIP49_SECURITY_UNKNOWN: Security level could not be determined
 *
 * Maps to the NIP-49 "security byte" (AD byte in XChaCha20-Poly1305 AEAD).
 */
typedef enum {
    GNOSTR_NIP49_SECURITY_INSECURE = 0x00,
    GNOSTR_NIP49_SECURITY_SECURE   = 0x01,
    GNOSTR_NIP49_SECURITY_UNKNOWN  = 0x02,
} GNostrNip49SecurityLevel;

/**
 * GNostrNip49:
 *
 * GObject wrapper for NIP-49 encrypted private key (ncryptsec) operations.
 *
 * Provides password-based encryption of Nostr private keys using scrypt KDF
 * and XChaCha20-Poly1305 AEAD, producing bech32-encoded "ncryptsec1..." strings.
 *
 * ## Properties
 *
 * - #GNostrNip49:ncryptsec - The encrypted key as a bech32 ncryptsec string (read-only)
 * - #GNostrNip49:security-level - The security level used during encryption (read-only)
 * - #GNostrNip49:log-n - The scrypt exponent (read-only)
 *
 * Since: 0.1
 */

/**
 * gnostr_nip49_new:
 *
 * Creates a new empty #GNostrNip49 instance.
 *
 * Returns: (transfer full): a new #GNostrNip49
 */
GNostrNip49 *gnostr_nip49_new(void);

/**
 * gnostr_nip49_encrypt:
 * @self: a #GNostrNip49
 * @privkey_hex: the 32-byte private key as 64 hex characters
 * @password: the password (UTF-8, NFKC-normalized internally)
 * @security: the security level to record
 * @log_n: scrypt exponent (e.g. 16 for fast, 21 for secure)
 * @error: (nullable): return location for a #GError
 *
 * Encrypts a private key into an ncryptsec bech32 string.
 * On success, the "ncryptsec" property is updated.
 *
 * Returns: (transfer full) (nullable): the ncryptsec string, or %NULL on error
 */
gchar *gnostr_nip49_encrypt(GNostrNip49 *self,
                             const gchar *privkey_hex,
                             const gchar *password,
                             GNostrNip49SecurityLevel security,
                             guint8 log_n,
                             GError **error);

/**
 * gnostr_nip49_decrypt:
 * @self: a #GNostrNip49
 * @ncryptsec: the bech32-encoded ncryptsec string
 * @password: the password (UTF-8, NFKC-normalized internally)
 * @error: (nullable): return location for a #GError
 *
 * Decrypts an ncryptsec string to recover the private key.
 * On success, updates the "ncryptsec", "security-level", and "log-n" properties.
 *
 * Returns: (transfer full) (nullable): the private key as 64 hex characters, or %NULL on error.
 *   The caller should securely wipe and free the returned string when done.
 */
gchar *gnostr_nip49_decrypt(GNostrNip49 *self,
                             const gchar *ncryptsec,
                             const gchar *password,
                             GError **error);

/**
 * gnostr_nip49_decrypt_to_keys:
 * @self: a #GNostrNip49
 * @ncryptsec: the bech32-encoded ncryptsec string
 * @password: the password (UTF-8, NFKC-normalized internally)
 * @error: (nullable): return location for a #GError
 *
 * Decrypts an ncryptsec string and returns a #GNostrKeys instance.
 * Convenience method that combines decrypt + GNostrKeys creation.
 *
 * Returns: (transfer full) (nullable): a new #GNostrKeys with the decrypted key, or %NULL on error
 */
GObject *gnostr_nip49_decrypt_to_keys(GNostrNip49 *self,
                                       const gchar *ncryptsec,
                                       const gchar *password,
                                       GError **error);

/**
 * gnostr_nip49_get_ncryptsec:
 * @self: a #GNostrNip49
 *
 * Gets the current ncryptsec string (set after encrypt or decrypt).
 *
 * Returns: (transfer none) (nullable): the ncryptsec string, or %NULL if not set
 */
const gchar *gnostr_nip49_get_ncryptsec(GNostrNip49 *self);

/**
 * gnostr_nip49_get_security_level:
 * @self: a #GNostrNip49
 *
 * Gets the security level from the last encrypt/decrypt operation.
 *
 * Returns: the #GNostrNip49SecurityLevel
 */
GNostrNip49SecurityLevel gnostr_nip49_get_security_level(GNostrNip49 *self);

/**
 * gnostr_nip49_get_log_n:
 * @self: a #GNostrNip49
 *
 * Gets the scrypt exponent from the last encrypt/decrypt operation.
 *
 * Returns: the log_n value
 */
guint8 gnostr_nip49_get_log_n(GNostrNip49 *self);

G_END_DECLS

#endif /* GNOSTR_NIP49_H */
