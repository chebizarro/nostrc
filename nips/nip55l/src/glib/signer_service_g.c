#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <ctype.h>

#include "nostr/nip55l/signer_ops.h"
#include <nip19.h>
#include <keys.h>
#include <nostr-utils.h>
#include "signer_dbus.h"
#include "nostr/nip55l/error.h"
#include "nip55l_dbus_names.h"
#include "nip55l_dbus_errors.h"

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
  gboolean found = FALSE; gboolean allow = FALSE; gboolean expired = FALSE;
  gchar *key = g_strdup_printf("%s:%s", app_id, identity);
  gchar *val = g_key_file_get_string(kf, method, key, NULL);
  if (val){
    /* Format: "allow" | "deny" | "allow:<until_unixts>" | "deny:<until_unixts>" */
    const char *p = strchr(val, ':');
    guint64 until = 0;
    if (p){
      allow = (g_ascii_strncasecmp(val, "allow", 5)==0);
      until = g_ascii_strtoull(p+1, NULL, 10);
      guint64 now = (guint64)(g_get_real_time() / 1000000);
      expired = (until > 0 && now >= until);
    } else {
      allow = (g_strcmp0(val, "allow")==0);
    }
    found = TRUE;
    g_free(val);
  }
  g_free(key);
  g_key_file_unref(kf);
  if (!found || expired) return FALSE;
  *decision_out = allow;
  return TRUE;
}

static gboolean acl_save_decision(const char *method, const char *app_id, const char *identity, gboolean allow, guint64 ttl_seconds){
  if (!method || !app_id || !identity) return FALSE;
  GKeyFile *kf = g_key_file_new();
  GError *err=NULL;
  g_key_file_load_from_file(kf, acl_file_path(), G_KEY_FILE_NONE, NULL);
  gchar *key = g_strdup_printf("%s:%s", app_id, identity);
  gchar *val = NULL;
  if (ttl_seconds > 0){
    guint64 now = (guint64)(g_get_real_time() / 1000000);
    guint64 until = now + ttl_seconds;
    val = g_strdup_printf("%s:%" G_GUINT64_FORMAT, allow?"allow":"deny", until);
  } else {
    val = g_strdup(allow?"allow":"deny");
  }
  g_key_file_set_string(kf, method, key, val);
  g_free(val);
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
    gchar *msg = g_strdup_printf("get_public_key failed (rc=%d)", rc);
    g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
    g_free(msg);
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
  g_message("handle_sign_event: app_id=%s identity=%s sender=%s", app_id?app_id:"(null)", identity?identity:"(null)", sender?sender:"(null)");
  gboolean acl_decision = FALSE;
  if (acl_load_decision("SignEvent", app_id, identity, &acl_decision)) {
    if (acl_decision) {
      char *sig=NULL; int rc = nostr_nip55l_sign_event(eventJson, identity, app_id, &sig);
      if (rc!=0 || !sig) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "sign failed"); return TRUE; }
      nostr_signer_complete_sign_event(object, invocation, sig);
      free(sig); return TRUE;
    } else {
      g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy");
      return TRUE;
    }
  }
  if (!rate_limit_ok_ms(sender, 100)) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "rate limited"); return TRUE; }
  if (!pending) pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_free);
  gchar *req_id = g_strdup_printf("%p", (void*)invocation);
  PendingSign *ps = g_new0(PendingSign, 1);
  ps->event_json = g_strdup(eventJson);
  ps->identity = g_strdup(identity);
  ps->app_id = g_strdup(app_id);
  ps->invocation = invocation; /* keep */
  g_hash_table_insert(pending, g_strdup(req_id), ps);

  gchar *preview = build_event_preview(eventJson);
  g_message("emit ApprovalRequested: app_id=%s identity=%s request_id=%s", app_id?app_id:"(null)", identity?identity:"(null)", req_id?req_id:"(null)");
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
                                       const gchar *request_id, gboolean decision, gboolean remember, guint64 ttl_seconds)
{
  (void)object;
  gboolean handled = FALSE; const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  g_message("handle_approve_request: request_id=%s decision=%s remember=%s ttl=%" G_GUINT64_FORMAT,
            request_id?request_id:"(null)", decision?"true":"false", remember?"true":"false", ttl_seconds);
  if (pending && request_id) {
    PendingSign *ps = g_hash_table_lookup(pending, request_id);
    if (ps) {
      handled = TRUE;
      if (decision) {
        /* Fallback: if identity is empty, use the active identity */
        if (!ps->identity || !*ps->identity) {
          char *npub = NULL; int grc = nostr_nip55l_get_public_key(&npub);
          if (grc==0 && npub) {
            g_free(ps->identity);
            ps->identity = g_strdup(npub);
            g_message("approve: identity empty; using active npub=%s", ps->identity);
            free(npub);
          } else {
            g_message("approve: identity empty and active npub unavailable (rc=%d)", grc);
          }
        }
        char *sig=NULL; int rc = nostr_nip55l_sign_event(ps->event_json, ps->identity, ps->app_id, &sig);
        g_message("approve: signing rc=%d app_id=%s identity=%s", rc, ps->app_id?ps->app_id:"(null)", ps->identity?ps->identity:"(null)");
        if (rc==0 && sig) {
          nostr_signer_complete_approve_request(object, invocation, TRUE);
          g_dbus_method_invocation_return_value(ps->invocation, g_variant_new("(s)", sig));
          g_message("approve: sign success, returning signature");
          free(sig);
          if (remember) {
            acl_save_decision("SignEvent",
                              (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                              ps->identity ? ps->identity : "",
                              decision,
                              ttl_seconds);
          }
        } else {
          g_message("approve: sign failed, returning error");
          g_dbus_method_invocation_return_dbus_error(ps->invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "sign failed");
          nostr_signer_complete_approve_request(object, invocation, FALSE);
        }
      } else {
        g_message("approve: user denied");
        g_dbus_method_invocation_return_dbus_error(ps->invocation, ORG_NOSTR_SIGNER_ERR_APPROVAL, "user denied");
        if (remember) {
          acl_save_decision("SignEvent",
                            (ps->app_id && *ps->app_id) ? ps->app_id : (sender?sender:""),
                            ps->identity ? ps->identity : "",
                            decision,
                            ttl_seconds);
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
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "nip04 encrypt failed"); return TRUE; }
  nostr_signer_complete_nip04_encrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip04_decrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *cipher, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip04_decrypt(cipher, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "nip04 decrypt failed"); return TRUE; }
  nostr_signer_complete_nip04_decrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip44_encrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *plaintext, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip44_encrypt(plaintext, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "nip44 encrypt failed"); return TRUE; }
  nostr_signer_complete_nip44_encrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_nip44_decrypt(NostrSigner *object, GDBusMethodInvocation *invocation,
                                     const gchar *cipher, const gchar *pubKey, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_nip44_decrypt(cipher, pubKey, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "nip44 decrypt failed"); return TRUE; }
  nostr_signer_complete_nip44_decrypt(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_decrypt_zap_event(NostrSigner *object, GDBusMethodInvocation *invocation,
                                         const gchar *eventJson, const gchar *identity)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_decrypt_zap_event(eventJson, identity, &out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "zap decrypt failed"); return TRUE; }
  nostr_signer_complete_decrypt_zap_event(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_get_relays(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  (void)object;
  char *out=NULL; int rc = nostr_nip55l_get_relays(&out);
  if (rc!=0 || !out) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "get relays failed"); return TRUE; }
  nostr_signer_complete_get_relays(object, invocation, out);
  free(out); return TRUE;
}

static gboolean handle_store_key(NostrSigner *object, GDBusMethodInvocation *invocation,
                                  const gchar *key, const gchar *identity)
{
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!signer_mutations_allowed()) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_PERMISSION, "key mutations disabled"); return TRUE; }
  if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "rate limited"); return TRUE; }
  int rc = nostr_nip55l_store_key(key, identity);
  if (rc == 0) {
    /* Prefer deriving npub directly from the provided key to avoid relying on
     * environment/libsecret/keychain fallbacks. If derivation fails, fall back
     * to querying the active public key. */
    const char *out_npub_c = "";
    char *to_free = NULL;
    if (key && *key) {
      char *sk_hex = NULL;
      if (g_str_has_prefix(key, "nsec1")) {
        uint8_t sk[32];
        if (nostr_nip19_decode_nsec(key, sk) == 0) {
          sk_hex = (char*)malloc(65);
          if (sk_hex) { for (int i=0;i<32;i++){ sprintf(sk_hex+2*i, "%02x", sk[i]); } sk_hex[64]='\0'; }
        }
      } else {
        /* Accept 64-hex secret key */
        size_t n = strlen(key);
        gboolean hex64 = (n==64);
        if (hex64) {
          hex64 = TRUE;
          for (size_t i=0;i<n;i++){
            char c = key[i];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { hex64=FALSE; break; }
          }
        }
        if (hex64) {
          sk_hex = g_strdup(key);
          if (sk_hex) { for (size_t i=0;i<n;i++){ sk_hex[i] = (char)g_ascii_tolower(sk_hex[i]); } }
        }
      }
      if (sk_hex) {
        char *pk_hex = nostr_key_get_public(sk_hex);
        free(sk_hex);
        if (pk_hex) {
          uint8_t pk[32];
          if (nostr_hex2bin(pk, pk_hex, sizeof pk)) {
            char *npub_tmp=NULL;
            if (nostr_nip19_encode_npub(pk, &npub_tmp) == 0 && npub_tmp) {
              out_npub_c = to_free = npub_tmp;
            }
          }
          free(pk_hex);
        }
      }
    }
    if (to_free == NULL) {
      /* Fallback to active key */
      char *npub = NULL; int grc = nostr_nip55l_get_public_key(&npub);
      if (grc==0 && npub) { out_npub_c = to_free = npub; }
    }
    g_message("StoreKey ok=true; returning npub='%s' (empty means unavailable)", out_npub_c);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", TRUE, out_npub_c));
    if (to_free) free(to_free);
  } else {
    /* Map core error codes to DBus error names for actionable client messages */
    const char *ename = "org.nostr.Signer.Error";
    const char *emsg = "store failed";
    switch (rc) {
      case NOSTR_SIGNER_ERROR_INVALID_KEY:
        ename = "org.nostr.Signer.InvalidKey"; emsg = "invalid private key"; break;
      case NOSTR_SIGNER_ERROR_BACKEND:
        ename = "org.nostr.Signer.SecretServiceUnavailable"; emsg = "secret storage backend unavailable or failed"; break;
      case NOSTR_SIGNER_ERROR_INVALID_ARG:
        ename = "org.nostr.Signer.InvalidArgument"; emsg = "invalid argument"; break;
      case NOSTR_SIGNER_ERROR_NOT_FOUND:
        ename = "org.nostr.Signer.NotFound"; emsg = "not found"; break;
      default:
        ename = "org.nostr.Signer.Failure"; emsg = "operation failed"; break;
    }
    g_dbus_method_invocation_return_dbus_error(invocation, ename, emsg);
  }
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
