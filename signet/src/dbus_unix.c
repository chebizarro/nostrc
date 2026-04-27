/* SPDX-License-Identifier: MIT
 *
 * dbus_unix.c - D-Bus Unix transport for Signet v2.
 *
 * Uses GDBus (from GLib/GIO) to register on the system bus.
 * SO_PEERCRED UID is resolved to agent_id via callback.
 * All method calls go through capability engine before dispatch.
 */

#include "signet/dbus_unix.h"
#include "dbus_common.h"
#include "signet/key_store.h"
#include "signet/capability.h"
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_secrets.h"
#include "signet/audit_logger.h"
#include "signet/util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <glib.h>
#include <sodium.h>

/* libnostr event signing + NIP-04 + NIP-44 */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#include <json.h>

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

/* ----------------------------- Shared dispatch ----------------------------- */

static void signet_dbus_handle_method(GDBusConnection *connection,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *method_name,
                                       GVariant *parameters,
                                       GDBusMethodInvocation *invocation,
                                       gpointer user_data) {
  (void)object_path;
  SignetDbusServer *ds = (SignetDbusServer *)user_data;

  char *agent_id = signet_dbus_resolve_sender(ds, connection, sender);
  if (!agent_id) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "net.signet.Error.Unauthorized",
        "Could not resolve caller UID to agent_id");
    return;
  }

  SignetDbusDispatchContext ctx = {
    .keys = ds->keys,
    .policy = ds->policy,
    .store = ds->store,
    .audit = ds->audit,
    .transport = "dbus_unix",
  };
  signet_dbus_dispatch_authenticated(&ctx, agent_id, interface_name,
                                      method_name, parameters, invocation);
  g_free(agent_id);
}

static const GDBusInterfaceVTable signer_vtable = {
  .method_call = signet_dbus_handle_method,
};

static const GDBusInterfaceVTable credentials_vtable = {
  .method_call = signet_dbus_handle_method,
};

/* ----------------------------- bus lifecycle ------------------------------ */

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
  if (!cfg || !cfg->keys) return NULL;  /* policy may be NULL (skips capability check) */

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
