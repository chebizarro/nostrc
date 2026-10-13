/* Real shared service; disposable environment key, no key-store mutations.
 * Synchronisation is callback/source driven; timeouts are failure bounds. */
#include "signer_approval_service.h"
#include "nip55l_dbus_errors.h"
#include <glib/gstdio.h>
#include <string.h>
#include <nostr-kinds.h>
#include <sys/resource.h>
#include <signal.h>

static const gchar KEY[] = "0000000000000000000000000000000000000000000000000000000000000001";
static const gchar PEER[] = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
typedef struct { guint count; gchar *error, *result; } Reply;
typedef struct { guint count; gboolean ok; gchar *error; } Decision;
typedef struct { GPtrArray *pending; guint completed; } Watch;
static void reply(gpointer data, const gchar *error, const gchar *message, const gchar *result) {
  (void)message;
  Reply *r = data; g_assert_cmpuint(++r->count, ==, 1);
  r->error = g_strdup(error); r->result = g_strdup(result);
}
static void reply_clear(Reply *r) {
  g_free(r->error); if (r->result) { memset(r->result, 0, strlen(r->result)); g_free(r->result); }
}
static void decided(gpointer data, gboolean ok, const gchar *error, const gchar *message) {
  (void)message;
  Decision *d = data; g_assert_cmpuint(++d->count, ==, 1); d->ok = ok; d->error = g_strdup(error);
}
static void changed(gpointer data, GVariant *snapshot) {
  Watch *w = data; const gchar *state = NULL;
  g_assert_true(g_variant_lookup(snapshot, "state", "&s", &state));
  if (!strcmp(state, "PENDING")) g_ptr_array_add(w->pending, g_variant_ref(snapshot));
  else w->completed++;
}
static gboolean failure_bound(gpointer data) { (void)data; g_error("shared service did not complete"); return G_SOURCE_REMOVE; }
static void wait_count(guint *count, guint target) {
  guint bound = g_timeout_add_seconds(20, failure_bound, NULL);
  while (*count < target) g_main_context_iteration(NULL, TRUE);
  g_source_remove(bound);
}
static void wait_prompts(Watch *w, guint target) {
  guint bound = g_timeout_add_seconds(20, failure_bound, NULL);
  while (w->pending->len < target) g_main_context_iteration(NULL, TRUE);
  g_source_remove(bound);
}
static const gchar *request_id(Watch *w, guint index) {
  const gchar *id = NULL;
  g_assert_true(g_variant_lookup(w->pending->pdata[index], "request_id", "&s", &id));
  g_assert_cmpuint(strlen(id), ==, 64); return id;
}
static SignerApprovalService *start(Watch *w) {
  const gchar *files[] = {"signer-grants.ini", "signer-permissions-v2.ini"};
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "gnostr", files[i], NULL);
    g_unlink(path);
  }
  w->pending = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  SignerApprovalService *s = signer_approval_service_new(NULL);
  signer_approval_service_subscribe(s, changed, w); return s;
}
static void stop(SignerApprovalService *s, Watch *w) {
  signer_approval_service_shutdown(s); signer_approval_service_unref(s); g_ptr_array_unref(w->pending);
}
static SignerCaller local_caller(void) {
  return (SignerCaller){.kind=SIGNER_CALLER_EXE, .principal="exe:/test/client", .sender="test", .same_uid=TRUE};
}
static SignerGateHandle *submit(SignerApprovalService *s, SignerCaller *who, const gchar *connection,
    SignerGateOp op, const gchar *a, const gchar *b, Reply *r) {
  SignerGateContext context = {.transport=SIGNER_TRANSPORT_NIP46, .no_identity=op==SIGNER_GATE_PING || op==SIGNER_GATE_CONNECT};
  return signer_approval_service_submit(s, who, connection, op, a, b, "", &context, reply, r);
}
static void test_nip46_caller(void) {
  g_autofree gchar *upper = g_ascii_strup(PEER, -1);
  g_autoptr(SignerCaller) who = signer_caller_for_nip46(upper);
  g_autofree gchar *principal = g_strconcat("nip46:", PEER, NULL);
  g_assert_cmpstr(who->principal, ==, principal); g_assert_cmpstr(who->sender, ==, principal);
  g_assert_false(who->same_uid); g_assert_false(who->unattested); g_assert_false(signer_caller_is_approver(who));
  /* Even an accidentally copied local trust flag can never make it a UI. */
  who->same_uid = TRUE; who->unattested = TRUE;
  g_assert_false(signer_caller_is_approver(who));
  g_assert_null(signer_caller_for_nip46("bad"));
}
static void test_join_cancel(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply a = {0}, b = {0}, other = {0};
  g_autoptr(SignerGateHandle) ha = submit(s, &who, "connection-a", SIGNER_GATE_NIP44_ENCRYPT, "same", PEER, &a);
  g_autofree gchar *upper = g_ascii_strup(PEER, -1);
  g_autoptr(SignerGateHandle) hb = submit(s, &who, "connection-b", SIGNER_GATE_NIP44_ENCRYPT, "same", upper, &b);
  g_autoptr(SignerGateHandle) hc = submit(s, &who, "connection-c", SIGNER_GATE_NIP44_ENCRYPT, "different", PEER, &other);
  wait_prompts(&w, 2);
  g_autoptr(GVariant) info = signer_approval_service_info(s, request_id(&w, 0));
  guint calls = 0; g_assert_true(g_variant_lookup(info, "calls", "u", &calls)); g_assert_cmpuint(calls, ==, 2);
  signer_approval_service_connection_closed(s, "connection-a");
  g_assert_cmpuint(a.count, ==, 1); g_assert_cmpstr(a.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED);
  signer_gate_cancel(ha); g_assert_cmpuint(a.count, ==, 1); /* idempotent */
  Decision d = {0}, e = {0};
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &d);
  signer_approval_service_decide(s, request_id(&w, 1), FALSE, FALSE, 0, decided, &e);
  wait_count(&b.count, 1); wait_count(&d.count, 1);
  g_assert_nonnull(b.result); g_assert_null(b.error); g_assert_true(d.ok);
  g_assert_cmpstr(other.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL);
  g_assert_cmpuint(w.completed, ==, 2); g_assert_cmpuint(signer_approval_service_pending_count(s), ==, 0);
  reply_clear(&a); reply_clear(&b); reply_clear(&other); stop(s, &w);
}
static void test_canonical_event_and_scope(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply a = {0}, b = {0}, other = {0};
  g_autoptr(SignerGateHandle) ha = submit(s, &who, "a", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"same\"}", NULL, &a);
  g_autoptr(SignerGateHandle) hb = submit(s, &who, "b", SIGNER_GATE_SIGN_EVENT,
      "{ \"content\":\"same\", \"tags\":[], \"kind\":1 }", NULL, &b);
  SignerCaller different = who; different.principal = "exe:/test/other";
  g_autoptr(SignerGateHandle) hc = submit(s, &different, "c", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"same\"}", NULL, &other);
  wait_prompts(&w, 2);
  g_autoptr(GVariant) info = signer_approval_service_info(s, request_id(&w, 0));
  guint calls = 0, kind = 99;
  g_assert_true(g_variant_lookup(info, "calls", "u", &calls)); g_assert_cmpuint(calls, ==, 2);
  g_assert_true(g_variant_lookup(info, "event_kind", "u", &kind)); g_assert_cmpuint(kind, ==, 1);
  signer_gate_cancel(ha); signer_gate_cancel(hb); signer_gate_cancel(hc);
  g_assert_cmpuint(w.completed, ==, 2); reply_clear(&a); reply_clear(&b); reply_clear(&other); stop(s, &w);
}
static void test_pause_generation_and_no_identity(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w);
  g_autoptr(SignerCaller) who = signer_caller_for_nip46(PEER);
  Reply ping = {0}; Decision d = {0};
  g_unsetenv("NOSTR_SIGNER_SECKEY_HEX"); /* PING and CONNECT need no custody */
  g_autoptr(SignerGateHandle) hp = submit(s, who, who->sender, SIGNER_GATE_PING, NULL, NULL, &ping);
  wait_prompts(&w, 1);
  g_autoptr(GVariant) info = signer_approval_service_info(s, request_id(&w, 0));
  const gchar *identity = NULL; g_assert_true(g_variant_lookup(info, "identity", "&s", &identity)); g_assert_cmpstr(identity, ==, "");
  g_assert_null(g_variant_lookup_value(info, "event_kind", NULL));
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &d);
  /* EXECUTING job hasn't handed its result to the transport. A pause/resume
   * must cancel it even if crypto finishes before the owner dispatches it. */
  guint64 generation = signer_approval_service_generation(s);
  signer_approval_service_set_paused(s, TRUE); signer_approval_service_set_paused(s, FALSE);
  g_assert_cmpuint(signer_approval_service_generation(s), ==, generation + 1);
  g_assert_cmpstr(ping.error, ==, ORG_NOSTR_SIGNER_ERR_PAUSED); g_assert_null(ping.result); g_assert_false(d.ok);
  Reply connect = {0}; Decision e = {0};
  g_autoptr(SignerGateHandle) hc = submit(s, who, "new-connection", SIGNER_GATE_CONNECT, NULL, NULL, &connect);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), TRUE, TRUE, 0, decided, &e);
  wait_count(&connect.count, 1); wait_count(&e.count, 1);
  g_assert_cmpstr(connect.result, ==, "ack"); g_assert_true(e.ok);
  g_autoptr(GVariant) grants = signer_approval_service_list_grants(s); g_assert_cmpuint(g_variant_n_children(grants), ==, 1);
  reply_clear(&ping); reply_clear(&connect); stop(s, &w); g_setenv("NOSTR_SIGNER_SECKEY_HEX", KEY, TRUE);
}
#ifdef NIP55L_TEST_TRUST_ENV
typedef struct { GMutex lock; GCond cond; gboolean ready, release; guint executions; } Barrier;
static void after_backend(gpointer data) {
  Barrier *b = data;
  g_mutex_lock(&b->lock); b->executions++; b->ready = TRUE; g_cond_signal(&b->cond);
  while (!b->release) g_cond_wait(&b->cond, &b->lock);
  g_mutex_unlock(&b->lock);
}
static void test_deny_queued_execution(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  SignerCaller blocker = who; blocker.principal = "exe:/test/blocker";
  Barrier barrier = {0}; g_mutex_init(&barrier.lock); g_cond_init(&barrier.cond);
  signer_approval_service_test_after_backend(s, after_backend, &barrier);
  Reply occupied = {0}, queued = {0}, deny = {0};
  Decision occupied_decision = {0}, queued_decision = {0}, deny_decision = {0};
  g_autoptr(SignerGateHandle) h0 = submit(s, &blocker, "blocker", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &occupied);
  g_autoptr(SignerGateHandle) h1 = submit(s, &who, "queued", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"queued A\"}", NULL, &queued);
  g_autoptr(SignerGateHandle) h2 = submit(s, &who, "deny", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"deny B\"}", NULL, &deny);
  wait_prompts(&w, 3);
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &occupied_decision);
  g_mutex_lock(&barrier.lock);
  gint64 bound = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!barrier.ready) g_assert_true(g_cond_wait_until(&barrier.cond, &barrier.lock, bound));
  g_mutex_unlock(&barrier.lock);
  /* The worker cannot start A: it is still occupied by the first job. */
  signer_approval_service_decide(s, request_id(&w, 1), TRUE, FALSE, 0, decided, &queued_decision);
  g_assert_cmpuint(queued.count, ==, 0);
  signer_approval_service_decide(s, request_id(&w, 2), FALSE, TRUE, 0, decided, &deny_decision);
  g_assert_cmpuint(queued.count, ==, 1); g_assert_false(queued_decision.ok);
  g_assert_cmpstr(queued.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL); g_assert_null(queued.result);
  g_assert_cmpstr(deny.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL);
  g_mutex_lock(&barrier.lock); barrier.release = TRUE; g_cond_signal(&barrier.cond); g_mutex_unlock(&barrier.lock);
  wait_count(&occupied.count, 1);
  /* FIFO sentinel proves the cancelled job passed through the worker.
   * Result suppression alone is insufficient: A must never call perform. */
  Reply sentinel = {0}; Decision sentinel_decision = {0};
  g_autoptr(SignerGateHandle) h3 = submit(s, &blocker, "sentinel", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &sentinel);
  wait_prompts(&w, 4);
  signer_approval_service_decide(s, request_id(&w, 3), TRUE, FALSE, 0, decided, &sentinel_decision);
  wait_count(&sentinel.count, 1); wait_count(&sentinel_decision.count, 1);
  g_mutex_lock(&barrier.lock); g_assert_cmpuint(barrier.executions, ==, 2); g_mutex_unlock(&barrier.lock);
  g_assert_cmpuint(queued.count, ==, 1); g_assert_nonnull(sentinel.result);
  gchar *file = g_build_filename(g_get_user_config_dir(), "gnostr", "signer-permissions-v2.ini", NULL);
  g_assert_cmpint(g_unlink(file), ==, 0); g_free(file);
  reply_clear(&occupied); reply_clear(&queued); reply_clear(&deny); reply_clear(&sentinel);
  signer_approval_service_test_after_backend(s, NULL, NULL); stop(s, &w);
  g_mutex_clear(&barrier.lock); g_cond_clear(&barrier.cond);
}
static SignerTrustTier full_trust(const gchar *principal, gboolean *consented, gpointer data) {
  (void)principal; *consented = TRUE; return *(SignerTrustTier *)data;
}
static void test_tier_lowered_queued_revision(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Barrier barrier = {0}; g_mutex_init(&barrier.lock); g_cond_init(&barrier.cond);
  SignerTrustTier tier = SIGNER_TRUST_FULL;
  SignerPermissions *permissions = signer_approval_service_permissions(s);
  signer_permissions_set_trust_lookup(permissions, full_trust, &tier);
  signer_approval_service_test_after_backend(s, after_backend, &barrier);
  Reply blocker = {0}, queued = {0}, sentinel = {0}; Decision first = {0}, last = {0};
  g_autoptr(SignerGateHandle) hb = submit(s, &who, "blocker", SIGNER_GATE_CONNECT, NULL, NULL, &blocker);
  wait_prompts(&w, 1); signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &first);
  g_mutex_lock(&barrier.lock); gint64 bound = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!barrier.ready) g_assert_true(g_cond_wait_until(&barrier.cond, &barrier.lock, bound));
  g_mutex_unlock(&barrier.lock);
  /* Full auto-admits PING, queued behind the blocked worker. No denial is
   * introduced: only a revision change can prevent the old auto-authority. */
  g_autoptr(SignerGateHandle) hq = submit(s, &who, "queued", SIGNER_GATE_PING, NULL, NULL, &queued);
  g_assert_cmpuint(w.pending->len, ==, 1); g_assert_cmpuint(queued.count, ==, 0);
  tier = SIGNER_TRUST_LOW; signer_permissions_policy_changed(permissions);
  g_mutex_lock(&barrier.lock); barrier.release = TRUE; g_cond_signal(&barrier.cond); g_mutex_unlock(&barrier.lock);
  wait_count(&queued.count, 1);
  g_assert_cmpstr(queued.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED); g_assert_null(queued.result);
  g_autoptr(SignerGateHandle) hs = submit(s, &who, "sentinel", SIGNER_GATE_CONNECT, NULL, NULL, &sentinel);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), TRUE, FALSE, 0, decided, &last);
  wait_count(&sentinel.count, 1); wait_count(&last.count, 1);
  g_mutex_lock(&barrier.lock); g_assert_cmpuint(barrier.executions, ==, 2); g_mutex_unlock(&barrier.lock);
  reply_clear(&blocker); reply_clear(&queued); reply_clear(&sentinel);
  signer_approval_service_test_after_backend(s, NULL, NULL); stop(s, &w);
  g_mutex_clear(&barrier.lock); g_cond_clear(&barrier.cond);
}
static gboolean release_expired_worker(gpointer data) {
  Barrier *barrier = data;
  g_mutex_lock(&barrier->lock); barrier->release = TRUE;
  g_cond_signal(&barrier->cond); g_mutex_unlock(&barrier->lock); return G_SOURCE_REMOVE;
}
static void test_expiry_queued_revision(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Barrier barrier = {0}; g_mutex_init(&barrier.lock); g_cond_init(&barrier.cond);
  signer_approval_service_test_after_backend(s, after_backend, &barrier);
  Reply blocker = {0}, queued = {0}, sentinel = {0}; Decision first = {0}, last = {0};
  g_autoptr(SignerGateHandle) hb = submit(s, &who, "blocker", SIGNER_GATE_CONNECT, NULL, NULL, &blocker);
  wait_prompts(&w, 1); signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &first);
  g_mutex_lock(&barrier.lock); gint64 bound = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!barrier.ready) g_assert_true(g_cond_wait_until(&barrier.cond, &barrier.lock, bound));
  g_mutex_unlock(&barrier.lock);
  SignerPermissions *permissions = signer_approval_service_permissions(s);
  g_assert_true(signer_permissions_remember(permissions, who.principal, NULL, "ping", -1, TRUE, 1, NULL));
  g_autoptr(SignerGateHandle) hq = submit(s, &who, "queued", SIGNER_GATE_PING, NULL, NULL, &queued);
  g_assert_cmpuint(w.pending->len, ==, 1); g_assert_cmpuint(queued.count, ==, 0);
  /* This source advances a real permission's absolute expiry, not relay
   * delivery. No policy mutation occurs while the job is queued. */
  g_timeout_add(2000, release_expired_worker, &barrier);
  wait_count(&queued.count, 1);
  g_assert_cmpstr(queued.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED); g_assert_null(queued.result);
  g_autoptr(SignerGateHandle) hs = submit(s, &who, "sentinel", SIGNER_GATE_CONNECT, NULL, NULL, &sentinel);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), TRUE, FALSE, 0, decided, &last);
  wait_count(&sentinel.count, 1); wait_count(&last.count, 1);
  g_mutex_lock(&barrier.lock); g_assert_cmpuint(barrier.executions, ==, 2); g_mutex_unlock(&barrier.lock);
  reply_clear(&blocker); reply_clear(&queued); reply_clear(&sentinel);
  signer_approval_service_test_after_backend(s, NULL, NULL); stop(s, &w);
  g_mutex_clear(&barrier.lock); g_cond_clear(&barrier.cond);
}
static void test_pause_after_backend(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Barrier barrier = {0}; g_mutex_init(&barrier.lock); g_cond_init(&barrier.cond);
  signer_approval_service_test_after_backend(s, after_backend, &barrier);
  Reply crypto = {0}; Decision d = {0};
  g_autoptr(SignerGateHandle) h = submit(s, &who, "crypto", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"delayed result\"}", NULL, &crypto);
  wait_prompts(&w, 1);
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, TRUE, 0, decided, &d);
  g_mutex_lock(&barrier.lock);
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!barrier.ready) g_assert_true(g_cond_wait_until(&barrier.cond, &barrier.lock, deadline));
  g_mutex_unlock(&barrier.lock);
  /* Cryptography returned, but transport hasn't received anything. */
  g_assert_cmpuint(crypto.count, ==, 0);
  signer_approval_service_set_paused(s, TRUE); signer_approval_service_set_paused(s, FALSE);
  g_assert_cmpstr(crypto.error, ==, ORG_NOSTR_SIGNER_ERR_PAUSED); g_assert_null(crypto.result); g_assert_false(d.ok);
  g_mutex_lock(&barrier.lock); barrier.release = TRUE; g_cond_signal(&barrier.cond); g_mutex_unlock(&barrier.lock);
  signer_approval_service_test_after_backend(s, NULL, NULL);
  /* A subsequent serialized job is the barrier that proves the old worker
   * returned; its old-generation result must never resurrect a reply/grant. */
  Reply next = {0}; Decision e = {0};
  g_autoptr(SignerGateHandle) hn = submit(s, &who, "next", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &next);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), TRUE, FALSE, 0, decided, &e);
  wait_count(&next.count, 1); wait_count(&e.count, 1);
  g_assert_cmpuint(crypto.count, ==, 1);
  g_autoptr(GVariant) grants = signer_approval_service_list_grants(s);
  for (gsize i = 0; i < g_variant_n_children(grants); i++) {
    GVariant *row = g_variant_get_child_value(grants, i);
    const gchar *kind = NULL; g_variant_get_child(row, 0, "&s", &kind);
    g_assert_cmpstr(kind, !=, "event"); g_variant_unref(row);
  }
  reply_clear(&crypto); reply_clear(&next); stop(s, &w);
  g_mutex_clear(&barrier.lock); g_cond_clear(&barrier.cond);
}
#endif
static void test_scoped_remember_and_revoke(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  g_autofree gchar *note = g_strdup_printf("{\"kind\":%d,\"tags\":[],\"content\":\"note\"}", NOSTR_KIND_TEXT_NOTE);
  g_autofree gchar *reaction = g_strdup_printf("{\"kind\":%d,\"tags\":[],\"content\":\"+\"}", NOSTR_KIND_REACTION);
  Reply first = {0}, repeat = {0}, other = {0}, after_revoke = {0}; Decision d = {0}, e = {0};
  g_autoptr(SignerGateHandle) h1 = submit(s, &who, "first", SIGNER_GATE_SIGN_EVENT, note, NULL, &first);
  wait_prompts(&w, 1);
  const gchar *identity = NULL; g_assert_true(g_variant_lookup(w.pending->pdata[0], "identity", "&s", &identity));
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, TRUE, 600, decided, &d);
  wait_count(&first.count, 1); g_assert_nonnull(first.result); g_assert_true(d.ok);
  g_autoptr(GVariant) legacy = signer_approval_service_list_grants(s);
  g_assert_cmpuint(g_variant_n_children(legacy), ==, 0);
  g_autoptr(GVariant) rows = signer_permissions_list(signer_approval_service_permissions(s), who.principal, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  g_autoptr(GVariant) row = g_variant_get_child_value(rows, 0); gint kind;
  g_assert_true(g_variant_lookup(row, "event-kind", "i", &kind)); g_assert_cmpint(kind, ==, NOSTR_KIND_TEXT_NOTE);
  g_autoptr(SignerGateHandle) h2 = submit(s, &who, "repeat", SIGNER_GATE_SIGN_EVENT, note, NULL, &repeat);
  wait_count(&repeat.count, 1); g_assert_nonnull(repeat.result); g_assert_cmpuint(w.pending->len, ==, 1);
  g_autoptr(SignerGateHandle) h3 = submit(s, &who, "other", SIGNER_GATE_SIGN_EVENT, reaction, NULL, &other);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), FALSE, TRUE, 600, decided, &e);
  g_assert_cmpstr(other.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL); g_assert_true(e.ok);
  gboolean removed = FALSE;
  g_assert_true(signer_approval_service_revoke_grant(s, "event", who.principal, identity, &removed)); g_assert_true(removed);
  g_autoptr(SignerGateHandle) h4 = submit(s, &who, "after-revoke", SIGNER_GATE_SIGN_EVENT, note, NULL, &after_revoke);
  wait_prompts(&w, 3); signer_gate_cancel(h4);
  reply_clear(&first); reply_clear(&repeat); reply_clear(&other); reply_clear(&after_revoke); stop(s, &w);
}
static void test_remember_write_failure(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply result = {0}; Decision d = {0};
  g_autofree gchar *note = g_strdup_printf("{\"kind\":%d,\"tags\":[],\"content\":\"not delivered\"}", NOSTR_KIND_TEXT_NOTE);
  g_autoptr(SignerGateHandle) h = submit(s, &who, "failure", SIGNER_GATE_SIGN_EVENT, note, NULL, &result);
  wait_prompts(&w, 1);
  /* Real atomic-write failure, not a parser/load failure: cap file growth
   * only after successful policy load and restore the process limit afterwards. */
  struct rlimit original, limited;
  g_assert_cmpint(getrlimit(RLIMIT_FSIZE, &original), ==, 0); limited = original; limited.rlim_cur = 1;
  void (*handler)(int) = signal(SIGXFSZ, SIG_IGN);
  g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &limited), ==, 0);
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, TRUE, 600, decided, &d);
  wait_count(&result.count, 1); wait_count(&d.count, 1);
  g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &original), ==, 0); signal(SIGXFSZ, handler);
  g_assert_cmpstr(result.error, ==, ORG_NOSTR_SIGNER_ERR_INTERNAL); g_assert_null(result.result); g_assert_false(d.ok);
  g_autoptr(GVariant) rows = signer_permissions_list(signer_approval_service_permissions(s), who.principal, NULL);
  g_assert_nonnull(rows); g_assert_cmpuint(g_variant_n_children(rows), ==, 0);
  reply_clear(&result); stop(s, &w);
}
static void test_nip04_roundtrip(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply encrypted = {0}, decrypted = {0}; Decision a = {0}, b = {0};
  g_autoptr(SignerGateHandle) he = submit(s, &who, "encrypt", SIGNER_GATE_NIP04_ENCRYPT, "round trip", PEER, &encrypted);
  wait_prompts(&w, 1); signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &a);
  wait_count(&encrypted.count, 1); g_assert_null(encrypted.error); g_assert_nonnull(encrypted.result);
  g_autoptr(SignerGateHandle) hd = submit(s, &who, "decrypt", SIGNER_GATE_NIP04_DECRYPT, encrypted.result, PEER, &decrypted);
  wait_prompts(&w, 2); signer_approval_service_decide(s, request_id(&w, 1), TRUE, FALSE, 0, decided, &b);
  wait_count(&decrypted.count, 1); g_assert_cmpstr(decrypted.result, ==, "round trip");
  reply_clear(&encrypted); reply_clear(&decrypted); stop(s, &w);
}
static void test_validation_and_deadline(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply invalid = {0}, expired = {0};
  SignerGateContext context = {.transport=SIGNER_TRANSPORT_NIP46, .no_identity=TRUE};
  g_autoptr(SignerGateHandle) hi = signer_approval_service_submit(s, &who, "a", SIGNER_GATE_SIGN_EVENT,
      "{}", NULL, "", &context, reply, &invalid);
  g_assert_cmpstr(invalid.error, ==, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT);
  context.deadline = g_get_monotonic_time() - 1;
  g_autoptr(SignerGateHandle) he = signer_approval_service_submit(s, &who, "b", SIGNER_GATE_PING,
      NULL, NULL, "", &context, reply, &expired);
  g_assert_cmpstr(expired.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT); g_assert_cmpuint(w.pending->len, ==, 0);
  reply_clear(&invalid); reply_clear(&expired); stop(s, &w);
}
static void test_metadata_and_revoke(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply zero = {0}, private = {0};
  g_autoptr(SignerGateHandle) hz = submit(s, &who, "zero", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":0,\"tags\":[],\"content\":\"profile\"}", NULL, &zero);
  g_autoptr(SignerGateHandle) hp = submit(s, &who, "private", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":4,\"tags\":[],\"content\":\"PRIVATE-CIPHERTEXT\"}", NULL, &private);
  wait_prompts(&w, 2);
  guint kind = 999;
  g_assert_true(g_variant_lookup(w.pending->pdata[0], "event_kind", "u", &kind)); g_assert_cmpuint(kind, ==, 0);
  GVariant *summary = g_variant_lookup_value(w.pending->pdata[1], "event_summary", G_VARIANT_TYPE_VARDICT);
  g_assert_nonnull(summary);
  gchar *dump = g_variant_print(summary, FALSE);
  g_assert_null(strstr(dump, "PRIVATE-CIPHERTEXT")); g_free(dump); g_variant_unref(summary);
  signer_approval_service_cancel_principal(s, who.principal);
  g_assert_cmpstr(zero.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED);
  g_assert_cmpstr(private.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED);
  Decision d = {0}; signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &d);
  g_assert_cmpstr(d.error, ==, ORG_NOSTR_SIGNER_ERR_NOT_FOUND); g_free(d.error);
  reply_clear(&zero); reply_clear(&private); stop(s, &w);
}
static void test_policy_recheck(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w); SignerCaller who = local_caller();
  Reply result = {0}; Decision decision = {0};
  g_autoptr(SignerGateHandle) h = submit(s, &who, "policy", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"must not sign\"}", NULL, &result);
  wait_prompts(&w, 1);
  gchar *file = g_build_filename(g_get_user_config_dir(), "gnostr", "signer-grants.ini", NULL);
  g_assert_true(g_file_set_contents(file, "[event]\nexe:/test/client|*=deny\n", -1, NULL));
  signer_approval_service_decide(s, request_id(&w, 0), TRUE, TRUE, 0, decided, &decision);
  g_assert_cmpuint(decision.count, ==, 1); g_assert_false(decision.ok);
  g_assert_cmpstr(result.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL); g_assert_null(result.result);
  reply_clear(&result); stop(s, &w); g_unlink(file); g_free(file);
}
static void test_headless_defaults(void) {
  Watch w = {0}; SignerApprovalService *s = start(&w);
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
#ifdef __APPLE__
  if (!exe) exe = g_strdup(g_getenv("NIP55L_APPROVAL_TEST_EXE"));
#endif
  g_assert_nonnull(exe);
  GStatBuf st; g_assert_cmpint(g_stat(exe, &st), ==, 0);
  SignerCaller who = {.kind=SIGNER_CALLER_EXE, .same_uid=TRUE, .exe=exe,
    .exe_dev=st.st_dev, .exe_ino=st.st_ino, .principal="exe:/test/headless", .sender="default"};
  g_autofree gchar *defaults = g_strdup_printf("get_public_key,get_relays@%s+nip44_decrypt@%s", exe, exe);
  g_setenv("NOSTR_SIGNER_TEST_DEFAULT_GRANTS", defaults, TRUE);
  Reply pubkey = {0};
  g_autoptr(SignerGateHandle) hp = submit(s, &who, "default", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &pubkey);
  wait_count(&pubkey.count, 1); g_assert_nonnull(pubkey.result); g_assert_cmpuint(w.pending->len, ==, 0);
  Reply relays = {0}, encrypt = {0}, decrypt = {0}; Decision enc = {0};
  g_autoptr(SignerGateHandle) hrel = submit(s, &who, "relays", SIGNER_GATE_GET_RELAYS, NULL, NULL, &relays);
  wait_count(&relays.count, 1); g_assert_cmpuint(w.pending->len, ==, 0);
  g_assert_cmpstr(relays.error, ==, ORG_NOSTR_SIGNER_ERR_NOT_FOUND); /* no configured relays, not a consent failure */
  g_autoptr(SignerGateHandle) henc = submit(s, &who, "encrypt", SIGNER_GATE_NIP44_ENCRYPT, "headless", PEER, &encrypt);
  wait_prompts(&w, 1); signer_approval_service_decide(s, request_id(&w, 0), TRUE, FALSE, 0, decided, &enc);
  wait_count(&encrypt.count, 1); g_assert_nonnull(encrypt.result);
  g_autoptr(SignerGateHandle) hdec = submit(s, &who, "decrypt", SIGNER_GATE_NIP44_DECRYPT, encrypt.result, PEER, &decrypt);
  wait_count(&decrypt.count, 1); g_assert_cmpstr(decrypt.result, ==, "headless"); g_assert_cmpuint(w.pending->len, ==, 1);
  Reply sign = {0};
  g_autoptr(SignerGateHandle) hsign = submit(s, &who, "sign", SIGNER_GATE_SIGN_EVENT,
      "{\"kind\":1,\"tags\":[],\"content\":\"not implicit\"}", NULL, &sign);
  wait_prompts(&w, 2); signer_gate_cancel(hsign);
  /* No default for remote callers, even if a bad adapter copied executable
   * attributes. No grant for signing, seal/convkey or encryption either. */
  who.kind = SIGNER_CALLER_NIP46;
  Reply remote = {0};
  g_autoptr(SignerGateHandle) hr = submit(s, &who, "remote", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &remote);
  wait_prompts(&w, 3); signer_gate_cancel(hr);
  g_assert_cmpstr(remote.error, ==, ORG_NOSTR_SIGNER_ERR_CANCELLED);
  who.kind = SIGNER_CALLER_EXE; who.exe_ino++;
  Reply wrong_inode = {0};
  g_autoptr(SignerGateHandle) hw = submit(s, &who, "wrong-inode", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &wrong_inode);
  wait_prompts(&w, 4); signer_gate_cancel(hw);
  who.exe_ino--; /* Explicit wildcard deny beats a specific allow and defaults. */
  gchar *path = g_build_filename(g_get_user_config_dir(), "gnostr", "signer-grants.ini", NULL);
  gchar *policy = g_strdup_printf("[get_public_key]\n%s|%s=allow\n%s|*=deny\n", who.principal, pubkey.result, who.principal);
  g_assert_true(g_file_set_contents(path, policy, -1, NULL)); g_free(path); g_free(policy);
  Reply denied = {0};
  g_autoptr(SignerGateHandle) hdeny = submit(s, &who, "denied", SIGNER_GATE_GET_PUBLIC_KEY, NULL, NULL, &denied);
  wait_count(&denied.count, 1); g_assert_cmpstr(denied.error, ==, ORG_NOSTR_SIGNER_ERR_APPROVAL);
  g_assert_cmpuint(w.pending->len, ==, 4);
  reply_clear(&pubkey); reply_clear(&remote); reply_clear(&wrong_inode); reply_clear(&relays);
  reply_clear(&encrypt); reply_clear(&decrypt); reply_clear(&sign); reply_clear(&denied);
  stop(s, &w); g_unsetenv("NOSTR_SIGNER_TEST_DEFAULT_GRANTS"); g_free(exe);
}
int main(int argc, char **argv) {
  /* Fresh config for repeatable grant assertions; never the owner's store. */
  gchar *config = g_dir_make_tmp("nip55l-approval-XXXXXX", NULL);
  g_assert_nonnull(config); g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_test_init(&argc, &argv, NULL);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", KEY, TRUE);
  g_setenv("NIP55L_APPROVAL_TEST_EXE", argv[0], TRUE);
  g_test_add_func("/approval/nip46-caller", test_nip46_caller);
  g_test_add_func("/approval/join-cancel", test_join_cancel);
  g_test_add_func("/approval/canonical-event-scope", test_canonical_event_and_scope);
  g_test_add_func("/approval/pause-generation-no-identity", test_pause_generation_and_no_identity);
#ifdef NIP55L_TEST_TRUST_ENV
  g_test_add_func("/approval/pause-after-backend", test_pause_after_backend);
  g_test_add_func("/approval/deny-queued-execution", test_deny_queued_execution);
  g_test_add_func("/approval/tier-lowered-queued-revision", test_tier_lowered_queued_revision);
  g_test_add_func("/approval/expiry-queued-revision", test_expiry_queued_revision);
#endif
  g_test_add_func("/approval/scoped-remember-revoke", test_scoped_remember_and_revoke);
  g_test_add_func("/approval/remember-write-failure", test_remember_write_failure);
  g_test_add_func("/approval/nip04-roundtrip", test_nip04_roundtrip);
  g_test_add_func("/approval/validation-deadline", test_validation_and_deadline);
  g_test_add_func("/approval/metadata-revoke", test_metadata_and_revoke);
  g_test_add_func("/approval/headless-defaults", test_headless_defaults);
  g_test_add_func("/approval/policy-recheck", test_policy_recheck);
  gint result = g_test_run();
  gchar *file = g_build_filename(config, "gnostr", "signer-grants.ini", NULL);
  g_unlink(file); g_free(file);
  file = g_build_filename(config, "gnostr", "signer-permissions-v2.ini", NULL); g_unlink(file); g_free(file);
  file = g_build_filename(config, "gnostr", NULL); g_rmdir(file); g_free(file);
  g_rmdir(config); g_free(config); return result;
}
