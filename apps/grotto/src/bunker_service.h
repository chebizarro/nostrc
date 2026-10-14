/* bunker_service.h - NIP-46 bunker service for remote signing
 *
 * Implements NIP-46 remote signer functionality, allowing clients to
 * connect via nostrconnect:// URIs and request signing operations.
 *
 * Features:
 * - Accept nostrconnect:// connection requests
 * - Generate bunker:// URIs for sharing
 * - ACL-based authorization
 * - Event kind filtering
 * - UI prompts for approval
 */
#ifndef APPS_GNOSTR_SIGNER_BUNKER_SERVICE_H
#define APPS_GNOSTR_SIGNER_BUNKER_SERVICE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BunkerService BunkerService;

/* Bunker state */
typedef enum {
  BUNKER_STATE_STOPPED,
  BUNKER_STATE_STARTING,
  BUNKER_STATE_RUNNING,
  BUNKER_STATE_ERROR,
  BUNKER_STATE_DEGRADED,
  BUNKER_STATE_OFFLINE,
  BUNKER_STATE_STOPPING
} BunkerState;

/* Connection info */
typedef struct {
  gchar *client_pubkey;   /* Client's public key (hex) */
  gchar *app_name;        /* Application name (if provided) */
  gchar **permissions;    /* Granted permissions */
  gint64 connected_at;    /* Connection timestamp */
  gint64 last_request;    /* Last request timestamp */
  guint request_count;    /* Total requests from this client */
} BunkerConnection;

/* Signing request info for UI prompts */
typedef struct {
  gchar *request_id;
  gchar *client_pubkey;
  gchar *method;          /* NIP-46 method (sign_event, etc.) */
  gchar *event_json;      /* Event to sign (if sign_event) */
  gint event_kind;        /* Event kind (if sign_event) */
  gchar *preview;         /* Preview text for UI */
} BunkerSignRequest;

/* Callback types */
typedef void (*BunkerStateChangedCb)(BunkerState state, const gchar *error, gpointer user_data);
typedef void (*BunkerConnectionCb)(const BunkerConnection *conn, gpointer user_data);
/* Owner-thread lookup: TRUE only for persisted paired admission; returns an
 * allocated pinned account identity when identity is non-NULL. NULL asks only
 * for admission. G06 supplies new-pairing/token hooks. */
typedef gboolean (*BunkerClientLookupCb)(const gchar *client_hex, gchar **identity, gpointer data);
void bunker_service_set_client_lookup(BunkerService *, BunkerClientLookupCb, gpointer);
void bunker_service_set_no_approval_ui(BunkerService *, gboolean);
const gchar *bunker_service_get_error(BunkerService *);
const gchar *bunker_service_get_transport_pubkey(BunkerService *);

/* Called only after explicit CONNECT consent, before any acknowledgement. */
typedef gboolean (*BunkerPairingCommitCb)(const gchar *client_hex, const gchar *identity,
    const gchar *label, const gchar *const *relays, gint tier, gpointer data);
void bunker_service_set_pairing_commit(BunkerService *, BunkerPairingCommitCb, gpointer);
GVariant *bunker_service_create_pairing(BunkerService *, const gchar *identity, const gchar *label, GError **);
gchar *bunker_service_accept_connect_uri(BunkerService *, const gchar *uri, const gchar *identity, GError **);
gboolean bunker_service_cancel_pairing(BunkerService *, const gchar *id);
void bunker_service_invalidate_pairings(BunkerService *);
/* Stage a new client's explicit tier choice, without creating admission. */
gboolean bunker_service_pairing_tier(BunkerService *, const gchar *client_hex, gint tier);
gboolean bunker_service_reconfigure(BunkerService *, const gchar *const *relays, const gchar *identity);
GVariant *bunker_service_relay_status(BunkerService *);

typedef gboolean (*BunkerAuthorizeCb)(const BunkerSignRequest *req, gpointer user_data);

/* Create a new bunker service */
BunkerService *bunker_service_new(void);

/* Free the bunker service */
void bunker_service_free(BunkerService *bs);

/* Start the bunker service.
 * @relays: Array of relay URLs to listen on
 * @identity: npub to use as bunker identity
 */
gboolean bunker_service_start(BunkerService *bs,
                              const gchar *const *relays,
                              const gchar *identity);

/* Stop the bunker service */
void bunker_service_stop(BunkerService *bs);
gboolean bunker_service_restart(BunkerService *bs);
void bunker_service_fail_transport(BunkerService *bs);
void bunker_service_fail_configuration(BunkerService *bs);
gboolean bunker_service_replay_healthy(BunkerService *bs);
/* ONLY after authenticated endpoint rotation and revocation of all pairings;
 * requires a stopped listener. Never clear replay guards on an ordinary restart. */
gboolean bunker_service_reset_replay(BunkerService *bs);

/* Get current state */
BunkerState bunker_service_get_state(BunkerService *bs);

/* Generate a bunker:// URI for sharing */
gchar *bunker_service_get_bunker_uri(BunkerService *bs, const gchar *secret);

/* Process a nostrconnect:// URI (incoming connection request) */
gboolean bunker_service_handle_connect_uri(BunkerService *bs, const gchar *uri);

/* List active connections */
GPtrArray *bunker_service_list_connections(BunkerService *bs);

/* Disconnect a client */
gboolean bunker_service_disconnect_client(BunkerService *bs, const gchar *client_pubkey);

/* Set allowed methods */
void bunker_service_set_allowed_methods(BunkerService *bs, const gchar *const *methods);
/* Signer-wide advertised relays, distinct from the effective client relay union. */
void bunker_service_set_configured_relays(BunkerService *, const gchar *const *relays);

/* Admission-only legacy inventory: empty admits no unpaired clients. */
void bunker_service_set_allowed_pubkeys(BunkerService *bs, const gchar *const *pubkeys);

/* Deprecated compatibility no-op: global kinds never grant permission. */
void bunker_service_set_auto_approve_kinds(BunkerService *bs, const gchar *const *kinds);

/* Set state change callback */
void bunker_service_set_state_callback(BunkerService *bs,
                                       BunkerStateChangedCb cb,
                                       gpointer user_data);

/* Set connection callback (for new connections) */
void bunker_service_set_connection_callback(BunkerService *bs,
                                            BunkerConnectionCb cb,
                                            gpointer user_data);

/* Set authorization callback (for sign requests) */
void bunker_service_set_authorize_callback(BunkerService *bs,
                                           BunkerAuthorizeCb cb,
                                           gpointer user_data);

/* Complete a pending authorization request */
void bunker_service_authorize_response(BunkerService *bs,
                                       const gchar *request_id,
                                       gboolean approved);

/* Free a connection info */
void bunker_connection_free(BunkerConnection *conn);

/* Free a sign request */
void bunker_sign_request_free(BunkerSignRequest *req);

/* Create a client session after approval (nostrc-09n)
 * Call this when user approves a request with "remember" checked
 * @client_pubkey: Client's public key (hex)
 * @app_name: Application name (optional)
 * @persistent: Whether to persist across restarts
 * @ttl_seconds: Session TTL (0 for default, -1 for never expires)
 */
void bunker_service_create_client_session(BunkerService *bs,
                                          const gchar *client_pubkey,
                                          const gchar *app_name,
                                          gboolean persistent,
                                          gint64 ttl_seconds);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_BUNKER_SERVICE_H */
