/* multisig_coordinator.c - Signing coordination implementation
 *
 * Coordinates multi-signature signing across local and remote co-signers.
 * Integrates with bunker_service for NIP-46 remote signing.
 *
 * Issue: nostrc-orz
 */
#include "multisig_coordinator.h"
#include "multisig_wallet.h"
#include "multisig_store.h"
#include "secret_store.h"
#include "bunker_service.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include <nostr/nip19/nip19.h>
#include <nostr/nip46/nip46_client.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default timeout for signing requests: 5 minutes */
#define DEFAULT_SIGNING_TIMEOUT_SECONDS 300

/* Session data for tracking callbacks */
typedef struct {
  gchar *session_id;
  gchar *wallet_id;
  gchar *event_json;
  gint event_kind;
  MultisigCoordinatorProgressCb progress_cb;
  MultisigCoordinatorCompleteCb complete_cb;
  gpointer user_data;
  guint timeout_source_id;
  GHashTable *pending_local;  /* npub -> TRUE if pending local signature */
} CoordinatorSession;

/* Remote signer connection */
typedef struct {
  gchar *npub;
  gchar *bunker_uri;
  RemoteSignerState state;
  gchar *error_message;
  gint64 last_contact;
  NostrNip46Session *nip46_session;
} RemoteConnection;

struct _MultisigCoordinator {
  GHashTable *sessions;           /* session_id -> CoordinatorSession* */
  GHashTable *remote_connections; /* npub -> RemoteConnection* */
  MultisigCoordinatorPromptCb prompt_cb;
  gpointer prompt_cb_ud;
};

static MultisigCoordinator *default_coordinator = NULL;

/* Forward declarations */
static void session_free(CoordinatorSession *session);
static void remote_connection_free(RemoteConnection *conn);
static void check_session_complete(MultisigCoordinator *coordinator,
                                   CoordinatorSession *session);
static void sign_with_local_key(MultisigCoordinator *coordinator,
                                CoordinatorSession *session,
                                const gchar *signer_npub);
static void request_remote_signature(MultisigCoordinator *coordinator,
                                     CoordinatorSession *session,
                                     const gchar *bunker_uri);

/* ======== Memory Management ======== */

static void session_free(CoordinatorSession *session) {
  if (!session) return;
  g_free(session->session_id);
  g_free(session->wallet_id);
  g_free(session->event_json);
  if (session->pending_local) {
    g_hash_table_destroy(session->pending_local);
  }
  if (session->timeout_source_id > 0) {
    g_source_remove(session->timeout_source_id);
  }
  g_free(session);
}

static void remote_connection_free(RemoteConnection *conn) {
  if (!conn) return;
  g_free(conn->npub);
  g_free(conn->bunker_uri);
  g_free(conn->error_message);
  if (conn->nip46_session) {
    nostr_nip46_session_free(conn->nip46_session);
  }
  g_free(conn);
}

void remote_signer_info_free(RemoteSignerInfo *info) {
  if (!info) return;
  g_free(info->npub);
  g_free(info->bunker_uri);
  g_free(info->error_message);
  g_free(info);
}

/* ======== Coordinator Instance ======== */

MultisigCoordinator *multisig_coordinator_get_default(void) {
  if (!default_coordinator) {
    default_coordinator = g_new0(MultisigCoordinator, 1);
    default_coordinator->sessions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                           g_free, (GDestroyNotify)session_free);
    default_coordinator->remote_connections = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                                      g_free, (GDestroyNotify)remote_connection_free);
  }
  return default_coordinator;
}

void multisig_coordinator_free(MultisigCoordinator *coordinator) {
  if (!coordinator) return;

  if (coordinator->sessions) {
    g_hash_table_destroy(coordinator->sessions);
  }
  if (coordinator->remote_connections) {
    g_hash_table_destroy(coordinator->remote_connections);
  }

  if (coordinator == default_coordinator) {
    default_coordinator = NULL;
  }
  g_free(coordinator);
}

void multisig_coordinator_set_prompt_callback(MultisigCoordinator *coordinator,
                                              MultisigCoordinatorPromptCb cb,
                                              gpointer user_data) {
  if (!coordinator) return;
  coordinator->prompt_cb = cb;
  coordinator->prompt_cb_ud = user_data;
}

/* ======== Session Timeout ======== */

static gboolean session_timeout_cb(gpointer user_data) {
  gchar *session_id = user_data;

  if (!default_coordinator) {
    g_free(session_id);
    return G_SOURCE_REMOVE;
  }

  CoordinatorSession *session = g_hash_table_lookup(default_coordinator->sessions, session_id);
  if (!session) {
    g_free(session_id);
    return G_SOURCE_REMOVE;
  }

  session->timeout_source_id = 0;

  /* Notify completion with timeout */
  if (session->complete_cb) {
    session->complete_cb(session->session_id, FALSE, NULL,
                         "Signing session timed out", session->user_data);
  }

  g_hash_table_remove(default_coordinator->sessions, session_id);
  g_free(session_id);
  return G_SOURCE_REMOVE;
}

/* ======== Signing Flow ======== */

gboolean multisig_coordinator_start_signing(MultisigCoordinator *coordinator,
                                            const gchar *wallet_id,
                                            const gchar *event_json,
                                            gboolean auto_sign_local,
                                            MultisigCoordinatorProgressCb progress_cb,
                                            MultisigCoordinatorCompleteCb complete_cb,
                                            gpointer user_data,
                                            gchar **out_session_id,
                                            GError **error) {
  if (!coordinator || !wallet_id || !event_json) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Invalid parameters");
    return FALSE;
  }

  /* Get wallet */
  MultisigWallet *wallet = NULL;
  MultisigResult rc = multisig_wallet_get(wallet_id, &wallet);
  if (rc != MULTISIG_OK || !wallet) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Wallet not found: %s", wallet_id);
    return FALSE;
  }

  /* Validate configuration */
  if (wallet->cosigners->len < wallet->threshold_m) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Not enough co-signers configured");
    multisig_wallet_free(wallet);
    return FALSE;
  }

  /* Start multisig signing session */
  gchar *session_id = NULL;
  rc = multisig_signing_start(wallet_id, event_json, DEFAULT_SIGNING_TIMEOUT_SECONDS,
                              NULL, NULL, NULL, &session_id, error);
  if (rc != MULTISIG_OK || !session_id) {
    multisig_wallet_free(wallet);
    return FALSE;
  }

  /* Create coordinator session */
  CoordinatorSession *session = g_new0(CoordinatorSession, 1);
  session->session_id = g_strdup(session_id);
  session->wallet_id = g_strdup(wallet_id);
  session->event_json = g_strdup(event_json);
  session->progress_cb = progress_cb;
  session->complete_cb = complete_cb;
  session->user_data = user_data;
  session->pending_local = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Extract event kind for display */
  const gchar *kind_str = strstr(event_json, "\"kind\"");
  if (kind_str) {
    kind_str = strchr(kind_str, ':');
    if (kind_str) {
      kind_str++;
      while (*kind_str == ' ') kind_str++;
      session->event_kind = (gint)g_ascii_strtoll(kind_str, NULL, 10);
    }
  }

  /* Set timeout */
  session->timeout_source_id = g_timeout_add_seconds(DEFAULT_SIGNING_TIMEOUT_SECONDS,
                                                      session_timeout_cb,
                                                      g_strdup(session_id));

  g_hash_table_replace(coordinator->sessions, g_strdup(session_id), session);

  g_message("multisig_coordinator: started signing session %s for wallet %s",
            session_id, wallet_id);

  /* Process each co-signer */
  for (guint i = 0; i < wallet->cosigners->len; i++) {
    MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);

    if (cs->type == COSIGNER_TYPE_LOCAL) {
      /* Local co-signer */
      if (auto_sign_local || cs->is_self) {
        /* Auto-sign with local key */
        sign_with_local_key(coordinator, session, cs->npub);
      } else if (coordinator->prompt_cb) {
        /* Add to pending and prompt user */
        g_hash_table_insert(session->pending_local, g_strdup(cs->npub),
                            GINT_TO_POINTER(TRUE));

        gboolean approved = coordinator->prompt_cb(session_id, event_json,
                                                    session->event_kind,
                                                    wallet->name,
                                                    coordinator->prompt_cb_ud);
        if (approved) {
          sign_with_local_key(coordinator, session, cs->npub);
          g_hash_table_remove(session->pending_local, cs->npub);
        } else {
          multisig_signing_reject(session_id, cs->npub, "User rejected");
          g_hash_table_remove(session->pending_local, cs->npub);
        }
      }
    } else if (cs->type == COSIGNER_TYPE_REMOTE_NIP46 && cs->bunker_uri) {
      /* Remote co-signer via NIP-46 */
      request_remote_signature(coordinator, session, cs->bunker_uri);
    }
  }

  if (out_session_id) {
    *out_session_id = session_id;
  } else {
    g_free(session_id);
  }

  multisig_wallet_free(wallet);
  return TRUE;
}

static void sign_with_local_key(MultisigCoordinator *coordinator,
                                CoordinatorSession *session,
                                const gchar *signer_npub) {
  (void)coordinator;

  if (!session || !signer_npub) return;

  g_message("multisig_coordinator: signing with local key %s", signer_npub);

  /* Sign the event using the local key */
  gchar *signature = NULL;
  SecretStoreResult rc = secret_store_sign_event(session->event_json, signer_npub, &signature);

  if (rc == SECRET_STORE_OK && signature) {
    /* Add partial signature to the session */
    GError *error = NULL;
    MultisigResult mr = multisig_signing_add_signature(session->session_id,
                                                        signer_npub,
                                                        signature,
                                                        &error);
    if (mr == MULTISIG_OK) {
      g_message("multisig_coordinator: local signature added from %s", signer_npub);

      /* Notify progress */
      if (session->progress_cb) {
        MultisigSigningSession *status = NULL;
        if (multisig_signing_get_status(session->session_id, &status) == MULTISIG_OK) {
          session->progress_cb(session->session_id,
                               status->signatures_collected,
                               status->signatures_required,
                               signer_npub,
                               session->user_data);
          multisig_signing_session_free(status);
        }
      }

      /* Check if complete */
      check_session_complete(coordinator, session);
    } else {
      g_warning("multisig_coordinator: failed to add signature: %s",
                error ? error->message : "unknown");
      g_clear_error(&error);
    }

    gn_secure_strfree(signature);
  } else {
    g_warning("multisig_coordinator: failed to sign with local key %s: %d",
              signer_npub, rc);
    multisig_signing_reject(session->session_id, signer_npub, "Local signing failed");
  }
}

static void request_remote_signature(MultisigCoordinator *coordinator,
                                     CoordinatorSession *session,
                                     const gchar *bunker_uri) {
  if (!coordinator || !session || !bunker_uri) return;

  g_message("multisig_coordinator: requesting remote signature via %s", bunker_uri);

  /* Extract npub from bunker URI */
  const gchar *pk_start = bunker_uri + strlen("bunker://");
  const gchar *pk_end = strchr(pk_start, '?');
  if (!pk_end) pk_end = pk_start + strlen(pk_start);

  gchar *pk_hex = g_strndup(pk_start, pk_end - pk_start);

  /* Check if we already have a connection */
  RemoteConnection *conn = NULL;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, coordinator->remote_connections);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    RemoteConnection *c = value;
    if (g_strcmp0(c->bunker_uri, bunker_uri) == 0) {
      conn = c;
      break;
    }
  }

  if (!conn) {
    /* Create a new connection entry */
    conn = g_new0(RemoteConnection, 1);
    conn->bunker_uri = g_strdup(bunker_uri);
    conn->state = REMOTE_SIGNER_CONNECTING;

    /* Convert hex to npub for storage key */
    guint8 pk_bytes[32];
    gboolean valid = TRUE;
    for (gsize i = 0; i < 32 && valid; i++) {
      gchar byte_str[3] = { pk_hex[i*2], pk_hex[i*2+1], '\0' };
      gchar *end = NULL;
      gulong val = strtoul(byte_str, &end, 16);
      if (end != byte_str + 2) valid = FALSE;
      else pk_bytes[i] = (guint8)val;
    }

    if (valid) {
      gchar *npub = NULL;
      nostr_nip19_encode_npub(pk_bytes, &npub);
      if (npub) {
        conn->npub = npub;
        g_hash_table_replace(coordinator->remote_connections, g_strdup(npub), conn);
      } else {
        conn->npub = g_strdup(pk_hex);
        g_hash_table_replace(coordinator->remote_connections, g_strdup(pk_hex), conn);
      }
    }
  }

  g_free(pk_hex);

  /* Create NIP-46 client session and connect to bunker */
  if (!conn->nip46_session) {
    conn->nip46_session = nostr_nip46_client_new();
    if (!conn->nip46_session) {
      conn->state = REMOTE_SIGNER_ERROR;
      conn->error_message = g_strdup("Failed to create NIP-46 session");
      g_warning("multisig_coordinator: failed to create NIP-46 session for %s", conn->npub);
      return;
    }

    /* Connect to the bunker - this initiates the NIP-46 handshake */
    int rc = nostr_nip46_client_connect(conn->nip46_session, bunker_uri, NULL);
    if (rc != 0) {
      conn->state = REMOTE_SIGNER_ERROR;
      conn->error_message = g_strdup("NIP-46 connection failed");
      g_warning("multisig_coordinator: NIP-46 connect failed for %s (rc=%d)", conn->npub, rc);
      nostr_nip46_session_free(conn->nip46_session);
      conn->nip46_session = NULL;
      return;
    }

    conn->state = REMOTE_SIGNER_CONNECTED;
    g_message("multisig_coordinator: connected to remote signer %s via NIP-46", conn->npub);
  }

  /* Send sign_event request */
  char *signed_event_json = NULL;
  int sign_rc = nostr_nip46_client_sign_event(conn->nip46_session,
                                               session->event_json,
                                               &signed_event_json);

  if (sign_rc == 0 && signed_event_json) {
    /* Extract signature from signed event JSON */
    const gchar *sig_start = strstr(signed_event_json, "\"sig\"");
    if (sig_start) {
      sig_start = strchr(sig_start, ':');
      if (sig_start) {
        sig_start++;
        while (*sig_start == ' ' || *sig_start == '"') sig_start++;
        const gchar *sig_end = strchr(sig_start, '"');
        if (sig_end && sig_end - sig_start == 128) {
          gchar *sig = g_strndup(sig_start, 128);

          /* Route signature to the coordinator */
          multisig_coordinator_receive_remote_signature(coordinator,
                                                         session->session_id,
                                                         conn->npub,
                                                         sig);
          g_free(sig);
          g_message("multisig_coordinator: received signature from %s", conn->npub);
        }
      }
    }
    free(signed_event_json);
  } else {
    /* Signing request sent but response pending (async) or failed */
    conn->state = REMOTE_SIGNER_CONNECTED;
    g_message("multisig_coordinator: sign_event request sent to %s", conn->npub);
  }

  conn->last_contact = (gint64)time(NULL);
  g_message("multisig_coordinator: remote signature request sent to %s", conn->npub);
}

static void check_session_complete(MultisigCoordinator *coordinator,
                                   CoordinatorSession *session) {
  if (!coordinator || !session) return;

  MultisigSigningSession *status = NULL;
  if (multisig_signing_get_status(session->session_id, &status) != MULTISIG_OK) {
    return;
  }

  if (status->is_complete) {
    /* Get final signature */
    gchar *signature = NULL;
    GError *error = NULL;
    MultisigResult rc = multisig_signing_get_final_signature(session->session_id,
                                                              &signature, &error);

    /* Cancel timeout */
    if (session->timeout_source_id > 0) {
      g_source_remove(session->timeout_source_id);
      session->timeout_source_id = 0;
    }

    /* Notify completion */
    if (session->complete_cb) {
      if (rc == MULTISIG_OK && signature) {
        session->complete_cb(session->session_id, TRUE, signature, NULL,
                             session->user_data);
      } else {
        session->complete_cb(session->session_id, FALSE, NULL,
                             error ? error->message : "Failed to get final signature",
                             session->user_data);
      }
    }

    if (signature) {
      gn_secure_strfree(signature);
    }
    g_clear_error(&error);

    g_message("multisig_coordinator: session %s complete", session->session_id);
  }

  multisig_signing_session_free(status);
}

/* ======== Manual Approval/Rejection ======== */

void multisig_coordinator_approve_local(MultisigCoordinator *coordinator,
                                        const gchar *session_id,
                                        const gchar *signer_npub) {
  if (!coordinator || !session_id || !signer_npub) return;

  CoordinatorSession *session = g_hash_table_lookup(coordinator->sessions, session_id);
  if (!session) return;

  if (g_hash_table_contains(session->pending_local, signer_npub)) {
    sign_with_local_key(coordinator, session, signer_npub);
    g_hash_table_remove(session->pending_local, signer_npub);
  }
}

void multisig_coordinator_reject_local(MultisigCoordinator *coordinator,
                                       const gchar *session_id,
                                       const gchar *signer_npub) {
  if (!coordinator || !session_id || !signer_npub) return;

  CoordinatorSession *session = g_hash_table_lookup(coordinator->sessions, session_id);
  if (!session) return;

  if (g_hash_table_contains(session->pending_local, signer_npub)) {
    multisig_signing_reject(session_id, signer_npub, "User rejected");
    g_hash_table_remove(session->pending_local, signer_npub);
  }
}

/* ======== Remote Signature Handling ======== */

void multisig_coordinator_receive_remote_signature(MultisigCoordinator *coordinator,
                                                   const gchar *session_id,
                                                   const gchar *signer_npub,
                                                   const gchar *partial_sig) {
  if (!coordinator || !session_id || !signer_npub || !partial_sig) return;

  CoordinatorSession *session = g_hash_table_lookup(coordinator->sessions, session_id);
  if (!session) {
    g_warning("multisig_coordinator: received signature for unknown session %s", session_id);
    return;
  }

  /* Update remote connection state */
  RemoteConnection *conn = g_hash_table_lookup(coordinator->remote_connections, signer_npub);
  if (conn) {
    conn->state = REMOTE_SIGNER_CONNECTED;
    conn->last_contact = (gint64)time(NULL);
  }

  /* Add partial signature */
  GError *error = NULL;
  MultisigResult rc = multisig_signing_add_signature(session_id, signer_npub,
                                                      partial_sig, &error);
  if (rc == MULTISIG_OK) {
    g_message("multisig_coordinator: received remote signature from %s", signer_npub);

    /* Notify progress */
    if (session->progress_cb) {
      MultisigSigningSession *status = NULL;
      if (multisig_signing_get_status(session_id, &status) == MULTISIG_OK) {
        session->progress_cb(session_id,
                             status->signatures_collected,
                             status->signatures_required,
                             signer_npub,
                             session->user_data);
        multisig_signing_session_free(status);
      }
    }

    /* Check if complete */
    check_session_complete(coordinator, session);
  } else {
    g_warning("multisig_coordinator: failed to add remote signature: %s",
              error ? error->message : "unknown");
    g_clear_error(&error);
  }
}

void multisig_coordinator_remote_rejected(MultisigCoordinator *coordinator,
                                          const gchar *session_id,
                                          const gchar *signer_npub,
                                          const gchar *reason) {
  if (!coordinator || !session_id || !signer_npub) return;

  CoordinatorSession *session = g_hash_table_lookup(coordinator->sessions, session_id);
  if (!session) return;

  multisig_signing_reject(session_id, signer_npub, reason);

  g_message("multisig_coordinator: remote signer %s rejected: %s",
            signer_npub, reason ? reason : "no reason");
}

/* ======== Session Management ======== */

void multisig_coordinator_cancel_session(MultisigCoordinator *coordinator,
                                         const gchar *session_id) {
  if (!coordinator || !session_id) return;

  CoordinatorSession *session = g_hash_table_lookup(coordinator->sessions, session_id);
  if (!session) return;

  /* Cancel underlying multisig session */
  multisig_signing_cancel(session_id);

  /* Notify completion with canceled status */
  if (session->complete_cb) {
    session->complete_cb(session_id, FALSE, NULL, "Signing canceled",
                         session->user_data);
  }

  g_hash_table_remove(coordinator->sessions, session_id);
  g_message("multisig_coordinator: canceled session %s", session_id);
}

gboolean multisig_coordinator_get_session_progress(MultisigCoordinator *coordinator,
                                                   const gchar *session_id,
                                                   guint *out_collected,
                                                   guint *out_required) {
  if (!coordinator || !session_id) return FALSE;

  MultisigSigningSession *status = NULL;
  if (multisig_signing_get_status(session_id, &status) != MULTISIG_OK) {
    return FALSE;
  }

  if (out_collected) *out_collected = status->signatures_collected;
  if (out_required) *out_required = status->signatures_required;

  multisig_signing_session_free(status);
  return TRUE;
}

/* ======== Remote Connection Management ======== */

gboolean multisig_coordinator_connect_remote(MultisigCoordinator *coordinator,
                                             const gchar *bunker_uri,
                                             GError **error) {
  if (!coordinator || !bunker_uri) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid parameters");
    return FALSE;
  }

  if (!g_str_has_prefix(bunker_uri, "bunker://")) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid bunker URI format");
    return FALSE;
  }

  /* Extract npub from bunker URI */
  const gchar *pk_start = bunker_uri + strlen("bunker://");
  const gchar *pk_end = strchr(pk_start, '?');
  if (!pk_end) pk_end = pk_start + strlen(pk_start);

  gsize pk_len = pk_end - pk_start;
  if (pk_len != 64) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid pubkey in bunker URI");
    return FALSE;
  }

  gchar *pk_hex = g_strndup(pk_start, pk_len);

  /* Convert to npub */
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

  if (!valid) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid hex in bunker URI");
    return FALSE;
  }

  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk_bytes, &npub) != 0 || !npub) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "Failed to encode npub");
    return FALSE;
  }

  /* Create or update connection */
  RemoteConnection *conn = g_hash_table_lookup(coordinator->remote_connections, npub);
  if (!conn) {
    conn = g_new0(RemoteConnection, 1);
    conn->npub = g_strdup(npub);
    conn->bunker_uri = g_strdup(bunker_uri);
    g_hash_table_replace(coordinator->remote_connections, g_strdup(npub), conn);
  }

  conn->last_contact = (gint64)time(NULL);

  g_free(npub);

  /* Create NIP-46 client session and connect to bunker */
  if (!conn->nip46_session) {
    conn->nip46_session = nostr_nip46_client_new();
    if (!conn->nip46_session) {
      conn->state = REMOTE_SIGNER_ERROR;
      conn->error_message = g_strdup("Failed to create NIP-46 session");
      g_warning("multisig_coordinator: failed to create NIP-46 session");
      g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                  "Failed to create NIP-46 session");
      return FALSE;
    }

    conn->state = REMOTE_SIGNER_CONNECTING;

    /* Connect to the bunker - this initiates the NIP-46 handshake */
    int rc = nostr_nip46_client_connect(conn->nip46_session, bunker_uri, NULL);
    if (rc != 0) {
      conn->state = REMOTE_SIGNER_ERROR;
      conn->error_message = g_strdup("NIP-46 connection failed");
      g_warning("multisig_coordinator: NIP-46 connect failed (rc=%d)", rc);
      nostr_nip46_session_free(conn->nip46_session);
      conn->nip46_session = NULL;
      g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                  "NIP-46 connection failed");
      return FALSE;
    }

    conn->state = REMOTE_SIGNER_CONNECTED;
    g_message("multisig_coordinator: connected to remote signer via NIP-46");
  }

  return TRUE;
}

void multisig_coordinator_disconnect_remote(MultisigCoordinator *coordinator,
                                            const gchar *npub) {
  if (!coordinator || !npub) return;

  RemoteConnection *conn = g_hash_table_lookup(coordinator->remote_connections, npub);
  if (conn) {
    if (conn->nip46_session) {
      nostr_nip46_session_free(conn->nip46_session);
      conn->nip46_session = NULL;
    }
    conn->state = REMOTE_SIGNER_DISCONNECTED;
    g_message("multisig_coordinator: disconnected from remote signer %s", npub);
  }
}

GPtrArray *multisig_coordinator_list_remote_signers(MultisigCoordinator *coordinator) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)remote_signer_info_free);

  if (!coordinator) return arr;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, coordinator->remote_connections);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    RemoteConnection *conn = value;

    RemoteSignerInfo *info = g_new0(RemoteSignerInfo, 1);
    info->npub = g_strdup(conn->npub);
    info->bunker_uri = g_strdup(conn->bunker_uri);
    info->state = conn->state;
    info->error_message = g_strdup(conn->error_message);
    info->last_contact = conn->last_contact;

    g_ptr_array_add(arr, info);
  }

  return arr;
}

RemoteSignerState multisig_coordinator_get_remote_signer_state(MultisigCoordinator *coordinator,
                                                               const gchar *npub) {
  if (!coordinator || !npub) {
    return REMOTE_SIGNER_DISCONNECTED;
  }

  RemoteConnection *conn = g_hash_table_lookup(coordinator->remote_connections, npub);
  if (!conn) {
    return REMOTE_SIGNER_DISCONNECTED;
  }

  return conn->state;
}
