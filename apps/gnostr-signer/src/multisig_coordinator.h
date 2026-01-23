/* multisig_coordinator.h - Signing coordination for multi-sig wallets
 *
 * Coordinates the signing process across multiple co-signers, handling
 * both local keys and remote NIP-46 bunker connections.
 *
 * Features:
 * - Request signatures from local keys automatically
 * - Connect to remote bunkers via NIP-46 for remote signatures
 * - Track signing progress with callbacks
 * - Handle retries and timeouts
 * - Aggregate partial signatures when threshold is met
 *
 * Issue: nostrc-orz
 */
#pragma once

#include <glib.h>
#include "multisig_wallet.h"
#include "bunker_service.h"

G_BEGIN_DECLS

/* Coordinator singleton */
typedef struct _MultisigCoordinator MultisigCoordinator;

/* Remote signer connection state */
typedef enum {
  REMOTE_SIGNER_DISCONNECTED,
  REMOTE_SIGNER_CONNECTING,
  REMOTE_SIGNER_CONNECTED,
  REMOTE_SIGNER_ERROR
} RemoteSignerState;

/* Remote signer info */
typedef struct {
  gchar *npub;
  gchar *bunker_uri;
  RemoteSignerState state;
  gchar *error_message;
  gint64 last_contact;
} RemoteSignerInfo;

/* Callback for signing progress updates */
typedef void (*MultisigCoordinatorProgressCb)(const gchar *session_id,
                                              guint collected,
                                              guint required,
                                              const gchar *latest_signer,
                                              gpointer user_data);

/* Callback when signing completes or fails */
typedef void (*MultisigCoordinatorCompleteCb)(const gchar *session_id,
                                              gboolean success,
                                              const gchar *signature,
                                              const gchar *error,
                                              gpointer user_data);

/* Callback for UI prompts */
typedef gboolean (*MultisigCoordinatorPromptCb)(const gchar *session_id,
                                                const gchar *event_json,
                                                gint event_kind,
                                                const gchar *wallet_name,
                                                gpointer user_data);

/**
 * multisig_coordinator_get_default:
 *
 * Get the singleton coordinator instance.
 * Returns: The coordinator
 */
MultisigCoordinator *multisig_coordinator_get_default(void);

/**
 * multisig_coordinator_set_prompt_callback:
 * @coordinator: The coordinator
 * @cb: Callback for prompting user approval
 * @user_data: User data for callback
 *
 * Set the callback used to prompt the user for signing approval.
 * The callback should return TRUE if the user approves.
 */
void multisig_coordinator_set_prompt_callback(MultisigCoordinator *coordinator,
                                              MultisigCoordinatorPromptCb cb,
                                              gpointer user_data);

/**
 * multisig_coordinator_start_signing:
 * @coordinator: The coordinator
 * @wallet_id: Wallet to use for signing
 * @event_json: Event JSON to sign
 * @auto_sign_local: Whether to auto-sign with local keys (no prompt)
 * @progress_cb: Callback for progress updates
 * @complete_cb: Callback when signing completes
 * @user_data: User data for callbacks
 * @out_session_id: (out): Output session ID (caller frees)
 * @error: (out) (optional): Location for error
 *
 * Start a coordinated multi-signature signing session.
 *
 * Flow:
 * 1. For each local co-signer with available key, either auto-sign
 *    or prompt user (depending on auto_sign_local)
 * 2. For each remote co-signer, connect via NIP-46 and request signature
 * 3. Collect partial signatures, calling progress_cb as each arrives
 * 4. When threshold is met, aggregate signatures and call complete_cb
 *
 * Returns: TRUE on success (session started)
 */
gboolean multisig_coordinator_start_signing(MultisigCoordinator *coordinator,
                                            const gchar *wallet_id,
                                            const gchar *event_json,
                                            gboolean auto_sign_local,
                                            MultisigCoordinatorProgressCb progress_cb,
                                            MultisigCoordinatorCompleteCb complete_cb,
                                            gpointer user_data,
                                            gchar **out_session_id,
                                            GError **error);

/**
 * multisig_coordinator_approve_local:
 * @coordinator: The coordinator
 * @session_id: Signing session ID
 * @signer_npub: Local signer's npub to approve for
 *
 * Approve signing for a local co-signer (after prompt callback returns TRUE).
 * This triggers the actual signing with the local key.
 */
void multisig_coordinator_approve_local(MultisigCoordinator *coordinator,
                                        const gchar *session_id,
                                        const gchar *signer_npub);

/**
 * multisig_coordinator_reject_local:
 * @coordinator: The coordinator
 * @session_id: Signing session ID
 * @signer_npub: Local signer's npub
 *
 * Reject signing for a local co-signer.
 */
void multisig_coordinator_reject_local(MultisigCoordinator *coordinator,
                                       const gchar *session_id,
                                       const gchar *signer_npub);

/**
 * multisig_coordinator_receive_remote_signature:
 * @coordinator: The coordinator
 * @session_id: Signing session ID
 * @signer_npub: Remote signer's npub
 * @partial_sig: The partial signature
 *
 * Handle a partial signature received from a remote co-signer.
 * Called by the NIP-46 bunker service integration.
 */
void multisig_coordinator_receive_remote_signature(MultisigCoordinator *coordinator,
                                                   const gchar *session_id,
                                                   const gchar *signer_npub,
                                                   const gchar *partial_sig);

/**
 * multisig_coordinator_remote_rejected:
 * @coordinator: The coordinator
 * @session_id: Signing session ID
 * @signer_npub: Remote signer's npub
 * @reason: Rejection reason
 *
 * Handle rejection from a remote co-signer.
 */
void multisig_coordinator_remote_rejected(MultisigCoordinator *coordinator,
                                          const gchar *session_id,
                                          const gchar *signer_npub,
                                          const gchar *reason);

/**
 * multisig_coordinator_cancel_session:
 * @coordinator: The coordinator
 * @session_id: Session to cancel
 *
 * Cancel an in-progress signing session.
 */
void multisig_coordinator_cancel_session(MultisigCoordinator *coordinator,
                                         const gchar *session_id);

/**
 * multisig_coordinator_get_session_progress:
 * @coordinator: The coordinator
 * @session_id: Session ID
 * @out_collected: (out): Signatures collected
 * @out_required: (out): Signatures required
 *
 * Get current progress for a session.
 * Returns: TRUE if session exists
 */
gboolean multisig_coordinator_get_session_progress(MultisigCoordinator *coordinator,
                                                   const gchar *session_id,
                                                   guint *out_collected,
                                                   guint *out_required);

/**
 * multisig_coordinator_connect_remote:
 * @coordinator: The coordinator
 * @bunker_uri: NIP-46 bunker URI
 * @error: (out) (optional): Location for error
 *
 * Establish a connection to a remote co-signer via NIP-46.
 * This connection can be reused for multiple signing sessions.
 * Returns: TRUE if connection initiated
 */
gboolean multisig_coordinator_connect_remote(MultisigCoordinator *coordinator,
                                             const gchar *bunker_uri,
                                             GError **error);

/**
 * multisig_coordinator_disconnect_remote:
 * @coordinator: The coordinator
 * @npub: Remote signer's npub
 *
 * Disconnect from a remote co-signer.
 */
void multisig_coordinator_disconnect_remote(MultisigCoordinator *coordinator,
                                            const gchar *npub);

/**
 * multisig_coordinator_list_remote_signers:
 * @coordinator: The coordinator
 *
 * List all known remote co-signers and their connection state.
 * Returns: GPtrArray of RemoteSignerInfo* (caller owns)
 */
GPtrArray *multisig_coordinator_list_remote_signers(MultisigCoordinator *coordinator);

/**
 * multisig_coordinator_get_remote_signer_state:
 * @coordinator: The coordinator
 * @npub: Remote signer's npub
 *
 * Get the connection state for a remote signer.
 * Returns: The state, or REMOTE_SIGNER_DISCONNECTED if unknown
 */
RemoteSignerState multisig_coordinator_get_remote_signer_state(MultisigCoordinator *coordinator,
                                                               const gchar *npub);

/**
 * remote_signer_info_free:
 * @info: Info to free
 */
void remote_signer_info_free(RemoteSignerInfo *info);

/**
 * multisig_coordinator_free:
 * @coordinator: Coordinator to free
 */
void multisig_coordinator_free(MultisigCoordinator *coordinator);

G_END_DECLS
