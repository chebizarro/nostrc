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

#include "nostr_nip46_client.h"
#include <nostr_nip19.h>
#include <stdlib.h>
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
  GNostrNip46Client *client;

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
  if (conn->client) {
    g_object_unref(conn->client);
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
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(pk_hex, NULL);
  g_free(pk_hex);

  if (!nip19) {
    return FALSE;
  }

  if (out_npub) *out_npub = g_strdup(gnostr_nip19_get_bech32(nip19));
  g_object_unref(nip19);

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

  /* Create NIP-46 client (GObject wrapper - hq-m2k0n) */
  conn->client = gnostr_nip46_client_new();
  if (!conn->client) {
    set_connection_state(client, conn, REMOTE_SIGNER_ERROR, "Failed to create client");
    g_free(secret);
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "Failed to create NIP-46 client");
    return FALSE;
  }

  /* Parse bunker URI and configure session */
  (void)secret;
  (void)our_identity_npub;
  GError *connect_error = NULL;
  gboolean ok = gnostr_nip46_client_connect_to_bunker(conn->client,
                                                       bunker_uri,
                                                       NULL /* permissions */,
                                                       &connect_error);
  g_free(secret);
  int rc = ok ? 0 : -1;
  g_clear_error(&connect_error);

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
    if (conn->client) {
      gnostr_nip46_client_stop(conn->client);
      g_object_unref(conn->client);
      conn->client = NULL;
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

  if (!conn->client) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "No NIP-46 client for signer");
    return FALSE;
  }

  /* Store pending session ID */
  g_free(conn->pending_session_id);
  conn->pending_session_id = g_strdup(session_id);

  /* Send sign_event request via GObject wrapper */
  gchar *signed_event_json = NULL;
  GError *sign_error = NULL;
  gboolean ok = gnostr_nip46_client_sign_event(conn->client, event_json,
                                                &signed_event_json, &sign_error);

  if (!ok) {
    g_free(conn->pending_session_id);
    conn->pending_session_id = NULL;

    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_REMOTE_FAILED,
                "Failed to send sign request: %s",
                sign_error ? sign_error->message : "unknown");
    g_clear_error(&sign_error);
    return FALSE;
  }

  /* Process signed_event_json response - extract signature and invoke callback */
  if (signed_event_json) {
    g_message("multisig_nip46: got signed event from %s", signer_npub);

    /* Extract the signature from the signed event JSON
     * Format: {..., "sig": "hexstring", ...} */
    const gchar *sig_start = strstr(signed_event_json, "\"sig\"");
    gchar *extracted_sig = NULL;

    if (sig_start) {
      sig_start = strchr(sig_start, ':');
      if (sig_start) {
        sig_start++;
        /* Skip whitespace and opening quote */
        while (*sig_start == ' ' || *sig_start == '\t') sig_start++;
        if (*sig_start == '"') sig_start++;

        /* Find end of signature (closing quote) */
        const gchar *sig_end = strchr(sig_start, '"');
        if (sig_end) {
          gsize sig_len = sig_end - sig_start;
          /* Schnorr signatures are 64 bytes = 128 hex chars */
          if (sig_len == 128) {
            extracted_sig = g_strndup(sig_start, sig_len);

            /* Validate hex characters */
            gboolean valid = TRUE;
            for (gsize i = 0; i < 128 && valid; i++) {
              gchar c = extracted_sig[i];
              if (!((c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F'))) {
                valid = FALSE;
              }
            }

            if (!valid) {
              g_free(extracted_sig);
              extracted_sig = NULL;
              g_warning("multisig_nip46: invalid signature hex from %s", signer_npub);
            }
          } else {
            g_warning("multisig_nip46: unexpected signature length %zu from %s",
                      sig_len, signer_npub);
          }
        }
      }
    }

    if (extracted_sig) {
      /* Update connection state */
      set_connection_state(client, conn, REMOTE_SIGNER_CONNECTED, NULL);
      conn->last_contact = (gint64)time(NULL);

      /* Invoke the signature callback to route to coordinator */
      if (client->signature_cb && conn->pending_session_id) {
        client->signature_cb(conn->pending_session_id, signer_npub,
                             extracted_sig, client->user_data);
        g_message("multisig_nip46: delivered signature from %s to session %s",
                  signer_npub, conn->pending_session_id);
      }

      g_free(extracted_sig);
    } else {
      /* Failed to extract signature - check if it's an error response */
      const gchar *error_start = strstr(signed_event_json, "\"error\"");
      if (error_start) {
        error_start = strchr(error_start, ':');
        if (error_start) {
          error_start++;
          while (*error_start == ' ' || *error_start == '"') error_start++;
          const gchar *error_end = strchr(error_start, '"');
          if (error_end) {
            gchar *error_msg = g_strndup(error_start, error_end - error_start);

            if (client->reject_cb && conn->pending_session_id) {
              client->reject_cb(conn->pending_session_id, signer_npub,
                                error_msg, client->user_data);
            }
            g_warning("multisig_nip46: signer %s returned error: %s",
                      signer_npub, error_msg);
            g_free(error_msg);
          }
        }
      } else {
        g_warning("multisig_nip46: could not extract signature from response");
      }
    }

    /* Clear pending session after processing */
    g_clear_pointer(&conn->pending_session_id, g_free);

    g_free(signed_event_json);
  } else {
    /* No immediate response - request is pending asynchronously */
    g_message("multisig_nip46: sign_event request sent to %s for session %s (async)",
              signer_npub, session_id);
  }

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
