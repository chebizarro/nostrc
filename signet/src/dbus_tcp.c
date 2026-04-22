/* SPDX-License-Identifier: MIT
 *
 * dbus_tcp.c - D-Bus TCP transport for Signet v2 cross-host LAN access.
 *
 * Creates a GDBusServer on TCP with SASL ANONYMOUS auth mechanism.
 * After connection, client must complete Nostr-signed challenge auth
 * via nostr_auth.c before any method call is dispatched.
 *
 * Uses GDBusServer (from GLib/GIO) for the TCP listener.
 */

#include "signet/dbus_tcp.h"
#include "signet/key_store.h"
#include "signet/capability.h"
#include "signet/nostr_auth.h"
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_secrets.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <glib.h>
#include <sodium.h>

/* libnostr event signing + NIP-04 */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#include <json.h>

#define SIGNET_TCP_OBJECT_PATH "/net/signet/Signer"

/* Introspection XML — same interfaces as Unix transport, plus Auth. */
static const char *signet_tcp_introspection_xml =
    "<node>"
    "  <interface name='net.signet.Auth'>"
    "    <method name='GetChallenge'>"
    "      <arg type='s' direction='in' name='agent_id'/>"
    "      <arg type='s' direction='out' name='challenge'/>"
    "    </method>"
    "    <method name='Authenticate'>"
    "      <arg type='s' direction='in' name='signed_auth_event'/>"
    "      <arg type='b' direction='out' name='ok'/>"
    "      <arg type='s' direction='out' name='agent_id'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='net.signet.Signer'>"
    "    <method name='GetPublicKey'>"
    "      <arg type='s' direction='out' name='pubkey_hex'/>"
    "    </method>"
    "    <method name='SignEvent'>"
    "      <arg type='s' direction='in' name='event_json'/>"
    "      <arg type='s' direction='out' name='signed_event_json'/>"
    "    </method>"
    "    <method name='Encrypt'>"
    "      <arg type='s' direction='in' name='plaintext'/>"
    "      <arg type='s' direction='in' name='peer_pubkey_hex'/>"
    "      <arg type='s' direction='in' name='algorithm'/>"
    "      <arg type='s' direction='out' name='ciphertext'/>"
    "    </method>"
    "    <method name='Decrypt'>"
    "      <arg type='s' direction='in' name='ciphertext'/>"
    "      <arg type='s' direction='in' name='peer_pubkey_hex'/>"
    "      <arg type='s' direction='in' name='algorithm'/>"
    "      <arg type='s' direction='out' name='plaintext'/>"
    "    </method>"
    "  </interface>"
    "  <interface name='net.signet.Credentials'>"
    "    <method name='GetSession'>"
    "      <arg type='s' direction='in' name='service_url'/>"
    "      <arg type='s' direction='out' name='session_token'/>"
    "      <arg type='x' direction='out' name='expires_at'/>"
    "    </method>"
    "    <method name='GetToken'>"
    "      <arg type='s' direction='in' name='credential_type'/>"
    "      <arg type='s' direction='out' name='token'/>"
    "      <arg type='x' direction='out' name='expires_at'/>"
    "    </method>"
    "    <method name='ListCredentials'>"
    "      <arg type='as' direction='out' name='credential_types'/>"
    "    </method>"
    "  </interface>"
    "</node>";

/* Per-connection state: tracks whether client has authenticated. */
typedef struct {
  char *agent_id;      /* set after successful auth */
  char *pubkey_hex;    /* set after successful auth */
  bool authenticated;
} TcpConnState;

static void tcp_conn_state_free(gpointer p) {
  TcpConnState *s = (TcpConnState *)p;
  if (!s) return;
  g_free(s->agent_id);
  g_free(s->pubkey_hex);
  g_free(s);
}

struct SignetDbusTcpServer {
  char *listen_address;
  SignetKeyStore *keys;
  SignetPolicyRegistry *policy;
  SignetStore *store;
  SignetChallengeStore *challenges;
  SignetAuditLogger *audit;
  const SignetFleetRegistry *fleet;

  GDBusServer *server;
  GDBusNodeInfo *node_info;
  GHashTable *conn_states;  /* GDBusConnection* → TcpConnState* */
  GMutex mu;
};

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

/* ----------------------------- Auth interface ----------------------------- */

static void signet_tcp_handle_auth(GDBusConnection *connection,
                                    const gchar *sender,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    const gchar *method_name,
                                    GVariant *parameters,
                                    GDBusMethodInvocation *invocation,
                                    gpointer user_data) {
  (void)sender;
  (void)object_path;
  (void)interface_name;
  SignetDbusTcpServer *ds = (SignetDbusTcpServer *)user_data;

  if (strcmp(method_name, "GetChallenge") == 0) {
    const char *agent_id = NULL;
    g_variant_get(parameters, "(&s)", &agent_id);

    if (!agent_id || !agent_id[0]) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.BadRequest", "agent_id required");
      return;
    }

    int64_t now = signet_now_unix();
    char *challenge = signet_challenge_issue(ds->challenges, agent_id, now);
    if (!challenge) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.Internal", "Failed to issue challenge");
      return;
    }

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(s)", challenge));
    g_free(challenge);

  } else if (strcmp(method_name, "Authenticate") == 0) {
    const char *auth_event = NULL;
    g_variant_get(parameters, "(&s)", &auth_event);

    int64_t now = signet_now_unix();
    char *agent_id = NULL;
    char *pubkey_hex = NULL;
    SignetAuthResult ar = signet_auth_verify(
        ds->challenges, ds->fleet, auth_event, now, &agent_id, &pubkey_hex);

    if (ar != SIGNET_AUTH_OK) {
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(bs)", FALSE, signet_auth_result_string(ar)));
      g_free(agent_id);
      g_free(pubkey_hex);
      return;
    }

    /* Store auth state for this connection. */
    TcpConnState *state = g_new0(TcpConnState, 1);
    state->agent_id = agent_id;
    state->pubkey_hex = pubkey_hex;
    state->authenticated = true;

    g_mutex_lock(&ds->mu);
    g_hash_table_replace(ds->conn_states, connection, state);
    g_mutex_unlock(&ds->mu);

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(bs)", TRUE, agent_id));

  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.UnknownMethod", "Unknown auth method");
  }
}

/* ----------------------------- Signer/Credentials dispatch ---------------- */

static void signet_tcp_handle_method(GDBusConnection *connection,
                                      const gchar *sender,
                                      const gchar *object_path,
                                      const gchar *interface_name,
                                      const gchar *method_name,
                                      GVariant *parameters,
                                      GDBusMethodInvocation *invocation,
                                      gpointer user_data) {
  (void)sender;
  (void)object_path;
  (void)parameters;
  SignetDbusTcpServer *ds = (SignetDbusTcpServer *)user_data;

  /* Check authentication state. */
  g_mutex_lock(&ds->mu);
  TcpConnState *state = (TcpConnState *)g_hash_table_lookup(
      ds->conn_states, connection);
  g_mutex_unlock(&ds->mu);

  if (!state || !state->authenticated) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Unauthenticated",
        "Must authenticate via net.signet.Auth.Authenticate first");
    return;
  }

  /* Capability check (skip if policy not configured). */
  if (ds->policy && !signet_policy_evaluate(ds->policy, state->agent_id, method_name, -1)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.CapabilityDenied",
        "Operation not permitted by policy");
    return;
  }

  /* Dispatch to same handlers as Unix transport. */
  if (strcmp(interface_name, "net.signet.Signer") == 0) {
    if (strcmp(method_name, "GetPublicKey") == 0) {
      char pubkey_hex[65];
      if (!signet_key_store_get_agent_pubkey(ds->keys, state->agent_id, pubkey_hex, sizeof(pubkey_hex))) {
        signet_audit_log_common(ds->audit, SIGNET_AUDIT_EVENT_KEY_ACCESS,
            &(SignetAuditCommonFields){ .identity = state->agent_id, .method = "GetPublicKey",
              .decision = "error", .reason_code = "key_not_found" },
            "{\"transport\":\"dbus_tcp\"}");
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Agent key not found");
        return;
      }
      signet_audit_log_common(ds->audit, SIGNET_AUDIT_EVENT_KEY_ACCESS,
          &(SignetAuditCommonFields){ .identity = state->agent_id, .method = "GetPublicKey",
            .decision = "allow", .reason_code = "ok" },
          "{\"transport\":\"dbus_tcp\"}");
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(s)", pubkey_hex));

    } else if (strcmp(method_name, "SignEvent") == 0) {
      const char *event_json = NULL;
      g_variant_get(parameters, "(&s)", &event_json);

      SignetLoadedKey lk;
      memset(&lk, 0, sizeof(lk));
      if (!signet_key_store_load_agent_key(ds->keys, state->agent_id, &lk)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Agent key not found");
        return;
      }

      NostrEvent *ev = nostr_event_new();
      if (!ev || nostr_event_deserialize(ev, event_json) != 0) {
        if (ev) nostr_event_free(ev);
        signet_loaded_key_clear(&lk);
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.BadRequest", "Invalid event JSON");
        return;
      }

      char sk_hex[65];
      for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
      sk_hex[64] = '\0';
      int sign_rc = nostr_event_sign(ev, sk_hex);
      sodium_memzero(sk_hex, sizeof(sk_hex));
      signet_loaded_key_clear(&lk);

      if (sign_rc != 0) {
        nostr_event_free(ev);
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.Internal", "Signing failed");
        return;
      }

      char *signed_json = nostr_event_serialize(ev);
      nostr_event_free(ev);
      signet_audit_log_common(ds->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
          &(SignetAuditCommonFields){ .identity = state->agent_id, .method = "SignEvent",
            .decision = "allow", .reason_code = "ok" },
          "{\"transport\":\"dbus_tcp\"}");
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(s)", signed_json ? signed_json : ""));
      free(signed_json);

    } else if (strcmp(method_name, "Encrypt") == 0) {
      const char *plaintext = NULL, *peer_pubkey = NULL, *algo = NULL;
      g_variant_get(parameters, "(&s&s&s)", &plaintext, &peer_pubkey, &algo);

      /* Determine algorithm: default to nip04 for backward compat. */
      bool use_nip44 = (algo && strcmp(algo, "nip44") == 0);
      bool use_nip04 = (!algo || algo[0] == '\0' || strcmp(algo, "nip04") == 0);
      if (!use_nip04 && !use_nip44) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.BadRequest",
            "Unsupported algorithm; use 'nip04', 'nip44', or empty");
        return;
      }

      SignetLoadedKey lk;
      memset(&lk, 0, sizeof(lk));
      if (!signet_key_store_load_agent_key(ds->keys, state->agent_id, &lk)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Agent key not found");
        return;
      }

      char *result_ct = NULL;
      int rc;

      if (use_nip44) {
        /* NIP-44 v2: peer_pubkey is hex-encoded x-only 32-byte key. */
        uint8_t peer_pk[32];
        bool pk_ok = (peer_pubkey && strlen(peer_pubkey) == 64);
        if (pk_ok) {
          for (int i = 0; i < 32; i++) {
            unsigned int byte;
            if (sscanf(peer_pubkey + i * 2, "%2x", &byte) != 1) { pk_ok = false; break; }
            peer_pk[i] = (uint8_t)byte;
          }
        }
        if (!pk_ok) {
          signet_loaded_key_clear(&lk);
          g_dbus_method_invocation_return_dbus_error(
              invocation, "net.signet.Error.BadRequest", "Invalid peer pubkey hex");
          return;
        }
        rc = nostr_nip44_encrypt_v2(lk.secret_key, peer_pk,
                                    (const uint8_t *)plaintext, strlen(plaintext),
                                    &result_ct);
      } else {
        /* NIP-04 */
        char sk_hex[65];
        for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
        sk_hex[64] = '\0';
        char *err_msg = NULL;
        rc = nostr_nip04_encrypt(plaintext, peer_pubkey, sk_hex, &result_ct, &err_msg);
        sodium_memzero(sk_hex, sizeof(sk_hex));
        free(err_msg);
      }
      signet_loaded_key_clear(&lk);

      if (rc != 0) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.Internal", "Encryption failed");
        return;
      }

      char audit_detail[128];
      snprintf(audit_detail, sizeof(audit_detail),
               "{\"transport\":\"dbus_tcp\",\"algo\":\"%s\"}", use_nip44 ? "nip44" : "nip04");
      signet_audit_log_common(ds->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
          &(SignetAuditCommonFields){ .identity = state->agent_id, .method = "Encrypt",
            .decision = "allow", .reason_code = "ok" }, audit_detail);
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(s)", result_ct));
      free(result_ct);

    } else if (strcmp(method_name, "Decrypt") == 0) {
      const char *ciphertext = NULL, *peer_pubkey = NULL, *algo = NULL;
      g_variant_get(parameters, "(&s&s&s)", &ciphertext, &peer_pubkey, &algo);

      /* Determine algorithm: default to nip04 for backward compat. */
      bool use_nip44 = (algo && strcmp(algo, "nip44") == 0);
      bool use_nip04 = (!algo || algo[0] == '\0' || strcmp(algo, "nip04") == 0);
      if (!use_nip04 && !use_nip44) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.BadRequest",
            "Unsupported algorithm; use 'nip04', 'nip44', or empty");
        return;
      }

      SignetLoadedKey lk;
      memset(&lk, 0, sizeof(lk));
      if (!signet_key_store_load_agent_key(ds->keys, state->agent_id, &lk)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Agent key not found");
        return;
      }

      char *result_pt = NULL;
      int rc;

      if (use_nip44) {
        /* NIP-44 v2: peer_pubkey is hex-encoded x-only 32-byte key. */
        uint8_t peer_pk[32];
        bool pk_ok = (peer_pubkey && strlen(peer_pubkey) == 64);
        if (pk_ok) {
          for (int i = 0; i < 32; i++) {
            unsigned int byte;
            if (sscanf(peer_pubkey + i * 2, "%2x", &byte) != 1) { pk_ok = false; break; }
            peer_pk[i] = (uint8_t)byte;
          }
        }
        if (!pk_ok) {
          signet_loaded_key_clear(&lk);
          g_dbus_method_invocation_return_dbus_error(
              invocation, "net.signet.Error.BadRequest", "Invalid peer pubkey hex");
          return;
        }
        uint8_t *raw_pt = NULL;
        size_t raw_pt_len = 0;
        rc = nostr_nip44_decrypt_v2(lk.secret_key, peer_pk, ciphertext, &raw_pt, &raw_pt_len);
        if (rc == 0 && raw_pt) {
          result_pt = (char *)g_malloc(raw_pt_len + 1);
          memcpy(result_pt, raw_pt, raw_pt_len);
          result_pt[raw_pt_len] = '\0';
          free(raw_pt);
        }
      } else {
        /* NIP-04 */
        char sk_hex[65];
        for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", lk.secret_key[i]);
        sk_hex[64] = '\0';
        char *err_msg = NULL;
        rc = nostr_nip04_decrypt(ciphertext, peer_pubkey, sk_hex, &result_pt, &err_msg);
        sodium_memzero(sk_hex, sizeof(sk_hex));
        free(err_msg);
      }
      signet_loaded_key_clear(&lk);

      if (rc != 0 || !result_pt) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.Internal", "Decryption failed");
        return;
      }

      char audit_detail[128];
      snprintf(audit_detail, sizeof(audit_detail),
               "{\"transport\":\"dbus_tcp\",\"algo\":\"%s\"}", use_nip44 ? "nip44" : "nip04");
      signet_audit_log_common(ds->audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST,
          &(SignetAuditCommonFields){ .identity = state->agent_id, .method = "Decrypt",
            .decision = "allow", .reason_code = "ok" }, audit_detail);
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(s)", result_pt));
      free(result_pt);

    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.UnknownMethod", "Unknown signer method");
    }
  } else if (strcmp(interface_name, "net.signet.Credentials") == 0) {
    int64_t now = signet_now_unix();

    if (strcmp(method_name, "GetSession") == 0) {
      const char *service_url = NULL;
      g_variant_get(parameters, "(&s)", &service_url);

      uint8_t raw[32];
      randombytes_buf(raw, sizeof(raw));
      char token_hex[65];
      for (int i = 0; i < 32; i++) sprintf(token_hex + i * 2, "%02x", raw[i]);
      token_hex[64] = '\0';
      sodium_memzero(raw, sizeof(raw));

      int64_t expires_at = now + 24 * 60 * 60;

      uint8_t lid_raw[16];
      randombytes_buf(lid_raw, sizeof(lid_raw));
      char lease_id[33];
      for (int i = 0; i < 16; i++) sprintf(lease_id + i * 2, "%02x", lid_raw[i]);
      lease_id[32] = '\0';

      char *meta = g_strdup_printf("{\"service_url\":\"%s\",\"transport\":\"dbus_tcp\"}",
                                   service_url ? service_url : "");
      if (ds->store)
        (void)signet_store_issue_lease(ds->store, lease_id, "session",
                                       state->agent_id, now, expires_at, meta);
      g_free(meta);

      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(sx)", token_hex, (gint64)expires_at));

    } else if (strcmp(method_name, "GetToken") == 0) {
      const char *cred_type = NULL;
      g_variant_get(parameters, "(&s)", &cred_type);

      if (!ds->store) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotConfigured", "Store not available");
        return;
      }

      SignetSecretRecord rec;
      memset(&rec, 0, sizeof(rec));
      int rc = signet_store_get_secret(ds->store, cred_type, &rec);
      if (rc != 0) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Credential not found");
        return;
      }

      int64_t expires_at = now + 3600;
      char *token_hex = NULL;
      if (rec.payload && rec.payload_len > 0) {
        token_hex = g_malloc(rec.payload_len * 2 + 1);
        for (size_t i = 0; i < rec.payload_len; i++)
          sprintf(token_hex + i * 2, "%02x", rec.payload[i]);
        token_hex[rec.payload_len * 2] = '\0';
      }
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(sx)", token_hex ? token_hex : "", (gint64)expires_at));
      g_free(token_hex);
      signet_secret_record_clear(&rec);

    } else if (strcmp(method_name, "ListCredentials") == 0) {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

      if (ds->store) {
        char **ids = NULL;
        char **labels = NULL;
        size_t count = 0;
        if (signet_store_list_secrets(ds->store, state->agent_id, &ids, &labels, &count) == 0) {
          for (size_t i = 0; i < count; i++) {
            if (ids[i])
              g_variant_builder_add(&builder, "s", ids[i]);
          }
          signet_store_free_secret_list(ids, labels, count);
        }
      }
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(as)", &builder));

    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.UnknownMethod", "Unknown credentials method");
    }
  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.UnknownInterface", "Unknown interface");
  }
}

/* ----------------------------- connection handling ------------------------ */

static const GDBusInterfaceVTable auth_vtable = {
  .method_call = signet_tcp_handle_auth,
};

static const GDBusInterfaceVTable signer_vtable = {
  .method_call = signet_tcp_handle_method,
};

static const GDBusInterfaceVTable credentials_vtable = {
  .method_call = signet_tcp_handle_method,
};

static gboolean
signet_tcp_on_new_connection(GDBusServer *server,
                              GDBusConnection *connection,
                              gpointer user_data) {
  (void)server;
  SignetDbusTcpServer *ds = (SignetDbusTcpServer *)user_data;
  GError *err = NULL;

  /* Register Auth interface. */
  GDBusInterfaceInfo *auth_iface = ds->node_info->interfaces[0];
  guint auth_id = g_dbus_connection_register_object(
      connection, SIGNET_TCP_OBJECT_PATH, auth_iface,
      &auth_vtable, ds, NULL, &err);
  if (auth_id == 0 && err) {
    g_warning("signet-tcp: failed to register Auth: %s", err->message);
    g_error_free(err);
    err = NULL;
  }

  /* Register Signer interface. */
  GDBusInterfaceInfo *signer_iface = ds->node_info->interfaces[1];
  g_dbus_connection_register_object(
      connection, SIGNET_TCP_OBJECT_PATH, signer_iface,
      &signer_vtable, ds, NULL, &err);
  if (err) { g_error_free(err); err = NULL; }

  /* Register Credentials interface. */
  GDBusInterfaceInfo *creds_iface = ds->node_info->interfaces[2];
  g_dbus_connection_register_object(
      connection, SIGNET_TCP_OBJECT_PATH, creds_iface,
      &credentials_vtable, ds, NULL, &err);
  if (err) { g_error_free(err); err = NULL; }

  return TRUE; /* claim the connection */
}

/* ------------------------------ public API -------------------------------- */

SignetDbusTcpServer *signet_dbus_tcp_server_new(const SignetDbusTcpServerConfig *cfg) {
  if (!cfg || !cfg->keys || !cfg->challenges)
    return NULL;  /* policy may be NULL (skips capability check) */

  SignetDbusTcpServer *ds = g_new0(SignetDbusTcpServer, 1);
  if (!ds) return NULL;

  ds->listen_address = g_strdup(cfg->listen_address
      ? cfg->listen_address : "tcp:host=127.0.0.1,port=47472");
  ds->keys = cfg->keys;
  ds->policy = cfg->policy;
  ds->store = cfg->store;
  ds->challenges = cfg->challenges;
  ds->audit = cfg->audit;
  ds->fleet = cfg->fleet;

  g_mutex_init(&ds->mu);
  ds->conn_states = g_hash_table_new_full(
      g_direct_hash, g_direct_equal, NULL, tcp_conn_state_free);

  ds->node_info = g_dbus_node_info_new_for_xml(signet_tcp_introspection_xml, NULL);
  if (!ds->node_info) {
    g_hash_table_destroy(ds->conn_states);
    g_mutex_clear(&ds->mu);
    g_free(ds->listen_address);
    g_free(ds);
    return NULL;
  }

  return ds;
}

void signet_dbus_tcp_server_free(SignetDbusTcpServer *ds) {
  if (!ds) return;
  signet_dbus_tcp_server_stop(ds);
  if (ds->node_info) g_dbus_node_info_unref(ds->node_info);
  g_mutex_lock(&ds->mu);
  g_hash_table_destroy(ds->conn_states);
  g_mutex_unlock(&ds->mu);
  g_mutex_clear(&ds->mu);
  g_free(ds->listen_address);
  g_free(ds);
}

int signet_dbus_tcp_server_start(SignetDbusTcpServer *ds) {
  if (!ds) return -1;
  if (ds->server) return 0;

  /* Generate a random GUID for the server. */
  char *guid = g_dbus_generate_guid();
  GError *err = NULL;

  ds->server = g_dbus_server_new_sync(
      ds->listen_address,
      G_DBUS_SERVER_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
      guid,
      NULL, /* GDBusAuthObserver */
      NULL, /* GCancellable */
      &err);

  g_free(guid);

  if (!ds->server) {
    if (err) {
      g_warning("signet-tcp: failed to create server: %s", err->message);
      g_error_free(err);
    }
    return -1;
  }

  g_signal_connect(ds->server, "new-connection",
                   G_CALLBACK(signet_tcp_on_new_connection), ds);

  g_dbus_server_start(ds->server);
  return 0;
}

void signet_dbus_tcp_server_stop(SignetDbusTcpServer *ds) {
  if (!ds || !ds->server) return;
  g_dbus_server_stop(ds->server);
  g_object_unref(ds->server);
  ds->server = NULL;
}
