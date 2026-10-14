#include "signer_approval_service.h"
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"
#include "nip55l_dbus_errors.h"
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr-json.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <secure_buf.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>

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
#define OP_COUNT SIGNER_GATE_OP_COUNT
typedef SignerGateOp SignerOp;

typedef struct { const gchar *kind; gboolean has_identity; gboolean peer_arg; } OpInfo;
static const OpInfo op_info[OP_COUNT] = {
 [OP_SIGN_EVENT] = {"event", TRUE, FALSE},
 [OP_CONVKEY] = {"nip44_conversation_key", TRUE, FALSE},
 [OP_GET_PUBLIC_KEY] = {"get_public_key", TRUE, FALSE},
 [OP_GET_RELAYS] = {"get_relays", FALSE, FALSE},
 [OP_NIP04_ENCRYPT] = {"nip04_encrypt", TRUE, TRUE},
 [OP_NIP04_DECRYPT] = {"nip04_decrypt", TRUE, TRUE},
 [OP_NIP44_ENCRYPT] = {"nip44_encrypt", TRUE, TRUE},
 [OP_NIP44_DECRYPT] = {"nip44_decrypt", TRUE, TRUE},
 [OP_NIP44_ENCRYPT_B64] = {"nip44_encrypt", TRUE, TRUE},
 [OP_NIP44_DECRYPT_B64] = {"nip44_decrypt", TRUE, TRUE},
 [OP_DECRYPT_ZAP] = {"zap_decrypt", TRUE, FALSE},
 [SIGNER_GATE_CONNECT] = {"connect", TRUE, FALSE},
 [SIGNER_GATE_PING] = {"ping", FALSE, FALSE}
};

typedef struct _Request Request;
typedef struct {
 SignerGateHandle *handle;
 gchar *connection;
 SignerGateReplyFn reply;
 gpointer data;
} Continuation;
struct _SignerGateHandle { gint refs; gint done; Request *request; Continuation *continuation; };
typedef struct { guint id; SignerApprovalNotifyFn fn; gpointer data; } Subscriber;
struct _SignerApprovalService {
 gint refs;
 GMainContext *context;
 GThread *owner;
 GHashTable *requests;
 GHashTable *prompt_rates;
 GPtrArray *subscribers;
 guint next_subscriber;
 GThreadPool *backend;
 gboolean paused, stopped, no_approval_ui;
 guint64 generation;
 SignerPermissions *permissions;
 void (*changed)(gpointer);
 gpointer changed_data;
#ifdef NIP55L_TEST_TRUST_ENV
 void (*after_backend)(gpointer);
 gpointer after_backend_data;
#endif
};
struct _Request {
 gint refs;
 SignerApprovalService *service;
 gchar *id, *join_key, *a, *b, *selector, *npub, *claimed;
 gchar *reason, *purpose, *flow_id, *preview, *error_code;
 gchar **pairing_relays;
 SignerCaller *who;
 SignerOp op;
 SignerTransport transport;
 SignerRequestState state;
 gboolean resolved, no_identity, no_approval_ui, existing_pairing, announced, decision, remember;
 gboolean finished, caller_disconnected;
 gint cancelled; /* atomic: worker checks before touching the backend */
 guint64 generation, ttl, permission_revision;
 gint64 deadline, expires_at;
 GSource *timeout;
 gboolean has_kind;
 guint32 event_kind;
 GPtrArray *calls;
 SignerApprovalDecisionFn decision_fn;
 gpointer decision_data;
};
typedef struct { SignerGateReplyFn fn; gpointer ud; } Reply;
typedef struct { SignerOp op; gchar *a, *b, *selector; Reply reply; SignerTransport transport; } Call;
static void reply_error(const Reply *r, const char *name, const char *message) {
 r->fn(r->ud, name, message, NULL);
}
static void reply_value(const Reply *r, const char *result) {
 r->fn(r->ud, NULL, NULL, result);
}

static void backend_worker(gpointer data, gpointer unused);
static void permissions_changed(gpointer data);
static gboolean owner_ok(SignerApprovalService *s) { return g_thread_self() == s->owner; }
static void finish(Request *r, SignerRequestState state, const gchar *error, const gchar *message);
static void request_unref(gpointer data);
static GVariant *snapshot(Request *r);
static Request *request_ref(Request *r) { g_atomic_int_inc(&r->refs); return r; }
static void wipe_free(gchar *text) { if (text) { secure_wipe(text, strlen(text)); g_free(text); } }

gchar *signer_gate_handle_request_id(SignerGateHandle *h) {
  return h && h->request ? g_strdup(h->request->id) : NULL;
}
SignerGateHandle *signer_gate_handle_ref(SignerGateHandle *h) {
  if (h) g_atomic_int_inc(&h->refs);
  return h;
}
void signer_gate_handle_unref(SignerGateHandle *h) {
  if (h && g_atomic_int_dec_and_test(&h->refs)) g_free(h);
}
static void continuation_free(Continuation *c) {
  g_atomic_int_set(&c->handle->done, 1);
  c->handle->request = NULL; c->handle->continuation = NULL;
  signer_gate_handle_unref(c->handle);
  g_free(c->connection); g_free(c);
}
static void request_unref(gpointer data) {
  Request *r = data;
  if (!g_atomic_int_dec_and_test(&r->refs)) return;
  g_assert(r->calls->len == 0);
  if (r->timeout) { g_source_destroy(r->timeout); g_source_unref(r->timeout); }
  g_ptr_array_unref(r->calls);
  g_strfreev(r->pairing_relays);
  g_free(r->id); g_free(r->join_key); wipe_free(r->a); g_free(r->b);
  wipe_free(r->selector); g_free(r->npub); g_free(r->claimed);
  wipe_free(r->reason); wipe_free(r->purpose); g_free(r->flow_id); wipe_free(r->preview);
  SignerApprovalService *service = r->service;
  g_free(r->error_code); signer_caller_free(r->who); g_free(r);
  signer_approval_service_unref(service);
}
SignerApprovalService *signer_approval_service_new(GMainContext *context) {
  SignerApprovalService *s = g_new0(SignerApprovalService, 1);
  s->refs = 1; s->owner = g_thread_self();
  s->context = g_main_context_ref(context ? context : g_main_context_default());
  s->requests = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, request_unref);
  s->prompt_rates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  s->subscribers = g_ptr_array_new_with_free_func(g_free);
  s->backend = g_thread_pool_new(backend_worker, NULL, 1, FALSE, NULL);
  s->permissions = signer_permissions_new(NULL);
  signer_permissions_set_changed(s->permissions, permissions_changed, s);
  s->generation = 1;
  return s;
}
SignerApprovalService *signer_approval_service_ref(SignerApprovalService *s) {
  g_atomic_int_inc(&s->refs); return s;
}
void signer_approval_service_unref(SignerApprovalService *s) {
  if (!s || !g_atomic_int_dec_and_test(&s->refs)) return;
  g_assert(g_hash_table_size(s->requests) == 0);
  g_thread_pool_free(s->backend, FALSE, FALSE);
  g_hash_table_unref(s->requests); g_hash_table_unref(s->prompt_rates); g_ptr_array_unref(s->subscribers);
  signer_permissions_free(s->permissions);
  g_main_context_unref(s->context); g_free(s);
}
guint signer_approval_service_subscribe(SignerApprovalService *s, SignerApprovalNotifyFn fn, gpointer data) {
  g_return_val_if_fail(owner_ok(s) && fn, 0);
  Subscriber *sub = g_new0(Subscriber, 1);
  *sub = (Subscriber){++s->next_subscriber, fn, data};
  g_ptr_array_add(s->subscribers, sub); return sub->id;
}
void signer_approval_service_unsubscribe(SignerApprovalService *s, guint id) {
  g_return_if_fail(owner_ok(s));
  for (guint i = 0; i < s->subscribers->len; i++)
    if (((Subscriber *)s->subscribers->pdata[i])->id == id) { g_ptr_array_remove_index(s->subscribers, i); break; }
}
void signer_approval_service_set_changed(SignerApprovalService *s, void (*fn)(gpointer), gpointer data) {
  g_return_if_fail(owner_ok(s)); s->changed = fn; s->changed_data = data;
}
static void table_changed(SignerApprovalService *s) { if (s->changed) s->changed(s->changed_data); }
static void notify_request(Request *r) {
  SignerApprovalService *s = r->service;
  table_changed(s);
  g_autoptr(GVariant) info = snapshot(r);
  /* Copy IDs, not callbacks: an observer may unsubscribe itself or another. */
  GArray *ids = g_array_new(FALSE, FALSE, sizeof(guint));
  for (guint i = 0; i < s->subscribers->len; i++) {
    guint id = ((Subscriber *)s->subscribers->pdata[i])->id; g_array_append_val(ids, id);
  }
  for (guint i = 0; i < ids->len; i++) {
    guint id = g_array_index(ids, guint, i);
    for (guint j = 0; j < s->subscribers->len; j++) {
      Subscriber sub = *(Subscriber *)s->subscribers->pdata[j];
      if (sub.id == id) { sub.fn(sub.data, info); break; }
    }
  }
  g_array_unref(ids);
}
static const gchar *state_name(SignerRequestState state) {
  static const gchar *names[] = {"PENDING", "EXECUTING", "SUCCEEDED", "DENIED", "EXPIRED", "CANCELLED", "FAILED"};
  return names[state];
}
static const gchar *transport_name(SignerTransport transport) {
  static const gchar *names[] = {"DBUS", "NIP5F", "NIP46", "MANAGEMENT"};
  return names[transport];
}
/* Approval summaries expose public identifiers and bounded public text, never
 * arbitrary tag values or private event bodies. Omit ambiguous/unsafe fields. */
static gboolean summary_hex_id(const gchar *value) {
  if (!value || strlen(value) != 64) return FALSE;
  for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(value[i])) return FALSE;
  return TRUE;
}
static void summary_id(GVariantBuilder *summary, const gchar *key, const gchar *value) {
  if (!summary_hex_id(value)) return;
  g_autofree gchar *lower = g_ascii_strdown(value, -1);
  g_variant_builder_add(summary, "{sv}", key, g_variant_new_string(lower));
}
static gchar *summary_text(const gchar *text) {
  g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
  gchar *out = g_utf8_substring(valid, 0, MIN((glong)280, g_utf8_strlen(valid, -1)));
  for (gchar *p = out; *p; p++) if ((guchar)*p < 32 || *p == 127) *p = ' ';
  return out;
}
static gboolean summary_private_kind(guint32 kind) {
  return kind == NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE || kind == NOSTR_KIND_SEAL ||
         kind == NOSTR_KIND_GIFT_WRAP || kind == NOSTR_KIND_DIRECT_MESSAGE ||
         kind == NOSTR_KIND_FILE_MESSAGE || kind == NOSTR_KIND_NOSTR_CONNECT;
}
static void summary_event(GVariantBuilder *summary, Request *r) {
  NostrEvent *event = nostr_event_new();
  if (!event) return;
  if (nostr_event_deserialize(event, r->a) != 0) { nostr_event_free(event); return; }
  NostrTags *tags = nostr_event_get_tags(event);
  guint32 count = nostr_tags_size(tags), deletes = 0, relay_count = 0;
  const gchar *target = NULL, *peer = NULL, *relay = NULL, *target_kind = NULL;
  g_autoptr(GHashTable) counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint32 i = 0; i < count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *name = nostr_tag_get_key(tag), *value = nostr_tag_get_value(tag);
    if (!name) continue;
    if (!strcmp(name, "e")) target = value; /* Last e/p is the reaction target. */
    if (!strcmp(name, "p")) peer = value;
    if (!strcmp(name, "k")) target_kind = value;
    if (!strcmp(name, "e") || !strcmp(name, "a")) deletes++;
    if (!strcmp(name, "relay")) { relay = value; relay_count++; }
    /* Names only: exclude long identifiers, control characters and payloads
     * disguised as names; cap distinct names to keep the wire summary bounded. */
    gboolean safe = *name && strlen(name) <= 32;
    for (const gchar *p = name; safe && *p; p++)
      safe = g_ascii_isalnum(*p) || *p == '_' || *p == '-';
    if (safe && (g_hash_table_contains(counts, name) || g_hash_table_size(counts) < 64)) {
      guint n = GPOINTER_TO_UINT(g_hash_table_lookup(counts, name));
      g_hash_table_replace(counts, g_strdup(name), GUINT_TO_POINTER(n + 1));
    }
  }
  g_variant_builder_add(summary, "{sv}", "tag_count", g_variant_new_uint32(count));
  GVariantBuilder names;
  g_variant_builder_init(&names, G_VARIANT_TYPE("a{su}"));
  /* Sorted output keeps reconciliation equality stable. */
  GList *keys = g_hash_table_get_keys(counts);
  keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
  for (GList *key = keys; key; key = key->next)
    g_variant_builder_add(&names, "{su}", (gchar *)key->data,
                          GPOINTER_TO_UINT(g_hash_table_lookup(counts, key->data)));
  g_list_free(keys);
  g_variant_builder_add(summary, "{sv}", "tag_counts", g_variant_builder_end(&names));
  if (r->event_kind == NOSTR_KIND_DELETION)
    g_variant_builder_add(summary, "{sv}", "delete_target_count", g_variant_new_uint32(deletes));
  gboolean safe_relay = relay && strlen(relay) <= 2048;
  for (const gchar *p = relay; safe_relay && *p; p++)
    safe_relay = (guchar)*p > 32 && (guchar)*p < 127;
  if (r->event_kind == NOSTR_KIND_CLIENT_AUTHENTICATION && relay_count == 1 && safe_relay) {
    g_autoptr(GUri) uri = g_uri_parse(relay, G_URI_FLAGS_NONE, NULL);
    if (uri && g_uri_get_host(uri) && *g_uri_get_host(uri) &&
        (!g_strcmp0(g_uri_get_scheme(uri), "wss") || !g_strcmp0(g_uri_get_scheme(uri), "ws")) &&
        !g_uri_get_userinfo(uri) && !g_uri_get_query(uri) && !g_uri_get_fragment(uri))
      g_variant_builder_add(summary, "{sv}", "auth_relay", g_variant_new_string(relay));
  }
  gboolean repost = r->event_kind == NOSTR_KIND_REPOST || r->event_kind == NOSTR_KIND_GENERIC_REPOST;
  if (repost || r->event_kind == NOSTR_KIND_REACTION) {
    summary_id(summary, "target_event_id", target);
    guint64 number;
    if (target_kind && g_ascii_string_to_unsigned(target_kind, 10, 0, G_MAXUINT16, &number, NULL))
      g_variant_builder_add(summary, "{sv}", "target_kind", g_variant_new_uint32(number));
    if (r->event_kind == NOSTR_KIND_REACTION) {
      g_autofree gchar *reaction = summary_text(nostr_event_get_content(event));
      g_variant_builder_add(summary, "{sv}", "reaction_value", g_variant_new_string(reaction));
    }
  }
  if (summary_private_kind(r->event_kind)) summary_id(summary, "recipient_peer", peer);
  if (r->event_kind == NOSTR_KIND_TEXT_NOTE || r->event_kind == NOSTR_KIND_ARTICLE ||
      r->event_kind == NOSTR_KIND_COMMENT) {
    g_autofree gchar *content = summary_text(nostr_event_get_content(event));
    g_variant_builder_add(summary, "{sv}", "content", g_variant_new_string(content));
  }
  nostr_event_free(event);
}
/* Display-only wire scope hints. Unknown text/URLs/tokens are never echoed;
 * neither this list nor connect consent is a signing grant. */
static GVariant *connect_hints(const gchar *csv) {
  const gchar *names[] = {"connect", "get_public_key", "sign_event", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt", "ping", "get_relays", NULL};
  GVariantBuilder hints; g_variant_builder_init(&hints, G_VARIANT_TYPE("as"));
  g_auto(GStrv) parts = g_strsplit(csv ? csv : "", ",", 65);
  for (guint i = 0; parts[i] && i < 64; i++) {
    const gchar *part = g_strstrip(parts[i]);
    if (g_strv_contains(names, part)) g_variant_builder_add(&hints, "s", part);
    else if (g_str_has_prefix(part, "sign_event:") && g_ascii_isdigit(part[11])) {
      gchar *end = NULL; guint64 kind = g_ascii_strtoull(part + 11, &end, 10);
      if (end && !*end && kind <= 65535) {
        g_autofree gchar *scope = g_strdup_printf("sign_event:%u", (guint)kind);
        g_variant_builder_add(&hints, "s", scope);
      }
    }
  }
  return g_variant_builder_end(&hints);
}
static GVariant *snapshot(Request *r) {
  GVariantBuilder b, summary;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
#define PUT(k,v) g_variant_builder_add(&b, "{sv}", k, g_variant_new_string((v) ? (v) : ""))
  PUT("request_id", r->id); PUT("kind", op_info[r->op].kind);
  PUT("operation", r->op == OP_SIGN_EVENT ? "sign_event" : op_info[r->op].kind);
  PUT("principal", r->who->principal);
  PUT("principal_kind", r->who->unattested ? "claimed" : signer_caller_kind_to_string(r->who->kind));
  PUT("app_id", r->who->app_id); PUT("exe", r->who->exe); PUT("via", r->who->via);
  PUT("claimed_app_id", r->claimed); PUT("identity", r->npub); PUT("preview", r->preview);
  PUT("transport", transport_name(r->transport)); PUT("reason", r->reason);
  PUT("reason_source", r->reason && *r->reason ? "caller" : "signer");
  PUT("flow_id", r->flow_id); PUT("state", state_name(r->state)); PUT("error_code", r->error_code);
#undef PUT
  g_variant_builder_add(&b, "{sv}", "attested", g_variant_new_boolean(r->who->attested));
  g_variant_builder_add(&b, "{sv}", "verified", g_variant_new_boolean(!r->who->unattested && r->who->principal));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(r->who->principal != NULL));
  g_variant_builder_add(&b, "{sv}", "calls", g_variant_new_uint32(r->calls->len));
  g_variant_builder_add(&b, "{sv}", "approval_requested", g_variant_new_boolean(r->announced));
  g_variant_builder_add(&b, "{sv}", "explicit_consent", g_variant_new_boolean(r->announced && r->decision));
  g_variant_builder_add(&b, "{sv}", "remember", g_variant_new_boolean(r->remember));
  g_variant_builder_add(&b, "{sv}", "caller_disconnected", g_variant_new_boolean(r->caller_disconnected));
  g_variant_builder_add(&b, "{sv}", "expires_at", g_variant_new_uint64(r->expires_at));
  gint64 left = MAX((gint64)0, r->deadline - g_get_monotonic_time());
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32((left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC));
  if (r->has_kind) g_variant_builder_add(&b, "{sv}", "event_kind", g_variant_new_uint32(r->event_kind));
  g_variant_builder_add(&summary, "{sv}", "type", g_variant_new_string(r->op == OP_SIGN_EVENT ? "event" : op_info[r->op].kind));
  /* Preview helpers contain only public event content/operation/peer. */
  const gchar *preview = r->preview ? r->preview : "";
  if (r->op == OP_SIGN_EVENT && r->event_kind != NOSTR_KIND_TEXT_NOTE &&
      r->event_kind != NOSTR_KIND_ARTICLE && r->event_kind != NOSTR_KIND_COMMENT)
    preview = summary_private_kind(r->event_kind) ? "Encrypted message" : "Event signing";
  g_autofree gchar *safe_preview = summary_text(preview);
  g_variant_builder_add(&summary, "{sv}", "preview", g_variant_new_string(safe_preview));
  if (op_info[r->op].peer_arg && r->b)
    g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string(r->b));
  if (r->op == OP_CONVKEY)
    g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string(r->a));
  if (r->op == SIGNER_GATE_CONNECT) {
    g_variant_builder_add(&summary, "{sv}", "requested_permissions_hint", connect_hints(r->a));
    if (r->pairing_relays) g_variant_builder_add(&summary, "{sv}", "relays", g_variant_new_strv((const gchar *const *)r->pairing_relays, -1));
    g_variant_builder_add(&summary, "{sv}", "requested_permissions_source", g_variant_new_string("client-hints"));
  }
  if (r->op == OP_SIGN_EVENT) summary_event(&summary, r);
  g_variant_builder_add(&b, "{sv}", "event_summary", g_variant_builder_end(&summary));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
static void complete_call(Request *r, Continuation *c, const gchar *error, const gchar *message, const gchar *result) {
  (void)r;
  SignerGateReplyFn fn = c->reply; gpointer data = c->data;
  continuation_free(c); /* detach before a reentrant transport callback */
  fn(data, error, message, result);
}
static void finish(Request *r, SignerRequestState state, const gchar *error, const gchar *message) {
  if (r->finished) return;
  request_ref(r);
  r->finished = TRUE; r->state = state;
  r->error_code = g_strdup(error ? error : state == SIGNER_REQUEST_FAILED ? ORG_NOSTR_SIGNER_ERR_INTERNAL : "");
  if (state == SIGNER_REQUEST_CANCELLED || state == SIGNER_REQUEST_EXPIRED) g_atomic_int_set(&r->cancelled, 1);
  if (r->timeout) { g_source_destroy(r->timeout); g_clear_pointer(&r->timeout, g_source_unref); }
  g_hash_table_remove(r->service->requests, r->id);
  while (r->calls->len) {
    Continuation *c = g_ptr_array_steal_index(r->calls, 0);
    complete_call(r, c, error, message, NULL);
  }
  notify_request(r);
  if (r->decision_fn) {
    SignerApprovalDecisionFn fn = r->decision_fn; r->decision_fn = NULL;
    fn(r->decision_data, state == SIGNER_REQUEST_SUCCEEDED || (state == SIGNER_REQUEST_DENIED && !r->decision), NULL, NULL);
  }
  request_unref(r);
}
void signer_gate_cancel(SignerGateHandle *h) {
  if (!h || !h->request) return;
  g_return_if_fail(owner_ok(h->request->service));
  Request *r = request_ref(h->request);
  Continuation *c = h->continuation;
  g_ptr_array_remove(r->calls, c);
  complete_call(r, c, ORG_NOSTR_SIGNER_ERR_CANCELLED, "request cancelled", NULL);
  if (!r->calls->len) finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "request cancelled");
  else table_changed(r->service);
  request_unref(r);
}

/* ---- grants ------------------------------------------------------------- */

/* Built-in grants for first-party services that run without a UI (the
 * nostr-homed session helpers read the npub/relays and open the secrets
 * envelope at login). Matched on the caller's executable by path AND
 * device/inode, for unsandboxed same-user callers only. This returns a
 * candidate default to the single evaluator; explicit denial takes precedence.
 * NIP55L_DEFAULT_GRANTS: "<kind>[,<kind>...]@<exe>[:<exe>...]" groups
 * separated by '+'. */
#ifndef NIP55L_DEFAULT_GRANTS
#define NIP55L_DEFAULT_GRANTS ""
#endif
static gboolean grants_lookup_default(const char *kind, const SignerCaller *who){
  if (!who || !who->same_uid || !who->exe || who->exe_ino == 0) return FALSE;
  if (who->kind != SIGNER_CALLER_EXE && who->kind != SIGNER_CALLER_SYSTEMD_SCOPE) return FALSE;
  gboolean hit = FALSE;
  const gchar *defaults = NIP55L_DEFAULT_GRANTS;
#ifdef NIP55L_TEST_TRUST_ENV
  const gchar *test_defaults = g_getenv("NOSTR_SIGNER_TEST_DEFAULT_GRANTS");
  if (test_defaults) defaults = test_defaults;
#endif
  g_auto(GStrv) groups = g_strsplit(defaults, "+", -1);
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
  if (kind == NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE || kind == NOSTR_KIND_SEAL ||
      kind == NOSTR_KIND_DIRECT_MESSAGE || kind == NOSTR_KIND_FILE_MESSAGE ||
      kind == NOSTR_KIND_GIFT_WRAP || kind == NOSTR_KIND_NOSTR_CONNECT) content = NULL;
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
    case OP_NIP04_ENCRYPT: return g_strdup_printf("encrypt a message for %s using legacy NIP-04 (no message authentication)", b);
    case OP_NIP44_ENCRYPT:
    case OP_NIP44_ENCRYPT_B64: return g_strdup_printf("encrypt a message for %s", b);
    case OP_NIP04_DECRYPT: return g_strdup_printf("decrypt a message from %s using legacy NIP-04 (no message authentication)", b);
    case OP_NIP44_DECRYPT:
    case OP_NIP44_DECRYPT_B64: return g_strdup_printf("decrypt a message from %s", b);
    case OP_DECRYPT_ZAP:       return g_strdup("decrypt a private zap");
    case SIGNER_GATE_CONNECT: return g_strdup("connect a remote client");
    case SIGNER_GATE_PING: return g_strdup("check signer availability");
    case OP_COUNT: break;
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
      rc = nostr_nip55l_resolve_npub(c->selector, &out);
      if (rc != 0 || !out) { free(out); rc = rc ? rc : NOSTR_SIGNER_ERROR_BACKEND; return_no_key(inv, c->op, rc); return rc; }
      return_string(inv, out, FALSE);
      return 0;
    case OP_GET_RELAYS:
      if (c->transport == SIGNER_TRANSPORT_NIP46) {
        /* Remote transport supplies its validated configured relay list; it
         * must not read the local NIP-07 relay file or fetch from the network. */
        rc = nostr_nip55l_relays_normalize_json(c->a, c->a ? strlen(c->a) : 0, &out);
        if (rc || !out) { free(out); reply_error(inv, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT, "invalid configured relays"); return rc; }
        return_string(inv, out, FALSE); return 0;
      }
      return perform_get_relays(inv);
    case OP_NIP04_ENCRYPT:     rc = nostr_nip55l_nip04_encrypt(c->a, c->b, c->selector, &out);     emsg = "nip04 encrypt failed"; break;
    case OP_NIP04_DECRYPT:     rc = nostr_nip55l_nip04_decrypt(c->a, c->b, c->selector, &out);     emsg = "nip04 decrypt failed"; break;
    case OP_NIP44_ENCRYPT:     rc = nostr_nip55l_nip44_encrypt(c->a, c->b, c->selector, &out);     emsg = "nip44 encrypt failed"; break;
    case OP_NIP44_DECRYPT:     rc = nostr_nip55l_nip44_decrypt(c->a, c->b, c->selector, &out);     emsg = "nip44 decrypt failed"; break;
    case OP_NIP44_ENCRYPT_B64: rc = nostr_nip55l_nip44_encrypt_b64(c->a, c->b, c->selector, &out); emsg = "nip44 encrypt failed"; break;
    case OP_NIP44_DECRYPT_B64: rc = nostr_nip55l_nip44_decrypt_b64(c->a, c->b, c->selector, &out); emsg = "nip44 decrypt failed"; break;
    case OP_DECRYPT_ZAP:       rc = nostr_nip55l_decrypt_zap_event(c->a, c->selector, &out);       emsg = "zap decrypt failed"; break;
    case SIGNER_GATE_CONNECT: reply_value(inv, "ack"); return 0;
    case SIGNER_GATE_PING: reply_value(inv, "pong"); return 0;
    case OP_COUNT: rc = NOSTR_SIGNER_ERROR_INVALID_ARG; emsg = "unknown operation"; break;
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

#define PENDING_MAX 64
#define PENDING_PER_CALLER_MAX 8
#define PENDING_CALLS_MAX 256
#define ARG_MAX_BYTES (1024 * 1024)
static guint pending_ttl_s(void) {
#ifdef NIP55L_TEST_TRUST_ENV
  const gchar *env = g_getenv("NOSTR_SIGNER_TEST_PENDING_TTL_S");
  guint64 v = env ? g_ascii_strtoull(env, NULL, 10) : 0;
  if (v > 0 && v < 300) return v;
#endif
  return 300;
}
static gboolean is_hex64(const gchar *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (const gchar *p = s; *p; p++) if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}
static gboolean bounded_text(const gchar *text, gsize bytes, glong chars) {
  return !text || (strlen(text) <= bytes && g_utf8_validate(text, -1, NULL) &&
                   (chars < 0 || g_utf8_strlen(text, -1) <= chars));
}
static gchar *new_id(void) {
  guint8 bytes[32];
  if (RAND_bytes(bytes, sizeof bytes) != 1) return NULL;
  gchar *id = g_malloc(65);
  for (guint i = 0; i < sizeof bytes; i++) g_snprintf(id + i * 2, 3, "%02x", bytes[i]);
  return id;
}
/* Length-delimited components, including NULL vs empty, avoid ambiguous
 * concatenations. Canonical event JSON and lowercase peer keys enter here. */
static void digest_part(GChecksum *sum, const gchar *text) {
  guint64 size = text ? strlen(text) + 1 : 0;
  guint64 encoded = GUINT64_TO_BE(size);
  g_checksum_update(sum, (const guchar *)&encoded, sizeof encoded);
  if (text) g_checksum_update(sum, (const guchar *)text, size - 1);
}
static gchar *join_key(Request *r) {
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  digest_part(sum, r->who->principal ? r->who->principal : r->who->sender);
  digest_part(sum, r->npub); digest_part(sum, op_info[r->op].kind);
  /* B64 vs UTF-8 is not the same operation despite sharing a grant section. */
  guint32 op = GUINT32_TO_BE(r->op);
  g_checksum_update(sum, (const guchar *)&op, sizeof op);
  digest_part(sum, r->a); digest_part(sum, r->b);
  gchar *key = g_strdup(g_checksum_get_string(sum)); g_checksum_free(sum); return key;
}
static gboolean timeout_request(gpointer data) {
  Request *r = data;
  finish(r, SIGNER_REQUEST_EXPIRED, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT, "approval timed out");
  return G_SOURCE_REMOVE;
}
static void set_timeout(Request *r) {
  if (r->timeout) { g_source_destroy(r->timeout); g_source_unref(r->timeout); }
  r->timeout = g_timeout_source_new(0);
  g_source_set_ready_time(r->timeout, r->deadline);
  g_source_set_callback(r->timeout, timeout_request, request_ref(r), request_unref);
  g_source_attach(r->timeout, r->service->context);
}

typedef struct { SignerGateHandle *handle; gchar *result, *error, *message; } Result;
typedef struct {
  Request *request;
  SignerApprovalService *service;
  gboolean resolve;
  gchar *npub, *selector;
  gint rc;
  guint64 permission_revision;
  gboolean stale_policy;
  GPtrArray *results;
#ifdef NIP55L_TEST_TRUST_ENV
  void (*after_backend)(gpointer);
  gpointer after_backend_data;
#endif
} Job;
static void capture(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  Result *r = data;
  r->error = g_strdup(error); r->message = g_strdup(message); r->result = g_strdup(result);
}
static void result_free(gpointer data) {
  Result *r = data;
  signer_gate_handle_unref(r->handle); wipe_free(r->result);
  g_free(r->error); g_free(r->message); g_free(r);
}
static void job_free(gpointer data) {
  Job *j = data;
  request_unref(j->request); signer_approval_service_unref(j->service);
  g_free(j->npub); wipe_free(j->selector); g_ptr_array_unref(j->results); g_free(j);
}
static void queue_job(Request *r, gboolean resolve);
static GPtrArray *requests_copy(SignerApprovalService *s);
static SignerPermissionResult request_policy(Request *r) {
  SignerPermissionResult policy = signer_permissions_evaluate(r->service->permissions, r->who->principal,
      r->npub, op_info[r->op].kind, r->has_kind ? (gint)r->event_kind : -1,
      grants_lookup_default(op_info[r->op].kind, r->who), NULL);
  /* Reconnecting an explicitly paired client is not new pairing consent.
   * Explicit deny / persistence errors still win; no signing scope is granted. */
  if (policy == SIGNER_PERMISSION_ASK && r->op == SIGNER_GATE_CONNECT && r->existing_pairing)
    return SIGNER_PERMISSION_ALLOW;
  return policy;
}
/* A canonical remote public selector is enough to reject an ASK in headless
 * mode. This is negative-only: ALLOW still resolves/pins the real backend key
 * and re-evaluates policy before execution. No keyring unlock for a denial. */
static gboolean headless_reject_before_backend(Request *r) {
  if (!(r->no_approval_ui || r->service->no_approval_ui) ||
      r->transport != SIGNER_TRANSPORT_NIP46 || r->who->kind != SIGNER_CALLER_NIP46)
    return FALSE;
  guint8 pubkey[32]; char *npub = NULL;
  gboolean valid = is_hex64(r->selector) ? nostr_hex2bin(pubkey, r->selector, sizeof pubkey) :
      nostr_nip19_decode_npub(r->selector, pubkey) == 0;
  if (!valid || nostr_nip19_encode_npub(pubkey, &npub) || !npub) { free(npub); return FALSE; }
  r->npub = g_strdup(npub); free(npub);
  SignerPermissionResult policy = request_policy(r);
  if (r->finished) return TRUE;
  if (policy == SIGNER_PERMISSION_ALLOW) { g_clear_pointer(&r->npub, g_free); return FALSE; }
  if (policy == SIGNER_PERMISSION_ASK)
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_NO_APPROVER, "approval UI disabled");
  else if (policy == SIGNER_PERMISSION_DENY)
    finish(r, SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy");
  else finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot load permissions");
  return TRUE;
}
static gboolean policy_denies(Request *r) {
  SignerPermissionResult policy = request_policy(r);
  return policy == SIGNER_PERMISSION_DENY || policy == SIGNER_PERMISSION_ERROR;
}
static gboolean remember_permission(Request *r, gboolean allow) {
  return signer_permissions_remember(r->service->permissions, r->who->principal,
      r->npub, op_info[r->op].kind, r->has_kind ? (gint)r->event_kind : -1, allow, r->ttl, NULL);
}
static void execute(Request *r) {
  if (r->finished) return;
  if (r->service->paused || r->generation != r->service->generation) {
    finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_PAUSED, "signing is paused"); return;
  }
  if (g_get_monotonic_time() >= r->deadline) {
    finish(r, SIGNER_REQUEST_EXPIRED, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT, "approval timed out"); return;
  }
  SignerPermissionResult policy = request_policy(r);
  r->permission_revision = signer_permissions_revision(r->service->permissions);
  if (r->finished) return;
  if (policy == SIGNER_PERMISSION_ERROR) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot load permissions"); return;
  }
  if (policy == SIGNER_PERMISSION_DENY) {
    finish(r, SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy"); return;
  }
  if (!r->decision && policy != SIGNER_PERMISSION_ALLOW) {
    finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "permissions changed before execution"); return;
  }
  r->state = SIGNER_REQUEST_EXECUTING;
  queue_job(r, FALSE);
  table_changed(r->service);
}
static void resolved(Request *r) {
  SignerApprovalService *s = r->service;
  if (r->finished) return;
  r->resolved = TRUE;
  if (s->paused || r->generation != s->generation) {
    finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_PAUSED, "signing is paused"); return;
  }
  SignerPermissionResult policy = request_policy(r);
  if (r->finished) return; /* a reload can invalidate this request */
  if (policy == SIGNER_PERMISSION_ERROR) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot load permissions"); return;
  }
  if (policy != SIGNER_PERMISSION_ASK) {
    if (policy == SIGNER_PERMISSION_ALLOW) execute(r);
    else finish(r, SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy");
    return;
  }
  if (r->no_approval_ui || s->no_approval_ui) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_NO_APPROVER, "approval UI disabled"); return;
  }
  r->join_key = join_key(r);
  GHashTableIter it; gpointer value;
  g_hash_table_iter_init(&it, s->requests);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Request *join = value;
    if (join == r || !join->announced || join->state != SIGNER_REQUEST_PENDING ||
        g_strcmp0(join->join_key, r->join_key)) continue;
    if (join->calls->len >= PENDING_CALLS_MAX) {
      finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "too many calls awaiting approval"); return;
    }
    Continuation *c = g_ptr_array_steal_index(r->calls, 0);
    c->handle->request = join; g_ptr_array_add(join->calls, c);
    if (r->deadline < join->deadline) {
      join->deadline = r->deadline; join->expires_at = r->expires_at; set_timeout(join);
    }
    r->finished = TRUE;
    if (r->timeout) { g_source_destroy(r->timeout); g_clear_pointer(&r->timeout, g_source_unref); }
    g_hash_table_remove(s->requests, r->id);
    table_changed(s);
    return;
  }
  guint mine = 0, total = 0;
  g_hash_table_iter_init(&it, s->requests);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Request *other = value;
    if (other == r || !other->announced || other->state != SIGNER_REQUEST_PENDING) continue;
    total++;
    if (r->who->principal ? !g_strcmp0(r->who->principal, other->who->principal) :
        (!other->who->principal && !g_strcmp0(r->who->sender, other->who->sender))) mine++;
  }
  if (mine >= PENDING_PER_CALLER_MAX || total >= PENDING_MAX) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_RATELIMIT, mine >= PENDING_PER_CALLER_MAX ?
      "too many of this application's requests are awaiting approval" : "too many requests awaiting approval"); return;
  }
  gint64 now = g_get_monotonic_time();
  gint64 *last = g_hash_table_lookup(s->prompt_rates, r->who->sender);
  if (last && now - *last < 100 * 1000) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "rate limited"); return;
  }
  gint64 *stamp = g_new(gint64, 1); *stamp = now;
  g_hash_table_replace(s->prompt_rates, g_strdup(r->who->sender), stamp);
  r->preview = build_preview(r->op, r->a, r->b);
  r->announced = TRUE;
  notify_request(r);
}
static gboolean job_done(gpointer data) {
  Job *j = data; Request *r = j->request;
  if (r->finished) return G_SOURCE_REMOVE;
  if (r->service->paused || r->generation != r->service->generation) {
    finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_PAUSED, "signing is paused"); return G_SOURCE_REMOVE;
  }
  if (j->resolve) {
    if (j->rc || !j->npub || !j->selector) {
      const gchar *error = j->rc == NOSTR_SIGNER_ERROR_INVALID_ARG ? ORG_NOSTR_SIGNER_ERR_INVALID_INPUT :
        (j->rc == NOSTR_SIGNER_ERROR_NOT_FOUND || j->rc == NOSTR_SIGNER_ERROR_INVALID_KEY) ? ORG_NOSTR_SIGNER_ERR_NO_KEY : ORG_NOSTR_SIGNER_ERR_INTERNAL;
      finish(r, SIGNER_REQUEST_FAILED, error, j->rc == NOSTR_SIGNER_ERROR_INVALID_ARG ?
        "identity must name a stored identity (npub, hex public key, key_id or label), not a secret key" : "no key configured for this identity");
    } else {
      r->npub = g_strdup(j->npub); wipe_free(r->selector); r->selector = g_strdup(j->npub);
      resolved(r);
    }
    return G_SOURCE_REMOVE;
  }
  if (policy_denies(r)) {
    finish(r, SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy"); return G_SOURCE_REMOVE;
  }
  if (!r->decision && request_policy(r) != SIGNER_PERMISSION_ALLOW) {
    finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "permissions changed before execution"); return G_SOURCE_REMOVE;
  }
  if (j->stale_policy) {
    /* Automatic admission has one continuation (joining happens only while
     * awaiting explicit consent). The worker stopped before perform: recheck
     * on the owner thread, then re-admit without duplicating cryptography. */
    g_assert(!r->decision && j->results->len == 1);
    execute(r); return G_SOURCE_REMOVE;
  }
  gboolean ok = TRUE;
  for (guint i = 0; i < j->results->len; i++) {
    Result *out = j->results->pdata[i];
    if (out->handle->request == r && out->error) ok = FALSE;
  }
  if (ok && r->remember && r->who->principal &&
      !remember_permission(r, TRUE)) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot save remembered permission"); return G_SOURCE_REMOVE;
  }
  for (guint i = 0; i < j->results->len && !r->finished; i++) {
    Result *out = j->results->pdata[i];
    if (out->handle->request != r) continue;
    Continuation *c = out->handle->continuation;
    g_ptr_array_remove(r->calls, c);
    complete_call(r, c, out->error, out->message, out->result);
    /* A transport callback can pause/revoke while delivering a joined reply. */
    if (!r->finished && (r->generation != r->service->generation || r->service->paused))
      finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_PAUSED, "signing is paused");
  }
  if (!r->finished) finish(r, ok ? SIGNER_REQUEST_SUCCEEDED : SIGNER_REQUEST_FAILED, NULL, NULL);
  return G_SOURCE_REMOVE;
}
static GMutex backend_lock;
static void backend_worker(gpointer data, gpointer unused) {
  (void)unused;
  Job *j = data; Request *r = j->request;
  /* Also serialise old-export workers against a replacement instance. */
  g_mutex_lock(&backend_lock);
  if (!g_atomic_int_get(&r->cancelled)) {
    if (j->resolve) {
      char *npub = NULL, *selector = NULL;
      j->rc = nostr_nip55l_normalize_selector(r->selector, &selector, &npub);
      j->npub = g_strdup(npub); j->selector = g_strdup(selector);
      free(npub); if (selector) { secure_wipe(selector, strlen(selector)); free(selector); }
    } else {
      for (guint i = 0; i < j->results->len; i++) {
        if (!r->decision && !signer_permissions_revision_is_current(j->service->permissions, j->permission_revision)) {
          j->stale_policy = TRUE; break;
        }
        Result *out = j->results->pdata[i];
        if (g_atomic_int_get(&r->cancelled)) break;
        if (g_atomic_int_get(&out->handle->done)) continue;
        if (r->npub) {
          char *npub = NULL;
          gint rc = nostr_nip55l_resolve_npub(r->selector, &npub);
          gboolean same = rc == 0 && npub && !strcmp(npub, r->npub);
          free(npub);
          if (!same) {
            capture(out, rc && rc != NOSTR_SIGNER_ERROR_NOT_FOUND ? ORG_NOSTR_SIGNER_ERR_INTERNAL : ORG_NOSTR_SIGNER_ERR_IDENTITY_CHANGED,
                    "the identity changed while awaiting approval", NULL); continue;
          }
        }
        if (g_atomic_int_get(&r->cancelled)) break;
        if (g_atomic_int_get(&out->handle->done)) continue;
        if (!r->decision && !signer_permissions_revision_is_current(j->service->permissions, j->permission_revision)) {
          j->stale_policy = TRUE; break;
        }
        Call call = {r->op, r->a, r->b, r->selector, {capture, out}, r->transport};
        perform(&call, r->claimed);
#ifdef NIP55L_TEST_TRUST_ENV
        if (j->after_backend) j->after_backend(j->after_backend_data);
#endif
      }
    }
  }
  g_mutex_unlock(&backend_lock);
  /* Always attach a source: invoke_full can run inline on the worker when
   * the context is temporarily unowned, violating the owner-thread rule. */
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, job_done, j, job_free);
  g_source_attach(source, j->service->context); g_source_unref(source);
}
static void queue_job(Request *r, gboolean resolve) {
  Job *j = g_new0(Job, 1);
  j->request = request_ref(r); j->service = signer_approval_service_ref(r->service); j->resolve = resolve;
  j->results = g_ptr_array_new_with_free_func(result_free);
  j->permission_revision = r->permission_revision;
#ifdef NIP55L_TEST_TRUST_ENV
  j->after_backend = r->service->after_backend; j->after_backend_data = r->service->after_backend_data;
#endif
  if (!resolve) for (guint i = 0; i < r->calls->len; i++) {
    Result *out = g_new0(Result, 1);
    out->handle = signer_gate_handle_ref(((Continuation *)r->calls->pdata[i])->handle);
    g_ptr_array_add(j->results, out);
  }
  g_thread_pool_push(r->service->backend, j, NULL);
}
void signer_approval_service_set_no_approval_ui(SignerApprovalService *s, gboolean disabled) {
  g_return_if_fail(owner_ok(s)); s->no_approval_ui = disabled;
}
SignerGateHandle *signer_approval_service_submit(SignerApprovalService *s,
    const SignerCaller *who, const gchar *conn_key, SignerGateOp op,
    const gchar *a, const gchar *b, const gchar *selector,
    const SignerGateContext *context, SignerGateReplyFn reply, gpointer data) {
  g_return_val_if_fail(owner_ok(s) && who && conn_key && reply, NULL);
  SignerGateHandle *handle = g_new0(SignerGateHandle, 1); handle->refs = 1;
  const gchar *error = NULL, *message = "invalid operation arguments";
  if (s->stopped) { error = ORG_NOSTR_SIGNER_ERR_CANCELLED; message = "signer stopped"; }
  else if (op < 0 || op >= OP_COUNT || !bounded_text(a, ARG_MAX_BYTES, -1) ||
      !bounded_text(b, 128, -1) || !bounded_text(selector, 4096, -1) ||
      (context && (!bounded_text(context->reason, 512, 160) || !bounded_text(context->purpose, 64, -1) ||
                   !bounded_text(context->flow_id, 64, -1) || !bounded_text(context->claimed_name, 4096, -1) ||
                   context->transport < SIGNER_TRANSPORT_DBUS || context->transport > SIGNER_TRANSPORT_MANAGEMENT))) error = ORG_NOSTR_SIGNER_ERR_INVALID_INPUT;
  else if ((op_info[op].peer_arg && !is_hex64(b)) || (op == OP_CONVKEY && !is_hex64(a)) ||
      (context && context->existing_pairing && (op != SIGNER_GATE_CONNECT || context->transport != SIGNER_TRANSPORT_NIP46 || who->kind != SIGNER_CALLER_NIP46)) ||
      (context && context->no_identity && ((op != SIGNER_GATE_CONNECT && op != SIGNER_GATE_PING) || (selector && *selector))) ||
      ((op_info[op].peer_arg || op == OP_DECRYPT_ZAP || op == OP_SIGN_EVENT) && !a)) error = ORG_NOSTR_SIGNER_ERR_INVALID_INPUT;
  else if (s->paused) { error = ORG_NOSTR_SIGNER_ERR_PAUSED; message = "signing is paused"; }
  if (error) { reply(data, error, message, NULL); return handle; }
  Request *r = g_new0(Request, 1);
  r->refs = 1; r->service = signer_approval_service_ref(s); r->id = new_id(); r->who = signer_caller_copy(who);
  /* Unidentified callers can join only within their own connection. */
  g_free(r->who->sender); r->who->sender = g_strdup(conn_key);
  r->op = op; r->a = g_strdup(a); r->b = b ? g_ascii_strdown(b, -1) : NULL;
  if (op == OP_CONVKEY) { g_free(r->a); r->a = g_ascii_strdown(a, -1); }
  r->selector = g_strdup(selector ? selector : "");
  r->transport = context ? context->transport : SIGNER_TRANSPORT_NIP5F;
  r->no_identity = !op_info[op].has_identity || (context && context->no_identity);
  r->no_approval_ui = context && context->no_approval_ui;
  r->existing_pairing = context && context->existing_pairing;
  r->claimed = g_strdup(context ? context->claimed_name : "");
  if (context && context->pairing_relays && op == SIGNER_GATE_CONNECT && context->transport == SIGNER_TRANSPORT_NIP46)
    r->pairing_relays = g_strdupv((gchar **)context->pairing_relays);
  r->reason = g_strdup(context ? context->reason : ""); r->purpose = g_strdup(context ? context->purpose : "");
  r->flow_id = g_strdup(context ? context->flow_id : ""); r->generation = s->generation;
  r->calls = g_ptr_array_new(); r->state = SIGNER_REQUEST_PENDING;
  Continuation *c = g_new0(Continuation, 1);
  *c = (Continuation){signer_gate_handle_ref(handle), g_strdup(conn_key), reply, data};
  handle->request = r; handle->continuation = c; g_ptr_array_add(r->calls, c);
  if (!r->id) {
    complete_call(r, g_ptr_array_steal_index(r->calls, 0), ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot generate request ID", NULL);
    request_unref(r); return handle;
  }
  g_hash_table_insert(s->requests, r->id, r);
  request_ref(r); /* observers may cancel during the synchronous no-identity path */
  if (op == OP_SIGN_EVENT) {
    NostrEvent *event = nostr_event_new(); char *kind = NULL;
    gboolean valid = event && nostr_event_deserialize(event, a) == 0 && nostr_json_get_raw(a, "kind", &kind) == 0;
    gchar *end = NULL;
    guint64 value = kind ? g_ascii_strtoull(kind, &end, 10) : 0;
    valid = valid && kind && g_ascii_isdigit(kind[0]) && end && !*end && value <= 65535;
    char *canonical = valid ? nostr_event_serialize(event) : NULL;
    if (event) nostr_event_free(event);
    free(kind);
    if (!canonical) {
      finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT, "event JSON is not a valid Nostr event");
      request_unref(r); return handle;
    }
    wipe_free(r->a); r->a = g_strdup(canonical); free(canonical);
    r->has_kind = TRUE; r->event_kind = value;
  }
  if (g_hash_table_size(s->requests) > PENDING_CALLS_MAX) {
    finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_RATELIMIT, "too many requests awaiting approval");
  } else {
    gint64 now = g_get_monotonic_time();
    r->deadline = now + (gint64)pending_ttl_s() * G_USEC_PER_SEC;
    if (context && context->deadline > 0) r->deadline = MIN(r->deadline, context->deadline);
    r->expires_at = g_get_real_time() / G_USEC_PER_SEC + MAX((gint64)0, r->deadline - now) / G_USEC_PER_SEC;
    if (r->deadline <= now) finish(r, SIGNER_REQUEST_EXPIRED, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT, "approval timed out");
    else {
      set_timeout(r);
      if (r->no_identity) resolved(r);
      else if (!headless_reject_before_backend(r)) queue_job(r, TRUE);
    }
  }
  request_unref(r); return handle;
}
/* A persisted deny takes effect before returning the decision, including
 * for jobs already admitted to the worker queue. Mark the whole affected
 * set before invoking transport callbacks (which can re-enter the service).
 * The worker's atomic cancellation checks precede identity/key use. */
static void permissions_changed(gpointer data) {
  SignerApprovalService *s = data;
  table_changed(s);
  g_autoptr(GPtrArray) affected = requests_copy(s);
  for (guint i = affected->len; i > 0; i--) {
    Request *r = affected->pdata[i - 1];
    if (!r->resolved || r->finished || !policy_denies(r))
      g_ptr_array_remove_index(affected, i - 1);
    else g_atomic_int_set(&r->cancelled, TRUE);
  }
  for (guint i = 0; i < affected->len; i++)
    finish(affected->pdata[i], SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "denied by policy");
}
void signer_approval_service_decide(SignerApprovalService *s, const gchar *id,
    gboolean allow, gboolean remember, guint64 ttl, SignerApprovalDecisionFn fn, gpointer data) {
  g_return_if_fail(owner_ok(s) && fn);
  Request *r = g_hash_table_lookup(s->requests, id);
  if (!r || !r->announced || r->state != SIGNER_REQUEST_PENDING) {
    fn(data, FALSE, ORG_NOSTR_SIGNER_ERR_NOT_FOUND, "This request expired; ask the app to try again"); return;
  }
  request_ref(r);
  r->decision = allow; r->remember = remember; r->ttl = ttl; r->decision_fn = fn; r->decision_data = data;
  if (g_get_monotonic_time() >= r->deadline) finish(r, SIGNER_REQUEST_EXPIRED, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT, "approval timed out");
  else if (allow) execute(r);
  else {
    gboolean saved = remember && r->who->principal;
    if (saved && !remember_permission(r, FALSE))
      finish(r, SIGNER_REQUEST_FAILED, ORG_NOSTR_SIGNER_ERR_INTERNAL, "cannot save remembered permission");
    else {
      finish(r, SIGNER_REQUEST_DENIED, ORG_NOSTR_SIGNER_ERR_APPROVAL, "user denied");
    }
  }
  request_unref(r);
}
GVariant *signer_approval_service_info(SignerApprovalService *s, const gchar *id) {
  g_return_val_if_fail(owner_ok(s), NULL);
  Request *r = g_hash_table_lookup(s->requests, id);
  return r && r->announced && r->state == SIGNER_REQUEST_PENDING ? snapshot(r) : NULL;
}
GVariant *signer_approval_service_list_pending(SignerApprovalService *s) {
  g_return_val_if_fail(owner_ok(s), NULL);
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a(sssss)"));
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->requests);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Request *r = value;
    if (r->announced && r->state == SIGNER_REQUEST_PENDING)
      g_variant_builder_add(&b, "(sssss)", r->who->principal ? r->who->principal : "", r->npub ? r->npub : "",
                            op_info[r->op].kind, r->preview ? r->preview : "", r->id);
  }
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
GVariant *signer_approval_service_list_grants(SignerApprovalService *s) {
  g_return_val_if_fail(owner_ok(s), NULL);
  return signer_permissions_list_legacy(s->permissions, NULL);
}
gboolean signer_approval_service_revoke_grant(SignerApprovalService *s,
    const gchar *kind, const gchar *principal, const gchar *identity, gboolean *removed) {
  g_return_val_if_fail(owner_ok(s), FALSE);
  gboolean ok = signer_permissions_clear(s->permissions, principal, identity, kind, removed, NULL);
  if (ok && *removed) {
    g_autoptr(GPtrArray) affected = requests_copy(s);
    for (guint i = affected->len; i > 0; i--) {
      Request *r = affected->pdata[i - 1];
      if (!r->resolved || r->finished || g_strcmp0(r->who->principal, principal) ||
          (kind && strcmp(op_info[r->op].kind, kind)) ||
          (identity && strcmp(identity, "*") && g_strcmp0(r->npub ? r->npub : "", identity)))
        g_ptr_array_remove_index(affected, i - 1);
      else g_atomic_int_set(&r->cancelled, TRUE);
    }
    for (guint i = 0; i < affected->len; i++)
      finish(affected->pdata[i], SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "permission revoked");
  }
  return ok;
}
guint signer_approval_service_pending_count(SignerApprovalService *s) {
  g_return_val_if_fail(owner_ok(s), 0);
  return g_hash_table_size(s->requests);
}
static GPtrArray *requests_copy(SignerApprovalService *s) {
  GPtrArray *requests = g_ptr_array_new_with_free_func(request_unref);
  GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->requests);
  while (g_hash_table_iter_next(&it, NULL, &value)) g_ptr_array_add(requests, request_ref(value));
  return requests;
}
void signer_approval_service_prune_connections(SignerApprovalService *s,
    gboolean (*alive)(const gchar *, gpointer), gpointer data) {
  g_return_if_fail(owner_ok(s));
  g_autoptr(GHashTable) keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GPtrArray) requests = requests_copy(s);
  for (guint i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    for (guint j = 0; j < r->calls->len; j++)
      g_hash_table_add(keys, g_strdup(((Continuation *)r->calls->pdata[j])->connection));
  }
  GHashTableIter it; gpointer key; g_hash_table_iter_init(&it, keys);
  while (g_hash_table_iter_next(&it, &key, NULL))
    if (!alive(key, data)) signer_approval_service_connection_closed(s, key);
}
void signer_approval_service_connection_closed(SignerApprovalService *s, const gchar *key) {
  g_return_if_fail(owner_ok(s));
  g_hash_table_remove(s->prompt_rates, key);
  g_autoptr(GPtrArray) requests = requests_copy(s);
  g_autoptr(GPtrArray) handles = g_ptr_array_new_with_free_func((GDestroyNotify)signer_gate_handle_unref);
  for (guint i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    for (guint j = 0; j < r->calls->len; j++) {
      Continuation *c = r->calls->pdata[j];
      if (!g_strcmp0(c->connection, key)) g_ptr_array_add(handles, signer_gate_handle_ref(c->handle));
    }
  }
  for (guint i = 0; i < handles->len; i++) {
    SignerGateHandle *h = handles->pdata[i];
    if (h->request && h->request->calls->len == 1) h->request->caller_disconnected = TRUE;
    signer_gate_cancel(h);
  }
}
void signer_approval_service_cancel_principal(SignerApprovalService *s, const gchar *principal) {
  g_return_if_fail(owner_ok(s));
  g_autoptr(GPtrArray) requests = requests_copy(s);
  for (guint i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    if (!g_strcmp0(r->who->principal, principal))
      finish(r, SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "client revoked");
  }
}
void signer_approval_service_fail_request(SignerApprovalService *s, const gchar *id,
                                         const gchar *error, const gchar *message) {
  g_return_if_fail(owner_ok(s));
  Request *r = g_hash_table_lookup(s->requests, id);
  if (r && r->state == SIGNER_REQUEST_PENDING) finish(r, SIGNER_REQUEST_FAILED, error, message);
}
void signer_approval_service_set_paused(SignerApprovalService *s, gboolean paused) {
  g_return_if_fail(owner_ok(s));
  if (s->paused == paused) return;
  s->paused = paused;
  table_changed(s);
  if (!paused) return;
  s->generation++;
  g_autoptr(GPtrArray) requests = requests_copy(s);
  for (guint i = 0; i < requests->len; i++)
    finish(requests->pdata[i], SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_PAUSED, "signing is paused");
}
gboolean signer_approval_service_is_paused(SignerApprovalService *s) { return s->paused; }
guint64 signer_approval_service_generation(SignerApprovalService *s) { return s->generation; }
SignerPermissions *signer_approval_service_permissions(SignerApprovalService *s) {
  g_return_val_if_fail(owner_ok(s), NULL); return s->permissions;
}
#ifdef NIP55L_TEST_TRUST_ENV
void signer_approval_service_test_after_backend(SignerApprovalService *s, void (*hook)(gpointer), gpointer data) {
  g_return_if_fail(owner_ok(s) && g_hash_table_size(s->requests) == 0);
  s->after_backend = hook; s->after_backend_data = data;
}
#endif
void signer_approval_service_shutdown(SignerApprovalService *s) {
  g_return_if_fail(owner_ok(s));
  s->stopped = TRUE;
  g_autoptr(GPtrArray) requests = requests_copy(s);
  for (guint i = 0; i < requests->len; i++)
    finish(requests->pdata[i], SIGNER_REQUEST_CANCELLED, ORG_NOSTR_SIGNER_ERR_CANCELLED, "signer stopped");
}
