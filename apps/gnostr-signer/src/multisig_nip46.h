/* multisig_nip46.h - NIP-46 integration for multi-sig remote co-signers
 *
 * Handles communication with remote co-signers via NIP-46 bunker protocol.
 * This module bridges the multisig_coordinator with the NIP-46 client library.
 *
 * Features:
 * - Connect to remote bunkers for signing requests
 * - Send sign_event requests to remote co-signers
 * - Handle responses and route to coordinator
 * - Manage connection state for each remote signer
 *
 * Issue: nostrc-orz
 */
#pragma once

#include <glib.h>
#include "multisig_wallet.h"
#include "multisig_coordinator.h"

G_BEGIN_DECLS

/* NIP-46 client for multisig operations */
typedef struct _MultisigNip46Client MultisigNip46Client;

/* Callback when a remote signature is received */
typedef void (*MultisigNip46SignatureCb)(const gchar *session_id,
                                         const gchar *signer_npub,
                                         const gchar *partial_sig,
                                         gpointer user_data);

/* Callback when a remote signer rejects */
typedef void (*MultisigNip46RejectCb)(const gchar *session_id,
                                      const gchar *signer_npub,
                                      const gchar *reason,
                                      gpointer user_data);

/* Callback for connection state changes */
typedef void (*MultisigNip46StateCb)(const gchar *npub,
                                     RemoteSignerState state,
                                     const gchar *error,
                                     gpointer user_data);

/**
 * multisig_nip46_client_new:
 *
 * Create a new NIP-46 client for multisig operations.
 * Returns: New client instance
 */
MultisigNip46Client *multisig_nip46_client_new(void);

/**
 * multisig_nip46_client_free:
 * @client: Client to free
 *
 * Free a NIP-46 client and all connections.
 */
void multisig_nip46_client_free(MultisigNip46Client *client);

/**
 * multisig_nip46_client_set_callbacks:
 * @client: The client
 * @signature_cb: Callback for signatures
 * @reject_cb: Callback for rejections
 * @state_cb: Callback for state changes
 * @user_data: User data for callbacks
 *
 * Set callbacks for NIP-46 events.
 */
void multisig_nip46_client_set_callbacks(MultisigNip46Client *client,
                                         MultisigNip46SignatureCb signature_cb,
                                         MultisigNip46RejectCb reject_cb,
                                         MultisigNip46StateCb state_cb,
                                         gpointer user_data);

/**
 * multisig_nip46_connect:
 * @client: The client
 * @bunker_uri: bunker:// URI to connect to
 * @our_identity_npub: Our identity for signing the connection
 * @error: (out) (optional): Location for error
 *
 * Connect to a remote bunker. The connection is asynchronous;
 * state changes are reported via the state callback.
 * Returns: TRUE if connection initiated
 */
gboolean multisig_nip46_connect(MultisigNip46Client *client,
                                const gchar *bunker_uri,
                                const gchar *our_identity_npub,
                                GError **error);

/**
 * multisig_nip46_disconnect:
 * @client: The client
 * @signer_npub: npub of signer to disconnect
 *
 * Disconnect from a remote signer.
 */
void multisig_nip46_disconnect(MultisigNip46Client *client,
                               const gchar *signer_npub);

/**
 * multisig_nip46_request_signature:
 * @client: The client
 * @signer_npub: npub of remote signer
 * @session_id: Multisig signing session ID
 * @event_json: Event JSON to sign
 * @error: (out) (optional): Location for error
 *
 * Request a signature from a remote co-signer.
 * The response will be delivered via the signature callback.
 * Returns: TRUE if request sent
 */
gboolean multisig_nip46_request_signature(MultisigNip46Client *client,
                                          const gchar *signer_npub,
                                          const gchar *session_id,
                                          const gchar *event_json,
                                          GError **error);

/**
 * multisig_nip46_get_state:
 * @client: The client
 * @signer_npub: npub of signer
 *
 * Get the connection state for a remote signer.
 * Returns: Current state
 */
RemoteSignerState multisig_nip46_get_state(MultisigNip46Client *client,
                                           const gchar *signer_npub);

/**
 * multisig_nip46_is_connected:
 * @client: The client
 * @signer_npub: npub of signer
 *
 * Check if a remote signer is connected.
 * Returns: TRUE if connected
 */
gboolean multisig_nip46_is_connected(MultisigNip46Client *client,
                                     const gchar *signer_npub);

/**
 * multisig_nip46_get_default:
 *
 * Get the singleton NIP-46 client for multisig operations.
 * Returns: The client instance
 */
MultisigNip46Client *multisig_nip46_get_default(void);

G_END_DECLS
