/* multisig_nip46.c - NIP-46 integration implementation
 *
 * Handles NIP-46 client connections for requesting signatures
 * from remote co-signers during multi-signature operations.
 *
 * Issue: nostrc-orz
 */
#include "multisig_nip46.h"
#include "multisig_coordinator.h"
#include "secret_store.h"
#include "secure-mem.h"

#include <nostr/nip46/nip46_client.h>
#include <nostr/nip46/nip46_uri.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <time.h>

/* Connection entry */
typedef struct {
  gchar *npub;
  gchar *bunker_uri;
  gchar **relays;
  gsize n_relays;
  RemoteSignerState state;
  gchar *error_message;
  gint64 last_contact;
  NostrNip46Session *session;

  /* Pending request tracking */
  gchar *pending_session_id;
} RemoteSignerConnection;

struct _MultisigNip46Client {
  GHashTable *connections;  /* npub -> RemoteSignerConnection* */
  gchar *our_identity_npub;

  /* Callbacks */
  MultisigNip46SignatureCb signature_cb;
  MultisigNip46RejectCb reject_cb;
  MultisigNip46StateCb state_cb;
  gpointer user_data;
};

static MultisigNip46Client *default_client = NULL;

/* ======== Memory Management ======== */

static void remote_connection_free(RemoteSignerConnection *conn) {
  if (!conn) return;
  g_free(conn->npub);
  g_free(conn->bunker_uri);
  g_free(conn->error_message);
  g_free(conn->pending_session_id);
  if (conn->relays) {
    g_strfreev(conn->relays);
  }
  if (conn->session) {
    nostr_nip46_session_free(conn->session);
  }
  g_free(conn);
}

MultisigNip46Client *multisig_nip46_client_new(void) {
  MultisigNip46Client *client = g_new0(MultisigNip46Client, 1);
  client->connections = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, (GDestroyNotify)remote_connection_free);
  return client;
}

void multisig_nip46_client_free(MultisigNip46Client *client) {
  if (!client) return;

  if (client->connections) {
    g_hash_table_destroy(client->connections);
  }
  g_free(client->our_identity_npub);

  if (client == default_client) {
    default_client = NULL;
  }
  g_free(client);
}

MultisigNip46Client *multisig_nip46_get_default(void) {
  if (!default_client) {
    default_client = multisig_nip46_client_new();
  }
  return default_client;
}

/* ======== Callbacks ======== */

void multisig_nip46_client_set_callbacks(MultisigNip46Client *client,
                                         MultisigNip46SignatureCb signature_cb,
                                         MultisigNip46RejectCb reject_cb,
                                         MultisigNip46StateCb state_cb,
                                         gpointer user_data) {
  if (!client) return;
  client->signature_cb = signature_cb;
  client->reject_cb = reject_cb;
  client->state_cb = state_cb;
  client->user_data = user_data;
}

static void set_connection_state(MultisigNip46Client *client,
                                 RemoteSignerConnection *conn,
                                 RemoteSignerState state,
                                 const gchar *error) {
  if (!client || !conn) return;

  conn->state = state;
  g_free(conn->error_message);
  conn->error_message = error ? g_strdup(error) : NULL;

  if (client->state_cb) {
    client->state_cb(conn->npub, state, error, client->user_data);
  }
}

/* ======== NIP-46 Library Callbacks ======== */

static int nip46_response_cb(const char *method,
                             const char *result,
                             const char *error,
                             void *user_data) {
  RemoteSignerConnection *conn = user_data;
  if (!conn) return -1;

  MultisigNip46Client *client = multisig_nip46_get_default();

  conn->last_contact = (gint64)time(NULL);

  if (g_strcmp0(method, "sign_event") == 0) {
    if (result && !error) {
      /* Signature received */
      set_connection_state(client, conn, REMOTE_SIGNER_CONNECTED, NULL);

      if (client->signature_cb && conn->pending_session_id) {
        client->signature_cb(conn->pending_session_id, conn->npub,
                             result, client->user_data);
      }
    } else {
      /* Signing failed or rejected */
      if (client->reject_cb && conn->pending_session_id) {
        client->reject_cb(conn->pending_session_id, conn->npub,
                          error ? error : "Unknown error", client->user_data);
      }
    }

    g_clear_pointer(&conn->pending_session_id, g_free);
  } else if (g_strcmp0(method, "connect") == 0) {
    if (!error) {
      set_connection_state(client, conn, REMOTE_SIGNER_CONNECTED, NULL);
      g_message("multisig_nip46: connected to %s", conn->npub);
    } else {
      set_connection_state(client, conn, REMOTE_SIGNER_ERROR, error);
      g_warning("multisig_nip46: connection to %s failed: %s", conn->npub, error);
    }
  }

  return 0;
}

/* ======== Connection Management ======== */

static gboolean parse_bunker_uri(const gchar *bunker_uri,
                                 gchar **out_npub,
                                 gchar ***out_relays,
                                 gsize *out_n_relays,
                                 gchar **out_secret) {
  if (!bunker_uri || !g_str_has_prefix(bunker_uri, "bunker://")) {
    return FALSE;
  }

  /* Parse: bunker://PUBKEY_HEX?relay=...&relay=...&secret=... */
  const gchar *pk_start = bunker_uri + strlen("bunker://");
  const gchar *pk_end = strchr(pk_start, '?');
  if (!pk_end) pk_end = pk_start + strlen(pk_start);

  gsize pk_len = pk_end - pk_start;
  if (pk_len != 64) {
    return FALSE;
  }

  gchar *pk_hex = g_strndup(pk_start, pk_len);

  /* Convert hex to npub */
  guint8 pk_bytes[32];
  gboolean valid = TRUE;
  for (gsize i = 0; i < 32 && valid; i++) {
    gchar byte_str[3] = { pk_hex[i*2], pk_hex[i*2+1], '\0' };
    gchar *end = NULL;
    gulong val = strtoul(byte_str, &end, 16);
    if (end != byte_str + 2) valid = FALSE;
    else pk_bytes[i] = (guint8)val;
  }
  g_free(pk_hex);

  if (!valid) return FALSE;

  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk_bytes, &npub) != 0 || !npub) {
    return FALSE;
  }

  if (out_npub) *out_npub = npub;
  else g_free(npub);

  /* Parse query parameters */
  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  gchar *secret = NULL;

  if (*pk_end == '?') {
    gchar *query = g_strdup(pk_end + 1);
    gchar **params = g_strsplit(query, "&", -1);

    for (gint i = 0; params[i]; i++) {
      if (g_str_has_prefix(params[i], "relay=")) {
        g_ptr_array_add(relays, g_strdup(params[i] + strlen("relay=")));
      } else if (g_str_has_prefix(params[i], "secret=")) {
        secret = g_strdup(params[i] + strlen("secret="));
      }
    }

    g_strfreev(params);
    g_free(query);
  }

  if (out_relays) {
    g_ptr_array_add(relays, NULL);
    *out_relays = (gchar**)g_ptr_array_free(relays, FALSE);
    if (out_n_relays) {
      *out_n_relays = 0;
      if (*out_relays) {
        while ((*out_relays)[*out_n_relays]) (*out_n_relays)++;
      }
    }
  } else {
    g_ptr_array_unref(relays);
  }

  if (out_secret) *out_secret = secret;
  else g_free(secret);

  return TRUE;
}

gboolean multisig_nip46_connect(MultisigNip46Client *client,
                                const gchar *bunker_uri,
                                const gchar *our_identity_npub,
                                GError **error) {
  if (!client || !bunker_uri) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid parameters");
    return FALSE;
  }

  /* Parse the bunker URI */
  gchar *npub = NULL;
  gchar **relays = NULL;
  gsize n_relays = 0;
  gchar *secret = NULL;

  if (!parse_bunker_uri(bunker_uri, &npub, &relays, &n_relays, &secret)) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid bunker URI format");
    return FALSE;
  }

  /* Check if already connected */
  RemoteSignerConnection *existing = g_hash_table_lookup(client->connections, npub);
  if (existing && existing->state == REMOTE_SIGNER_CONNECTED) {
    g_free(npub);
    g_strfreev(relays);
    g_free(secret);
    return TRUE;  /* Already connected */
  }

  /* Store our identity for signing */
  g_free(client->our_identity_npub);
  client->our_identity_npub = g_strdup(our_identity_npub);

  /* Create connection entry */
  RemoteSignerConnection *conn = g_new0(RemoteSignerConnection, 1);
  conn->npub = npub;
  conn->bunker_uri = g_strdup(bunker_uri);
  conn->relays = relays;
  conn->n_relays = n_relays;
  conn->state = REMOTE_SIGNER_CONNECTING;
  conn->last_contact = (gint64)time(NULL);

  g_hash_table_replace(client->connections, g_strdup(npub), conn);

  /* Create NIP-46 client session */
  NostrNip46ClientCallbacks cbs = {
    .response_cb = nip46_response_cb,
    .user_data = conn
  };

  conn->session = nostr_nip46_client_new(&cbs);
  if (!conn->session) {
    set_connection_state(client, conn, REMOTE_SIGNER_ERROR, "Failed to create session");
    g_free(secret);
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "Failed to create NIP-46 session");
    return FALSE;
  }

  /* Initiate connection */
  int rc = nostr_nip46_client_connect(conn->session,
                                       bunker_uri,
                                       secret,
                                       our_identity_npub);
  g_free(secret);

  if (rc != 0) {
    set_connection_state(client, conn, REMOTE_SIGNER_ERROR, "Connection initiation failed");
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_REMOTE_FAILED,
                "Failed to initiate NIP-46 connection");
    return FALSE;
  }

  g_message("multisig_nip46: connecting to %s via %zu relays", npub, n_relays);
  return TRUE;
}

void multisig_nip46_disconnect(MultisigNip46Client *client,
                               const gchar *signer_npub) {
  if (!client || !signer_npub) return;

  RemoteSignerConnection *conn = g_hash_table_lookup(client->connections, signer_npub);
  if (conn) {
    if (conn->session) {
      nostr_nip46_session_free(conn->session);
      conn->session = NULL;
    }
    set_connection_state(client, conn, REMOTE_SIGNER_DISCONNECTED, NULL);
    g_message("multisig_nip46: disconnected from %s", signer_npub);
  }
}

/* ======== Signature Requests ======== */

gboolean multisig_nip46_request_signature(MultisigNip46Client *client,
                                          const gchar *signer_npub,
                                          const gchar *session_id,
                                          const gchar *event_json,
                                          GError **error) {
  if (!client || !signer_npub || !session_id || !event_json) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Invalid parameters");
    return FALSE;
  }

  RemoteSignerConnection *conn = g_hash_table_lookup(client->connections, signer_npub);
  if (!conn) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "No connection to signer: %s", signer_npub);
    return FALSE;
  }

  if (conn->state != REMOTE_SIGNER_CONNECTED) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_REMOTE_FAILED,
                "Signer not connected: %s", signer_npub);
    return FALSE;
  }

  if (!conn->session) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "No NIP-46 session for signer");
    return FALSE;
  }

  /* Store pending session ID */
  g_free(conn->pending_session_id);
  conn->pending_session_id = g_strdup(session_id);

  /* Send sign_event request */
  int rc = nostr_nip46_client_sign_event(conn->session, event_json);

  if (rc != 0) {
    g_free(conn->pending_session_id);
    conn->pending_session_id = NULL;

    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_REMOTE_FAILED,
                "Failed to send sign request");
    return FALSE;
  }

  g_message("multisig_nip46: sent sign_event request to %s for session %s",
            signer_npub, session_id);
  return TRUE;
}

/* ======== State Queries ======== */

RemoteSignerState multisig_nip46_get_state(MultisigNip46Client *client,
                                           const gchar *signer_npub) {
  if (!client || !signer_npub) {
    return REMOTE_SIGNER_DISCONNECTED;
  }

  RemoteSignerConnection *conn = g_hash_table_lookup(client->connections, signer_npub);
  if (!conn) {
    return REMOTE_SIGNER_DISCONNECTED;
  }

  return conn->state;
}

gboolean multisig_nip46_is_connected(MultisigNip46Client *client,
                                     const gchar *signer_npub) {
  return multisig_nip46_get_state(client, signer_npub) == REMOTE_SIGNER_CONNECTED;
}
