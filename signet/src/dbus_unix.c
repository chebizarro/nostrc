/* SPDX-License-Identifier: MIT
 *
 * dbus_unix.c - D-Bus Unix transport for Signet v2.
 *
 * Uses GDBus (from GLib/GIO) to register on the system bus.
 * SO_PEERCRED UID is resolved to agent_id via callback.
 * All method calls go through capability engine before dispatch.
 */

#include "signet/dbus_unix.h"
#include "signet/key_store.h"
#include "signet/capability.h"
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

#define SIGNET_DBUS_BUS_NAME "net.signet.Signer"
#define SIGNET_DBUS_OBJECT_PATH "/net/signet/Signer"

/* Introspection XML for both interfaces. */
static const char *signet_dbus_introspection_xml =
    "<node>"
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

struct SignetDbusServer {
  SignetKeyStore *keys;
  SignetPolicyRegistry *policy;
  SignetStore *store;
  SignetAuditLogger *audit;
  SignetUidResolver uid_resolver;
  void *uid_resolver_data;
  bool use_system_bus;

  guint bus_owner_id;
  GDBusConnection *conn;
  guint signer_reg_id;
  guint creds_reg_id;
  GDBusNodeInfo *node_info;
};

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

/* ----------------------------- UID resolution ----------------------------- */

static char *signet_dbus_resolve_sender(SignetDbusServer *ds,
                                         GDBusConnection *conn,
                                         const char *sender) {
  if (!ds->uid_resolver) return NULL;

  /* Get the UID of the D-Bus sender via the bus daemon. */
  GError *err = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetConnectionUnixUser",
      g_variant_new("(s)", sender),
      G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

  if (!result) {
    if (err) g_error_free(err);
    return NULL;
  }

  guint32 uid = 0;
  g_variant_get(result, "(u)", &uid);
  g_variant_unref(result);

  return ds->uid_resolver((uid_t)uid, ds->uid_resolver_data);
}

/* ----------------------------- Signer methods ----------------------------- */

static void signet_dbus_handle_signer(GDBusConnection *connection,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *method_name,
                                       GVariant *parameters,
                                       GDBusMethodInvocation *invocation,
                                       gpointer user_data) {
  (void)object_path;
  (void)interface_name;
  SignetDbusServer *ds = (SignetDbusServer *)user_data;

  char *agent_id = signet_dbus_resolve_sender(ds, connection, sender);
  if (!agent_id) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Unauthorized",
        "Could not resolve caller UID to agent_id");
    return;
  }

  /* Capability check. */
  if (!signet_policy_evaluate(ds->policy, agent_id, method_name, -1)) {
    g_free(agent_id);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.CapabilityDenied",
        "Operation not permitted by policy");
    return;
  }

  if (strcmp(method_name, "GetPublicKey") == 0) {
    SignetLoadedKey lk;
    memset(&lk, 0, sizeof(lk));
    if (!signet_key_store_load_agent_key(ds->keys, agent_id, &lk)) {
      g_free(agent_id);
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.NotFound", "Agent key not found");
      return;
    }
    /* Convert secret key → public key hex. */
    uint8_t pk[32];
    if (crypto_scalarmult_ed25519_base_noclamp(pk, lk.secret_key) != 0) {
      signet_loaded_key_clear(&lk);
      g_free(agent_id);
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

  } else if (strcmp(method_name, "SignEvent") == 0) {
    const char *event_json = NULL;
    g_variant_get(parameters, "(&s)", &event_json);

    /* TODO: Deserialize event, extract kind for kind-filtering, sign with
     * agent key via nostr_event_sign_secure(), return signed event JSON.
     * For now, return a placeholder indicating the signing pipeline. */
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotImplemented",
        "SignEvent dispatch not yet wired to signing pipeline");

  } else if (strcmp(method_name, "Encrypt") == 0 ||
             strcmp(method_name, "Decrypt") == 0) {
    /* TODO: Wire to NIP-04/NIP-44 encryption via nip55l ops or direct. */
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.NotImplemented",
        "Encrypt/Decrypt not yet wired");
  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.UnknownMethod", "Unknown method");
  }

  g_free(agent_id);
}

/* ----------------------------- Credentials methods ----------------------- */

static void signet_dbus_handle_credentials(GDBusConnection *connection,
                                            const gchar *sender,
                                            const gchar *object_path,
                                            const gchar *interface_name,
                                            const gchar *method_name,
                                            GVariant *parameters,
                                            GDBusMethodInvocation *invocation,
                                            gpointer user_data) {
  (void)object_path;
  (void)interface_name;
  SignetDbusServer *ds = (SignetDbusServer *)user_data;

  char *agent_id = signet_dbus_resolve_sender(ds, connection, sender);
  if (!agent_id) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Unauthorized",
        "Could not resolve caller UID to agent_id");
    return;
  }

  if (!signet_policy_evaluate(ds->policy, agent_id, method_name, -1)) {
    g_free(agent_id);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.CapabilityDenied",
        "Operation not permitted by policy");
    return;
  }

  int64_t now = signet_now_unix();

  if (strcmp(method_name, "GetSession") == 0) {
    const char *service_url = NULL;
    g_variant_get(parameters, "(&s)", &service_url);

    /* Generate session token and issue a lease. */
    uint8_t raw[32];
    randombytes_buf(raw, sizeof(raw));
    char token_hex[65];
    for (int i = 0; i < 32; i++) sprintf(token_hex + i * 2, "%02x", raw[i]);
    token_hex[64] = '\0';
    sodium_memzero(raw, sizeof(raw));

    int64_t expires_at = now + 24 * 60 * 60; /* 24h default */

    /* Generate a unique lease ID. */
    uint8_t lid_raw[16];
    randombytes_buf(lid_raw, sizeof(lid_raw));
    char lease_id[33];
    for (int i = 0; i < 16; i++) sprintf(lease_id + i * 2, "%02x", lid_raw[i]);
    lease_id[32] = '\0';

    char *meta = g_strdup_printf("{\"service_url\":\"%s\"}", service_url ? service_url : "");
    (void)signet_store_issue_lease(ds->store, lease_id, "session",
                                    agent_id, now, expires_at, meta);
    g_free(meta);

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(sx)", token_hex, (gint64)expires_at));

  } else if (strcmp(method_name, "GetToken") == 0) {
    const char *cred_type = NULL;
    g_variant_get(parameters, "(&s)", &cred_type);

    /* Look up credential from secrets store. */
    SignetSecretRecord rec;
    memset(&rec, 0, sizeof(rec));
    int rc = signet_store_get_secret(ds->store, cred_type, &rec);
    if (rc != 0) {
      g_free(agent_id);
      g_dbus_method_invocation_return_dbus_error(
          invocation, "net.signet.Error.NotFound", "Credential not found");
      return;
    }

    int64_t expires_at = now + 3600; /* 1h default lease */

    /* Convert binary payload to hex for transport. */
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
    /* Return available credential types from the secrets store. */
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

    /* TODO: Implement listing from secrets table filtered by agent policy.
     * For now return empty list. */

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(as)", &builder));

  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.UnknownMethod", "Unknown method");
  }

  g_free(agent_id);
}

/* ----------------------------- bus lifecycle ------------------------------ */

static const GDBusInterfaceVTable signer_vtable = {
  .method_call = signet_dbus_handle_signer,
};

static const GDBusInterfaceVTable credentials_vtable = {
  .method_call = signet_dbus_handle_credentials,
};

static void signet_on_bus_acquired(GDBusConnection *conn,
                                    const gchar *name,
                                    gpointer user_data) {
  (void)name;
  SignetDbusServer *ds = (SignetDbusServer *)user_data;
  ds->conn = conn;

  GError *err = NULL;

  /* Register net.signet.Signer interface. */
  GDBusInterfaceInfo *signer_iface = ds->node_info->interfaces[0];
  ds->signer_reg_id = g_dbus_connection_register_object(
      conn, SIGNET_DBUS_OBJECT_PATH, signer_iface,
      &signer_vtable, ds, NULL, &err);
  if (ds->signer_reg_id == 0 && err) {
    g_warning("signet: failed to register Signer interface: %s", err->message);
    g_error_free(err);
    err = NULL;
  }

  /* Register net.signet.Credentials interface. */
  GDBusInterfaceInfo *creds_iface = ds->node_info->interfaces[1];
  ds->creds_reg_id = g_dbus_connection_register_object(
      conn, SIGNET_DBUS_OBJECT_PATH, creds_iface,
      &credentials_vtable, ds, NULL, &err);
  if (ds->creds_reg_id == 0 && err) {
    g_warning("signet: failed to register Credentials interface: %s", err->message);
    g_error_free(err);
  }
}

static void signet_on_name_acquired(GDBusConnection *conn,
                                     const gchar *name,
                                     gpointer user_data) {
  (void)conn;
  (void)user_data;
  g_message("signet: D-Bus name '%s' acquired", name);
}

static void signet_on_name_lost(GDBusConnection *conn,
                                 const gchar *name,
                                 gpointer user_data) {
  (void)conn;
  (void)user_data;
  g_warning("signet: D-Bus name '%s' lost", name);
}

/* ------------------------------ public API -------------------------------- */

SignetDbusServer *signet_dbus_server_new(const SignetDbusServerConfig *cfg) {
  if (!cfg || !cfg->keys || !cfg->policy) return NULL;

  SignetDbusServer *ds = g_new0(SignetDbusServer, 1);
  if (!ds) return NULL;

  ds->keys = cfg->keys;
  ds->policy = cfg->policy;
  ds->store = cfg->store;
  ds->audit = cfg->audit;
  ds->uid_resolver = cfg->uid_resolver;
  ds->uid_resolver_data = cfg->uid_resolver_data;
  ds->use_system_bus = cfg->use_system_bus;

  ds->node_info = g_dbus_node_info_new_for_xml(signet_dbus_introspection_xml, NULL);
  if (!ds->node_info) {
    g_free(ds);
    return NULL;
  }

  return ds;
}

void signet_dbus_server_free(SignetDbusServer *ds) {
  if (!ds) return;
  signet_dbus_server_stop(ds);
  if (ds->node_info)
    g_dbus_node_info_unref(ds->node_info);
  g_free(ds);
}

int signet_dbus_server_start(SignetDbusServer *ds) {
  if (!ds) return -1;
  if (ds->bus_owner_id > 0) return 0; /* already started */

  GBusType bus_type = ds->use_system_bus ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION;

  ds->bus_owner_id = g_bus_own_name(
      bus_type, SIGNET_DBUS_BUS_NAME,
      G_BUS_NAME_OWNER_FLAGS_NONE,
      signet_on_bus_acquired,
      signet_on_name_acquired,
      signet_on_name_lost,
      ds, NULL);

  return (ds->bus_owner_id > 0) ? 0 : -1;
}

void signet_dbus_server_stop(SignetDbusServer *ds) {
  if (!ds) return;

  if (ds->conn) {
    if (ds->signer_reg_id > 0) {
      g_dbus_connection_unregister_object(ds->conn, ds->signer_reg_id);
      ds->signer_reg_id = 0;
    }
    if (ds->creds_reg_id > 0) {
      g_dbus_connection_unregister_object(ds->conn, ds->creds_reg_id);
      ds->creds_reg_id = 0;
    }
    ds->conn = NULL; /* owned by GDBus, not us */
  }

  if (ds->bus_owner_id > 0) {
    g_bus_unown_name(ds->bus_owner_id);
    ds->bus_owner_id = 0;
  }
}
