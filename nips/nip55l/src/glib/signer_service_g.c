#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <ctype.h>

#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip19/nip19.h"
#include <keys.h>
#include <nostr-event.h>
#include <nostr-json.h>
#include <nostr-utils.h>
#include <secure_buf.h>
#include "signer_dbus.h"
#include "nostr/nip55l/error.h"
#include "nip55l_dbus_names.h"
#include "nip55l_dbus_errors.h"
#include "signer_caller.h"
#include "signer_gate.h"

/* Generated skeleton instance */
static NostrSigner *signer_skel = NULL;
static GDBusConnection *signer_bus = NULL; /* the connection it is exported on */

/* ==========================================================================
 * Access control (nip55l 0.4.0: nostrc-y02q, nostrc-phk4, nostrc-1e31,
 * nostrc-eie5). Every method that uses or reveals the user's key material or
 * configuration goes through gate() - D-Bus methods via gated_call(), the
 * NIP-5F socket via signer_gate_submit() (signer_gate.h, nostrc-q23h):
 *
 *   1. principal  - who is calling, derived from the bus connection or the
 *                   socket's kernel-reported peer (signer_caller.h), never
 *                   from an argument. The caller's app_id argument is only
 *                   honoured as a web-origin sub-principal when the caller is
 *                   the installed browser bridge; otherwise it is a label
 *                   shown to the user ("claims to be ...").
 *   2. identity   - the npub the identity selector resolves to (the key the
 *                   operation will actually use). One canonical form for the
 *                   ACL key, for lookup and for "remember" alike.
 *   3. grant      - $XDG_CONFIG_HOME/gnostr/signer-grants.ini, section = the
 *                   request kind, key "<principal>|<npub>" (or "|*" for any
 *                   identity), value allow|deny[:<until-unix-ts>]; then the
 *                   built-in defaults for first-party headless services.
 *   4. approval   - ApprovalRequested(principal, npub, kind, preview, id);
 *                   answered by ApproveRequest, which only the installed
 *                   approval UI may call. Identical queued calls from one
 *                   connection share a request, so a burst of DM decrypts
 *                   raises one prompt.
 * ======================================================================== */

typedef enum {
  OP_SIGN_EVENT = 0,
  OP_CONVKEY,
  OP_GET_PUBLIC_KEY,
  OP_GET_RELAYS,
  OP_NIP04_ENCRYPT,
  OP_NIP04_DECRYPT,
  OP_NIP44_ENCRYPT,
  OP_NIP44_DECRYPT,
  OP_NIP44_ENCRYPT_B64,
  OP_NIP44_DECRYPT_B64,
  OP_DECRYPT_ZAP,
  OP_COUNT
} SignerOp;

typedef struct {
  const char *kind;     /* ApprovalRequested kind == grants section */
  gboolean has_identity;
  gboolean coalesce;    /* queued calls may share one approval */
  gboolean peer_arg;    /* argument b is a 64-hex peer key */
} OpInfo;

static const OpInfo op_info[OP_COUNT] = {
  [OP_SIGN_EVENT]        = { "event",                  TRUE,  FALSE, FALSE },
  [OP_CONVKEY]           = { "nip44_conversation_key", TRUE,  FALSE, FALSE },
  [OP_GET_PUBLIC_KEY]    = { "get_public_key",         TRUE,  TRUE,  FALSE },
  [OP_GET_RELAYS]        = { "get_relays",             FALSE, TRUE,  FALSE },
  [OP_NIP04_ENCRYPT]     = { "nip04_encrypt",          TRUE,  TRUE,  TRUE  },
  [OP_NIP04_DECRYPT]     = { "nip04_decrypt",          TRUE,  TRUE,  TRUE  },
  [OP_NIP44_ENCRYPT]     = { "nip44_encrypt",          TRUE,  TRUE,  TRUE  },
  [OP_NIP44_DECRYPT]     = { "nip44_decrypt",          TRUE,  TRUE,  TRUE  },
  [OP_NIP44_ENCRYPT_B64] = { "nip44_encrypt",          TRUE,  TRUE,  TRUE  },
  [OP_NIP44_DECRYPT_B64] = { "nip44_decrypt",          TRUE,  TRUE,  TRUE  },
  [OP_DECRYPT_ZAP]       = { "zap_decrypt",            TRUE,  TRUE,  FALSE },
};

/* Where a call's outcome goes: a D-Bus method invocation, or a callback
 * (signer_gate.h: the NIP-5F socket). */
typedef struct {
  GDBusMethodInvocation *invocation;
  SignerGateReplyFn fn;
  gpointer ud;
} Reply;

static void reply_error(const Reply *r, const char *ename, const char *msg){
  if (r->invocation) g_dbus_method_invocation_return_dbus_error(r->invocation, ename, msg);
  else r->fn(r->ud, ename, msg, NULL);
}

/* Bus connections (unique names) that called EnableTypedApprovalErrors
 * (nip55l 0.5.0). Forgotten when the connection leaves the bus. */
static GHashTable *typed_senders = NULL;

/* An approval failure: @typed_name for a caller that opted in to typed
 * approval errors, Error.ApprovalDenied (the pre-0.5.0 contract) for anyone
 * else, including every NIP-5F socket caller. The message never changes. */
static void reply_approval_error(const Reply *r, const char *typed_name, const char *msg){
  gboolean typed = r->invocation && typed_senders &&
    g_hash_table_contains(typed_senders, g_dbus_method_invocation_get_sender(r->invocation));
  reply_error(r, typed ? typed_name : ORG_NOSTR_SIGNER_ERR_APPROVAL, msg);
}

static void reply_value(const Reply *r, const char *out){
  if (r->invocation) g_dbus_method_invocation_return_value(r->invocation, g_variant_new("(s)", out));
  else r->fn(r->ud, NULL, NULL, out);
}

/* One parked method call. */
typedef struct {
  SignerOp op;
  gchar *a;          /* event JSON / peer / plaintext / ciphertext */
  gchar *b;          /* peer pubkey for the NIP-04/44 calls */
  gchar *selector;   /* identity as the caller passed it */
  Reply reply;
} Call;

static void call_free(gpointer data){
  Call *c = data;
  if (!c) return;
  if (c->a) { memset(c->a, 0, strlen(c->a)); g_free(c->a); }
  g_free(c->b);
  /* A selector may be the secret key itself. */
  if (c->selector) { memset(c->selector, 0, strlen(c->selector)); g_free(c->selector); }
  g_free(c);
}

/* A request awaiting ApproveRequest: one or more coalesced calls. */
typedef struct {
  gchar *id;
  SignerOp op;
  SignerCaller *who;     /* principal (copy) */
  gchar *claimed;        /* caller-supplied app_id */
  gchar *npub;           /* ACL identity; NULL for identity-less kinds */
  gchar *preview;
  GPtrArray *calls;      /* Call* */
  guint timeout_id;
} Pending;

static void pending_free(gpointer data){
  Pending *p = data;
  if (!p) return;
  if (p->timeout_id) g_source_remove(p->timeout_id);
  g_free(p->id);
  signer_caller_free(p->who);
  g_free(p->claimed);
  g_free(p->npub);
  g_free(p->preview);
  if (p->calls) g_ptr_array_unref(p->calls);
  g_free(p);
}

static GHashTable *pending = NULL; /* id -> Pending* (owned) */

/* Parked requests are bounded so a flood cannot grow the table without
 * limit; ids are a monotonic counter, never reused (an address-based id could
 * be recycled after a request completes and let a stale ApproveRequest decide
 * a new call). Unanswered requests expire so a missing UI cannot pin them. */
#define PENDING_MAX 64
#define PENDING_PER_CALLER_MAX 8   /* one app cannot fill the table for the others */
#define PENDING_CALLS_MAX 256
#define PENDING_TTL_S 300
static guint pending_ttl_s(void){
#ifdef NIP55L_TEST_TRUST_ENV
  /* Test builds only: a short TTL so the timeout path can be exercised. */
  const char *env = g_getenv("NOSTR_SIGNER_TEST_PENDING_TTL_S");
  guint64 v = env ? g_ascii_strtoull(env, NULL, 10) : 0;
  if (v > 0 && v < PENDING_TTL_S) return (guint)v;
#endif
  return PENDING_TTL_S;
}
static gchar *next_request_id(void){
  static guint64 counter = 0;
  return g_strdup_printf("req-%" G_GUINT64_FORMAT, ++counter);
}

/* Key mutations (StoreKey, ClearKey, CreateProfile) are for the signer's
 * own UI, which is identified like an approver: by executable path and
 * inode, or Flatpak id, never on a platform that cannot attest the caller.
 * NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1 is the escape hatch for tests and
 * scripts: with it, any caller may. */
static gboolean signer_mutations_allowed(GDBusMethodInvocation *invocation){
  const char *env = g_getenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
  if (env && g_strcmp0(env, "1")==0) return TRUE;
  g_autoptr(SignerCaller) c = signer_caller_identify_fresh(
      g_dbus_method_invocation_get_connection(invocation),
      g_dbus_method_invocation_get_sender(invocation));
  if (!c || c->unattested) return FALSE;
  return signer_caller_is_approver(c);
}

/* Per-sender minimum interval; entries leave when the sender disconnects. */
static GHashTable *rate_tables[2];
static gboolean rate_limit_ok_ms(guint table, const char *sender, guint interval_ms){
  if (!rate_tables[table]) rate_tables[table] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!sender) sender = "";
  gint64 *prev = g_hash_table_lookup(rate_tables[table], sender);
  gint64 now = g_get_monotonic_time();
  if (prev && now - *prev < (gint64)interval_ms * 1000) return FALSE;
  gint64 *v = g_new(gint64, 1); *v = now;
  g_hash_table_insert(rate_tables[table], g_strdup(sender), v);
  return TRUE;
}
#define RATE_PROMPT   0
#define RATE_MUTATION 1
static gboolean rate_limit_ok(const char *sender){ return rate_limit_ok_ms(RATE_MUTATION, sender, 500); }

/* ---- grants ------------------------------------------------------------- */

/* nip55l < 0.4.0 kept "<app_id>:<identity>" entries in signer-acl.ini. Those
 * were keyed on a string any caller could claim, so they are not carried
 * over; the file is left alone and nothing reads it any more. */
static gchar *grants_file_path(void){
  const char *conf = g_get_user_config_dir();
  if (!conf) conf = g_get_home_dir();
  gchar *dir = g_build_filename(conf, "gnostr", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "signer-grants.ini", NULL);
  g_free(dir);
  return path;
}

static GKeyFile *grants_kf = NULL;
static gint64 grants_mtime = -1;
static goffset grants_size = -1;

/* The grants file, reloaded when it changes on disk. Borrowed; never NULL. */
static GKeyFile *grants_get(void){
  gchar *path = grants_file_path();
  GStatBuf st;
  gint64 mtime = -2; goffset size = -2;
  if (g_stat(path, &st) == 0) { mtime = (gint64)st.st_mtime; size = (goffset)st.st_size; }
  if (!grants_kf || mtime != grants_mtime || size != grants_size) {
    if (grants_kf) g_key_file_unref(grants_kf);
    grants_kf = g_key_file_new();
    if (mtime != -2) g_key_file_load_from_file(grants_kf, path, G_KEY_FILE_NONE, NULL);
    grants_mtime = mtime; grants_size = size;
  }
  g_free(path);
  return grants_kf;
}

/* Value "allow" | "deny" | "allow:<until>" | "deny:<until>" (until 0 =
 * never). FALSE when absent or unparseable. */
static gboolean grant_parse(const char *val, gboolean *allow, guint64 *until){
  if (!val) return FALSE;
  const char *rest;
  if (g_str_has_prefix(val, "allow")) { *allow = TRUE; rest = val + 5; }
  else if (g_str_has_prefix(val, "deny")) { *allow = FALSE; rest = val + 4; }
  else return FALSE;
  *until = 0;
  if (*rest == ':') *until = g_ascii_strtoull(rest + 1, NULL, 10);
  else if (*rest != '\0') return FALSE;
  return TRUE;
}

/* As grant_parse, and FALSE when expired. */
static gboolean grant_value(const char *val, gboolean *allow){
  gboolean a;
  guint64 until;
  if (!grant_parse(val, &a, &until)) return FALSE;
  if (until > 0 && (guint64)(g_get_real_time() / 1000000) >= until) return FALSE;
  *allow = a;
  return TRUE;
}

static gboolean grants_lookup_file(const char *kind, const char *principal, const char *npub,
                                   gboolean *allow){
  GKeyFile *kf = grants_get();
  const char *ids[2] = { npub, "*" };
  for (int i = 0; i < 2; i++) {
    if (!ids[i] || !*ids[i]) continue;
    g_autofree gchar *key = g_strdup_printf("%s|%s", principal, ids[i]);
    g_autofree gchar *val = g_key_file_get_string(kf, kind, key, NULL);
    if (grant_value(val, allow)) return TRUE;
  }
  return FALSE;
}

/* Built-in grants for first-party services that run without a UI (the
 * nostr-homed session helpers read the npub/relays and open the secrets
 * envelope at login). Matched on the caller's executable by path AND
 * device/inode, for unsandboxed same-user callers only; an explicit entry in
 * the grants file (e.g. a deny) takes precedence.
 * NIP55L_DEFAULT_GRANTS: "<kind>[,<kind>...]@<exe>[:<exe>...]" groups
 * separated by '+'. */
#ifndef NIP55L_DEFAULT_GRANTS
#define NIP55L_DEFAULT_GRANTS ""
#endif
static gboolean grants_lookup_default(const char *kind, const SignerCaller *who){
  if (!who || !who->same_uid || !who->exe || who->exe_ino == 0) return FALSE;
  if (who->kind != SIGNER_CALLER_EXE && who->kind != SIGNER_CALLER_SYSTEMD_SCOPE) return FALSE;
  gboolean hit = FALSE;
  g_auto(GStrv) groups = g_strsplit(NIP55L_DEFAULT_GRANTS, "+", -1);
  for (guint i = 0; groups[i] && !hit; i++) {
    gchar *at = strchr(groups[i], '@');
    if (!at) continue;
    *at = '\0';
    g_auto(GStrv) kinds = g_strsplit(groups[i], ",", -1);
    if (!g_strv_contains((const gchar * const *)kinds, kind)) continue;
    g_auto(GStrv) exes = g_strsplit(at + 1, ":", -1);
    for (guint j = 0; exes[j] && !hit; j++) {
      if (g_strcmp0(exes[j], who->exe) != 0) continue;
      GStatBuf st;
      hit = g_stat(exes[j], &st) == 0 && (guint64)st.st_dev == who->exe_dev &&
            (guint64)st.st_ino == who->exe_ino;
    }
  }
  return hit;
}

static gboolean grants_lookup(const char *kind, const SignerCaller *who, const char *npub,
                              gboolean *allow){
  if (who->principal && grants_lookup_file(kind, who->principal, npub, allow)) return TRUE;
  if (who->kind != SIGNER_CALLER_WEB_ORIGIN && grants_lookup_default(kind, who)) {
    *allow = TRUE;
    return TRUE;
  }
  return FALSE;
}

static gboolean grants_write(GKeyFile *kf){
  gsize len = 0;
  g_autofree gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_autofree gchar *path = grants_file_path();
  GError *err = NULL;
  gboolean ok = data && g_file_set_contents_full(path, data, (gssize)len,
                                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err);
  if (!ok) {
    g_warning("nostr-signer: cannot save grants to %s: %s", path, err ? err->message : "?");
    g_clear_error(&err);
  }
  grants_mtime = -1; /* reload on next use */
  return ok;
}

static gboolean grants_save(const char *kind, const char *principal, const char *npub,
                            gboolean allow, guint64 ttl_seconds){
  if (!kind || !principal || !*principal) return FALSE;
  GKeyFile *kf = grants_get();
  g_autofree gchar *key = g_strdup_printf("%s|%s", principal, (npub && *npub) ? npub : "*");
  g_autofree gchar *val = NULL;
  if (ttl_seconds > 0) {
    guint64 until = (guint64)(g_get_real_time() / 1000000) + ttl_seconds;
    val = g_strdup_printf("%s:%" G_GUINT64_FORMAT, allow ? "allow" : "deny", until);
  } else {
    val = g_strdup(allow ? "allow" : "deny");
  }
  g_key_file_set_string(kf, kind, key, val);
  return grants_write(kf);
}

/* Remove one entry (and its section when that empties). */
static gboolean grants_remove(const char *kind, const char *principal, const char *identity,
                              gboolean *removed){
  *removed = FALSE;
  GKeyFile *kf = grants_get();
  g_autofree gchar *key = g_strdup_printf("%s|%s", principal, identity);
  if (!g_key_file_remove_key(kf, kind, key, NULL)) return TRUE;
  *removed = TRUE;
  gsize n = 0;
  g_auto(GStrv) left = g_key_file_get_keys(kf, kind, &n, NULL);
  if (n == 0) g_key_file_remove_group(kf, kind, NULL);
  return grants_write(kf);
}

/* ---- errors ------------------------------------------------------------- */

static void return_sign_error(const Reply *invocation, int rc){
  switch (rc) {
    case NOSTR_SIGNER_ERROR_INVALID_JSON:
    case NOSTR_SIGNER_ERROR_INVALID_ARG:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT, "event JSON is not a valid Nostr event");
      break;
    case NOSTR_SIGNER_ERROR_NOT_FOUND:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_NO_KEY, "no key configured for this identity");
      break;
    default:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "sign failed");
      break;
  }
}

static void return_convkey_error(const Reply *invocation, int rc){
  switch (rc) {
    case NOSTR_SIGNER_ERROR_INVALID_KEY:
    case NOSTR_SIGNER_ERROR_INVALID_ARG:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT,
        "peer is not a 64-hex x-only public key on secp256k1");
      break;
    case NOSTR_SIGNER_ERROR_NOT_FOUND:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_NO_KEY, "no key configured for this identity");
      break;
    default:
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "conversation key derivation failed");
      break;
  }
}

/* The selector resolves to no key: answer without prompting. */
static void return_no_key(const Reply *invocation, SignerOp op, int rc){
  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND || rc == NOSTR_SIGNER_ERROR_INVALID_KEY) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_NO_KEY,
      op == OP_GET_PUBLIC_KEY ? "No key configured. Please set up a key in Grotto first."
                              : "no key configured for this identity");
  } else {
    gchar *msg = g_strdup_printf("key lookup failed (rc=%d)", rc);
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, msg);
    g_free(msg);
  }
}

static void return_string(const Reply *invocation, char *out, gboolean wipe){
  reply_value(invocation, out);
  if (wipe) memset(out, 0, strlen(out));
  free(out);
}

/* ---- performing an allowed call ----------------------------------------- */

/* What a person is asked to sign, in words: the kind by name and the
 * content's start. Parsed as an event (a hand scan stopped at the first
 * escaped quote, and cut at 96 bytes with no sign that it was cut;
 * nostrc-wic1). Display only. */
#define PREVIEW_MAX_CHARS 280
static const char *event_kind_name(int kind){
  switch (kind) {
    case 0:     return "Profile";
    case 1:     return "Note";
    case 3:     return "Follow list";
    case 4:     return "Encrypted message (NIP-04)";
    case 5:     return "Deletion request";
    case 6:     return "Repost";
    case 7:     return "Reaction";
    case 13:    return "Sealed message";
    case 14:    return "Chat message";
    case 16:    return "Repost";
    case 1059:  return "Gift-wrapped message";
    case 1984:  return "Report";
    case 9734:  return "Zap request";
    case 10002: return "Relay list";
    case 10050: return "Inbox relay list";
    case 22242: return "Relay sign-in";
    case 24133: return "Remote signer message";
    case 27235: return "HTTP sign-in";
    case 30023: return "Article";
    default:    return NULL;
  }
}

static gchar *build_event_preview(const char *event_json){
  if (!event_json) return g_strdup("");
  NostrEvent *ev = nostr_event_new();
  if (!ev || nostr_event_deserialize(ev, event_json) != 0) {
    if (ev) nostr_event_free(ev);
    return g_strdup("an event that could not be read");
  }
  int kind = nostr_event_get_kind(ev);
  const char *name = event_kind_name(kind);
  g_autofree gchar *label = name ? g_strdup_printf("%s (kind %d)", name, kind)
                                 : g_strdup_printf("Kind %d", kind);
  const char *content = nostr_event_get_content(ev);
  GString *out = g_string_new(label);
  if (content && *content) {
    g_autofree gchar *valid = g_utf8_make_valid(content, -1);
    glong n = g_utf8_strlen(valid, -1);
    g_autofree gchar *head = g_utf8_substring(valid, 0, MIN(n, PREVIEW_MAX_CHARS));
    for (gchar *c = head; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    g_string_append_printf(out, ": %s%s", head, n > PREVIEW_MAX_CHARS ? "\u2026" : "");
  }
  nostr_event_free(ev);
  return g_string_free(out, FALSE);
}

static gchar *build_preview(SignerOp op, const char *a, const char *b){
  switch (op) {
    case OP_SIGN_EVENT:        return build_event_preview(a);
    case OP_CONVKEY:           return g_strdup_printf("derive NIP-44 conversation key with %s", a);
    case OP_GET_PUBLIC_KEY:    return g_strdup("read your public key");
    case OP_GET_RELAYS:        return g_strdup("read your relay list");
    case OP_NIP04_ENCRYPT:
    case OP_NIP44_ENCRYPT:
    case OP_NIP44_ENCRYPT_B64: return g_strdup_printf("encrypt a message for %s", b);
    case OP_NIP04_DECRYPT:
    case OP_NIP44_DECRYPT:
    case OP_NIP44_DECRYPT_B64: return g_strdup_printf("decrypt a message from %s", b);
    case OP_DECRYPT_ZAP:       return g_strdup("decrypt a private zap");
    case OP_COUNT:             break;
  }
  return g_strdup("");
}

/* GetRelays source 2: the relay set a user configured in the grotto
 * GUI (GSettings org.nostr.Grotto "relays"). Only an explicitly written
 * value counts; the schema default is a list of public relays, not the
 * user's configuration. Absent schema (headless nip55l install) = no source. */
#define GNOSTR_SIGNER_SCHEMA_ID "org.nostr.Grotto"
static int get_relays_from_gsettings(char **out_json){
  *out_json = NULL;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, GNOSTR_SIGNER_SCHEMA_ID, TRUE) : NULL;
  if (!schema) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  gboolean has_key = g_settings_schema_has_key(schema, "relays");
  g_settings_schema_unref(schema);
  if (!has_key) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  GSettings *settings = g_settings_new(GNOSTR_SIGNER_SCHEMA_ID);
  GVariant *user = g_settings_get_user_value(settings, "relays");
  g_object_unref(settings);
  if (!user) return NOSTR_SIGNER_ERROR_NOT_FOUND;
  gsize n = 0;
  const gchar **urls = g_variant_get_strv(user, &n);
  int rc = nostr_nip55l_relays_from_list(urls, n, out_json);
  g_free(urls);
  g_variant_unref(user);
  if (rc == NOSTR_SIGNER_ERROR_INVALID_ARG) {
    g_warning("GetRelays: ignoring invalid relay URL in %s relays setting", GNOSTR_SIGNER_SCHEMA_ID);
    rc = NOSTR_SIGNER_ERROR_NOT_FOUND;
  }
  return rc;
}

static int perform_get_relays(const Reply *invocation){
  char *out=NULL; int rc = nostr_nip55l_get_relays(&out);
  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) rc = get_relays_from_gsettings(&out);
  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) {
    /* Expected state, not a failure: callers fall back to their own relays. */
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_NOT_FOUND,
      "no relays configured ($XDG_CONFIG_HOME/nostr/relays.conf or grotto relays)");
    return rc;
  }
  if (rc == NOSTR_SIGNER_ERROR_INVALID_JSON) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_CONFIG,
      "relays.conf is malformed: expected a JSON array of ws:// or wss:// URL strings");
    return rc;
  }
  if (rc!=0 || !out) {
    free(out);
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL, "get relays failed");
    return rc ? rc : NOSTR_SIGNER_ERROR_BACKEND;
  }
  return_string(invocation, out, FALSE);
  return 0;
}

/* Run an allowed call and complete its invocation. Returns the core rc. */
static int perform(const Call *c, const char *claimed_app_id){
  const Reply *inv = &c->reply;
  char *out = NULL;
  int rc = 0;
  const char *emsg = NULL;
  switch (c->op) {
    case OP_SIGN_EVENT:
      /* nip55l 0.2.0 contract: SignEvent returns the complete signed event. */
      rc = nostr_nip55l_sign_event_json(c->a, c->selector, claimed_app_id, &out);
      if (rc != 0 || !out) { free(out); rc = rc ? rc : NOSTR_SIGNER_ERROR_BACKEND; return_sign_error(inv, rc); return rc; }
      return_string(inv, out, FALSE);
      return 0;
    case OP_CONVKEY:
      rc = nostr_nip55l_nip44_conversation_key(c->a, c->selector, &out);
      if (rc != 0 || !out) { free(out); rc = rc ? rc : NOSTR_SIGNER_ERROR_BACKEND; return_convkey_error(inv, rc); return rc; }
      return_string(inv, out, TRUE);
      return 0;
    case OP_GET_PUBLIC_KEY:
      rc = nostr_nip55l_get_public_key(&out);
      if (rc != 0 || !out) { free(out); rc = rc ? rc : NOSTR_SIGNER_ERROR_BACKEND; return_no_key(inv, c->op, rc); return rc; }
      return_string(inv, out, FALSE);
      return 0;
    case OP_GET_RELAYS:
      return perform_get_relays(inv);
    case OP_NIP04_ENCRYPT:     rc = nostr_nip55l_nip04_encrypt(c->a, c->b, c->selector, &out);     emsg = "nip04 encrypt failed"; break;
    case OP_NIP04_DECRYPT:     rc = nostr_nip55l_nip04_decrypt(c->a, c->b, c->selector, &out);     emsg = "nip04 decrypt failed"; break;
    case OP_NIP44_ENCRYPT:     rc = nostr_nip55l_nip44_encrypt(c->a, c->b, c->selector, &out);     emsg = "nip44 encrypt failed"; break;
    case OP_NIP44_DECRYPT:     rc = nostr_nip55l_nip44_decrypt(c->a, c->b, c->selector, &out);     emsg = "nip44 decrypt failed"; break;
    case OP_NIP44_ENCRYPT_B64: rc = nostr_nip55l_nip44_encrypt_b64(c->a, c->b, c->selector, &out); emsg = "nip44 encrypt failed"; break;
    case OP_NIP44_DECRYPT_B64: rc = nostr_nip55l_nip44_decrypt_b64(c->a, c->b, c->selector, &out); emsg = "nip44 decrypt failed"; break;
    case OP_DECRYPT_ZAP:       rc = nostr_nip55l_decrypt_zap_event(c->a, c->selector, &out);       emsg = "zap decrypt failed"; break;
    case OP_COUNT:             rc = NOSTR_SIGNER_ERROR_INVALID_ARG; emsg = "unknown operation"; break;
  }
  if (rc != 0 || !out) {
    free(out);
    reply_error(inv, ORG_NOSTR_SIGNER_ERR_INTERNAL, emsg);
    return rc ? rc : NOSTR_SIGNER_ERROR_BACKEND;
  }
  gboolean plaintext = c->op == OP_NIP04_DECRYPT || c->op == OP_NIP44_DECRYPT ||
                       c->op == OP_NIP44_DECRYPT_B64 || c->op == OP_DECRYPT_ZAP;
  return_string(inv, out, plaintext);
  return 0;
}

/* ---- request lifecycle -------------------------------------------------- */

static gboolean is_hex64(const char *s){
  if (!s || strlen(s) != 64) return FALSE;
  for (const char *p = s; *p; p++) if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

/* Complete every call of @p with an approval failure (reply_approval_error). */
static void pending_fail(Pending *p, const char *typed_name, const char *msg){
  for (guint i = 0; i < p->calls->len; i++) {
    Call *c = g_ptr_array_index(p->calls, i);
    reply_approval_error(&c->reply, typed_name, msg);
  }
  g_ptr_array_set_size(p->calls, 0);
}

static void pending_finish(Pending *p, gboolean decision){
  if (signer_skel) nostr_signer_emit_approval_completed(signer_skel, p->id, decision);
  pending_free(p);
}

static gboolean on_pending_timeout(gpointer data){
  const char *id = data;
  Pending *p = pending ? g_hash_table_lookup(pending, id) : NULL;
  if (!p) return G_SOURCE_REMOVE;
  g_hash_table_steal(pending, id);
  p->timeout_id = 0; /* this source is being removed */
  g_message("nostr-signer: %s (%s) was not answered within %u s", p->id, op_info[p->op].kind, pending_ttl_s());
  pending_fail(p, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT, "approval timed out");
  pending_finish(p, FALSE);
  return G_SOURCE_REMOVE;
}

/* An identical request from the same connection that new calls may join. */
static Pending *pending_find_joinable(const char *sender, SignerOp op, const SignerCaller *who,
                                      const char *npub, const char *selector, const char *claimed){
  if (!pending || !op_info[op].coalesce) return NULL;
  GHashTableIter it; gpointer k, v;
  g_hash_table_iter_init(&it, pending);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    Pending *p = v;
    Call *first = p->calls->len ? g_ptr_array_index(p->calls, 0) : NULL;
    if (first && first->op == op && g_strcmp0(p->who->sender, sender) == 0 &&
        g_strcmp0(p->who->principal, who->principal) == 0 && g_strcmp0(p->npub, npub) == 0 &&
        g_strcmp0(first->selector, selector) == 0 && g_strcmp0(p->claimed, claimed) == 0)
      return p;
  }
  return NULL;
}

#ifndef NIP55L_APPROVER_BUS_NAME
#define NIP55L_APPROVER_BUS_NAME "org.nostr.Grotto"
#endif
/* Is an approval UI on the bus? Without one a prompt could never be
 * answered, so the call fails at once instead of hanging until it expires. */
static gboolean approver_present(GDBusConnection *bus){
  static const gchar name[] = NIP55L_APPROVER_BUS_NAME;
  if (name[0] == '\0') return TRUE;
  if (!bus || !signer_skel) return FALSE;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  gboolean has = FALSE;
  if (r) g_variant_get(r, "(b)", &has);
  return has;
}

static const char NO_APPROVER_MSG[] =
  "approval required but no approval agent is running (start Grotto)";

/* StartServiceByName returned: either the approval UI owns its name now (it
 * lists the pending requests itself), or it could not be started and the
 * request that asked for it fails as it would have without a UI. */
static void on_approver_summoned(GObject *source, GAsyncResult *res, gpointer data){
  g_autofree char *id = data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
  if (r) return;
  Pending *p = pending ? g_hash_table_lookup(pending, id) : NULL;
  if (!p) return;
  if (approver_present(signer_bus)) return; /* started by someone else meanwhile */
  g_message("nostr-signer: %s needs approval but the approval UI (%s) could not be started: %s",
            p->id, NIP55L_APPROVER_BUS_NAME, err ? err->message : "?");
  g_hash_table_steal(pending, id);
  if (p->timeout_id) { g_source_remove(p->timeout_id); p->timeout_id = 0; }
  pending_fail(p, ORG_NOSTR_SIGNER_ERR_NO_APPROVER, NO_APPROVER_MSG);
  pending_finish(p, FALSE);
}

/* Ask the bus to start the approval UI for request @id. Asynchronous: the UI
 * calls back into this daemon while it starts (ListPendingRequests), which
 * a synchronous wait here would deadlock. */
static void approver_summon(GDBusConnection *bus, const char *id){
  static const gchar name[] = NIP55L_APPROVER_BUS_NAME;
  if (!bus || name[0] == '\0') return;
  g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "StartServiceByName", g_variant_new("(su)", name, 0u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 25000, NULL, on_approver_summoned, g_strdup(id));
}

/* The access-control path for one call from @base (reached over the
 * connection @sender), whichever transport it came by. */
static void gate(const Reply *invocation, const SignerCaller *base, const char *sender, SignerOp op,
                 const char *a, const char *b, const char *selector, const char *claimed){
  GDBusConnection *bus = signer_bus;
  if (!selector) selector = "";
  if (!claimed) claimed = "";

  /* Reject malformed input before anyone is asked to approve it. */
  if (op == OP_CONVKEY && !is_hex64(a)) { return_convkey_error(invocation, NOSTR_SIGNER_ERROR_INVALID_KEY); return; }
  if (op_info[op].peer_arg && !is_hex64(b)) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT,
      "pubKey is not a 64-hex x-only public key");
    return;
  }
  g_autofree gchar *peer_lc = NULL;
  if (op == OP_CONVKEY) peer_lc = g_ascii_strdown(a, -1);

  /* 1. principal */
  g_autoptr(SignerCaller) origin = NULL;
  const SignerCaller *who = base;
  if (base->unattested) {
    origin = signer_caller_for_claim(base, claimed);
    who = origin;
  } else if (*claimed && signer_caller_is_web_origin(claimed) && signer_caller_may_assert_origin(base)) {
    origin = signer_caller_for_origin(base, claimed);
    who = origin;
  }

  /* 2. canonical identity. The caller's selector is normalised first: it is
   * never read as key material (a 64-hex is a pubkey, nsec is refused) and
   * an npub/hex must name a known identity exactly (nostrc-a4w5). */
  g_autofree char *npub_m = NULL;
  g_autofree char *sel_m = NULL;
  if (op_info[op].has_identity) {
    char *np = NULL, *ns = NULL;
    int rc = nostr_nip55l_normalize_selector(selector, &ns, &np);
    if (rc == NOSTR_SIGNER_ERROR_INVALID_ARG) {
      free(np); free(ns);
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT,
        "identity must name a stored identity (npub, hex public key, key_id or label), not a secret key");
      return;
    }
    if (rc != 0 || !np || !ns) { free(np); free(ns); return_no_key(invocation, op, rc ? rc : NOSTR_SIGNER_ERROR_NOT_FOUND); return; }
    npub_m = g_strdup(np);
    sel_m = g_strdup(ns);
    free(np);
    if (ns) { memset(ns, 0, strlen(ns)); free(ns); }
    selector = sel_m;
  }

  Call call = { op, (gchar *)(peer_lc ? peer_lc : a), (gchar *)b, (gchar *)selector, *invocation };

  /* 3. grant */
  gboolean allow = FALSE;
  if (grants_lookup(op_info[op].kind, who, npub_m, &allow)) {
    if (allow) (void)perform(&call, claimed);
    else reply_error(invocation, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy");
    return;
  }

  /* 4. approval */
  if (!pending) pending = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, pending_free);
  Pending *join = pending_find_joinable(sender, op, who, npub_m, selector, claimed);
  if (join) {
    if (join->calls->len >= PENDING_CALLS_MAX) {
      reply_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT,
        "too many calls awaiting approval");
      return;
    }
    Call *c = g_new0(Call, 1);
    *c = (Call){ op, g_strdup(call.a), g_strdup(b), g_strdup(selector), *invocation };
    g_ptr_array_add(join->calls, c);
    return;
  }
  guint mine = 0;
  {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, pending);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      const SignerCaller *o = ((Pending *)v)->who;
      /* Same principal, or (unidentified) the same connection. */
      if (who->principal ? g_strcmp0(o->principal, who->principal) == 0
                         : (!o->principal && g_strcmp0(o->sender, sender) == 0))
        mine++;
    }
  }
  if (mine >= PENDING_PER_CALLER_MAX) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT,
      "too many of this application's requests are awaiting approval");
    return;
  }
  if (g_hash_table_size(pending) >= PENDING_MAX) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT,
      "too many requests awaiting approval");
    return;
  }
  if (!rate_limit_ok_ms(RATE_PROMPT, sender, 100)) {
    reply_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "rate limited");
    return;
  }
  /* No approval UI on the bus: the request is still queued, and the UI is
   * started for it (approver_summon); it lists pending requests when it
   * subscribes. Only if it cannot be started does the request fail. */
  gboolean summon = !approver_present(bus);

  Pending *p = g_new0(Pending, 1);
  p->id = next_request_id();
  p->op = op;
  p->who = signer_caller_copy(who);
  p->claimed = g_strdup(claimed);
  p->npub = g_strdup(npub_m);
  p->preview = build_preview(op, call.a, b);
  p->calls = g_ptr_array_new_with_free_func(call_free);
  Call *c = g_new0(Call, 1);
  *c = (Call){ op, g_strdup(call.a), g_strdup(b), g_strdup(selector), *invocation };
  g_ptr_array_add(p->calls, c);
  p->timeout_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, pending_ttl_s(), on_pending_timeout,
                                             g_strdup(p->id), g_free);
  g_hash_table_insert(pending, p->id, p);

  g_message("nostr-signer: %s %s by %s (claimed '%s') identity=%s",
            p->id, op_info[op].kind, who->principal ? who->principal : "(unidentified)",
            claimed, npub_m ? npub_m : "-");
  nostr_signer_emit_approval_requested(signer_skel,
    who->principal ? who->principal : "",
    npub_m ? npub_m : selector,
    op_info[op].kind, p->preview, p->id);
  if (summon) approver_summon(bus, p->id);
}

static void gated_call(NostrSigner *object, GDBusMethodInvocation *invocation, SignerOp op,
                       const char *a, const char *b, const char *selector, const char *claimed){
  (void)object;
  const Reply reply = { invocation, NULL, NULL };
  const char *sender = g_dbus_method_invocation_get_sender(invocation);
  const SignerCaller *base = signer_caller_lookup(g_dbus_method_invocation_get_connection(invocation), sender);
  gate(&reply, base, sender, op, a, b, selector, claimed);
}

void signer_gate_submit(const SignerCaller *who, const gchar *conn_key, SignerGateOp op,
                        const gchar *a, const gchar *b, const gchar *selector,
                        SignerGateReplyFn reply_fn, gpointer user_data){
  g_return_if_fail(who && conn_key && reply_fn);
  const Reply reply = { NULL, reply_fn, user_data };
  SignerOp sop;
  switch (op) {
    case SIGNER_GATE_GET_PUBLIC_KEY: sop = OP_GET_PUBLIC_KEY; break;
    case SIGNER_GATE_SIGN_EVENT:     sop = OP_SIGN_EVENT; break;
    case SIGNER_GATE_NIP44_ENCRYPT:  sop = OP_NIP44_ENCRYPT; break;
    case SIGNER_GATE_NIP44_DECRYPT:  sop = OP_NIP44_DECRYPT; break;
    default:
      reply_error(&reply, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT, "unknown operation");
      return;
  }
  /* No app_id: a socket peer has nothing to claim, and is never unattested
   * (the kernel reports its PID on every platform the socket runs on). */
  gate(&reply, who, conn_key, sop, a, b, selector, "");
}

/* ---- ApproveRequest / GetApprovalInfo ----------------------------------- */

static gboolean require_approver(GDBusMethodInvocation *invocation){
  /* Identified afresh: a cached identity could predate an exec. */
  g_autoptr(SignerCaller) c = signer_caller_identify_fresh(
      g_dbus_method_invocation_get_connection(invocation),
      g_dbus_method_invocation_get_sender(invocation));
  if (signer_caller_is_approver(c)) return TRUE;
  g_message("nostr-signer: refused approval call from %s (%s)",
            c->principal ? c->principal : "(unidentified)", c->sender ? c->sender : "?");
  g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_PERMISSION,
    "only the signer's approval UI may answer approval requests");
  return FALSE;
}

#ifdef NIP55L_TEST_TRUST_ENV
/* Test builds only: each approving ApproveRequest swaps the env-lane active
 * key ($NOSTR_SIGNER_SECKEY_HEX) with $NOSTR_SIGNER_TEST_SWAP_KEY_ON_APPROVE,
 * as an account switch during the prompt would, so the approval-time
 * identity check can be exercised without a key store. */
static void test_swap_active_key(void){
  const char *other = g_getenv("NOSTR_SIGNER_TEST_SWAP_KEY_ON_APPROVE");
  if (!other || !*other) return;
  g_autofree gchar *next = g_strdup(other);
  g_autofree gchar *cur = g_strdup(g_getenv("NOSTR_SIGNER_SECKEY_HEX"));
  g_setenv("NOSTR_SIGNER_TEST_SWAP_KEY_ON_APPROVE", cur ? cur : "", TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", next, TRUE);
}
#endif

static gboolean pending_bus_sender_alive(const Pending *p)
{
  Call *first = p->calls->len ? g_ptr_array_index(p->calls, 0) : NULL;
  if (!first) return FALSE;
  if (!first->reply.invocation) return TRUE; /* NIP-5F has its own close handler. */
  const char *sender = g_dbus_method_invocation_get_sender(first->reply.invocation);
  if (!signer_bus || !sender || sender[0] != ':') return FALSE;
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(signer_bus,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "NameHasOwner", g_variant_new("(s)", sender), G_VARIANT_TYPE("(b)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  gboolean alive = FALSE;
  if (reply) g_variant_get(reply, "(b)", &alive);
  return alive;
}

static gboolean handle_approve_request(NostrSigner *object, GDBusMethodInvocation *invocation,
                                       const gchar *request_id, gboolean decision, gboolean remember, guint64 ttl_seconds)
{
  if (!require_approver(invocation)) return TRUE;
  Pending *p = (pending && request_id) ? g_hash_table_lookup(pending, request_id) : NULL;
  if (!p) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      ORG_NOSTR_SIGNER_ERR_NOT_FOUND,
      "This request expired; ask the app to try again");
    return TRUE;
  }
  if (!pending_bus_sender_alive(p)) {
    g_hash_table_steal(pending, request_id);
    pending_fail(p, ORG_NOSTR_SIGNER_ERR_APPROVAL, "caller disconnected");
    nostr_signer_complete_approve_request(object, invocation, FALSE);
    pending_finish(p, FALSE);
    return TRUE;
  }
  g_hash_table_steal(pending, request_id);
  g_message("nostr-signer: %s %s by %s: %s%s", p->id, op_info[p->op].kind,
            p->who->principal ? p->who->principal : "(unidentified)",
            decision ? "approved" : "denied", remember ? " (remembered)" : "");

  gboolean ok = TRUE;
  if (decision) {
#ifdef NIP55L_TEST_TRUST_ENV
    test_swap_active_key();
#endif
    /* The approved identity is the one the user saw; if the selector now
     * resolves elsewhere (active account switched), do not use the new key. */
    Call *first = g_ptr_array_index(p->calls, 0);
    gboolean same = TRUE;
    if (p->npub) {
      char *now_npub = NULL;
      same = nostr_nip55l_resolve_npub(first->selector, &now_npub) == 0 && now_npub &&
             strcmp(now_npub, p->npub) == 0;
      free(now_npub);
    }
    if (!same) {
      pending_fail(p, ORG_NOSTR_SIGNER_ERR_IDENTITY_CHANGED, "the identity changed while awaiting approval");
      ok = FALSE;
    } else {
      for (guint i = 0; i < p->calls->len; i++)
        if (perform(g_ptr_array_index(p->calls, i), p->claimed) != 0) ok = FALSE;
      g_ptr_array_set_size(p->calls, 0);
    }
  } else {
    pending_fail(p, ORG_NOSTR_SIGNER_ERR_APPROVAL, "user denied");
  }
  /* Never persist an allow for a request that failed identity binding or
   * execution. A remembered denial is still a valid policy decision. */
  if (remember && (!decision || ok)) {
    if (p->who->principal)
      grants_save(op_info[p->op].kind, p->who->principal, p->npub, decision, ttl_seconds);
    else
      g_message("nostr-signer: %s: caller is unidentified; decision not remembered", p->id);
  }
  nostr_signer_complete_approve_request(object, invocation, ok);
  pending_finish(p, decision && ok);
  return TRUE;
}

static gboolean handle_get_approval_info(NostrSigner *object, GDBusMethodInvocation *invocation,
                                         const gchar *request_id)
{
  if (!require_approver(invocation)) return TRUE;
  Pending *p = (pending && request_id) ? g_hash_table_lookup(pending, request_id) : NULL;
  if (!p) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_NOT_FOUND,
      "no such pending request");
    return TRUE;
  }
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
#define PUT_S(k, v) g_variant_builder_add(&b, "{sv}", k, g_variant_new_string((v) ? (v) : ""))
  PUT_S("request_id", p->id);
  PUT_S("kind", op_info[p->op].kind);
  PUT_S("principal", p->who->principal);
  PUT_S("principal_kind", p->who->unattested ? "claimed" : signer_caller_kind_to_string(p->who->kind));
  PUT_S("app_id", p->who->app_id);
  PUT_S("exe", p->who->exe);
  PUT_S("via", p->who->via);
  PUT_S("claimed_app_id", p->claimed);
  PUT_S("identity", p->npub);
  PUT_S("preview", p->preview);
#undef PUT_S
  g_variant_builder_add(&b, "{sv}", "attested", g_variant_new_boolean(p->who->attested));
  g_variant_builder_add(&b, "{sv}", "verified", g_variant_new_boolean(!p->who->unattested && p->who->principal != NULL));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(p->who->principal != NULL));
  g_variant_builder_add(&b, "{sv}", "calls", g_variant_new_uint32(p->calls->len));
  nostr_signer_complete_get_approval_info(object, invocation, g_variant_builder_end(&b));
  return TRUE;
}

static gboolean handle_list_pending_requests(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  if (!require_approver(invocation)) return TRUE;
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(sssss)"));
  if (pending) {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, pending);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      Pending *p = v;
      Call *first = p->calls->len ? g_ptr_array_index(p->calls, 0) : NULL;
      g_variant_builder_add(&b, "(sssss)",
        p->who->principal ? p->who->principal : "",
        p->npub ? p->npub : (first && first->selector ? first->selector : ""),
        op_info[p->op].kind, p->preview ? p->preview : "", p->id);
    }
  }
  nostr_signer_complete_list_pending_requests(object, invocation, g_variant_builder_end(&b));
  return TRUE;
}

static void forget_sender(const char *name);

/* ---- ListGrants / RevokeGrant (nostrc-yjky) ----------------------------- */

static gboolean handle_list_grants(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  if (!require_approver(invocation)) return TRUE;
  GKeyFile *kf = grants_get();
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(sssbt)"));
  g_auto(GStrv) kinds = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; kinds && kinds[i]; i++) {
    g_auto(GStrv) keys = g_key_file_get_keys(kf, kinds[i], NULL, NULL);
    for (guint j = 0; keys && keys[j]; j++) {
      const char *bar = strrchr(keys[j], '|');
      g_autofree gchar *val = g_key_file_get_string(kf, kinds[i], keys[j], NULL);
      gboolean allow;
      guint64 until;
      if (!bar || bar == keys[j] || !bar[1] || !grant_parse(val, &allow, &until)) continue;
      g_autofree gchar *principal = g_strndup(keys[j], (gsize)(bar - keys[j]));
      g_variant_builder_add(&b, "(sssbt)", kinds[i], principal, bar + 1, allow, until);
    }
  }
  nostr_signer_complete_list_grants(object, invocation, g_variant_builder_end(&b));
  return TRUE;
}

static gboolean handle_revoke_grant(NostrSigner *object, GDBusMethodInvocation *invocation,
                                    const gchar *kind, const gchar *principal, const gchar *identity)
{
  if (!require_approver(invocation)) return TRUE;
  if (!kind || !*kind || !principal || !*principal || !identity || !*identity ||
      strchr(principal, '|') || strchr(identity, '|')) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT,
                                               "kind, principal and identity are required");
    return TRUE;
  }
  gboolean removed = FALSE;
  if (!grants_remove(kind, principal, identity, &removed)) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
                                               "cannot write the grants file");
    return TRUE;
  }
  if (removed) g_message("nostr-signer: grant revoked: %s %s|%s", kind, principal, identity);
  nostr_signer_complete_revoke_grant(object, invocation, removed);
  return TRUE;
}

/* A connection left the bus: forget it, and drop requests only it was
 * waiting for (their replies have nowhere to go). */
static void on_name_owner_changed(GDBusConnection *conn, const gchar *sender_name,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *signal_name, GVariant *params, gpointer user_data){
  (void)conn; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name; (void)user_data;
  const gchar *name = NULL, *old_owner = NULL, *new_owner = NULL;
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (!name || name[0] != ':' || (new_owner && *new_owner)) return;
#ifdef NIP55L_TEST_TRUST_ENV
  /* Keep the pending call until ApproveRequest in the liveness regression fixture. */
  if (g_getenv("NOSTR_SIGNER_TEST_DEFER_OWNER_CLEANUP")) return;
#endif
  signer_caller_forget(name);
  forget_sender(name);
}

void signer_gate_connection_closed(const gchar *conn_key){
  if (conn_key) forget_sender(conn_key);
}

static void forget_sender(const char *name){
  for (guint t = 0; t < G_N_ELEMENTS(rate_tables); t++)
    if (rate_tables[t]) g_hash_table_remove(rate_tables[t], name);
  if (typed_senders) g_hash_table_remove(typed_senders, name);
  if (!pending) return;
  GPtrArray *gone = g_ptr_array_new();
  GHashTableIter it; gpointer k, v;
  g_hash_table_iter_init(&it, pending);
  while (g_hash_table_iter_next(&it, &k, &v))
    if (g_strcmp0(((Pending *)v)->who->sender, name) == 0) g_ptr_array_add(gone, v);
  for (guint i = 0; i < gone->len; i++) {
    Pending *p = g_ptr_array_index(gone, i);
    g_hash_table_steal(pending, p->id);
    pending_fail(p, ORG_NOSTR_SIGNER_ERR_APPROVAL, "caller disconnected");
    pending_finish(p, FALSE);
  }
  g_ptr_array_unref(gone);
}

/* ---- EnableTypedApprovalErrors (nip55l 0.5.0) -------------------------- */

/* Opt the calling connection in to typed approval errors. Ungated: it
 * changes only which error name this caller's own failures carry. */
static gboolean handle_enable_typed_approval_errors(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  const char *sender = g_dbus_method_invocation_get_sender(invocation);
  if (sender) {
    if (!typed_senders) typed_senders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(typed_senders, g_strdup(sender));
  }
  nostr_signer_complete_enable_typed_approval_errors(object, invocation);
  return TRUE;
}

/* ========== Generated handler glue ========== */

static gboolean handle_get_public_key(NostrSigner *o, GDBusMethodInvocation *inv)
{ gated_call(o, inv, OP_GET_PUBLIC_KEY, NULL, NULL, "", ""); return TRUE; }
static gboolean handle_get_public_key_for_app(NostrSigner *o, GDBusMethodInvocation *inv, const gchar *app_id)
{ gated_call(o, inv, OP_GET_PUBLIC_KEY, NULL, NULL, "", app_id); return TRUE; }

static gboolean handle_get_relays(NostrSigner *o, GDBusMethodInvocation *inv)
{ gated_call(o, inv, OP_GET_RELAYS, NULL, NULL, "", ""); return TRUE; }
static gboolean handle_get_relays_for_app(NostrSigner *o, GDBusMethodInvocation *inv, const gchar *app_id)
{ gated_call(o, inv, OP_GET_RELAYS, NULL, NULL, "", app_id); return TRUE; }

static gboolean handle_sign_event(NostrSigner *o, GDBusMethodInvocation *inv,
                                  const gchar *eventJson, const gchar *identity, const gchar *app_id)
{ gated_call(o, inv, OP_SIGN_EVENT, eventJson, NULL, identity, app_id); return TRUE; }

static gboolean handle_nip44_derive_conversation_key(NostrSigner *o, GDBusMethodInvocation *inv,
                                                     const gchar *peerPubKey, const gchar *identity,
                                                     const gchar *app_id)
{ gated_call(o, inv, OP_CONVKEY, peerPubKey, NULL, identity, app_id); return TRUE; }

#define CRYPT_HANDLERS(name, OP)                                                            \
  static gboolean handle_##name(NostrSigner *o, GDBusMethodInvocation *inv,                 \
                                const gchar *text, const gchar *pubKey, const gchar *identity) \
  { gated_call(o, inv, OP, text, pubKey, identity, ""); return TRUE; }                      \
  static gboolean handle_##name##_for_app(NostrSigner *o, GDBusMethodInvocation *inv,       \
                                          const gchar *text, const gchar *pubKey,           \
                                          const gchar *identity, const gchar *app_id)       \
  { gated_call(o, inv, OP, text, pubKey, identity, app_id); return TRUE; }
CRYPT_HANDLERS(nip04_encrypt, OP_NIP04_ENCRYPT)
CRYPT_HANDLERS(nip04_decrypt, OP_NIP04_DECRYPT)
CRYPT_HANDLERS(nip44_encrypt, OP_NIP44_ENCRYPT)
CRYPT_HANDLERS(nip44_decrypt, OP_NIP44_DECRYPT)
CRYPT_HANDLERS(nip44_encrypt_b64, OP_NIP44_ENCRYPT_B64)
CRYPT_HANDLERS(nip44_decrypt_b64, OP_NIP44_DECRYPT_B64)
#undef CRYPT_HANDLERS

static gboolean handle_decrypt_zap_event(NostrSigner *o, GDBusMethodInvocation *inv,
                                         const gchar *eventJson, const gchar *identity)
{ gated_call(o, inv, OP_DECRYPT_ZAP, eventJson, NULL, identity, ""); return TRUE; }
static gboolean handle_decrypt_zap_event_for_app(NostrSigner *o, GDBusMethodInvocation *inv,
                                                 const gchar *eventJson, const gchar *identity,
                                                 const gchar *app_id)
{ gated_call(o, inv, OP_DECRYPT_ZAP, eventJson, NULL, identity, app_id); return TRUE; }

static gboolean handle_store_key(NostrSigner *object, GDBusMethodInvocation *invocation,
                                  const gchar *key, const gchar *identity)
{
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!signer_mutations_allowed(invocation)) { g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_PERMISSION, "key mutations are for the signer's own UI (or NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1)"); return TRUE; }
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
          if (sk_hex) { for (int i=0;i<32;i++){ snprintf(sk_hex+2*i, 3, "%02x", sk[i]); } sk_hex[64]='\0'; }
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
  if (!signer_mutations_allowed(invocation)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "key mutations are for the signer's own UI (or NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1)"); return TRUE; }
  if (!rate_limit_ok(sender)) { g_dbus_method_invocation_return_error_literal(invocation, G_IO_ERROR, G_IO_ERROR_BUSY, "rate limited"); return TRUE; }
  int rc = nostr_nip55l_clear_key(identity);
  nostr_signer_complete_clear_key(object, invocation, rc==0);
  return TRUE;
}


/* nostrc-bml6: move keys out of the legacy libsecret schemas once per
 * keyring. Off the main loop so an unlock prompt (only raised when legacy
 * items exist) never delays D-Bus service. The outcome goes to the journal. */
static gpointer keyring_migration_thread(gpointer data) {
  (void)data;
  nostr_nip55l_keyring_migration r;
  int rc = nostr_nip55l_migrate_legacy_keys(&r);
  if (r.already_done) {
    g_debug("keyring-migration: marker present, nothing to do");
  } else if (r.found > 0 || rc != 0) {
    g_message("keyring-migration: rc=%d found=%u migrated=%u skipped=%u failed=%u marker=%s",
              rc, r.found, r.migrated, r.skipped, r.failed,
              r.marker_written ? "written" : "not-written");
  }
  return NULL;
}

static void start_keyring_migration(void) {
#ifdef NIP55L_TEST_TRUST_ENV
  /* Test builds: on macOS the Keychain is the user's real login keychain
   * (no per-bus backend as with gnome-keyring), so tests switch it off. */
  if (g_getenv("NOSTR_SIGNER_TEST_NO_MIGRATION")) return;
#endif
  static gsize started = 0;
  if (!g_once_init_enter(&started)) return;
  GError *err = NULL;
  GThread *t = g_thread_try_new("keyring-migrate", keyring_migration_thread, NULL, &err);
  if (t) g_thread_unref(t);
  else {
    g_warning("keyring-migration: could not start thread: %s", err ? err->message : "unknown");
    g_clear_error(&err);
  }
  g_once_init_leave(&started, 1);
}

/* ---------------------------------------------------------------------------
 * CreateProfile (nostrc-jvbl): generate a new random keypair, store it, return npub.
 * Approval UI only, or NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1.
 * Signature: (ssssb) -> (bs)
 *   in:  display_name, passphrase, recovery_hint, label, use_hardware_key
 *   out: ok, npub
 * passphrase, recovery_hint, label and use_hardware_key are reserved.
 * ------------------------------------------------------------------------- */
static gboolean handle_create_profile(NostrSigner *object, GDBusMethodInvocation *invocation,
                                       const gchar *display_name, const gchar *passphrase,
                                       const gchar *recovery_hint, const gchar *label,
                                       gboolean use_hardware_key)
{
  (void)passphrase; (void)recovery_hint; (void)label; (void)use_hardware_key;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!signer_mutations_allowed(invocation)) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_PERMISSION,
      "key mutations are for the signer's own UI (or NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1)");
    return TRUE;
  }
  if (!rate_limit_ok(sender)) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "rate limited");
    return TRUE;
  }

  /* Generate a new random keypair */
  char *sk_hex = nostr_key_generate_private();
  if (!sk_hex) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      "failed to generate key");
    return TRUE;
  }

  /* Store the key; use display_name as the identity selector if provided */
  const char *sel = (display_name && *display_name) ? display_name : "";
  int rc = nostr_nip55l_store_key(sk_hex, sel);
  if (rc != 0) {
    secure_wipe(sk_hex, strlen(sk_hex));
    free(sk_hex);
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      "failed to store generated key");
    return TRUE;
  }

  /* Derive npub for the response */
  char *pk_hex = nostr_key_get_public(sk_hex);
  secure_wipe(sk_hex, strlen(sk_hex));
  free(sk_hex);
  if (!pk_hex) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      "failed to derive public key");
    return TRUE;
  }

  uint8_t pk[32];
  if (!nostr_hex2bin(pk, pk_hex, sizeof pk)) {
    free(pk_hex);
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      "failed to encode public key");
    return TRUE;
  }
  free(pk_hex);

  char *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0 || !npub) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      "failed to encode npub");
    return TRUE;
  }

  g_message("CreateProfile: ok=true npub=%s", npub);
  nostr_signer_complete_create_profile(object, invocation, TRUE, npub);
  free(npub);
  return TRUE;
}

/* ---------------------------------------------------------------------------
 * ListIdentities (nostrc-oh0s): the npubs of every identity the signer holds
 * (stored ones plus the environment key, nostrc-sic82). No approval gating.
 * Signature: () -> (as). Error.Internal when they cannot be listed: an empty
 * array would claim the signer holds none (nostrc-sjyl3).
 * ------------------------------------------------------------------------- */
static gboolean handle_list_identities(NostrSigner *object, GDBusMethodInvocation *invocation)
{
  char **npubs = NULL;
  int count = 0;
  int rc = nostr_nip55l_list_identities(&npubs, &count);
  if (rc != 0) {
    g_dbus_method_invocation_return_dbus_error(invocation, ORG_NOSTR_SIGNER_ERR_INTERNAL,
      rc == NOSTR_SIGNER_ERROR_NOT_FOUND
        ? "no key store is available and no key is configured in the signer's environment"
        : "the key store could not be read");
    return TRUE;
  }
  /* Build a null-terminated string array for the D-Bus response */
  const gchar **arr = g_new0(const gchar*, count + 1);
  for (int i = 0; i < count; i++)
    arr[i] = npubs[i];
  arr[count] = NULL;
  nostr_signer_complete_list_identities(object, invocation, arr);
  for (int i = 0; i < count; i++)
    free(npubs[i]);
  free(npubs);
  g_free(arr);
  return TRUE;
}

static guint name_owner_sub = 0;

/* Requests awaiting the person's answer (for a daemon deciding whether it
 * may exit). */
guint signer_pending_count(void){
  return pending ? g_hash_table_size(pending) : 0;
}

guint signer_export(GDBusConnection *conn, const char *object_path) {
  if (signer_skel) return 1;
  signer_skel = nostr_signer_skeleton_new();
  /* Connect gdbus-codegen handler signals */
  static const struct { const char *signal; GCallback cb; } handlers[] = {
    { "handle-get-public-key",                G_CALLBACK(handle_get_public_key) },
    { "handle-get-public-key-for-app",        G_CALLBACK(handle_get_public_key_for_app) },
    { "handle-sign-event",                    G_CALLBACK(handle_sign_event) },
    { "handle-approve-request",               G_CALLBACK(handle_approve_request) },
    { "handle-get-approval-info",             G_CALLBACK(handle_get_approval_info) },
    { "handle-list-pending-requests",         G_CALLBACK(handle_list_pending_requests) },
    { "handle-list-grants",                   G_CALLBACK(handle_list_grants) },
    { "handle-revoke-grant",                  G_CALLBACK(handle_revoke_grant) },
    { "handle-enable-typed-approval-errors",  G_CALLBACK(handle_enable_typed_approval_errors) },
    { "handle-nip04-encrypt",                 G_CALLBACK(handle_nip04_encrypt) },
    { "handle-nip04-encrypt-for-app",         G_CALLBACK(handle_nip04_encrypt_for_app) },
    { "handle-nip04-decrypt",                 G_CALLBACK(handle_nip04_decrypt) },
    { "handle-nip04-decrypt-for-app",         G_CALLBACK(handle_nip04_decrypt_for_app) },
    { "handle-nip44-encrypt",                 G_CALLBACK(handle_nip44_encrypt) },
    { "handle-nip44-encrypt-for-app",         G_CALLBACK(handle_nip44_encrypt_for_app) },
    { "handle-nip44-decrypt",                 G_CALLBACK(handle_nip44_decrypt) },
    { "handle-nip44-decrypt-for-app",         G_CALLBACK(handle_nip44_decrypt_for_app) },
    { "handle-nip44-encrypt-b64",             G_CALLBACK(handle_nip44_encrypt_b64) },
    { "handle-nip44-encrypt-b64-for-app",     G_CALLBACK(handle_nip44_encrypt_b64_for_app) },
    { "handle-nip44-decrypt-b64",             G_CALLBACK(handle_nip44_decrypt_b64) },
    { "handle-nip44-decrypt-b64-for-app",     G_CALLBACK(handle_nip44_decrypt_b64_for_app) },
    { "handle-decrypt-zap-event",             G_CALLBACK(handle_decrypt_zap_event) },
    { "handle-decrypt-zap-event-for-app",     G_CALLBACK(handle_decrypt_zap_event_for_app) },
    { "handle-nip44-derive-conversation-key", G_CALLBACK(handle_nip44_derive_conversation_key) },
    { "handle-get-relays",                    G_CALLBACK(handle_get_relays) },
    { "handle-get-relays-for-app",            G_CALLBACK(handle_get_relays_for_app) },
    { "handle-store-key",                     G_CALLBACK(handle_store_key) },
    { "handle-clear-key",                     G_CALLBACK(handle_clear_key) },
    { "handle-create-profile",                G_CALLBACK(handle_create_profile) },
    { "handle-list-identities",               G_CALLBACK(handle_list_identities) },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(handlers); i++)
    g_signal_connect(signer_skel, handlers[i].signal, handlers[i].cb, NULL);

  if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(signer_skel), conn, object_path, NULL)) {
    g_object_unref(signer_skel); signer_skel = NULL; return 0;
  }
  g_set_object(&signer_bus, conn);
  if (conn && !name_owner_sub)
    name_owner_sub = g_dbus_connection_signal_subscribe(conn, "org.freedesktop.DBus",
        "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
  start_keyring_migration();
  return 1; /* dummy reg id */
}

void signer_unexport(GDBusConnection *conn, guint reg_id) {
  (void)reg_id;
  if (conn && name_owner_sub) { g_dbus_connection_signal_unsubscribe(conn, name_owner_sub); name_owner_sub = 0; }
  if (signer_skel) {
    g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(signer_skel));
    g_object_unref(signer_skel); signer_skel = NULL;
  }
  g_clear_object(&signer_bus);
}
