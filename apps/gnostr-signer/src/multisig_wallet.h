/* multisig_wallet.h - Multi-signature wallet support for gnostr-signer
 *
 * Implements m-of-n threshold signature schemes where m signers out of n
 * total co-signers must approve before a signature is produced.
 *
 * Features:
 * - Configurable m-of-n threshold (e.g., 2-of-3)
 * - Local and remote (NIP-46) co-signers
 * - Partial signature aggregation
 * - Signing progress tracking
 *
 * Reference: NIP-46 for remote signing, MuSig2 for aggregated signatures
 *
 * Issue: nostrc-orz
 */
#ifndef APPS_GNOSTR_SIGNER_MULTISIG_WALLET_H
#define APPS_GNOSTR_SIGNER_MULTISIG_WALLET_H

#include <glib.h>

G_BEGIN_DECLS

/* Error domain for GError integration */
#define MULTISIG_WALLET_ERROR (multisig_wallet_error_quark())
GQuark multisig_wallet_error_quark(void);

/* Result codes */
typedef enum {
  MULTISIG_OK = 0,
  MULTISIG_ERR_INVALID_CONFIG,    /* Invalid threshold configuration */
  MULTISIG_ERR_INVALID_SIGNER,    /* Invalid signer info */
  MULTISIG_ERR_NOT_FOUND,         /* Wallet or signing session not found */
  MULTISIG_ERR_THRESHOLD_NOT_MET, /* Not enough signatures collected */
  MULTISIG_ERR_DUPLICATE,         /* Duplicate signature or signer */
  MULTISIG_ERR_BACKEND,           /* Backend/storage error */
  MULTISIG_ERR_TIMEOUT,           /* Signing session timed out */
  MULTISIG_ERR_CANCELED,          /* Signing was canceled */
  MULTISIG_ERR_REMOTE_FAILED      /* Remote signer communication failed */
} MultisigResult;

/* Co-signer types */
typedef enum {
  COSIGNER_TYPE_LOCAL,       /* Local key in secret store */
  COSIGNER_TYPE_REMOTE_NIP46 /* Remote signer via NIP-46 bunker */
} CosignerType;

/* Co-signer status in a signing session */
typedef enum {
  COSIGNER_STATUS_PENDING,   /* Waiting for signature */
  COSIGNER_STATUS_REQUESTED, /* Signature request sent */
  COSIGNER_STATUS_SIGNED,    /* Signature received */
  COSIGNER_STATUS_REJECTED,  /* Signer rejected the request */
  COSIGNER_STATUS_TIMEOUT,   /* Signer timed out */
  COSIGNER_STATUS_ERROR      /* Communication error */
} CosignerStatus;

/* Co-signer definition */
typedef struct {
  gchar *id;                    /* Unique identifier for this co-signer */
  gchar *npub;                  /* Public key (npub format) */
  gchar *label;                 /* User-friendly display name */
  CosignerType type;            /* Local or remote */
  gchar *bunker_uri;            /* NIP-46 bunker URI (for remote signers) */
  gboolean is_self;             /* TRUE if this is the local user's key */
} MultisigCosigner;

/* Multi-signature wallet configuration */
typedef struct {
  gchar *wallet_id;             /* Unique wallet identifier */
  gchar *name;                  /* User-defined wallet name */
  guint threshold_m;            /* Number of required signatures (m) */
  guint total_n;                /* Total number of co-signers (n) */
  GPtrArray *cosigners;         /* Array of MultisigCosigner* */
  gchar *aggregated_pubkey;     /* Combined public key (for receiving) */
  gint64 created_at;            /* Creation timestamp */
  gint64 updated_at;            /* Last update timestamp */
} MultisigWallet;

/* Signing session for tracking partial signatures */
typedef struct {
  gchar *session_id;            /* Unique session identifier */
  gchar *wallet_id;             /* Associated wallet */
  gchar *event_json;            /* Event to be signed */
  gint event_kind;              /* Event kind for display */
  gchar *event_id;              /* Event ID being signed */
  guint signatures_collected;   /* Number of signatures received */
  guint signatures_required;    /* Number needed (threshold_m) */
  GPtrArray *partial_sigs;      /* Array of partial signature data */
  GHashTable *signer_status;    /* npub -> CosignerStatus */
  gint64 created_at;            /* Session start time */
  gint64 expires_at;            /* Session expiry time */
  gboolean is_complete;         /* TRUE when threshold met */
  gchar *final_signature;       /* Aggregated signature when complete */
} MultisigSigningSession;

/* Callback types */
typedef void (*MultisigProgressCb)(MultisigSigningSession *session,
                                   const gchar *signer_npub,
                                   CosignerStatus status,
                                   gpointer user_data);

typedef void (*MultisigCompleteCb)(MultisigSigningSession *session,
                                   gboolean success,
                                   const gchar *error,
                                   gpointer user_data);

/* ======== Wallet Management ======== */

/**
 * multisig_wallet_create:
 * @name: User-friendly wallet name
 * @threshold_m: Number of required signatures
 * @total_n: Total number of co-signers
 * @out_wallet_id: (out): Output wallet ID (caller frees)
 * @error: (out) (optional): Location for error
 *
 * Create a new multi-signature wallet configuration.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_create(const gchar *name,
                                      guint threshold_m,
                                      guint total_n,
                                      gchar **out_wallet_id,
                                      GError **error);

/**
 * multisig_wallet_add_cosigner:
 * @wallet_id: Wallet to modify
 * @cosigner: Co-signer to add (wallet takes ownership)
 * @error: (out) (optional): Location for error
 *
 * Add a co-signer to a wallet. The wallet must not exceed total_n signers.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_add_cosigner(const gchar *wallet_id,
                                            MultisigCosigner *cosigner,
                                            GError **error);

/**
 * multisig_wallet_remove_cosigner:
 * @wallet_id: Wallet to modify
 * @cosigner_id: Co-signer ID to remove
 * @error: (out) (optional): Location for error
 *
 * Remove a co-signer from a wallet. Cannot reduce below threshold_m.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_remove_cosigner(const gchar *wallet_id,
                                               const gchar *cosigner_id,
                                               GError **error);

/**
 * multisig_wallet_get:
 * @wallet_id: Wallet ID to retrieve
 * @out_wallet: (out): Output wallet (caller frees with multisig_wallet_free)
 *
 * Retrieve a wallet by ID.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_get(const gchar *wallet_id,
                                   MultisigWallet **out_wallet);

/**
 * multisig_wallet_list:
 *
 * List all configured multi-signature wallets.
 * Returns: GPtrArray of MultisigWallet* (caller owns array)
 */
GPtrArray *multisig_wallet_list(void);

/**
 * multisig_wallet_delete:
 * @wallet_id: Wallet to delete
 * @error: (out) (optional): Location for error
 *
 * Delete a wallet configuration. Does not affect stored keys.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_delete(const gchar *wallet_id,
                                      GError **error);

/**
 * multisig_wallet_save:
 * @wallet: Wallet to save
 * @error: (out) (optional): Location for error
 *
 * Persist wallet configuration to storage.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_wallet_save(MultisigWallet *wallet,
                                    GError **error);

/* ======== Signing Sessions ======== */

/**
 * multisig_signing_start:
 * @wallet_id: Wallet to use for signing
 * @event_json: Event JSON to sign
 * @timeout_seconds: Session timeout (0 for default 5 minutes)
 * @progress_cb: Callback for progress updates (optional)
 * @complete_cb: Callback when signing completes or fails
 * @user_data: User data for callbacks
 * @out_session_id: (out): Output session ID (caller frees)
 * @error: (out) (optional): Location for error
 *
 * Start a multi-signature signing session. Automatically requests
 * signatures from all co-signers (local and remote).
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_signing_start(const gchar *wallet_id,
                                      const gchar *event_json,
                                      gint timeout_seconds,
                                      MultisigProgressCb progress_cb,
                                      MultisigCompleteCb complete_cb,
                                      gpointer user_data,
                                      gchar **out_session_id,
                                      GError **error);

/**
 * multisig_signing_add_signature:
 * @session_id: Signing session ID
 * @signer_npub: Public key of signer
 * @partial_sig: Partial signature data
 * @error: (out) (optional): Location for error
 *
 * Add a partial signature to a session. Called when a co-signer
 * approves and signs.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_signing_add_signature(const gchar *session_id,
                                              const gchar *signer_npub,
                                              const gchar *partial_sig,
                                              GError **error);

/**
 * multisig_signing_reject:
 * @session_id: Signing session ID
 * @signer_npub: Public key of signer who rejected
 * @reason: Optional rejection reason
 *
 * Record that a co-signer rejected the signing request.
 */
void multisig_signing_reject(const gchar *session_id,
                             const gchar *signer_npub,
                             const gchar *reason);

/**
 * multisig_signing_get_status:
 * @session_id: Session to query
 * @out_session: (out): Output session info (caller frees)
 *
 * Get current status of a signing session.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_signing_get_status(const gchar *session_id,
                                           MultisigSigningSession **out_session);

/**
 * multisig_signing_cancel:
 * @session_id: Session to cancel
 *
 * Cancel an in-progress signing session.
 */
void multisig_signing_cancel(const gchar *session_id);

/**
 * multisig_signing_get_final_signature:
 * @session_id: Completed session ID
 * @out_signature: (out): Aggregated signature (caller frees)
 * @error: (out) (optional): Location for error
 *
 * Get the final aggregated signature from a completed session.
 * Returns: MULTISIG_OK on success
 */
MultisigResult multisig_signing_get_final_signature(const gchar *session_id,
                                                    gchar **out_signature,
                                                    GError **error);

/* ======== Helpers ======== */

/**
 * multisig_cosigner_new:
 * @npub: Public key in bech32 format
 * @label: Display name
 * @type: Local or remote
 *
 * Create a new co-signer definition.
 * Returns: New MultisigCosigner (caller owns)
 */
MultisigCosigner *multisig_cosigner_new(const gchar *npub,
                                        const gchar *label,
                                        CosignerType type);

/**
 * multisig_cosigner_new_remote:
 * @bunker_uri: NIP-46 bunker:// URI
 * @label: Display name
 *
 * Create a new remote co-signer from a bunker URI.
 * Extracts the public key from the URI.
 * Returns: New MultisigCosigner (caller owns)
 */
MultisigCosigner *multisig_cosigner_new_remote(const gchar *bunker_uri,
                                               const gchar *label);

/**
 * multisig_cosigner_free:
 * @cosigner: Co-signer to free
 */
void multisig_cosigner_free(MultisigCosigner *cosigner);

/**
 * multisig_wallet_free:
 * @wallet: Wallet to free
 */
void multisig_wallet_free(MultisigWallet *wallet);

/**
 * multisig_signing_session_free:
 * @session: Session to free
 */
void multisig_signing_session_free(MultisigSigningSession *session);

/**
 * multisig_result_to_string:
 * @result: Result code
 *
 * Get human-readable string for result code.
 * Returns: Static string
 */
const gchar *multisig_result_to_string(MultisigResult result);

/**
 * multisig_cosigner_status_to_string:
 * @status: Co-signer status
 *
 * Get human-readable string for co-signer status.
 * Returns: Static string
 */
const gchar *multisig_cosigner_status_to_string(CosignerStatus status);

/**
 * multisig_validate_config:
 * @threshold_m: Required signatures
 * @total_n: Total signers
 * @error: (out) (optional): Location for error
 *
 * Validate a threshold configuration.
 * Returns: TRUE if valid (1 <= m <= n, n >= 1)
 */
gboolean multisig_validate_config(guint threshold_m,
                                  guint total_n,
                                  GError **error);

/**
 * multisig_format_progress:
 * @collected: Signatures collected
 * @required: Signatures required
 *
 * Format progress string (e.g., "2 of 3 signatures collected").
 * Returns: Newly allocated string (caller frees)
 */
gchar *multisig_format_progress(guint collected, guint required);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_MULTISIG_WALLET_H */
