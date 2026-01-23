/* backup-recovery.h - Backup and recovery functionality for gnostr-signer
 *
 * Provides NIP-49 encrypted backup (ncryptsec) and BIP-39 mnemonic
 * export/import for Nostr identity keys.
 *
 * NIP-49 uses scrypt for key derivation and XChaCha20-Poly1305 for encryption.
 * Mnemonic support uses NIP-06 (BIP-39/BIP-32 derivation path m/44'/1237'/0'/0/0).
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Error domain for backup/recovery operations */
#define GN_BACKUP_ERROR (gn_backup_error_quark())
GQuark gn_backup_error_quark(void);

/* Error codes for backup/recovery operations */
typedef enum {
  GN_BACKUP_ERROR_INVALID_KEY,       /* Invalid key format (not nsec/hex) */
  GN_BACKUP_ERROR_INVALID_PASSWORD,  /* Password is NULL or empty */
  GN_BACKUP_ERROR_INVALID_ENCRYPTED, /* Invalid ncryptsec format */
  GN_BACKUP_ERROR_DECRYPT_FAILED,    /* Decryption failed (wrong password or corrupted) */
  GN_BACKUP_ERROR_ENCRYPT_FAILED,    /* Encryption failed */
  GN_BACKUP_ERROR_INVALID_MNEMONIC,  /* Invalid mnemonic (wrong word count or checksum) */
  GN_BACKUP_ERROR_DERIVATION_FAILED, /* Key derivation from mnemonic failed */
  GN_BACKUP_ERROR_FILE_IO,           /* File I/O error */
  GN_BACKUP_ERROR_NOT_AVAILABLE      /* Feature not available */
} GnBackupError;

/* NIP-49 security level for key derivation
 * Higher log_n values provide better security but slower derivation */
typedef enum {
  GN_BACKUP_SECURITY_FAST = 16,      /* ~0.1s - for testing/development */
  GN_BACKUP_SECURITY_NORMAL = 19,    /* ~1s - reasonable security */
  GN_BACKUP_SECURITY_HIGH = 21,      /* ~4s - high security */
  GN_BACKUP_SECURITY_PARANOID = 22   /* ~8s - maximum security */
} GnBackupSecurityLevel;

/* Export a private key as NIP-49 encrypted string (ncryptsec).
 *
 * @nsec: Private key in nsec1... bech32 format or 64-character hex
 * @password: Password for encryption (UTF-8, will be NFKC normalized)
 * @security: Security level (scrypt log_n parameter)
 * @out_ncryptsec: (out): Output ncryptsec string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_export_nip49(const gchar *nsec,
                                 const gchar *password,
                                 GnBackupSecurityLevel security,
                                 gchar **out_ncryptsec,
                                 GError **error);

/* Import a private key from NIP-49 encrypted string.
 *
 * @encrypted: NIP-49 encrypted string (ncryptsec1...)
 * @password: Password for decryption (UTF-8)
 * @out_nsec: (out): Output nsec string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_import_nip49(const gchar *encrypted,
                                 const gchar *password,
                                 gchar **out_nsec,
                                 GError **error);

/* Export a private key as BIP-39 mnemonic (if key was derived from mnemonic).
 *
 * Note: This only works if the key was originally derived from a mnemonic
 * using NIP-06 derivation. For keys generated directly, this will fail.
 *
 * @nsec: Private key in nsec1... bech32 format or 64-character hex
 * @out_mnemonic: (out): Output mnemonic string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 *
 * Note: Currently not implemented as recovering mnemonic from derived key
 * is not possible. This function always returns FALSE with GN_BACKUP_ERROR_NOT_AVAILABLE.
 */
gboolean gn_backup_export_mnemonic(const gchar *nsec,
                                    gchar **out_mnemonic,
                                    GError **error);

/* Import a private key from BIP-39 mnemonic using NIP-06 derivation.
 *
 * @mnemonic: BIP-39 mnemonic (12/15/18/21/24 English words)
 * @passphrase: Optional BIP-39 passphrase (can be NULL for empty)
 * @account: Account index for derivation (usually 0)
 * @out_nsec: (out): Output nsec string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_import_mnemonic(const gchar *mnemonic,
                                    const gchar *passphrase,
                                    guint account,
                                    gchar **out_nsec,
                                    GError **error);

/* Generate a new BIP-39 mnemonic and derive a key from it.
 *
 * @word_count: Number of words (12, 15, 18, 21, or 24)
 * @passphrase: Optional BIP-39 passphrase (can be NULL for empty)
 * @out_mnemonic: (out): Output mnemonic string (caller frees with g_free)
 * @out_nsec: (out): Output nsec string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_generate_mnemonic(gint word_count,
                                      const gchar *passphrase,
                                      gchar **out_mnemonic,
                                      gchar **out_nsec,
                                      GError **error);

/* Export a private key to a file as NIP-49 encrypted backup.
 *
 * @nsec: Private key in nsec1... bech32 format or 64-character hex
 * @password: Password for encryption (UTF-8)
 * @security: Security level
 * @filepath: Path to output file
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_export_to_file(const gchar *nsec,
                                   const gchar *password,
                                   GnBackupSecurityLevel security,
                                   const gchar *filepath,
                                   GError **error);

/* Import a private key from a NIP-49 encrypted backup file.
 *
 * @filepath: Path to input file
 * @password: Password for decryption (UTF-8)
 * @out_nsec: (out): Output nsec string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_import_from_file(const gchar *filepath,
                                     const gchar *password,
                                     gchar **out_nsec,
                                     GError **error);

/* ============================================================
 * Backup File Format with Metadata
 * ============================================================
 *
 * These functions export/import backup files in a JSON format that
 * includes metadata alongside the encrypted key:
 *
 * {
 *   "version": 1,
 *   "format": "gnostr-backup",
 *   "created_at": "2025-01-23T12:00:00Z",
 *   "identity_name": "My Nostr Key",
 *   "npub": "npub1...",
 *   "ncryptsec": "ncryptsec1...",
 *   "security_level": "normal"
 * }
 */

/* Metadata structure for backup files */
typedef struct {
  gchar *identity_name;    /* User-friendly name for the identity */
  gchar *npub;             /* Public key (npub1...) */
  gchar *created_at;       /* ISO 8601 timestamp */
  gchar *ncryptsec;        /* The encrypted key */
  GnBackupSecurityLevel security_level;
  gint version;            /* Format version */
} GnBackupMetadata;

/* Free a GnBackupMetadata structure */
void gn_backup_metadata_free(GnBackupMetadata *meta);

/* Export a private key to a file with metadata.
 *
 * Creates a JSON file containing the encrypted key and metadata.
 *
 * @nsec: Private key in nsec1... bech32 format or 64-character hex
 * @password: Password for encryption (UTF-8)
 * @security: Security level
 * @identity_name: (nullable): User-friendly name for the identity
 * @filepath: Path to output file
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_export_to_file_with_metadata(const gchar *nsec,
                                                  const gchar *password,
                                                  GnBackupSecurityLevel security,
                                                  const gchar *identity_name,
                                                  const gchar *filepath,
                                                  GError **error);

/* Import a private key from a backup file (with or without metadata).
 *
 * This function handles both the new JSON format with metadata and
 * the legacy plain ncryptsec format for backwards compatibility.
 *
 * @filepath: Path to input file
 * @password: Password for decryption (UTF-8)
 * @out_nsec: (out): Output nsec string (caller frees with g_free)
 * @out_metadata: (out) (optional): Output metadata (caller frees with gn_backup_metadata_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_import_from_file_with_metadata(const gchar *filepath,
                                                    const gchar *password,
                                                    gchar **out_nsec,
                                                    GnBackupMetadata **out_metadata,
                                                    GError **error);

/* Create backup metadata as a JSON string (for display or custom storage).
 *
 * @nsec: Private key to derive npub from
 * @password: Password for encryption
 * @security: Security level
 * @identity_name: (nullable): User-friendly name
 * @out_json: (out): Output JSON string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_create_metadata_json(const gchar *nsec,
                                          const gchar *password,
                                          GnBackupSecurityLevel security,
                                          const gchar *identity_name,
                                          gchar **out_json,
                                          GError **error);

/* Parse backup metadata from a JSON string.
 *
 * @json: JSON string to parse
 * @out_metadata: (out): Output metadata (caller frees with gn_backup_metadata_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_parse_metadata_json(const gchar *json,
                                         GnBackupMetadata **out_metadata,
                                         GError **error);

/* Validate a NIP-49 encrypted string format (without decrypting).
 *
 * @encrypted: String to validate
 *
 * Returns: TRUE if the format appears valid
 */
gboolean gn_backup_validate_ncryptsec(const gchar *encrypted);

/* Validate a BIP-39 mnemonic (word count and checksum).
 *
 * @mnemonic: Mnemonic to validate
 *
 * Returns: TRUE if the mnemonic is valid
 */
gboolean gn_backup_validate_mnemonic(const gchar *mnemonic);

/* Get the public key (npub) for an nsec.
 *
 * @nsec: Private key in nsec1... bech32 format or 64-character hex
 * @out_npub: (out): Output npub string (caller frees with g_free)
 * @error: (out): Error information
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_backup_get_npub(const gchar *nsec,
                             gchar **out_npub,
                             GError **error);

G_END_DECLS
