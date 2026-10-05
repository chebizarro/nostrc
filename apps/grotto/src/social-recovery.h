/* social-recovery.h - Social recovery using Shamir's Secret Sharing
 *
 * Provides functionality to split a private key into shares that can be
 * distributed to trusted guardians. A configurable threshold (k-of-n) of
 * shares is required to reconstruct the original key.
 *
 * Features:
 * - Shamir's Secret Sharing (SSS) for key splitting
 * - Guardian management with npub identifiers
 * - NIP-04 encryption of shares before distribution
 * - Guardian configuration persistence
 * - Recovery flow with share collection
 *
 * Security considerations:
 * - Shares are encrypted with NIP-04 before sending to guardians
 * - Original key is never stored after splitting
 * - Threshold must be at least 2 to prevent single point of failure
 * - Maximum 255 shares supported (GF(2^8) arithmetic)
 */
#ifndef APPS_GNOSTR_SIGNER_SOCIAL_RECOVERY_H
#define APPS_GNOSTR_SIGNER_SOCIAL_RECOVERY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Error domain for social recovery operations */
#define GN_SOCIAL_RECOVERY_ERROR (gn_social_recovery_error_quark())
GQuark gn_social_recovery_error_quark(void);

/* Error codes */
typedef enum {
  GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,    /* Invalid threshold/shares count */
  GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,       /* Invalid key format */
  GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,     /* Invalid or corrupted share */
  GN_SOCIAL_RECOVERY_ERROR_THRESHOLD_NOT_MET, /* Not enough shares provided */
  GN_SOCIAL_RECOVERY_ERROR_RECONSTRUCTION,    /* Failed to reconstruct key */
  GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,        /* Share encryption failed */
  GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,        /* Share decryption failed */
  GN_SOCIAL_RECOVERY_ERROR_STORAGE,           /* Failed to store/load config */
  GN_SOCIAL_RECOVERY_ERROR_GUARDIAN_NOT_FOUND /* Guardian npub not in config */
} GnSocialRecoveryError;

/* A single share from Shamir's Secret Sharing */
typedef struct {
  guint8 index;           /* Share index (1-255, 0 is reserved) */
  guint8 *data;           /* Share data (same length as secret) */
  gsize data_len;         /* Length of share data */
} GnSSSShare;

/* A guardian who holds a share */
typedef struct {
  gchar *npub;            /* Guardian's public key (npub1...) */
  gchar *label;           /* User-friendly name for the guardian */
  guint8 share_index;     /* Index of the share assigned to this guardian */
  gint64 assigned_at;     /* Unix timestamp when share was assigned */
  gboolean confirmed;     /* Whether guardian confirmed receipt */
} GnGuardian;

/* Configuration for social recovery of an identity */
typedef struct {
  gchar *owner_npub;      /* The identity being protected (npub1...) */
  guint8 threshold;       /* Minimum shares needed to recover (k) */
  guint8 total_shares;    /* Total number of shares created (n) */
  GPtrArray *guardians;   /* Array of GnGuardian* */
  gint64 created_at;      /* Unix timestamp when recovery was set up */
  gint64 last_verified;   /* Unix timestamp of last verification test */
  gchar *version;         /* Configuration version for migrations */
} GnRecoveryConfig;

/* ============================================================
 * Shamir's Secret Sharing Core
 * ============================================================ */

/**
 * gn_sss_split:
 * @secret: The secret to split (e.g., 32-byte private key)
 * @secret_len: Length of the secret
 * @threshold: Minimum shares needed to reconstruct (k)
 * @total_shares: Total number of shares to create (n)
 * @out_shares: (out): Output array of shares (caller frees with gn_sss_shares_free)
 * @error: (out): Error information
 *
 * Split a secret into multiple shares using Shamir's Secret Sharing.
 *
 * Constraints:
 * - threshold >= 2 (single share shouldn't reveal secret)
 * - threshold <= total_shares <= 255
 * - secret_len > 0
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_sss_split(const guint8 *secret,
                      gsize secret_len,
                      guint8 threshold,
                      guint8 total_shares,
                      GPtrArray **out_shares,
                      GError **error);

/**
 * gn_sss_combine:
 * @shares: Array of GnSSSShare* (at least threshold shares)
 * @threshold: The threshold used when splitting
 * @out_secret: (out): Reconstructed secret (caller frees with gnostr_secure_free)
 * @out_len: (out): Length of reconstructed secret
 * @error: (out): Error information
 *
 * Reconstruct a secret from shares using Lagrange interpolation.
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_sss_combine(GPtrArray *shares,
                        guint8 threshold,
                        guint8 **out_secret,
                        gsize *out_len,
                        GError **error);

/**
 * gn_sss_share_free:
 * @share: Share to free (NULL is safe)
 *
 * Securely clear and free a share.
 */
void gn_sss_share_free(GnSSSShare *share);

/**
 * gn_sss_shares_free:
 * @shares: Array of shares to free (NULL is safe)
 *
 * Free an array of shares, securely clearing each one.
 */
void gn_sss_shares_free(GPtrArray *shares);

/* ============================================================
 * Share Encoding/Decoding
 * ============================================================ */

/**
 * gn_sss_share_encode:
 * @share: Share to encode
 *
 * Encode a share as a base64 string for transport.
 * Format: "sss1:<index>:<base64_data>"
 *
 * Returns: Newly allocated string, or NULL on error
 */
gchar *gn_sss_share_encode(const GnSSSShare *share);

/**
 * gn_sss_share_decode:
 * @encoded: Encoded share string
 * @error: (out): Error information
 *
 * Decode a share from its string representation.
 *
 * Returns: Newly allocated share, or NULL on error
 */
GnSSSShare *gn_sss_share_decode(const gchar *encoded, GError **error);

/**
 * gn_sss_share_validate:
 * @encoded: Encoded share string
 *
 * Validate that a string appears to be a valid encoded share.
 *
 * Returns: TRUE if format is valid
 */
gboolean gn_sss_share_validate(const gchar *encoded);

/* ============================================================
 * Guardian Management
 * ============================================================ */

/**
 * gn_guardian_new:
 * @npub: Guardian's public key
 * @label: (nullable): Display name
 *
 * Create a new guardian entry.
 *
 * Returns: New guardian (caller frees with gn_guardian_free)
 */
GnGuardian *gn_guardian_new(const gchar *npub, const gchar *label);

/**
 * gn_guardian_free:
 * @guardian: Guardian to free (NULL is safe)
 */
void gn_guardian_free(GnGuardian *guardian);

/**
 * gn_guardian_dup:
 * @guardian: Guardian to duplicate
 *
 * Returns: New copy of guardian
 */
GnGuardian *gn_guardian_dup(const GnGuardian *guardian);

/* ============================================================
 * Recovery Configuration
 * ============================================================ */

/**
 * gn_recovery_config_new:
 * @owner_npub: The identity being protected
 *
 * Create a new empty recovery configuration.
 *
 * Returns: New config (caller frees with gn_recovery_config_free)
 */
GnRecoveryConfig *gn_recovery_config_new(const gchar *owner_npub);

/**
 * gn_recovery_config_free:
 * @config: Config to free (NULL is safe)
 */
void gn_recovery_config_free(GnRecoveryConfig *config);

/**
 * gn_recovery_config_add_guardian:
 * @config: Recovery configuration
 * @guardian: Guardian to add (config takes ownership)
 *
 * Add a guardian to the configuration.
 *
 * Returns: TRUE on success, FALSE if guardian npub already exists
 */
gboolean gn_recovery_config_add_guardian(GnRecoveryConfig *config,
                                         GnGuardian *guardian);

/**
 * gn_recovery_config_remove_guardian:
 * @config: Recovery configuration
 * @npub: Guardian's npub to remove
 *
 * Remove a guardian from the configuration.
 *
 * Returns: TRUE if removed, FALSE if not found
 */
gboolean gn_recovery_config_remove_guardian(GnRecoveryConfig *config,
                                            const gchar *npub);

/**
 * gn_recovery_config_find_guardian:
 * @config: Recovery configuration
 * @npub: Guardian's npub
 *
 * Find a guardian by npub.
 *
 * Returns: Guardian pointer (owned by config), or NULL if not found
 */
GnGuardian *gn_recovery_config_find_guardian(GnRecoveryConfig *config,
                                             const gchar *npub);

/* ============================================================
 * High-Level Recovery Operations
 * ============================================================ */

/**
 * gn_social_recovery_setup:
 * @nsec: The private key to protect (nsec1... or hex)
 * @threshold: Minimum guardians needed for recovery
 * @guardians: Array of GnGuardian* (will be assigned shares)
 * @out_config: (out): Recovery configuration with assigned shares
 * @out_encrypted_shares: (out): GPtrArray of encrypted share strings (one per guardian)
 * @error: (out): Error information
 *
 * Set up social recovery for an identity.
 *
 * This function:
 * 1. Splits the key using Shamir's Secret Sharing
 * 2. Encrypts each share for its guardian using NIP-04
 * 3. Creates a recovery configuration
 *
 * The encrypted shares should be sent to each guardian (e.g., via DM).
 * The configuration should be stored locally.
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gn_social_recovery_setup(const gchar *nsec,
                                  guint8 threshold,
                                  GPtrArray *guardians,
                                  GnRecoveryConfig **out_config,
                                  GPtrArray **out_encrypted_shares,
                                  GError **error);

/**
 * gn_social_recovery_encrypt_share:
 * @share: The share to encrypt
 * @owner_nsec: Private key of the identity owner (for NIP-04)
 * @guardian_npub: Public key of the guardian
 * @out_encrypted: (out): Encrypted share as JSON string
 * @error: (out): Error information
 *
 * Encrypt a share for a specific guardian using NIP-04.
 *
 * Returns: TRUE on success
 */
gboolean gn_social_recovery_encrypt_share(const GnSSSShare *share,
                                          const gchar *owner_nsec,
                                          const gchar *guardian_npub,
                                          gchar **out_encrypted,
                                          GError **error);

/**
 * gn_social_recovery_decrypt_share:
 * @encrypted: Encrypted share JSON string
 * @guardian_nsec: Private key of the guardian (for NIP-04)
 * @owner_npub: Public key of the identity owner
 * @out_share: (out): Decrypted share
 * @error: (out): Error information
 *
 * Decrypt a share received from the identity owner.
 *
 * Returns: TRUE on success
 */
gboolean gn_social_recovery_decrypt_share(const gchar *encrypted,
                                          const gchar *guardian_nsec,
                                          const gchar *owner_npub,
                                          GnSSSShare **out_share,
                                          GError **error);

/**
 * gn_social_recovery_recover:
 * @collected_shares: Array of decoded GnSSSShare* from guardians
 * @threshold: Recovery threshold
 * @out_nsec: (out): Recovered private key as nsec1...
 * @error: (out): Error information
 *
 * Recover a private key from collected shares.
 *
 * Returns: TRUE on success
 */
gboolean gn_social_recovery_recover(GPtrArray *collected_shares,
                                    guint8 threshold,
                                    gchar **out_nsec,
                                    GError **error);

/* ============================================================
 * Configuration Persistence
 * ============================================================ */

/**
 * gn_recovery_config_save:
 * @config: Configuration to save
 * @error: (out): Error information
 *
 * Save recovery configuration to disk.
 * Configurations are stored per-identity in the app data directory.
 *
 * Returns: TRUE on success
 */
gboolean gn_recovery_config_save(GnRecoveryConfig *config, GError **error);

/**
 * gn_recovery_config_load:
 * @owner_npub: Identity to load configuration for
 * @error: (out): Error information
 *
 * Load recovery configuration from disk.
 *
 * Returns: Configuration, or NULL if not found/error
 */
GnRecoveryConfig *gn_recovery_config_load(const gchar *owner_npub, GError **error);

/**
 * gn_recovery_config_delete:
 * @owner_npub: Identity to delete configuration for
 * @error: (out): Error information
 *
 * Delete stored recovery configuration.
 *
 * Returns: TRUE on success
 */
gboolean gn_recovery_config_delete(const gchar *owner_npub, GError **error);

/**
 * gn_recovery_config_exists:
 * @owner_npub: Identity to check
 *
 * Check if recovery configuration exists for an identity.
 *
 * Returns: TRUE if configuration exists
 */
gboolean gn_recovery_config_exists(const gchar *owner_npub);

/**
 * gn_recovery_config_to_json:
 * @config: Configuration to serialize
 *
 * Serialize configuration to JSON string.
 *
 * Returns: JSON string (caller frees)
 */
gchar *gn_recovery_config_to_json(GnRecoveryConfig *config);

/**
 * gn_recovery_config_from_json:
 * @json: JSON string to parse
 * @error: (out): Error information
 *
 * Deserialize configuration from JSON string.
 *
 * Returns: Configuration, or NULL on error
 */
GnRecoveryConfig *gn_recovery_config_from_json(const gchar *json, GError **error);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/**
 * gn_social_recovery_validate_threshold:
 * @threshold: Proposed threshold
 * @total_guardians: Total number of guardians
 * @error: (out): Error information
 *
 * Validate that a threshold value is acceptable.
 * - threshold >= 2
 * - threshold <= total_guardians
 * - total_guardians <= 255
 *
 * Returns: TRUE if valid
 */
gboolean gn_social_recovery_validate_threshold(guint8 threshold,
                                               guint8 total_guardians,
                                               GError **error);

/**
 * gn_social_recovery_format_share_message:
 * @encrypted_share: The encrypted share JSON
 * @guardian_label: Name of the guardian
 * @owner_npub: Owner's public key
 *
 * Format a message to send to a guardian with their share.
 * Includes instructions for the guardian.
 *
 * Returns: Formatted message string (caller frees)
 */
gchar *gn_social_recovery_format_share_message(const gchar *encrypted_share,
                                               const gchar *guardian_label,
                                               const gchar *owner_npub);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SOCIAL_RECOVERY_H */
