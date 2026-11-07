/* SPDX-License-Identifier: MIT */
/* Deterministic dispatcher overlap, generation and teardown regressions. */
#include "relay_pool_private.h"
#include "relay-private.h"
#include "test_check.h"
#include <nostr-envelope.h>
#include <glib.h>
#include <pthread.h>
#include <string.h>

#define ID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ID_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

typedef struct {
  GMutex mu;
  GCond cv;
  int calls;
  gboolean accepted, aborted, entered, release, park;
} Result;

static void result_init(Result *r) {
  memset(r, 0, sizeof(*r));
  g_mutex_init(&r->mu);
  g_cond_init(&r->cv);
}
static void result_clear(Result *r) {
  g_mutex_clear(&r->mu);
  g_cond_clear(&r->cv);
}
static void outcome(const char *id, bool accepted, const char *reason, void *data) {
  CHECK(id && strlen(id) == 64);
  Result *r = data;
  g_mutex_lock(&r->mu);
  r->calls++;
  r->accepted = accepted;
  r->aborted = !g_strcmp0(reason, "transport-aborted:");
  if (r->park) {
    r->entered = TRUE;
    g_cond_broadcast(&r->cv);
    gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!r->release) CHECK(g_cond_wait_until(&r->cv, &r->mu, end));
  }
  g_mutex_unlock(&r->mu);
}
static NostrRelay *relay_new(void) {
  Error *err = NULL;
  NostrRelay *r = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
  CHECK(r && !err);
  nostr_relay_set_auto_reconnect(r, false);
  return r;
}
static void deliver(NostrRelay *relay, const char *id, bool ok) {
  char *frame = g_strdup_printf("[\"OK\",\"%s\",%s,\"blocked: fixture\"]", id, ok ? "true" : "false");
  NostrEnvelope *env = nostr_envelope_parse(frame);
  CHECK(env);
  nostr_relay_dispatch_control_envelope(relay, env);
  nostr_envelope_free(env);
  g_free(frame);
}
static SignetRelayPool *pool_new(void) {
  SignetRelayPoolConfig cfg = {0};
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  CHECK(rp);
  return rp;
}
static void test_overlap_and_once(void) {
  SignetRelayPool *rp = pool_new();
  NostrRelay *relay = relay_new();
  Result a, b, same_id;
  result_init(&a); result_init(&b); result_init(&same_id);
  signet_relay_pool_watch_auth_ok(rp, relay, ID_C, NULL);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &a);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_B, outcome, &b);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &same_id);
  deliver(relay, ID_A, true);
  deliver(relay, ID_C, true); /* must still schedule post-AUTH */
  deliver(relay, ID_B, false);
  deliver(relay, ID_A, false); /* duplicate: no second terminal */
  deliver(relay, ID_B, true);
  CHECK(a.calls == 1 && a.accepted);
  CHECK(same_id.calls == 1 && same_id.accepted);
  CHECK(b.calls == 1 && !b.accepted);
  signet_relay_pool_free(rp); /* queued resubscribe is cancelled before free */
  while (g_main_context_iteration(NULL, FALSE)) {}
  nostr_relay_free(relay);
  result_clear(&a); result_clear(&b); result_clear(&same_id);
}
static void test_generation_and_abort(void) {
  SignetRelayPool *rp = pool_new();
  NostrRelay *relay = relay_new();
  Result a, b;
  result_init(&a); result_init(&b);
  signet_relay_pool_watch_auth_ok(rp, relay, ID_C, NULL);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &a);
  nsync_mu_lock(&relay->priv->mutex);
  relay->priv->auth_generation++;
  nsync_mu_unlock(&relay->priv->mutex);
  Result fresh; result_init(&fresh);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &fresh);
  deliver(relay, ID_C, true); /* previous generation cannot re-authenticate */
  deliver(relay, ID_A, true);
  CHECK(a.calls == 0); /* old generation owns an independent terminal/deadline */
  CHECK(fresh.calls == 1 && fresh.accepted);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_B, outcome, &b);
  signet_relay_pool_stop(rp);
  CHECK(b.calls == 1 && b.aborted);
  CHECK(a.calls == 1 && a.aborted);
  result_clear(&fresh);
  deliver(relay, ID_B, true);
  CHECK(b.calls == 1);
  signet_relay_pool_free(rp);
  nostr_relay_free(relay);
  result_clear(&a); result_clear(&b);
}
typedef struct { NostrRelay *relay; } Delivery;
static void *deliver_thread(void *data) {
  deliver(((Delivery *)data)->relay, ID_A, true);
  return NULL;
}
static void test_concurrent_duplicate(void) {
  SignetRelayPool *rp = pool_new();
  NostrRelay *relay = relay_new();
  Result a; result_init(&a); a.park = TRUE;
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &a);
  Delivery d = {relay};
  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, deliver_thread, &d));
  g_mutex_lock(&a.mu);
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!a.entered) CHECK(g_cond_wait_until(&a.cv, &a.mu, end));
  g_mutex_unlock(&a.mu);
  deliver(relay, ID_A, true);
  g_mutex_lock(&a.mu);
  CHECK(a.calls == 1);
  a.release = TRUE;
  g_cond_broadcast(&a.cv);
  g_mutex_unlock(&a.mu);
  CHECK(!pthread_join(thread, NULL));
  signet_relay_pool_free(rp);
  nostr_relay_free(relay);
  result_clear(&a);
}
static void test_queued_auth_secret_teardown(gboolean reconfigure) {
  SignetRelayPoolConfig cfg = {.auth_sk_hex =
    "0000000000000000000000000000000000000000000000000000000000000001"};
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  NostrRelay *relay = relay_new();
  signet_relay_pool_watch_auth_ok(rp, relay, ID_C, NULL);
  NostrEnvelope *env = nostr_envelope_parse("[\"AUTH\",\"queued-secret-lifetime-fixture\"]");
  CHECK(env);
  nostr_relay_dispatch_control_envelope(relay, env);
  nostr_envelope_free(env);
  /* Do not dispatch the default context: stop must cancel and destroy the
   * secret-bearing AUTH source itself, including its copied signing key. */
  if (reconfigure) {
    const char *replacement[] = {"ws://127.0.0.1:1"};
    CHECK(!signet_relay_pool_set_relays(rp, replacement, 1));
  }
  signet_relay_pool_free(rp);
  while (g_main_context_iteration(NULL, FALSE)) {}
  nostr_relay_free(relay);
}

static void stop_from_callback(const char *id, bool accepted, const char *reason, void *data) {
  (void)id; (void)accepted; (void)reason;
  signet_relay_pool_stop(data); /* must not wait for this callback's own ref */
}
static void test_callback_stop(void) {
  SignetRelayPool *rp = pool_new();
  NostrRelay *relay = relay_new();
  Result pending; result_init(&pending);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, stop_from_callback, rp);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_B, outcome, &pending);
  deliver(relay, ID_A, true);
  while (g_main_context_iteration(NULL, FALSE)) {}
  CHECK(pending.calls == 1 && pending.aborted);
  signet_relay_pool_free(rp);
  nostr_relay_free(relay);
  result_clear(&pending);
}

static void test_relay_cannot_spoof_transport_terminal(void) {
  SignetRelayPool *rp = pool_new();
  NostrRelay *relay = relay_new();
  Result result; result_init(&result);
  signet_relay_pool_watch_publish_ok(rp, relay, ID_A, outcome, &result);
  NostrEnvelope *env = nostr_envelope_parse("[\"OK\",\"" ID_A "\",false,\"transport-aborted:\"]");
  CHECK(env);
  nostr_relay_dispatch_control_envelope(relay, env);
  nostr_envelope_free(env);
  CHECK(result.calls == 1 && !result.accepted && !result.aborted);
  signet_relay_pool_free(rp);
  nostr_relay_free(relay);
  result_clear(&result);
}

int main(void) {
  test_relay_cannot_spoof_transport_terminal();
  test_callback_stop();
  test_queued_auth_secret_teardown(FALSE);
  test_queued_auth_secret_teardown(TRUE);
  test_overlap_and_once();
  test_generation_and_abort();
  test_concurrent_duplicate();
  puts("test_relay_pool_ok_lifetime: PASS (overlap, once, generation, abort, queued teardown)");
  return 0;
}
