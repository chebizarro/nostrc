/* accounts_store.h - Multi-account management for gnostr-signer
 *
 * Manages identity metadata (npub, labels) with persistence via INI file.
 * Actual secret keys are stored via secret_store (libsecret/Keychain).
 *
 * Features:
 * - Multiple identities with labels
 * - Active identity selection
 * - Integration with GSettings for persistence
 * - Key type metadata for multi-algorithm support (nostrc-bq0)
 */
#pragma once
#include <glib.h>
#include "key_provider.h"

G_BEGIN_DECLS

typedef struct _AccountsStore AccountsStore;

typedef struct {
  gchar *id;           /* identity selector: npub */
  gchar *label;        /* user-defined display label */
  gboolean has_secret; /* whether secret key is available in secure storage */
  gboolean watch_only; /* TRUE if this is a watch-only account (public key only) */
  GnKeyType key_type;  /* cryptographic key type (nostrc-bq0) */
} AccountEntry;

/* Create a new accounts store */
AccountsStore *accounts_store_new(void);

/* Free the accounts store */
void accounts_store_free(AccountsStore *as);

/* Load accounts from disk */
void accounts_store_load(AccountsStore *as);

/* Save accounts to disk */
void accounts_store_save(AccountsStore *as);

/* Add a new account. Returns FALSE if id already exists.
 * @id: npub identifier
 * @label: optional display label
 */
gboolean accounts_store_add(AccountsStore *as, const gchar *id, const gchar *label);

/* Remove an account by id */
gboolean accounts_store_remove(AccountsStore *as, const gchar *id);

/* List all accounts.
 * Returns: GPtrArray of AccountEntry* (caller owns array; use accounts_store_entry_free)
 */
GPtrArray *accounts_store_list(AccountsStore *as);

/* Free an account entry */
void accounts_store_entry_free(AccountEntry *entry);

/* Set the active identity */
void accounts_store_set_active(AccountsStore *as, const gchar *id);

/* Get the active identity.
 * Returns FALSE if no active set; if out_id provided, it's newly allocated
 */
gboolean accounts_store_get_active(AccountsStore *as, gchar **out_id);

/* Update label for an existing id. Returns FALSE if id not found. */
gboolean accounts_store_set_label(AccountsStore *as, const gchar *id, const gchar *label);

/* Get the number of accounts */
guint accounts_store_count(AccountsStore *as);

/* Check if an account exists */
gboolean accounts_store_exists(AccountsStore *as, const gchar *id);

/* Find account by partial match (for npub search) */
AccountEntry *accounts_store_find(AccountsStore *as, const gchar *query);

/* Sync with secret store - adds any keys found in secure storage that aren't tracked */
void accounts_store_sync_with_secrets(AccountsStore *as);

/* Import a key and add to accounts. Returns npub on success, NULL on failure.
 * @key: nsec or hex private key
 * @label: optional display label
 * @out_npub: output npub (caller frees)
 */
gboolean accounts_store_import_key(AccountsStore *as, const gchar *key,
                                   const gchar *label, gchar **out_npub);

/* Generate a new keypair and add to accounts.
 * @label: optional display label
 * @out_npub: output npub (caller frees)
 */
gboolean accounts_store_generate_key(AccountsStore *as, const gchar *label,
                                     gchar **out_npub);

/* Generate a new keypair with specific key type (nostrc-bq0).
 * @label: optional display label
 * @key_type: cryptographic key type to use
 * @out_npub: output npub (caller frees)
 */
gboolean accounts_store_generate_key_with_type(AccountsStore *as,
                                               const gchar *label,
                                               GnKeyType key_type,
                                               gchar **out_npub);

/* Import a key with specific key type (nostrc-bq0).
 * @key: nsec or hex private key
 * @label: optional display label
 * @key_type: cryptographic key type (or GN_KEY_TYPE_UNKNOWN for auto-detect)
 * @out_npub: output npub (caller frees)
 */
gboolean accounts_store_import_key_with_type(AccountsStore *as,
                                             const gchar *key,
                                             const gchar *label,
                                             GnKeyType key_type,
                                             gchar **out_npub);

/* Get the key type for an account (nostrc-bq0).
 * @id: account identifier (npub)
 * Returns: The key type, or GN_KEY_TYPE_SECP256K1 as default
 */
GnKeyType accounts_store_get_key_type(AccountsStore *as, const gchar *id);

/* Set the key type for an account (nostrc-bq0).
 * @id: account identifier (npub)
 * @key_type: the key type to set
 * Returns: TRUE on success
 */
gboolean accounts_store_set_key_type(AccountsStore *as,
                                     const gchar *id,
                                     GnKeyType key_type);

/* Get the display name for an account (label if set, else truncated npub) */
gchar *accounts_store_get_display_name(AccountsStore *as, const gchar *id);

/* Import a public key only (watch-only account).
 * @pubkey: npub or hex public key
 * @label: optional display label
 * @out_npub: output npub (caller frees)
 * Returns TRUE on success
 */
gboolean accounts_store_import_pubkey(AccountsStore *as, const gchar *pubkey,
                                      const gchar *label, gchar **out_npub);

/* Check if an account is watch-only (no private key, explicitly imported as pubkey) */
gboolean accounts_store_is_watch_only(AccountsStore *as, const gchar *id);

/* Change notification callback types */
typedef enum {
  ACCOUNTS_CHANGE_ADDED,
  ACCOUNTS_CHANGE_REMOVED,
  ACCOUNTS_CHANGE_ACTIVE,
  ACCOUNTS_CHANGE_LABEL
} AccountsChangeType;

typedef void (*AccountsChangedCb)(AccountsStore *as, AccountsChangeType change,
                                   const gchar *id, gpointer user_data);

/* Register a change notification callback.
 * Returns a handler ID that can be used to unregister.
 */
guint accounts_store_connect_changed(AccountsStore *as, AccountsChangedCb cb,
                                     gpointer user_data);

/* Unregister a change notification callback */
void accounts_store_disconnect_changed(AccountsStore *as, guint handler_id);

/* Get singleton instance for global access */
AccountsStore *accounts_store_get_default(void);

/* ======== Async API for startup optimization ======== */

/**
 * AccountsStoreSyncCallback:
 * @as: the accounts store that was synced
 * @user_data: user data passed to the async function
 *
 * Callback type for accounts_store_sync_with_secrets_async.
 */
typedef void (*AccountsStoreSyncCallback)(AccountsStore *as, gpointer user_data);

/**
 * accounts_store_sync_with_secrets_async:
 * @as: the accounts store to sync
 * @callback: function to call when sync completes
 * @user_data: data to pass to callback
 *
 * Asynchronously sync accounts with secret store. This runs the blocking
 * secret service enumeration in a thread pool to avoid blocking the
 * main thread during application startup.
 */
void accounts_store_sync_with_secrets_async(AccountsStore *as,
                                            AccountsStoreSyncCallback callback,
                                            gpointer user_data);

G_END_DECLS
