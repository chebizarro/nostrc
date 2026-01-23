/* multisig_store.h - Partial signature storage for multi-sig wallets
 *
 * Manages storage of partial signatures during multi-signature signing sessions.
 * Provides secure storage with encryption for sensitive signature data.
 *
 * Features:
 * - Store partial signatures indexed by session and signer
 * - Automatic expiry of stale sessions
 * - Secure memory handling for signature data
 * - Support for resuming interrupted signing sessions
 *
 * Issue: nostrc-orz
 */
#pragma once

#include <glib.h>
#include "multisig_wallet.h"

G_BEGIN_DECLS

/* Partial signature entry */
typedef struct {
  gchar *session_id;      /* Parent signing session */
  gchar *signer_npub;     /* Signer's public key */
  gchar *partial_sig;     /* The partial signature (secure memory) */
  gint64 received_at;     /* Timestamp of receipt */
  gboolean verified;      /* Whether signature was verified */
} MultisigPartialSig;

/* Store singleton */
typedef struct _MultisigStore MultisigStore;

/**
 * multisig_store_get_default:
 *
 * Get the singleton partial signature store.
 * Returns: The store instance
 */
MultisigStore *multisig_store_get_default(void);

/**
 * multisig_store_add_partial:
 * @store: The store
 * @session_id: Signing session ID
 * @signer_npub: Signer's public key
 * @partial_sig: The partial signature
 * @error: (out) (optional): Location for error
 *
 * Store a partial signature. The signature data is copied to secure memory.
 * Returns: TRUE on success
 */
gboolean multisig_store_add_partial(MultisigStore *store,
                                    const gchar *session_id,
                                    const gchar *signer_npub,
                                    const gchar *partial_sig,
                                    GError **error);

/**
 * multisig_store_get_partial:
 * @store: The store
 * @session_id: Signing session ID
 * @signer_npub: Signer's public key
 * @out_partial_sig: (out): The partial signature (secure memory, caller frees)
 *
 * Retrieve a stored partial signature.
 * Returns: TRUE if found
 */
gboolean multisig_store_get_partial(MultisigStore *store,
                                    const gchar *session_id,
                                    const gchar *signer_npub,
                                    gchar **out_partial_sig);

/**
 * multisig_store_list_partials:
 * @store: The store
 * @session_id: Signing session ID
 *
 * List all partial signatures for a session.
 * Returns: GPtrArray of MultisigPartialSig* (caller owns)
 */
GPtrArray *multisig_store_list_partials(MultisigStore *store,
                                        const gchar *session_id);

/**
 * multisig_store_count_partials:
 * @store: The store
 * @session_id: Signing session ID
 *
 * Count partial signatures for a session.
 * Returns: Number of partial signatures stored
 */
guint multisig_store_count_partials(MultisigStore *store,
                                    const gchar *session_id);

/**
 * multisig_store_remove_partial:
 * @store: The store
 * @session_id: Signing session ID
 * @signer_npub: Signer's public key
 *
 * Remove a specific partial signature.
 * Returns: TRUE if removed
 */
gboolean multisig_store_remove_partial(MultisigStore *store,
                                       const gchar *session_id,
                                       const gchar *signer_npub);

/**
 * multisig_store_clear_session:
 * @store: The store
 * @session_id: Signing session ID
 *
 * Remove all partial signatures for a session.
 * Returns: Number of signatures removed
 */
guint multisig_store_clear_session(MultisigStore *store,
                                   const gchar *session_id);

/**
 * multisig_store_expire_old:
 * @store: The store
 * @max_age_seconds: Maximum age of signatures to keep
 *
 * Remove signatures older than the specified age.
 * Returns: Number of signatures removed
 */
guint multisig_store_expire_old(MultisigStore *store,
                                gint64 max_age_seconds);

/**
 * multisig_store_save:
 * @store: The store
 *
 * Persist the store to disk.
 * Note: Partial signatures are encrypted before storage.
 */
void multisig_store_save(MultisigStore *store);

/**
 * multisig_store_load:
 * @store: The store
 *
 * Load the store from disk.
 */
void multisig_store_load(MultisigStore *store);

/**
 * multisig_partial_sig_free:
 * @partial: The partial signature entry to free
 *
 * Free a partial signature entry (uses secure memory clearing).
 */
void multisig_partial_sig_free(MultisigPartialSig *partial);

/**
 * multisig_store_free:
 * @store: The store to free
 *
 * Free the store and all stored signatures.
 */
void multisig_store_free(MultisigStore *store);

G_END_DECLS
