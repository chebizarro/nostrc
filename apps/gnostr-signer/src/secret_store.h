/* secret_store.h - Secure key storage abstraction for gnostr-signer
 *
 * This module provides a unified API for storing Nostr private keys securely
 * using platform-specific backends:
 * - Linux: libsecret (GNOME Keyring / KDE Wallet)
 * - macOS: Security.framework Keychain
 *
 * Keys are stored with metadata (npub, label, owner, fingerprint) for
 * multi-account support with flexible lookup by npub, key_id, or fingerprint.
 *
 * Schema attributes:
 * - key_id: Primary identifier (typically the npub)
 * - npub: Bech32-encoded public key (npub1...)
 * - fingerprint: First 8 hex chars of pubkey for quick lookup
 * - label: User-friendly display name
 * - hardware: "true" if hardware key reference
 * - owner_uid/owner_username: Unix user association
 * - created_at: ISO 8601 timestamp
 */
#ifndef APPS_GNOSTR_SIGNER_SECRET_STORE_H
#define APPS_GNOSTR_SIGNER_SECRET_STORE_H

#include <glib.h>
#include <gio/gio.h>  /* For GTask, GAsyncResult, GCancellable */
#include <sys/types.h>

G_BEGIN_DECLS

/* Error domain for GError integration */
#define SECRET_STORE_ERROR (secret_store_error_quark())
GQuark secret_store_error_quark(void);

/* Result codes */
typedef enum {
  SECRET_STORE_OK = 0,
  SECRET_STORE_ERR_INVALID_KEY,
  SECRET_STORE_ERR_NOT_FOUND,
  SECRET_STORE_ERR_BACKEND,
  SECRET_STORE_ERR_PERMISSION,
  SECRET_STORE_ERR_DUPLICATE
} SecretStoreResult;

/* Identity entry returned from list operations */
typedef struct {
  gchar *npub;              /* Public key in bech32 format */
  gchar *key_id;            /* Internal identifier (may equal npub) */
  gchar *label;             /* User-defined label */
  gboolean has_owner;       /* Whether owner_uid is set */
  uid_t owner_uid;          /* Unix user ID owner (if has_owner) */
  gchar *owner_username;    /* Unix username (if has_owner) */
} SecretStoreEntry;

/* Store a private key securely.
 * @key: nsec1... or 64-hex or ncrypt...
 * @label: Optional display label
 * @link_to_user: If TRUE, associate with current Unix user
 * Returns: OK on success, error code otherwise
 */
SecretStoreResult secret_store_add(const gchar *key,
                                   const gchar *label,
                                   gboolean link_to_user);

/* Remove a key from secure storage.
 * @selector: npub, key_id, or fingerprint (8-char hex prefix)
 *
 * Uses libsecret's secret_password_clear_sync for secure deletion.
 * Attempts lookup by npub first, then key_id, then fingerprint.
 */
SecretStoreResult secret_store_remove(const gchar *selector);

/* List all stored identities.
 * Returns: GPtrArray of SecretStoreEntry* (caller owns array and entries)
 */
GPtrArray *secret_store_list(void);

/* Free a SecretStoreEntry */
void secret_store_entry_free(SecretStoreEntry *entry);

/* Lookup identity by fingerprint (pubkey hex prefix).
 * @fingerprint: Hex prefix of pubkey (4-64 chars, typically 8)
 * @out_entry: Output entry (caller must free with secret_store_entry_free)
 *
 * Returns the first matching entry. Useful for quick lookup when full
 * npub is not available.
 */
SecretStoreResult secret_store_lookup_by_fingerprint(const gchar *fingerprint,
                                                      SecretStoreEntry **out_entry);

/* Get the secret key for a given selector.
 * @selector: npub or key_id
 * @out_nsec: Output nsec1... string (caller frees)
 * Returns: OK on success
 */
SecretStoreResult secret_store_get_secret(const gchar *selector,
                                          gchar **out_nsec);

/* Update the label for an identity.
 * @selector: npub or key_id
 * @new_label: New label to set
 */
SecretStoreResult secret_store_set_label(const gchar *selector,
                                         const gchar *new_label);

/* Get the public key (npub) for a selector.
 * @selector: npub, key_id, or NULL for active/default
 * @out_npub: Output npub string (caller frees)
 */
SecretStoreResult secret_store_get_public_key(const gchar *selector,
                                              gchar **out_npub);

/* Sign an event using the key for selector.
 * @event_json: Event JSON to sign
 * @selector: npub, key_id, or NULL for active/default
 * @out_signature: Output signature hex (caller frees)
 */
SecretStoreResult secret_store_sign_event(const gchar *event_json,
                                          const gchar *selector,
                                          gchar **out_signature);

/* Generate a new keypair and store it.
 * @label: Optional display label
 * @link_to_user: Associate with current Unix user
 * @out_npub: Output npub (caller frees)
 */
SecretStoreResult secret_store_generate(const gchar *label,
                                        gboolean link_to_user,
                                        gchar **out_npub);

/* Check if secure storage backend is available */
gboolean secret_store_is_available(void);

/* Get the backend name (e.g., "libsecret", "Keychain", "none") */
const gchar *secret_store_backend_name(void);

/* ======== Async API for startup optimization ======== */

/**
 * SecretStoreListCallback:
 * @entries: (transfer full): GPtrArray of SecretStoreEntry*, or NULL on error
 * @user_data: user data passed to the async function
 *
 * Callback type for secret_store_list_async.
 */
typedef void (*SecretStoreListCallback)(GPtrArray *entries, gpointer user_data);

/**
 * secret_store_list_async:
 * @callback: function to call when listing completes
 * @user_data: data to pass to callback
 *
 * Asynchronously list all stored identities. This runs the blocking
 * secret service enumeration in a thread pool to avoid blocking the
 * main thread during application startup.
 *
 * The callback receives ownership of the GPtrArray.
 */
void secret_store_list_async(SecretStoreListCallback callback, gpointer user_data);

/**
 * SecretStoreAvailableCallback:
 * @available: whether the secret store backend is available
 * @user_data: user data passed to the async function
 *
 * Callback type for secret_store_check_available_async.
 */
typedef void (*SecretStoreAvailableCallback)(gboolean available, gpointer user_data);

/**
 * secret_store_check_available_async:
 * @callback: function to call when check completes
 * @user_data: data to pass to callback
 *
 * Asynchronously check if the secret store backend is available.
 */
void secret_store_check_available_async(SecretStoreAvailableCallback callback, gpointer user_data);

/* ======== GIO-style Async API ======== */

/**
 * secret_store_add_async:
 * @key: nsec1... or 64-hex or ncrypt...
 * @label: Optional display label
 * @link_to_user: If TRUE, associate with current Unix user
 * @cancellable: (nullable): Optional GCancellable
 * @callback: Callback when operation completes
 * @user_data: Data for callback
 *
 * Asynchronously store a private key. The key is copied to secure memory
 * immediately and cleared from the task data when complete.
 */
void secret_store_add_async(const gchar *key,
                            const gchar *label,
                            gboolean link_to_user,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);

/**
 * secret_store_add_finish:
 * @result: GAsyncResult from callback
 * @error: (out) (optional): Location for error
 *
 * Finish an async add operation.
 * Returns: Result code (also sets @error on failure)
 */
SecretStoreResult secret_store_add_finish(GAsyncResult *result, GError **error);

/**
 * secret_store_remove_async:
 * @selector: npub, key_id, or fingerprint
 * @cancellable: (nullable): Optional GCancellable
 * @callback: Callback when operation completes
 * @user_data: Data for callback
 *
 * Asynchronously remove a key from secure storage.
 */
void secret_store_remove_async(const gchar *selector,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

/**
 * secret_store_remove_finish:
 * @result: GAsyncResult from callback
 * @error: (out) (optional): Location for error
 *
 * Finish an async remove operation.
 * Returns: Result code (also sets @error on failure)
 */
SecretStoreResult secret_store_remove_finish(GAsyncResult *result, GError **error);

/* ======== Error Utilities ======== */

/**
 * secret_store_result_to_string:
 * @result: A SecretStoreResult code
 *
 * Get a human-readable string for a result code.
 * Returns: Static string describing the result
 */
const gchar *secret_store_result_to_string(SecretStoreResult result);

/**
 * secret_store_result_to_gerror:
 * @result: A SecretStoreResult code
 * @error: (out) (optional): Location to store error
 *
 * Convert a result code to a GError. Does nothing if result is OK or error is NULL.
 * The error domain is SECRET_STORE_ERROR and the code is the result value.
 */
void secret_store_result_to_gerror(SecretStoreResult result, GError **error);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SECRET_STORE_H */
