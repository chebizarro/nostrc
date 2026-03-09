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

  /* Capability check. */
  if (!signet_policy_evaluate(ds->policy, state->agent_id, method_name, -1)) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.CapabilityDenied",
        "Operation not permitted by policy");
    return;
  }

  /* Dispatch to same handlers as Unix transport.
   * For now, delegate via the interface name. */
  if (strcmp(interface_name, "net.signet.Signer") == 0) {
    if (strcmp(method_name, "GetPublicKey") == 0) {
      SignetLoadedKey lk;
      memset(&lk, 0, sizeof(lk));
      if (!signet_key_store_load_agent_key(ds->keys, state->agent_id, &lk)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.NotFound", "Agent key not found");
        return;
      }
      uint8_t pk[32];
      if (crypto_scalarmult_ed25519_base_noclamp(pk, lk.secret_key) != 0) {
        signet_loaded_key_clear(&lk);
        g_dbus_method_invocation_return_dbus_error(
            invocation, "net.signet.Error.Internal", "Key derivation failed");
        return;
      }
      char hex[65];
      for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", pk[i]);
      hex[64] = '\0';
      sodium_memzero(pk, sizeof(pk));
      signet_loaded_key_clear(&lk);
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(s)", hex));

    } else {
      /* SignEvent, Encrypt, Decrypt — TODO: wire to signing pipeline. */
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.NotImplemented",
          "Method not yet implemented for TCP transport");
    }
  } else if (strcmp(interface_name, "net.signet.Credentials") == 0) {
    /* TODO: Credential methods — same as Unix transport. */
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotImplemented",
        "Credentials not yet implemented for TCP transport");
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
  if (!cfg || !cfg->keys || !cfg->policy || !cfg->challenges)
    return NULL;

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
