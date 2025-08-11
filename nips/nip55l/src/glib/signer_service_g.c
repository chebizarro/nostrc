#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

#include "nostr/nip55l/signer_ops.h"

/* Embed introspection XML to avoid build-time codegen */
static const gchar *signer_xml =
  "<node>"
  "  <interface name='com.nostr.Signer'>"
  "    <method name='GetPublicKey'>"
  "      <arg type='s' direction='out' name='npub'/>"
  "    </method>"
  "    <method name='SignEvent'>"
  "      <arg type='s' direction='in' name='eventJson'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='in' name='app_id'/>"
  "      <arg type='s' direction='out' name='signature'/>"
  "    </method>"
  "    <method name='ApproveRequest'>"
  "      <arg type='s' direction='in' name='request_id'/>"
  "      <arg type='b' direction='in' name='decision'/>"
  "      <arg type='b' direction='in' name='remember'/>"
  "      <arg type='b' direction='out' name='ok'/>"
  "    </method>"
  "    <method name='NIP04Encrypt'>"
  "      <arg type='s' direction='in' name='plaintext'/>"
  "      <arg type='s' direction='in' name='pubKey'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='out' name='encryptedText'/>"
  "    </method>"
  "    <method name='NIP04Decrypt'>"
  "      <arg type='s' direction='in' name='encryptedText'/>"
  "      <arg type='s' direction='in' name='pubKey'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='out' name='plaintext'/>"
  "    </method>"
  "    <method name='NIP44Encrypt'>"
  "      <arg type='s' direction='in' name='plaintext'/>"
  "      <arg type='s' direction='in' name='pubKey'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='out' name='encryptedText'/>"
  "    </method>"
  "    <method name='NIP44Decrypt'>"
  "      <arg type='s' direction='in' name='encryptedText'/>"
  "      <arg type='s' direction='in' name='pubKey'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='out' name='plaintext'/>"
  "    </method>"
  "    <method name='DecryptZapEvent'>"
  "      <arg type='s' direction='in' name='eventJson'/>"
  "      <arg type='s' direction='in' name='current_user'/>"
  "      <arg type='s' direction='out' name='decryptedEvent'/>"
  "    </method>"
  "    <method name='GetRelays'>"
  "      <arg type='s' direction='out' name='relaysJson'/>"
  "    </method>"
  "    <method name='StoreSecret'>"
  "      <arg type='s' direction='in' name='secret'/>"
  "      <arg type='s' direction='in' name='account'/>"
  "      <arg type='b' direction='out' name='ok'/>"
  "    </method>"
  "    <method name='ClearSecret'>"
  "      <arg type='s' direction='in' name='account'/>"
  "      <arg type='b' direction='out' name='ok'/>"
  "    </method>"
  "    <signal name='ApprovalRequested'>"
  "      <arg type='s' name='app_id'/>"
  "      <arg type='s' name='account'/>"
  "      <arg type='s' name='kind'/>"
  "      <arg type='s' name='preview'/>"
  "      <arg type='s' name='request_id'/>"
  "    </signal>"
  "    <signal name='ApprovalCompleted'>"
  "      <arg type='s' name='request_id'/>"
  "      <arg type='b' name='decision'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *introspection_data = NULL;
typedef struct {
  char *event_json;
  char *current_user;
  char *app_id;
  GDBusMethodInvocation *invocation; /* kept until decision */
} PendingSign;
static void pending_free(gpointer data){
  PendingSign *ps = (PendingSign*)data;
  if (!ps) return;
  g_free(ps->event_json);
  g_free(ps->current_user);
  g_free(ps->app_id);
  /* ps->invocation is finished elsewhere; do not unref here */
  g_free(ps);
}
static GHashTable *pending = NULL; /* id(string) -> PendingSign* */

/* Simple ACL and rate limiter for mutation methods */
static gboolean signer_mutations_allowed(void){
  const char *env = g_getenv("NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS");
  return (env && g_strcmp0(env, "1")==0);
}

static gboolean rate_limit_ok(const char *sender){
  static GHashTable *last_call = NULL;
  if (!last_call) last_call = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const gchar *key = sender ? sender : "default";
  gpointer val = g_hash_table_lookup(last_call, key);
  gint64 now = g_get_monotonic_time();
  const gint64 interval_us = 500 * 1000; /* 500ms */
  if (val){
    gint64 prev = (gint64)(intptr_t)val;
    if (now - prev < interval_us) return FALSE;
  }
  g_hash_table_replace(last_call, g_strdup(key), (gpointer)(intptr_t)now);
  return TRUE;
}

static void on_method_call(GDBusConnection *connection,
                           const gchar *sender,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
  (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;

  if (g_strcmp0(method_name, "GetPublicKey") == 0) {
    char *npub=NULL; int rc = nostr_nip55l_get_public_key(&npub);
    if (rc!=0 || !npub) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "get_public_key failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", npub));
    free(npub);
    return;
  }
  if (g_strcmp0(method_name, "SignEvent") == 0) {
    const gchar *eventJson; const gchar *current_user; const gchar *app_id;
    g_variant_get(parameters, "(sss)", &eventJson, &current_user, &app_id);
    if (!pending) pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_free);

    gchar *req_id = g_uuid_string_random();
    PendingSign *ps = g_new0(PendingSign, 1);
    ps->event_json = g_strdup(eventJson);
    ps->current_user = g_strdup(current_user);
    ps->app_id = g_strdup(app_id);
    ps->invocation = invocation; /* keep, do not finish now */
    g_hash_table_insert(pending, g_strdup(req_id), ps);

    const gchar *preview = eventJson; /* TODO: shorten */
    g_dbus_connection_emit_signal(connection,
                                  NULL,
                                  object_path,
                                  interface_name,
                                  "ApprovalRequested",
                                  g_variant_new("(sssss)", app_id ? app_id : "",
                                                current_user ? current_user : "",
                                                "event", preview ? preview : "",
                                                req_id),
                                  NULL);
    g_free(req_id);
    return; /* wait for ApproveRequest */
  }
  if (g_strcmp0(method_name, "NIP04Encrypt") == 0) {
    const gchar *plaintext; const gchar *pubKey; const gchar *current_user;
    g_variant_get(parameters, "(sss)", &plaintext, &pubKey, &current_user);
    char *out=NULL; int rc = nostr_nip55l_nip04_encrypt(plaintext, pubKey, current_user, &out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip04 encrypt failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }
  if (g_strcmp0(method_name, "NIP04Decrypt") == 0) {
    const gchar *cipher; const gchar *pubKey; const gchar *current_user;
    g_variant_get(parameters, "(sss)", &cipher, &pubKey, &current_user);
    char *out=NULL; int rc = nostr_nip55l_nip04_decrypt(cipher, pubKey, current_user, &out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip04 decrypt failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }
  if (g_strcmp0(method_name, "NIP44Encrypt") == 0) {
    const gchar *plaintext; const gchar *pubKey; const gchar *current_user;
    g_variant_get(parameters, "(sss)", &plaintext, &pubKey, &current_user);
    char *out=NULL; int rc = nostr_nip55l_nip44_encrypt(plaintext, pubKey, current_user, &out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip44 encrypt failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }
  if (g_strcmp0(method_name, "NIP44Decrypt") == 0) {
    const gchar *cipher; const gchar *pubKey; const gchar *current_user;
    g_variant_get(parameters, "(sss)", &cipher, &pubKey, &current_user);
    char *out=NULL; int rc = nostr_nip55l_nip44_decrypt(cipher, pubKey, current_user, &out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip44 decrypt failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }
  if (g_strcmp0(method_name, "DecryptZapEvent") == 0) {
    const gchar *eventJson; const gchar *current_user;
    g_variant_get(parameters, "(ss)", &eventJson, &current_user);
    char *out=NULL; int rc = nostr_nip55l_decrypt_zap_event(eventJson, current_user, &out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "zap decrypt failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }
  if (g_strcmp0(method_name, "GetRelays") == 0) {
    char *out=NULL; int rc = nostr_nip55l_get_relays(&out);
    if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "get relays failed"); return; }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out));
    free(out); return;
  }

  if (g_strcmp0(method_name, "ApproveRequest") == 0) {
    const gchar *req_id; gboolean decision; gboolean remember;
    g_variant_get(parameters, "(sbb)", &req_id, &decision, &remember);
    (void)remember; /* TODO: persist policy */
    if (!pending) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no pending"); return; }
    PendingSign *ps = g_hash_table_lookup(pending, req_id);
    if (!ps) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "unknown request"); return; }

    /* emit completion */
    g_dbus_connection_emit_signal(connection,
                                  NULL,
                                  object_path,
                                  interface_name,
                                  "ApprovalCompleted",
                                  g_variant_new("(sb)", req_id, decision),
                                  NULL);

    if (!decision) {
      g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_CANCELLED, "rejected");
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
      g_hash_table_remove(pending, req_id);
      return;
    }
    char *sig=NULL; int rc = nostr_nip55l_sign_event(ps->event_json, ps->current_user, ps->app_id, &sig);
    if (rc!=0 || !sig) {
      g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "sign failed");
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
      g_hash_table_remove(pending, req_id);
      return;
    }
    g_dbus_method_invocation_return_value(ps->invocation, g_variant_new("(s)", sig));
    free(sig);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
    g_hash_table_remove(pending, req_id);
    return;
  }

  if (g_strcmp0(method_name, "StoreSecret") == 0) {
    if (!signer_mutations_allowed()) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "secret mutations disabled"); return; }
    if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return; }
    const gchar *secret; const gchar *account;
    g_variant_get(parameters, "(ss)", &secret, &account);
    int rc = nostr_nip55l_store_secret(secret, account);
    if (rc == 0) { g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE)); return; }
    g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "store secret failed"); return;
  }

  if (g_strcmp0(method_name, "ClearSecret") == 0) {
    if (!signer_mutations_allowed()) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "secret mutations disabled"); return; }
    if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return; }
    const gchar *account; g_variant_get(parameters, "(s)", &account);
    int rc = nostr_nip55l_clear_secret(account);
    if (rc == 0) { g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE)); return; }
    g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "clear secret failed"); return;
  }

  g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown method");
}

static const GDBusInterfaceVTable vtable = {
  on_method_call,
  NULL,
  NULL,
};

guint signer_export(GDBusConnection *conn, const char *object_path) {
  if (!introspection_data) {
    GError *error=NULL;
    introspection_data = g_dbus_node_info_new_for_xml(signer_xml, &error);
    if (!introspection_data) {
      if (error) { g_error_free(error); }
      return 0;
    }
  }
  const GDBusInterfaceInfo *iface_const = g_dbus_node_info_lookup_interface(introspection_data, "com.nostr.Signer");
  if (!iface_const) return 0;
  GDBusInterfaceInfo *iface = (GDBusInterfaceInfo*)iface_const; /* API expects non-const */
  return g_dbus_connection_register_object(conn, object_path, iface, &vtable, NULL, NULL, NULL);
}

void signer_unexport(GDBusConnection *conn, guint reg_id) {
  if (reg_id != 0)
    g_dbus_connection_unregister_object(conn, reg_id);
}
