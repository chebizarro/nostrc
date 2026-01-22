/* secret_store.h - Secure key storage abstraction for gnostr-signer
 *
 * This module provides a unified API for storing Nostr private keys securely
 * using platform-specific backends:
 * - Linux: libsecret (GNOME Keyring / KDE Wallet)
 * - macOS: Security.framework Keychain
 *
 * Keys are stored with metadata (npub, label, owner) for multi-account support.
 */
#pragma once

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

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
 * @selector: npub or key_id
 */
SecretStoreResult secret_store_remove(const gchar *selector);

/* List all stored identities.
 * Returns: GPtrArray of SecretStoreEntry* (caller owns array and entries)
 */
GPtrArray *secret_store_list(void);

/* Free a SecretStoreEntry */
void secret_store_entry_free(SecretStoreEntry *entry);

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

G_END_DECLS
