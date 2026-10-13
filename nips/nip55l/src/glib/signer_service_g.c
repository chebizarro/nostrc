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
#include "signer_approval_service.h"

/* Generated skeleton instance */
static NostrSigner *signer_skel = NULL;
static GDBusConnection *signer_bus = NULL; /* the connection it is exported on */

static SignerApprovalService *approval_service = NULL;
static GHashTable *typed_senders = NULL;
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
typedef struct { SignerApprovalService *service; gchar *id; } Summon;
static void on_approver_summoned(GObject *source, GAsyncResult *res, gpointer data) {
  Summon *summon = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
  if (!reply && summon->service == approval_service && !approver_present(signer_bus))
    signer_approval_service_fail_request(summon->service, summon->id, ORG_NOSTR_SIGNER_ERR_NO_APPROVER, NO_APPROVER_MSG);
  signer_approval_service_unref(summon->service); g_free(summon->id); g_free(summon);
}

/* Ask the bus to start the approval UI for request @id. Asynchronous: the UI
 * calls back into this daemon while it starts (ListPendingRequests), which
 * a synchronous wait here would deadlock. */
static void approver_summon(GDBusConnection *bus, const char *id){
  static const gchar name[] = NIP55L_APPROVER_BUS_NAME;
  if (!bus || name[0] == '\0') return;
  Summon *summon = g_new0(Summon, 1);
  summon->service = signer_approval_service_ref(approval_service); summon->id = g_strdup(id);
  g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "StartServiceByName", g_variant_new("(su)", name, 0u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 25000, NULL, on_approver_summoned, summon);
}

typedef SignerGateOp SignerOp;
#define OP_SIGN_EVENT SIGNER_GATE_SIGN_EVENT
#define OP_CONVKEY SIGNER_GATE_CONVERSATION_KEY
#define OP_GET_PUBLIC_KEY SIGNER_GATE_GET_PUBLIC_KEY
#define OP_GET_RELAYS SIGNER_GATE_GET_RELAYS
#define OP_NIP04_ENCRYPT SIGNER_GATE_NIP04_ENCRYPT
#define OP_NIP04_DECRYPT SIGNER_GATE_NIP04_DECRYPT
#define OP_NIP44_ENCRYPT SIGNER_GATE_NIP44_ENCRYPT
#define OP_NIP44_DECRYPT SIGNER_GATE_NIP44_DECRYPT
#define OP_NIP44_ENCRYPT_B64 SIGNER_GATE_NIP44_ENCRYPT_B64
#define OP_NIP44_DECRYPT_B64 SIGNER_GATE_NIP44_DECRYPT_B64
#define OP_DECRYPT_ZAP SIGNER_GATE_DECRYPT_ZAP

typedef struct { GDBusMethodInvocation *invocation; gboolean wrapped; } BusReply;
static void bus_reply(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  BusReply *reply = data;
  if (error) {
    const gchar *sender = g_dbus_method_invocation_get_sender(reply->invocation);
    gboolean typed = typed_senders && g_hash_table_contains(typed_senders, sender);
    if (!typed && (!strcmp(error, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT) ||
                   !strcmp(error, ORG_NOSTR_SIGNER_ERR_NO_APPROVER) ||
                   !strcmp(error, ORG_NOSTR_SIGNER_ERR_IDENTITY_CHANGED) ||
                   !strcmp(error, ORG_NOSTR_SIGNER_ERR_CANCELLED))) error = ORG_NOSTR_SIGNER_ERR_APPROVAL;
    g_dbus_method_invocation_return_dbus_error(reply->invocation, error, message ? message : "operation failed");
  } else {
    GVariant *tuple = g_variant_new("(s)", result);
    g_dbus_method_invocation_return_value(reply->invocation, reply->wrapped ? g_variant_new("(v)", tuple) : tuple);
  }
  g_object_unref(reply->invocation); g_free(reply);
}
static void gated_call_full(NostrSigner *object, GDBusMethodInvocation *invocation, SignerOp op,
    const gchar *a, const gchar *b, const gchar *selector, const gchar *claimed,
    const SignerGateContext *context, gboolean wrapped) {
  (void)object;
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  const SignerCaller *base = signer_caller_lookup(g_dbus_method_invocation_get_connection(invocation), sender);
  g_autoptr(SignerCaller) principal = NULL;
  if (base->unattested) principal = signer_caller_for_claim(base, claimed ? claimed : "");
  else if (claimed && *claimed && signer_caller_is_web_origin(claimed) && signer_caller_may_assert_origin(base))
    principal = signer_caller_for_origin(base, claimed);
  SignerGateContext ctx = context ? *context : (SignerGateContext){0};
  ctx.transport = SIGNER_TRANSPORT_DBUS; ctx.claimed_name = claimed;
  BusReply *reply = g_new0(BusReply, 1);
  reply->invocation = g_object_ref(invocation); reply->wrapped = wrapped;
  g_autoptr(SignerGateHandle) handle = signer_approval_service_submit(approval_service,
      principal ? principal : base, sender, op, a, b, selector, &ctx, bus_reply, reply);
}
static void gated_call(NostrSigner *o, GDBusMethodInvocation *inv, SignerOp op,
    const gchar *a, const gchar *b, const gchar *selector, const gchar *claimed) {
  gated_call_full(o, inv, op, a, b, selector, claimed, NULL, FALSE);
}
SignerApprovalService *signer_get_approval_service(void) { return approval_service; }
SignerGateHandle *signer_gate_submit_full(const SignerCaller *who, const gchar *key, SignerGateOp op,
    const gchar *a, const gchar *b, const gchar *selector, const SignerGateContext *context,
    SignerGateReplyFn reply, gpointer data) {
  if (!approval_service) { reply(data, ORG_NOSTR_SIGNER_ERR_CANCELLED, "signer is not exported", NULL); return NULL; }
  return signer_approval_service_submit(approval_service, who, key, op, a, b, selector, context, reply, data);
}
SignerGateHandle *signer_gate_submit(const SignerCaller *who, const gchar *key, SignerGateOp op,
    const gchar *a, const gchar *b, const gchar *selector, SignerGateReplyFn reply, gpointer data) {
  SignerGateContext context = {0};
  context.transport = who && who->kind == SIGNER_CALLER_NIP46 ? SIGNER_TRANSPORT_NIP46 : SIGNER_TRANSPORT_NIP5F;
  context.no_identity = op == SIGNER_GATE_PING || (op == SIGNER_GATE_CONNECT && (!selector || !*selector));
  return signer_gate_submit_full(who, key, op, a, b, selector, &context, reply, data);
}
static void on_approval_change(gpointer data, GVariant *info) {
  (void)data;
  const gchar *state = NULL, *id = NULL;
  g_variant_lookup(info, "state", "&s", &state); g_variant_lookup(info, "request_id", "&s", &id);
  if (!signer_skel) return;
  if (!strcmp(state, "PENDING")) {
    const gchar *principal = "", *identity = "", *kind = "", *preview = "";
    g_variant_lookup(info, "principal", "&s", &principal); g_variant_lookup(info, "identity", "&s", &identity);
    g_variant_lookup(info, "kind", "&s", &kind); g_variant_lookup(info, "preview", "&s", &preview);
    nostr_signer_emit_approval_requested(signer_skel, principal, identity, kind, preview, id);
    if (!approver_present(signer_bus)) approver_summon(signer_bus, id);
  } else {
    gboolean announced = FALSE;
    g_variant_lookup(info, "approval_requested", "b", &announced);
    if (announced) nostr_signer_emit_approval_completed(signer_skel, id, !strcmp(state, "SUCCEEDED"));
  }
}

/* Only the plain gated client methods. Arguments are decoded once and sent
 * through gated_call_full, exactly as the generated direct handlers below. */
static gboolean handle_call_with_context(NostrSigner *o, GDBusMethodInvocation *inv,
    const gchar *method, GVariant *arguments, GVariant *context) {
  static const struct { const gchar *name, *signature; SignerOp op; } methods[] = {
    /* D-Bus cannot carry () inside v: no-input methods remain direct-only. */
    {"SignEvent", "(sss)", OP_SIGN_EVENT}, {"NIP44DeriveConversationKey", "(sss)", OP_CONVKEY},
    {"NIP04Encrypt", "(sss)", OP_NIP04_ENCRYPT}, {"NIP04Decrypt", "(sss)", OP_NIP04_DECRYPT},
    {"NIP44Encrypt", "(sss)", OP_NIP44_ENCRYPT}, {"NIP44Decrypt", "(sss)", OP_NIP44_DECRYPT}
  };
  SignerGateContext ctx = {0};
  g_autoptr(GVariant) tuple = NULL;
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  GVariantIter it; const gchar *key; GVariant *value;
  g_variant_iter_init(&it, context);
  while (g_variant_iter_next(&it, "{&sv}", &key, &value)) {
    gboolean valid = g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) && !g_hash_table_contains(seen, key);
    const gchar *text = valid ? g_variant_get_string(value, NULL) : "";
    if (!strcmp(key, "reason")) valid = valid && strlen(text) <= 512 && g_utf8_strlen(text, -1) <= 160;
    else if (!strcmp(key, "purpose") || !strcmp(key, "flow_id")) valid = valid && strlen(text) <= 64;
    else valid = FALSE;
    g_hash_table_add(seen, (gpointer)key);
    g_variant_unref(value);
    if (!valid) goto invalid;
  }
  g_variant_lookup(context, "reason", "&s", &ctx.reason);
  g_variant_lookup(context, "purpose", "&s", &ctx.purpose);
  g_variant_lookup(context, "flow_id", "&s", &ctx.flow_id);
  tuple = g_variant_get_variant(arguments);
  for (guint i = 0; i < G_N_ELEMENTS(methods); i++) {
    if (strcmp(method, methods[i].name)) continue;
    if (!g_variant_is_of_type(tuple, G_VARIANT_TYPE(methods[i].signature))) goto invalid;
    const gchar *a = NULL, *b = NULL, *selector = "", *claimed = "";
    if (methods[i].op == OP_SIGN_EVENT || methods[i].op == OP_CONVKEY)
      g_variant_get(tuple, "(&s&s&s)", &a, &selector, &claimed);
    else g_variant_get(tuple, "(&s&s&s)", &a, &b, &selector);
    gated_call_full(o, inv, methods[i].op, a, b, selector, claimed, &ctx, TRUE);
    return TRUE;
  }
invalid:
  g_dbus_method_invocation_return_dbus_error(inv, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT,
    "CallWithContext requires a plain gated client method, its exact input tuple, and bounded reason/purpose/flow_id strings");
  return TRUE;
}

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


static void approval_decided(gpointer data, gboolean ok, const gchar *error, const gchar *message) {
  GDBusMethodInvocation *inv = data;
  if (error) g_dbus_method_invocation_return_dbus_error(inv, error, message);
  else g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", ok));
  g_object_unref(inv);
}
static gboolean connection_alive(const gchar *key, gpointer data) {
  GDBusConnection *bus = data;
  if (key[0] != ':') return TRUE; /* socket/relay adapters own disconnects */
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", key), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  gboolean alive = FALSE; if (reply) g_variant_get(reply, "(b)", &alive); return alive;
}
static gboolean handle_approve_request(NostrSigner *o, GDBusMethodInvocation *inv,
    const gchar *id, gboolean decision, gboolean remember, guint64 ttl) {
  (void)o;
  if (!require_approver(inv)) return TRUE;
  g_autoptr(GVariant) before = signer_approval_service_info(approval_service, id);
  signer_approval_service_prune_connections(approval_service, connection_alive, signer_bus);
  g_autoptr(GVariant) after = signer_approval_service_info(approval_service, id);
  if (before && !after) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE)); return TRUE;
  }
#ifdef NIP55L_TEST_TRUST_ENV
  if (decision) test_swap_active_key();
#endif
  signer_approval_service_decide(approval_service, id, decision, remember, ttl, approval_decided, g_object_ref(inv));
  return TRUE;
}
static gboolean handle_get_approval_info(NostrSigner *o, GDBusMethodInvocation *inv, const gchar *id) {
  if (!require_approver(inv)) return TRUE;
  g_autoptr(GVariant) info = signer_approval_service_info(approval_service, id);
  if (!info) g_dbus_method_invocation_return_dbus_error(inv, ORG_NOSTR_SIGNER_ERR_NOT_FOUND, "no such pending request");
  else nostr_signer_complete_get_approval_info(o, inv, info);
  return TRUE;
}
static gboolean handle_list_pending_requests(NostrSigner *o, GDBusMethodInvocation *inv) {
  if (!require_approver(inv)) return TRUE;
  g_autoptr(GVariant) pending = signer_approval_service_list_pending(approval_service);
  nostr_signer_complete_list_pending_requests(o, inv, pending); return TRUE;
}
static gboolean handle_list_grants(NostrSigner *o, GDBusMethodInvocation *inv) {
  if (!require_approver(inv)) return TRUE;
  g_autoptr(GVariant) grants = signer_approval_service_list_grants(approval_service);
  if (!grants) g_dbus_method_invocation_return_dbus_error(inv, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot load permissions");
  else nostr_signer_complete_list_grants(o, inv, grants);
  return TRUE;
}
static gboolean handle_revoke_grant(NostrSigner *o, GDBusMethodInvocation *inv,
    const gchar *kind, const gchar *principal, const gchar *identity) {
  if (!require_approver(inv)) return TRUE;
    if (!kind || !*kind || !principal || !*principal || !identity || !*identity) {
    g_dbus_method_invocation_return_dbus_error(inv, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT, "kind, principal and identity are required"); return TRUE;
  }
  gboolean removed = FALSE;
  if (!signer_approval_service_revoke_grant(approval_service, kind, principal, identity, &removed))
    g_dbus_method_invocation_return_dbus_error(inv, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot write the grants file");
  else nostr_signer_complete_revoke_grant(o, inv, removed);
  return TRUE;
}
void signer_gate_connection_closed(const gchar *key) {
  if (!key) return;
  for (guint i = 0; i < G_N_ELEMENTS(rate_tables); i++) if (rate_tables[i]) g_hash_table_remove(rate_tables[i], key);
  if (typed_senders) g_hash_table_remove(typed_senders, key);
  if (approval_service) signer_approval_service_connection_closed(approval_service, key);
}
static void on_name_owner_changed(GDBusConnection *conn, const gchar *sender_name,
    const gchar *path, const gchar *interface, const gchar *signal, GVariant *params, gpointer data) {
  (void)conn; (void)sender_name; (void)path; (void)interface; (void)signal; (void)data;
  const gchar *name, *old_owner, *new_owner;
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (name[0] != ':' || *new_owner) return;
#ifdef NIP55L_TEST_TRUST_ENV
  if (g_getenv("NOSTR_SIGNER_TEST_DEFER_OWNER_CLEANUP")) return;
#endif
  signer_caller_forget(name); signer_gate_connection_closed(name);
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
  return approval_service ? signer_approval_service_pending_count(approval_service) : 0;
}

guint signer_export(GDBusConnection *conn, const char *object_path) {
  if (signer_skel) return 1;
  signer_skel = nostr_signer_skeleton_new();
  /* Connect gdbus-codegen handler signals */
  static const struct { const char *signal; GCallback cb; } handlers[] = {
    { "handle-call-with-context", G_CALLBACK(handle_call_with_context) },
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
  approval_service = signer_approval_service_new(g_main_context_get_thread_default());
  signer_approval_service_subscribe(approval_service, on_approval_change, NULL);
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
  if (approval_service) {
    signer_approval_service_shutdown(approval_service);
    signer_approval_service_unref(approval_service); approval_service = NULL;
  }
  if (signer_skel) {
    g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(signer_skel));
    g_object_unref(signer_skel); signer_skel = NULL;
  }
  g_clear_object(&signer_bus);
}
