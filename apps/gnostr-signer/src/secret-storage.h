/* secret-storage.h - Secure key storage abstraction for gnostr-signer
 *
 * This module provides a platform-independent API for securely storing
 * Nostr private keys using the system's secret service:
 *   - Linux: libsecret (GNOME Keyring / KDE Wallet via D-Bus Secret Service)
 *   - macOS: Security.framework Keychain
 *
 * Keys are stored with metadata attributes for easy management and
 * identification across multiple accounts.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_SECRET_STORAGE_H
#define APPS_GNOSTR_SIGNER_SECRET_STORAGE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GnSecretStorageError:
 * @GN_SECRET_STORAGE_ERROR_FAILED: General failure
 * @GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE: Secret service not available
 * @GN_SECRET_STORAGE_ERROR_NOT_FOUND: Key not found
 * @GN_SECRET_STORAGE_ERROR_PERMISSION_DENIED: Access denied
 * @GN_SECRET_STORAGE_ERROR_INVALID_DATA: Invalid key data
 * @GN_SECRET_STORAGE_ERROR_ALREADY_EXISTS: Key with same label already exists
 *
 * Error codes for secret storage operations.
 */
typedef enum {
  GN_SECRET_STORAGE_ERROR_FAILED,
  GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
  GN_SECRET_STORAGE_ERROR_NOT_FOUND,
  GN_SECRET_STORAGE_ERROR_PERMISSION_DENIED,
  GN_SECRET_STORAGE_ERROR_INVALID_DATA,
  GN_SECRET_STORAGE_ERROR_ALREADY_EXISTS
} GnSecretStorageError;

#define GN_SECRET_STORAGE_ERROR (gn_secret_storage_error_quark())
GQuark gn_secret_storage_error_quark(void);

/**
 * GnSecretStorageKeyInfo:
 * @label: User-defined label for the key
 * @npub: Public key in bech32 (npub1...) format
 * @key_type: Type of key ("nostr", "nip49", etc.)
 * @created_at: ISO 8601 timestamp of creation
 * @application: Application name that stored the key
 *
 * Information about a stored key entry.
 */
typedef struct {
  gchar *label;
  gchar *npub;
  gchar *key_type;
  gchar *created_at;
  gchar *application;
} GnSecretStorageKeyInfo;

/**
 * gn_secret_storage_key_info_free:
 * @info: A #GnSecretStorageKeyInfo to free
 *
 * Frees a #GnSecretStorageKeyInfo structure and all its members.
 */
void gn_secret_storage_key_info_free(GnSecretStorageKeyInfo *info);

/**
 * gn_secret_storage_init:
 * @error: Return location for error, or %NULL
 *
 * Initialize the secret service connection. This function should be called
 * once at application startup. On Linux, this connects to the D-Bus Secret
 * Service. On macOS, this prepares the Keychain access.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_secret_storage_init(GError **error);

/**
 * gn_secret_storage_shutdown:
 *
 * Release resources associated with the secret service connection.
 * Call this at application shutdown.
 */
void gn_secret_storage_shutdown(void);

/**
 * gn_secret_storage_is_available:
 *
 * Check if the secret storage backend is available and functional.
 *
 * Returns: %TRUE if secret storage is available
 */
gboolean gn_secret_storage_is_available(void);

/**
 * gn_secret_storage_get_backend_name:
 *
 * Get the name of the active secret storage backend.
 *
 * Returns: (transfer none): Backend name ("libsecret", "Keychain", or "none")
 */
const gchar *gn_secret_storage_get_backend_name(void);

/**
 * gn_secret_storage_store_key:
 * @label: User-defined label to identify this key
 * @nsec: Private key in nsec1 bech32 format or 64-char hex
 * @error: Return location for error, or %NULL
 *
 * Store a private key securely in the system's secret service.
 * The key is stored with metadata including:
 *   - Application name ("gnostr-signer")
 *   - Key type ("nostr")
 *   - Creation timestamp
 *   - Derived public key (npub)
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_secret_storage_store_key(const gchar *label,
                                     const gchar *nsec,
                                     GError **error);

/**
 * gn_secret_storage_retrieve_key:
 * @label: Label of the key to retrieve
 * @error: Return location for error, or %NULL
 *
 * Retrieve a private key from the secret storage by its label.
 *
 * Returns: (transfer full): The nsec1 bech32-encoded private key, or %NULL on error.
 *                           Free with g_free().
 */
gchar *gn_secret_storage_retrieve_key(const gchar *label, GError **error);

/**
 * gn_secret_storage_delete_key:
 * @label: Label of the key to delete
 * @error: Return location for error, or %NULL
 *
 * Delete a stored key from the secret storage.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gn_secret_storage_delete_key(const gchar *label, GError **error);

/**
 * gn_secret_storage_list_keys:
 * @error: Return location for error, or %NULL
 *
 * List all stored key labels for this application.
 *
 * Returns: (transfer full) (element-type GnSecretStorageKeyInfo):
 *          A #GPtrArray of #GnSecretStorageKeyInfo pointers, or %NULL on error.
 *          Free with g_ptr_array_unref().
 */
GPtrArray *gn_secret_storage_list_keys(GError **error);

/**
 * gn_secret_storage_key_exists:
 * @label: Label to check
 *
 * Check if a key with the given label exists in storage.
 *
 * Returns: %TRUE if the key exists
 */
gboolean gn_secret_storage_key_exists(const gchar *label);

/**
 * gn_secret_storage_get_key_info:
 * @label: Label of the key
 * @error: Return location for error, or %NULL
 *
 * Get metadata information about a stored key without retrieving
 * the secret itself.
 *
 * Returns: (transfer full): A #GnSecretStorageKeyInfo, or %NULL on error.
 *                           Free with gn_secret_storage_key_info_free().
 */
GnSecretStorageKeyInfo *gn_secret_storage_get_key_info(const gchar *label,
                                                        GError **error);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SECRET_STORAGE_H */
