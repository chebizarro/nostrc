/* SPDX-License-Identifier: MIT
 *
 * test_relay_pool_ok_lifetime.c - nostrc-tw7f: the data of signet's per-relay
 * OK callbacks outlives every call already running when the callback removes
 * itself.
 *
 * libnostr copies a callback under the relay mutex and calls it after
 * unlocking, so removing a callback does not wait for a call already running
 * on another worker (nostrc-flp7). Signet's publish-ack and AUTH OK callbacks
 * removed themselves and then g_free()d their data; a duplicate OK handled
 * concurrently on another thread then read the freed data. They now hand the
 * data to the relay with nostr_relay_set_ok_callback_full().
 *
 * Offline and deterministic, driven through libnostr's control-frame hook
 * (relay-private.h), as libnostr's test_relay_callback_lifetime does. One OK
 * parks mid-call (in the publish callback, or in the AUTH callback's log line
 * via a GLib log writer) while a duplicate OK runs the callback to its end,
 * which removes it; the parked call then resumes and reads its data. The old
 * code freed that data: a heap-use-after-free under ASan, and a crash without
 * it because ctest runs this with MallocScribble / MALLOC_PERTURB_. Every wait
 * is bounded; the bound is a failure, never a pass.
 */

#include "relay_pool_private.h"
#include "test_check.h"

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <nostr-envelope.h>
#include <nostr-relay.h>
#include "relay-private.h"

#define WAIT_BOUND_US (10 * G_USEC_PER_SEC)
#define EVENT_ID "0000000000000000000000000000000000000000000000000000000000007f37"

/* A call that parks when armed, until released. */
typedef struct {
  GMutex mu;
  GCond cv;
  gboolean armed;     /* the next matching call parks */
  gboolean entered;
  gboolean released;
  int calls;          /* matching calls seen */
} Gate;

static void gate_init(Gate *g) {
  memset(g, 0, sizeof *g);
  g_mutex_init(&g->mu);
  g_cond_init(&g->cv);
}

static void gate_clear(Gate *g) {
  g_mutex_clear(&g->mu);
  g_cond_clear(&g->cv);
}

/* Counts a matching call; parks it if the gate is armed (once). */
static void gate_pass(Gate *g) {
  g_mutex_lock(&g->mu);
  g->calls++;
  if (g->armed) {
    g->armed = FALSE;
    g->entered = TRUE;
    g_cond_broadcast(&g->cv);
    gint64 end = g_get_monotonic_time() + WAIT_BOUND_US;
    while (!g->released)
      CHECK(g_cond_wait_until(&g->cv, &g->mu, end));
  }
  g_mutex_unlock(&g->mu);
}

static void gate_wait_entered(Gate *g) {
  gint64 end = g_get_monotonic_time() + WAIT_BOUND_US;
  g_mutex_lock(&g->mu);
  while (!g->entered)
    CHECK(g_cond_wait_until(&g->cv, &g->mu, end));
  g_mutex_unlock(&g->mu);
}

static void gate_release(Gate *g) {
  g_mutex_lock(&g->mu);
  g->released = TRUE;
  g_cond_broadcast(&g->cv);
  g_mutex_unlock(&g->mu);
}

static int gate_calls(Gate *g) {
  g_mutex_lock(&g->mu);
  int n = g->calls;
  g_mutex_unlock(&g->mu);
  return n;
}

static NostrRelay *relay_new(void) {
  Error *err = NULL;
  NostrRelay *relay = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
  CHECK(relay != NULL && err == NULL);
  nostr_relay_set_auto_reconnect(relay, false);
  return relay;
}

/* Delivers ["OK", EVENT_ID, ok, ...] the way the relay's worker does. */
static void deliver_ok(NostrRelay *relay, bool ok) {
  char frame[160];
  g_snprintf(frame, sizeof frame, "[\"OK\",\"%s\",%s,\"%s\"]", EVENT_ID,
             ok ? "true" : "false", ok ? "" : "blocked: test");
  NostrEnvelope *env = nostr_envelope_parse(frame);
  CHECK(env != NULL);
  nostr_relay_dispatch_control_envelope(relay, env);
  nostr_envelope_free(env);
}

typedef struct { NostrRelay *relay; bool ok; } Delivery;

static void *deliver_thread(void *arg) {
  Delivery *d = arg;
  deliver_ok(d->relay, d->ok);
  return NULL;
}

/* ---- publish OK ----------------------------------------------------------- */

static Gate publish_gate;

static void on_publish_ok(const char *event_id, bool accepted, const char *reason,
                          void *user_data) {
  (void)accepted; (void)reason;
  CHECK(user_data == &publish_gate);
  CHECK(strcmp(event_id, EVENT_ID) == 0);
  gate_pass(&publish_gate);
}

static void test_publish_ok_parked_while_removed(void) {
  NostrRelay *relay = relay_new();
  gate_init(&publish_gate);
  signet_relay_pool_watch_publish_ok(relay, EVENT_ID, on_publish_ok, &publish_gate);

  publish_gate.armed = TRUE;
  pthread_t worker;
  Delivery first = { relay, true };
  CHECK(pthread_create(&worker, NULL, deliver_thread, &first) == 0);
  gate_wait_entered(&publish_gate);

  /* A duplicate OK runs the callback to its end: it removes itself. */
  deliver_ok(relay, true);
  CHECK(gate_calls(&publish_gate) == 2);

  /* The parked call resumes and reaches its own removal, reading its data. */
  gate_release(&publish_gate);
  CHECK(pthread_join(worker, NULL) == 0);

  deliver_ok(relay, true); /* removed: not called again */
  CHECK(gate_calls(&publish_gate) == 2);
  nostr_relay_free(relay);
  gate_clear(&publish_gate);
  printf("ok - publish OK callback removed while a call is parked\n");
}

/* A replaced or never-answered registration is released by the relay. */
static void test_publish_ok_replaced_and_freed(void) {
  NostrRelay *relay = relay_new();
  gate_init(&publish_gate);
  signet_relay_pool_watch_publish_ok(relay, EVENT_ID, on_publish_ok, &publish_gate);
  signet_relay_pool_watch_publish_ok(relay, EVENT_ID, on_publish_ok, &publish_gate);
  deliver_ok(relay, false);
  CHECK(gate_calls(&publish_gate) == 1);
  signet_relay_pool_watch_publish_ok(relay, EVENT_ID, on_publish_ok, &publish_gate);
  nostr_relay_free(relay);
  CHECK(gate_calls(&publish_gate) == 1);
  gate_clear(&publish_gate);
  printf("ok - replaced and unanswered publish OK registrations released\n");
}

/* ---- AUTH OK -------------------------------------------------------------- */

/* The AUTH OK callback has no caller-supplied code to park in: park it in its
 * "auth-ok: ... OK=true" log line, which comes before its data is read again. */
static Gate auth_gate;
static int auth_rejected_logs;

static GLogWriterOutput test_log_writer(GLogLevelFlags level, const GLogField *fields,
                                        gsize n_fields, gpointer user_data) {
  (void)user_data;
  const char *msg = NULL;
  for (gsize i = 0; i < n_fields; i++)
    if (strcmp(fields[i].key, "MESSAGE") == 0) msg = fields[i].value;
  if (msg && strstr(msg, "auth-ok:") && strstr(msg, "event=" EVENT_ID)) {
    if (strstr(msg, "OK=true")) gate_pass(&auth_gate);
    else if (strstr(msg, "OK=false")) g_atomic_int_inc(&auth_rejected_logs);
    return G_LOG_WRITER_HANDLED;
  }
  if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
    return g_log_writer_default(level, fields, n_fields, user_data);
  return G_LOG_WRITER_HANDLED; /* keep the test output readable */
}

static void test_auth_ok_parked_while_removed(void) {
  SignetRelayPoolConfig cfg = { 0 };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  CHECK(rp != NULL);
  NostrRelay *relay = relay_new();
  gate_init(&auth_gate);
  signet_relay_pool_watch_auth_ok(rp, relay, EVENT_ID, "wss://relay.test.invalid");

  auth_gate.armed = TRUE;
  pthread_t worker;
  Delivery accepted = { relay, true };
  CHECK(pthread_create(&worker, NULL, deliver_thread, &accepted) == 0);
  gate_wait_entered(&auth_gate);

  /* A rejection for the same AUTH event runs the callback to its end. */
  deliver_ok(relay, false);
  CHECK(g_atomic_int_get(&auth_rejected_logs) == 1);

  gate_release(&auth_gate);
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(gate_calls(&auth_gate) == 1);

  /* The accepted call scheduled the re-subscribe on the default context,
   * with its own copy of what it needs. */
  while (g_main_context_iteration(NULL, FALSE)) {}

  deliver_ok(relay, true); /* removed: not called again */
  CHECK(gate_calls(&auth_gate) == 1);
  nostr_relay_free(relay);
  signet_relay_pool_free(rp);
  gate_clear(&auth_gate);
  printf("ok - AUTH OK callback removed while a call is parked\n");
}

int main(void) {
  g_log_set_writer_func(test_log_writer, NULL, NULL);
  test_publish_ok_parked_while_removed();
  test_publish_ok_replaced_and_freed();
  test_auth_ok_parked_while_removed();
  printf("test_relay_pool_ok_lifetime: PASS\n");
  return 0;
}
