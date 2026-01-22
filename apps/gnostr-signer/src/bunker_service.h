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
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BunkerService BunkerService;

/* Bunker state */
typedef enum {
  BUNKER_STATE_STOPPED,
  BUNKER_STATE_STARTING,
  BUNKER_STATE_RUNNING,
  BUNKER_STATE_ERROR
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

/* Set allowed public keys (empty = allow all) */
void bunker_service_set_allowed_pubkeys(BunkerService *bs, const gchar *const *pubkeys);

/* Set auto-approve event kinds */
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

G_END_DECLS
