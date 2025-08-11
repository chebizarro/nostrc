#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include "nostr/nip55l/signer_ops.h"
#include "signer_dbus.h"

/* Generated skeleton instance */
static NostrSigner *signer_skel = NULL;
typedef struct {
  char *event_json;
  char *identity;
  char *app_id;
  GDBusMethodInvocation *invocation; /* kept until decision */
} PendingSign;
static void pending_free(gpointer data){
  PendingSign *ps = (PendingSign*)data;
  if (!ps) return;
  g_free(ps->event_json);
  g_free(ps->identity);
  g_free(ps->app_id);
  /* ps->invocation is finished elsewhere; do not unref here */
  g_free(ps);
}
static GHashTable *pending = NULL; /* id(string) -> PendingSign* */

/* Simple ACL and rate limiter for mutation methods */
static gboolean signer_mutations_allowed(void){
  const char *env = g_getenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
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
 * Format: INI sections by method; key is "app_id:identity"; value is "allow"/"deny". */
static gchar *acl_file_path(void){
  const char *conf = g_get_user_config_dir();
  if (!conf) conf = g_get_home_dir();
  gchar *dir = g_build_filename(conf, "gnostr", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "signer-acl.ini", NULL);
  g_free(dir);
  return path; /* caller frees */
}

static gboolean acl_load_decision(const char *method, const char *app_id, const char *identity, gboolean *decision_out){
  if (!method || !app_id || !identity || !decision_out) return FALSE;
  GKeyFile *kf = g_key_file_new();
  GError *err=NULL;
  if (!g_key_file_load_from_file(kf, acl_file_path(), G_KEY_FILE_NONE, &err)){
    if (err) g_clear_error(&err);
    g_key_file_unref(kf); return FALSE;
  }
  gboolean ok = FALSE;
  gchar *key = g_strdup_printf("%s:%s", app_id, identity);
  gchar *val = g_key_file_get_string(kf, method, key, NULL);
  if (val){ ok = (g_strcmp0(val, "allow")==0); g_free(val); }
  g_free(key);
  g_key_file_unref(kf);
  if (ok) *decision_out = TRUE; /* allow */
  return ok;
}

static gboolean acl_save_decision(const char *method, const char *app_id, const char *identity, gboolean allow){
  if (!method || !app_id || !identity) return FALSE;
  GKeyFile *kf = g_key_file_new();
  GError *err=NULL;
  g_key_file_load_from_file(kf, acl_file_path(), G_KEY_FILE_NONE, NULL);
  gchar *key = g_strdup_printf("%s:%s", app_id, identity);
  g_key_file_set_string(kf, method, key, allow?"allow":"deny");
  g_free(key);
  gsize len=0; gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (data){
    if (!g_file_set_contents(acl_file_path(), data, len, &err)){
      if (err){ g_clear_error(&err);}    
    }
    g_free(data);
  }
  g_key_file_unref(kf);
  return TRUE;
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
/* ========== Generated handler glue ========== */
static gboolean handle_get_public_key(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  (void)object;
  char *npub = NULL; int rc = nostr_nip55l_get_public_key(&npub);
  if (rc!=0 || !npub) {
    g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "get_public_key failed");
    return TRUE;
  }
  nostr_signer_complete_get_public_key(object, invocation, npub);
  free(npub);
  return TRUE;
}

static gboolean handle_sign_event(NostrSigner *object, GDBusMethodInvocation *invocation,
                                  const gchar *eventJson, const gchar *identity, const gchar *app_id)
{
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!app_id || !*app_id) app_id = sender ? sender : "";
  gboolean acl_decision = FALSE;
  if (acl_load_decision("SignEvent", app_id, identity, &acl_decision)) {
    if (acl_decision) {
      char *sig=NULL; int rc = nostr_nip55l_sign_event(eventJson, identity, app_id, &sig);
      if (rc!=0 || !sig) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "sign failed"); return TRUE; }
      nostr_signer_complete_sign_event(object, invocation, sig);
      free(sig); return TRUE;
    } else {
      g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "denied by policy");
      return TRUE;
    }
  }
  if (!rate_limit_ok_ms(sender, 100)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return TRUE; }
  if (!pending) pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_free);
  gchar *req_id = g_strdup_printf("%p", invocation);
  PendingSign *ps = g_new0(PendingSign, 1);
  ps->event_json = g_strdup(eventJson);
  ps->identity = g_strdup(identity);
  ps->app_id = g_strdup(app_id);
  ps->invocation = invocation; /* keep */
  g_hash_table_insert(pending, g_strdup(req_id), ps);

  gchar *preview = build_event_preview(eventJson);
  nostr_signer_emit_approval_requested(object,
    app_id?app_id:"",
    identity?identity:"",
    "event",
    preview?preview:"",
    req_id);
  g_free(preview);
  g_free(req_id);
  /* Do not complete invocation yet; ApproveRequest will finish it */
  return TRUE;
}

static gboolean handle_approve_request(NostrSigner *object, GDBusMethodInvocation *invocation,
                                       const gchar *request_id, gboolean decision, gboolean remember)
{
  (void)object;
  gboolean handled = FALSE; const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (pending && request_id) {
    PendingSign *ps = g_hash_table_lookup(pending, request_id);
    if (ps) {
      handled = TRUE;
      if (decision) {
        char *sig=NULL; int rc = nostr_nip55l_sign_event(ps->event_json, ps->identity, ps->app_id, &sig);
        if (rc==0 && sig) {
          nostr_signer_complete_approve_request(object, invocation, TRUE);
          g_dbus_method_invocation_return_value(ps->invocation, g_variant_new("(s)", sig));
          free(sig);
          if (remember) {
            acl_save_decision("SignEvent",
                              (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                              ps->identity ? ps->identity : "",
                              decision);
          }
        } else {
          g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "sign failed");
          nostr_signer_complete_approve_request(object, invocation, FALSE);
        }
      } else {
        g_dbus_method_invocation_return_error_literal(ps->invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "user denied");
        if (remember) {
          acl_save_decision("SignEvent",
                            (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                            ps->identity ? ps->identity : "",
                            decision);
        }
        nostr_signer_complete_approve_request(object, invocation, TRUE);
      }
      nostr_signer_emit_approval_completed(object, request_id, decision);
      g_hash_table_remove(pending, request_id);
    }
  }
  if (!handled) nostr_signer_complete_approve_request(object, invocation, FALSE);
  return TRUE;
}

static gboolean handle_nip04_encrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *plaintext, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip04_encrypt(plaintext, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip04 encrypt failed"); return TRUE; }
  nostr_signer_complete_nip04_encrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip04_decrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *cipher, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip04_decrypt(cipher, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip04 decrypt failed"); return TRUE; }
  nostr_signer_complete_nip04_decrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip44_encrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *plaintext, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip44_encrypt(plaintext, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip44 encrypt failed"); return TRUE; }
  nostr_signer_complete_nip44_encrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip44_decrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *cipher, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip44_decrypt(cipher, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "nip44 decrypt failed"); return TRUE; }
  nostr_signer_complete_nip44_decrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_decrypt_zap_event(NostrSigner *object, GDBusMethodInvocation *invocation,
                                         const gchar *eventJson, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_decrypt_zap_event(eventJson, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "zap decrypt failed"); return TRUE; }
  nostr_signer_complete_decrypt_zap_event(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_get_relays(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_get_relays(&out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "get relays failed"); return TRUE; }
  nostr_signer_complete_get_relays(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_store_key(NostrSigner *object, GDBusMethodInvocation *invocation,
                                 const gchar *key, const gchar *identity)
{
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!signer_mutations_allowed()) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "key mutations disabled"); return TRUE; }
  if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return TRUE; }
  int rc = nostr_nip55l_store_key(key, identity);
  nostr_signer_complete_store_key(object, invocation, rc==0);
  return TRUE;
}

static gboolean handle_clear_key(NostrSigner *object, GDBusMethodInvocation *invocation,
                                 const gchar *identity)
{
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!signer_mutations_allowed()) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "key mutations disabled"); return TRUE; }
  if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return TRUE; }
  int rc = nostr_nip55l_clear_key(identity);
  nostr_signer_complete_clear_key(object, invocation, rc==0);
  return TRUE;
}


guint signer_export(GDBusConnection *conn, const char *object_path) {
  (void)conn;
  if (signer_skel) return 1;
  signer_skel = nostr_signer_skeleton_new();
  /* Connect gdbus-codegen handler signals */
  g_signal_connect(signer_skel, "handle-get-public-key", G_CALLBACK(handle_get_public_key), NULL);
  g_signal_connect(signer_skel, "handle-sign-event", G_CALLBACK(handle_sign_event), NULL);
  g_signal_connect(signer_skel, "handle-approve-request", G_CALLBACK(handle_approve_request), NULL);
  g_signal_connect(signer_skel, "handle-nip04-encrypt", G_CALLBACK(handle_nip04_encrypt), NULL);
  g_signal_connect(signer_skel, "handle-nip04-decrypt", G_CALLBACK(handle_nip04_decrypt), NULL);
  g_signal_connect(signer_skel, "handle-nip44-encrypt", G_CALLBACK(handle_nip44_encrypt), NULL);
  g_signal_connect(signer_skel, "handle-nip44-decrypt", G_CALLBACK(handle_nip44_decrypt), NULL);
  g_signal_connect(signer_skel, "handle-decrypt-zap-event", G_CALLBACK(handle_decrypt_zap_event), NULL);
  g_signal_connect(signer_skel, "handle-get-relays", G_CALLBACK(handle_get_relays), NULL);
  g_signal_connect(signer_skel, "handle-store-key", G_CALLBACK(handle_store_key), NULL);
  g_signal_connect(signer_skel, "handle-clear-key", G_CALLBACK(handle_clear_key), NULL);

  if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(signer_skel), conn, object_path, NULL)) {
    g_object_unref(signer_skel); signer_skel = NULL; return 0;
  }
  return 1; /* dummy reg id */
}

void signer_unexport(GDBusConnection *conn, guint reg_id) {
  (void)conn; (void)reg_id;
  if (signer_skel) {
    g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(signer_skel));
    g_object_unref(signer_skel); signer_skel = NULL;
  }
}
