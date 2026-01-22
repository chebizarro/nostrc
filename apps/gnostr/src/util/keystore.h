/**
 * Secure Key Storage API
 *
 * Platform-native secure storage for Nostr private keys:
 * - Linux: libsecret (GNOME Keyring / KDE Wallet)
 * - macOS: Keychain Services
 *
 * Keys are identified by their npub (bech32 public key) and stored encrypted
 * with the user's authentication credentials.
 */

#ifndef GNOSTR_KEYSTORE_H
#define GNOSTR_KEYSTORE_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrKeyInfo:
 * @npub: The bech32-encoded public key (npub1...)
 * @label: Human-readable label for the key
 * @created_at: Unix timestamp when the key was stored
 *
 * Information about a stored key (does not contain the actual secret).
 */
typedef struct {
  char *npub;
  char *label;
  gint64 created_at;
} GnostrKeyInfo;

/**
 * gnostr_keystore_available:
 *
 * Check if secure key storage is available on this platform.
 *
 * Returns: %TRUE if key storage is available, %FALSE otherwise.
 */
gboolean gnostr_keystore_available(void);

/**
 * gnostr_keystore_store_key:
 * @npub: The bech32-encoded public key (npub1...) used as identifier
 * @nsec: The bech32-encoded private key (nsec1...) to store
 * @label: (nullable): Human-readable label for the key
 * @error: (out) (optional): Return location for error
 *
 * Store a private key in the platform's secure storage.
 * The key is identified by its corresponding public key (npub).
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean gnostr_keystore_store_key(const char *npub,
                                    const char *nsec,
                                    const char *label,
                                    GError **error);

/**
 * gnostr_keystore_store_key_async:
 * @npub: The bech32-encoded public key (npub1...)
 * @nsec: The bech32-encoded private key (nsec1...)
 * @label: (nullable): Human-readable label
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_keystore_store_key().
 */
void gnostr_keystore_store_key_async(const char *npub,
                                      const char *nsec,
                                      const char *label,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/**
 * gnostr_keystore_store_key_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async store operation.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean gnostr_keystore_store_key_finish(GAsyncResult *result,
                                           GError **error);

/**
 * gnostr_keystore_retrieve_key:
 * @npub: The bech32-encoded public key (npub1...) identifying the key
 * @error: (out) (optional): Return location for error
 *
 * Retrieve a private key from the platform's secure storage.
 *
 * Returns: (transfer full): The nsec on success, %NULL on error. Free with g_free().
 */
char *gnostr_keystore_retrieve_key(const char *npub, GError **error);

/**
 * gnostr_keystore_retrieve_key_async:
 * @npub: The bech32-encoded public key (npub1...)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_keystore_retrieve_key().
 */
void gnostr_keystore_retrieve_key_async(const char *npub,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/**
 * gnostr_keystore_retrieve_key_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async retrieve operation.
 *
 * Returns: (transfer full): The nsec on success, %NULL on error.
 */
char *gnostr_keystore_retrieve_key_finish(GAsyncResult *result,
                                           GError **error);

/**
 * gnostr_keystore_delete_key:
 * @npub: The bech32-encoded public key (npub1...) identifying the key to delete
 * @error: (out) (optional): Return location for error
 *
 * Delete a private key from the platform's secure storage.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean gnostr_keystore_delete_key(const char *npub, GError **error);

/**
 * gnostr_keystore_delete_key_async:
 * @npub: The bech32-encoded public key (npub1...)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_keystore_delete_key().
 */
void gnostr_keystore_delete_key_async(const char *npub,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

/**
 * gnostr_keystore_delete_key_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async delete operation.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean gnostr_keystore_delete_key_finish(GAsyncResult *result,
                                            GError **error);

/**
 * gnostr_keystore_list_keys:
 * @error: (out) (optional): Return location for error
 *
 * List all stored keys (without exposing the private keys).
 *
 * Returns: (transfer full) (element-type GnostrKeyInfo): A list of key info,
 *          or %NULL on error. Free with g_list_free_full(list, gnostr_key_info_free).
 */
GList *gnostr_keystore_list_keys(GError **error);

/**
 * gnostr_keystore_list_keys_async:
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_keystore_list_keys().
 */
void gnostr_keystore_list_keys_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/**
 * gnostr_keystore_list_keys_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async list operation.
 *
 * Returns: (transfer full) (element-type GnostrKeyInfo): A list of key info.
 */
GList *gnostr_keystore_list_keys_finish(GAsyncResult *result,
                                         GError **error);

/**
 * gnostr_keystore_has_key:
 * @npub: The bech32-encoded public key (npub1...)
 *
 * Check if a key exists in secure storage.
 *
 * Returns: %TRUE if the key exists, %FALSE otherwise.
 */
gboolean gnostr_keystore_has_key(const char *npub);

/**
 * gnostr_key_info_free:
 * @info: A #GnostrKeyInfo
 *
 * Free a key info structure.
 */
void gnostr_key_info_free(GnostrKeyInfo *info);

/**
 * gnostr_key_info_copy:
 * @info: A #GnostrKeyInfo
 *
 * Copy a key info structure.
 *
 * Returns: (transfer full): A copy of the key info.
 */
GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info);

/* Error domain */
#define GNOSTR_KEYSTORE_ERROR (gnostr_keystore_error_quark())
GQuark gnostr_keystore_error_quark(void);

/**
 * GnostrKeystoreError:
 * @GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE: Secure storage not available
 * @GNOSTR_KEYSTORE_ERROR_NOT_FOUND: Key not found
 * @GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED: Access denied (user cancelled auth)
 * @GNOSTR_KEYSTORE_ERROR_INVALID_KEY: Invalid key format
 * @GNOSTR_KEYSTORE_ERROR_STORAGE_FULL: Storage is full
 * @GNOSTR_KEYSTORE_ERROR_FAILED: Generic failure
 */
typedef enum {
  GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE,
  GNOSTR_KEYSTORE_ERROR_NOT_FOUND,
  GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED,
  GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
  GNOSTR_KEYSTORE_ERROR_STORAGE_FULL,
  GNOSTR_KEYSTORE_ERROR_FAILED
} GnostrKeystoreError;

G_END_DECLS

#endif /* GNOSTR_KEYSTORE_H */
