/**
 * GNostr Identity Management
 *
 * High-level identity management combining:
 * - Secure key storage (keystore)
 * - GSettings for preferences
 * - NIP-19 encoding/decoding
 *
 * The schema_id must be set via gnostr_identity_init() before
 * calling any GSettings-dependent functions. The library has no
 * opinion about schema names - apps provide their own.
 */

#ifndef GNOSTR_IDENTITY_H
#define GNOSTR_IDENTITY_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrIdentity:
 * @npub: The bech32-encoded public key (npub1...)
 * @label: Human-readable label (e.g., NIP-05 name)
 * @has_local_key: Whether nsec is stored in secure storage
 * @signer_type: Type of signer ("local", "nip55l", "nip46", "none")
 *
 * Represents a user identity in the app.
 */
typedef struct {
  char *npub;
  char *label;
  gboolean has_local_key;
  char *signer_type;
} GnostrIdentity;

/**
 * gnostr_identity_init:
 * @schema_id: GSettings schema ID for identity settings (e.g., "org.gnostr.Client")
 *
 * Initialize the identity module with the GSettings schema to use.
 * Must be called before gnostr_identity_get_current() or
 * gnostr_identity_set_current().
 */
void gnostr_identity_init(const char *schema_id);

/**
 * gnostr_identity_free:
 * @identity: A #GnostrIdentity
 *
 * Free an identity structure.
 */
void gnostr_identity_free(GnostrIdentity *identity);

/**
 * gnostr_identity_copy:
 * @identity: A #GnostrIdentity
 *
 * Copy an identity structure.
 *
 * Returns: (transfer full): A copy of the identity.
 */
GnostrIdentity *gnostr_identity_copy(const GnostrIdentity *identity);

/**
 * gnostr_identity_get_current:
 *
 * Get the currently active identity from GSettings.
 * Requires gnostr_identity_init() to have been called.
 *
 * Returns: (transfer full) (nullable): The current identity, or %NULL if not logged in.
 */
GnostrIdentity *gnostr_identity_get_current(void);

/**
 * gnostr_identity_set_current:
 * @npub: The npub to set as current (or %NULL to log out)
 *
 * Set the currently active identity.
 * Requires gnostr_identity_init() to have been called.
 */
void gnostr_identity_set_current(const char *npub);

/**
 * gnostr_identity_list_stored:
 * @error: (out) (optional): Return location for error
 *
 * List all identities with keys stored in secure storage.
 *
 * Returns: (transfer full) (element-type GnostrIdentity): List of identities.
 */
GList *gnostr_identity_list_stored(GError **error);

/**
 * gnostr_identity_import_nsec:
 * @nsec: The bech32-encoded private key (nsec1...)
 * @label: (nullable): Optional label for the key
 * @error: (out) (optional): Return location for error
 *
 * Import a private key into secure storage.
 * Derives the npub from the nsec and stores both.
 *
 * Returns: (transfer full): The npub on success, %NULL on error.
 */
char *gnostr_identity_import_nsec(const char *nsec,
                                   const char *label,
                                   GError **error);

/**
 * gnostr_identity_import_nsec_async:
 * @nsec: The bech32-encoded private key (nsec1...)
 * @label: (nullable): Optional label
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Async version of gnostr_identity_import_nsec().
 */
void gnostr_identity_import_nsec_async(const char *nsec,
                                        const char *label,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

/**
 * gnostr_identity_import_nsec_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finish async import operation.
 *
 * Returns: (transfer full): The npub on success, %NULL on error.
 */
char *gnostr_identity_import_nsec_finish(GAsyncResult *result,
                                          GError **error);

/**
 * gnostr_identity_get_nsec:
 * @npub: The public key identifying the identity
 * @error: (out) (optional): Return location for error
 *
 * Retrieve the private key for an identity from secure storage.
 * WARNING: Handle the returned nsec with care and clear it when done.
 *
 * Returns: (transfer full): The nsec on success, %NULL on error.
 */
char *gnostr_identity_get_nsec(const char *npub, GError **error);

/**
 * gnostr_identity_get_nsec_async:
 * @npub: The public key identifying the identity
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Async version of gnostr_identity_get_nsec().
 */
void gnostr_identity_get_nsec_async(const char *npub,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);

/**
 * gnostr_identity_get_nsec_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finish async get nsec operation.
 *
 * Returns: (transfer full): The nsec on success, %NULL on error.
 */
char *gnostr_identity_get_nsec_finish(GAsyncResult *result,
                                       GError **error);

/**
 * gnostr_identity_delete:
 * @npub: The public key identifying the identity to delete
 * @error: (out) (optional): Return location for error
 *
 * Delete an identity from secure storage.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean gnostr_identity_delete(const char *npub, GError **error);

/**
 * gnostr_identity_has_local_key:
 * @npub: The public key to check
 *
 * Check if an identity has a locally stored private key.
 *
 * Returns: %TRUE if key is stored locally.
 */
gboolean gnostr_identity_has_local_key(const char *npub);

/**
 * gnostr_identity_secure_storage_available:
 *
 * Check if secure key storage is available on this platform.
 *
 * Returns: %TRUE if secure storage is available.
 */
gboolean gnostr_identity_secure_storage_available(void);

/**
 * gnostr_identity_clear_nsec:
 * @nsec: The nsec string to clear
 *
 * Securely clear an nsec string from memory.
 * Sets all bytes to zero before freeing.
 */
void gnostr_identity_clear_nsec(char *nsec);

G_END_DECLS

#endif /* GNOSTR_IDENTITY_H */
