#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include "nostr/nip55l/signer_ops.h"

/* Embed introspection XML to avoid build-time codegen */
static const gchar *signer_xml =
  "<node>"
  "  <interface name='org.nostr.Signer'>"
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
  if (!sender) sender = "";
  gpointer v = g_hash_table_lookup(last_call, sender);
  gint64 now = g_get_monotonic_time();
  const gint64 interval_us = 500 * 1000; /* 500ms */
  if (v){
    gint64 prev = GPOINTER_TO_INT(v);
    if (now - prev < interval_us) return FALSE;
  }
  g_hash_table_insert(last_call, g_strdup(sender), GINT_TO_POINTER((int)now));
  return TRUE;
}

static gboolean rate_limit_ok_ms(const char *sender, guint interval_ms){
  static GHashTable *last_call = NULL;
  if (!last_call) last_call = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!sender) sender = "";
  gpointer v = g_hash_table_lookup(last_call, sender);
  gint64 now = g_get_monotonic_time();
  const gint64 interval_us = ((gint64)interval_ms) * 1000;
  if (v){
    gint64 prev = GPOINTER_TO_INT(v);
    if (now - prev < interval_us) return FALSE;
  }
  g_hash_table_insert(last_call, g_strdup(sender), GINT_TO_POINTER((int)now));
  return TRUE;
}

/* ACL persistence: ~/.config/gnostr/signer-acl.ini
 * Format: INI sections by method; key is "app_id:account"; value is "allow"/"deny". */
static gchar *acl_file_path(void){
  const char *conf = g_get_user_config_dir();
  if (!conf) conf = g_get_home_dir();
  gchar *dir = g_build_filename(conf, "gnostr", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "signer-acl.ini", NULL);
  g_free(dir);
  return path; /* caller frees */
}

static gboolean acl_load_decision(const char *method, const char *app_id, const char *account, gboolean *decision_out){
  if (!method || !app_id || !account || !decision_out) return FALSE;
  gboolean found = FALSE;
  gchar *path = acl_file_path();
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
    gchar *key = g_strdup_printf("%s:%s", app_id, account);
    if (g_key_file_has_group(kf, method) && g_key_file_has_key(kf, method, key, NULL)) {
      gchar *val = g_key_file_get_string(kf, method, key, NULL);
      if (val){
        if (g_strcmp0(val, "allow")==0) { *decision_out = TRUE; found = TRUE; }
        else if (g_strcmp0(val, "deny")==0) { *decision_out = FALSE; found = TRUE; }
        g_free(val);
      }
    }
    g_free(key);
  }
  if (err) g_error_free(err);
  g_key_file_unref(kf);
  g_free(path);
  return found;
}

static gboolean acl_save_decision(const char *method, const char *app_id, const char *account, gboolean allow){
  if (!method || !app_id || !account) return FALSE;
  gboolean ok = FALSE;
  gchar *path = acl_file_path();
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  (void)g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL); /* ok if missing */
  gchar *key = g_strdup_printf("%s:%s", app_id, account);
  g_key_file_set_string(kf, method, key, allow?"allow":"deny");
  gsize len=0; gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (data){ ok = (g_file_set_contents(path, data, len, &err) ? TRUE : FALSE); g_free(data); }
  if (err) { g_error_free(err); }
  g_free(key);
  g_key_file_unref(kf);
  g_free(path);
  return ok;
}

static gchar *build_event_preview(const char *event_json){
  if (!event_json) return g_strdup("");
  const char *p = strstr(event_json, "\"content\"");
  if (!p) return g_strndup(event_json, MIN((gsize)64, strlen(event_json)));
  p = strchr(p, ':'); if (!p) return g_strndup(event_json, MIN((gsize)64, strlen(event_json)));
  p++;
  while (*p==' '){ p++; }
  if (*p!='\"') return g_strndup(event_json, MIN((gsize)64, strlen(event_json)));
  p++;
  const char *start = p; const char *end = start;
  while (*end && *end!='\"') end++;
  gsize len = (gsize)(end-start);
  if (len > 96) len = 96;
  gchar *frag = g_strndup(start, len);
  for (gsize i=0;i<len;i++){ if (frag[i]=='\n' || frag[i]=='\r') frag[i]=' '; }
  return frag;
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
    /* app_id fallback to DBus sender if not provided */
    if (!app_id || !*app_id) app_id = sender ? sender : "";
    /* ACL pre-check: auto-allow/deny without prompting */
    gboolean acl_decision = FALSE;
    if (acl_load_decision("SignEvent", app_id, current_user, &acl_decision)) {
      if (!acl_decision) {
        g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "ACL deny");
        return;
      }
      char *sig=NULL; int rc = nostr_nip55l_sign_event(eventJson, current_user, app_id, &sig);
      if (rc!=0 || !sig) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "sign failed"); return; }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", sig));
      free(sig); return;
    }
    if (!rate_limit_ok_ms(sender, 100)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return; }
    if (!pending) pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_free);
    gchar *req_id = g_strdup_printf("%p", invocation);
    PendingSign *ps = g_new0(PendingSign, 1);
    ps->event_json = g_strdup(eventJson);
    ps->current_user = g_strdup(current_user);
    ps->app_id = g_strdup(app_id);
    ps->invocation = invocation; /* keep, do not finish now */
    g_hash_table_insert(pending, g_strdup(req_id), ps);

    gchar *preview = build_event_preview(eventJson);
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
    g_free(preview);
    g_free(req_id);
    return; /* wait for ApproveRequest */
  }
  if (g_strcmp0(method_name, "ApproveRequest") == 0) {
    const gchar *request_id; gboolean decision; gboolean remember;
    g_variant_get(parameters, "(sbb)", &request_id, &decision, &remember);
    gboolean handled = FALSE;
    if (pending && request_id) {
      PendingSign *ps = g_hash_table_lookup(pending, request_id);
      if (ps) {
        handled = TRUE;
        if (decision) {
          /* Sign and complete original SignEvent invocation */
          char *sig=NULL; int rc = nostr_nip55l_sign_event(ps->event_json, ps->current_user, ps->app_id, &sig);
          if (rc==0 && sig) {
            g_dbus_method_invocation_return_value(ps->invocation, g_variant_new("(s)", sig));
            free(sig);
            /* Persist decision if requested */
            if (remember) {
              acl_save_decision("SignEvent",
                                (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                                ps->current_user ? ps->current_user : "",
                                decision);
            }
          } else {
            g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "sign failed");
          }
        } else {
          g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "user denied");
          /* Persist decision if requested */
          if (remember) {
            acl_save_decision("SignEvent",
                              (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                              ps->current_user ? ps->current_user : "",
                              decision);
          }
        }
        /* Emit completion signal and remove pending */
        g_dbus_connection_emit_signal(connection,
                                      NULL,
                                      object_path,
                                      interface_name,
                                      "ApprovalCompleted",
                                      g_variant_new("(sb)", request_id, decision),
                                      NULL);
        g_hash_table_remove(pending, request_id);
      }
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", handled));
    return;
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
  const GDBusInterfaceInfo *iface_const = g_dbus_node_info_lookup_interface(introspection_data, "org.nostr.Signer");
  if (!iface_const) return 0;
  GDBusInterfaceInfo *iface = (GDBusInterfaceInfo*)iface_const; /* API expects non-const */
  return g_dbus_connection_register_object(conn, object_path, iface, &vtable, NULL, NULL, NULL);
}

void signer_unexport(GDBusConnection *conn, guint reg_id) {
  if (reg_id != 0)
    g_dbus_connection_unregister_object(conn, reg_id);
}
