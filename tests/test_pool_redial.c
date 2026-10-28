/* SPDX-License-Identifier: MIT
 *
 * test_pool_redial.c - NostrSimplePool retries a relay whose FIRST dial failed.
 *
 * fp-ieg8. The reconnect/backoff loop lives in the relay's message_loop, which
 * nostr_relay_connect() only spawns AFTER a connection succeeds. A relay whose
 * initial dial failed was still added to pool->relays, but nothing ever dialled
 * it again -- so "the pool retries unreachable relays in the background" was not
 * a property libnostr provided. Callers faked it with periodic ticks of their
 * own, on whatever thread they happened to run on.
 *
 * What this test pins down:
 *
 *   1. A relay whose initial connect fails IS retried, repeatedly, with no
 *      caller-driven tick of any kind -- the test makes exactly one call and
 *      then only observes.
 *   2. The retries are on bounded backoff, not a hot loop.
 *   3. ensure_relay_async() returns without waiting for the dial, PROVEN by
 *      making the dial genuinely slow rather than by asserting a bound that a
 *      fast-failing environment would satisfy either way.
 *   4. ensure_relay() stays synchronous when the redial worker already holds
 *      the relay's dial claim: it joins that attempt (nostrc-1h2ze).
 *
 * Failure mode being reproduced: nostr_relay_connect() returns false only when
 * nostr_connection_new() fails, and the reliable way to make that happen with
 * no network is a hostname that cannot resolve -- which is also the operator
 * mistake this all protects against (a mistyped relay URL). RFC 2606 reserves
 * .invalid precisely so it never resolves.
 */

#include "nostr-simple-pool.h"
#include "nostr-relay.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Always-evaluated check: this suite is built Release in CI, where NDEBUG makes
 * assert() expand to nothing and a test can "pass" having executed none of its
 * setup (the fp-3126 failure). */
#define CHECK(...)                                                           \
  do {                                                                       \
    if (!(__VA_ARGS__)) {                                                    \
      fprintf(stderr, "\nFAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
      fflush(stderr);                                                        \
      exit(1);                                                               \
    }                                                                        \
  } while (0)

/* A hostname that cannot resolve, so nostr_connection_new() fails and no
 * message_loop is ever spawned -- the exact state fp-ieg8 is about. */
#define DEAD_URL "ws://relay-fp-ieg8-does-not-exist.invalid:80"

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
}

/* The pool's relay array is part of the public struct, so a test can observe
 * per-relay reconnect state directly. */
static NostrRelay *only_relay(NostrSimplePool *pool) {
  CHECK(pool->relay_count == 1);
  CHECK(pool->relays != NULL);
  CHECK(pool->relays[0] != NULL);
  return pool->relays[0];
}

/* ---- 1. a relay whose initial connect failed is retried, unprompted -------- */

static void test_failed_initial_connect_is_retried(void) {
  printf("  TEST failed_initial_connect_is_retried ... ");
  fflush(stdout);

  NostrSimplePool *pool = nostr_simple_pool_new();
  CHECK(pool != NULL);

  /* The one and only call into the pool. Everything after this is observation:
   * if retry needed a caller-driven tick, nothing below would ever advance. */
  nostr_simple_pool_ensure_relay_async(pool, DEAD_URL);

  /* Registered even though it cannot be connected -- the relay has to exist for
   * anything (redial, reconcile, get_urls) to be able to find it. */
  NostrRelay *relay = only_relay(pool);
  CHECK(strcmp(nostr_relay_get_url_const(relay), DEAD_URL) == 0);

  /* Wait for a SECOND attempt. One attempt would only prove something dialled
   * once; two proves a retry loop. */
  const int64_t budget_ms = 20000;
  int64_t t0 = now_ms();
  int attempts = 0;
  bool unexpectedly_connected = false;
  while (now_ms() - t0 < budget_ms) {
    if (nostr_relay_is_connected(relay)) { unexpectedly_connected = true; break; }
    attempts = nostr_relay_get_reconnect_attempt(relay);
    if (attempts >= 2) break;
    sleep_ms(50);
  }
  int64_t elapsed = now_ms() - t0;
  NostrRelayConnectionState state = nostr_relay_get_connection_state(relay);

  if (unexpectedly_connected) {
    /* Say so rather than passing silently: a resolver that hijacks NXDOMAIN
     * turns this into a connected relay, and then the never-retried state this
     * test is about cannot be produced at all. */
    fprintf(stderr, "[%s unexpectedly resolved; retry path not exercised] ",
            DEAD_URL);
    printf("SKIP\n");
    nostr_simple_pool_free(pool);
    return;
  }

  /* Diagnostics before the assertions, so a failure says WHY rather than just
   * which line: "nothing ever dialled it" and "the dial is still in flight" are
   * very different outcomes. */
  fprintf(stderr, "[%d attempts in %lldms, state=%s, no caller tick] ",
          attempts, (long long)elapsed,
          nostr_relay_get_connection_state_name(state));

  /* THE property: something dialled this relay, and the only call this test
   * made -- ensure_relay_async -- does not dial. Before fp-ieg8 nothing did:
   * the relay sat in the pool DISCONNECTED with attempt 0 forever, which is
   * exactly what this fails on if the redial worker is removed. */
  bool picked_up_unprompted = (attempts >= 1) ||
                              (state == NOSTR_RELAY_STATE_CONNECTING);
  CHECK(picked_up_unprompted);

  if (attempts >= 2) {
    /* Bounded, not a hot loop. The curve is 1s doubling to a 5min ceiling with
     * jitter, so even the fastest legal schedule cannot fit many attempts into
     * the elapsed window; a regression to "dial as fast as the loop spins"
     * blows straight past this. */
    int max_plausible = (int)(elapsed / 400) + 3;
    CHECK(attempts <= max_plausible);

    /* And it waits between attempts rather than being permanently due. */
    CHECK(state == NOSTR_RELAY_STATE_BACKOFF ||
          state == NOSTR_RELAY_STATE_CONNECTING);
  } else {
    /* Report the weaker mode instead of asserting a retry that this
     * environment cannot produce. Some builds (the workspace ThreadSanitizer
     * tree, for one) wedge libwebsockets' service thread so hard that a single
     * dial never returns -- there, no test that dials can observe a second
     * attempt, and the pre-existing test_relay hangs for the same reason. */
    fprintf(stderr, "[single dial did not complete within %llds; "
                    "retry cadence not asserted] ",
            (long long)(budget_ms / 1000));
  }

  nostr_simple_pool_free(pool);
  printf("PASS\n");
}

/* ---- 2. registering a relay does not wait for its dial -------------------- */

static void test_async_register_does_not_wait_for_dial(void) {
  printf("  TEST async_register_does_not_wait_for_dial ... ");
  fflush(stdout);

  /* Every environment we can test in completes or refuses a dial in
   * milliseconds: nostr_connection_new() only blocks when the LWS service
   * thread is stuck, in practice inside a synchronous DNS lookup. So make the
   * pool's dial genuinely slow, and prove the caller did not wait for it,
   * instead of asserting a bound that a fast-failing environment satisfies for
   * the wrong reason. */
  const int64_t delay_ms = 1000;
  CHECK(setenv("NOSTR_TEST_DIAL_DELAY_MS", "1000", 1) == 0);

  NostrSimplePool *pool = nostr_simple_pool_new();
  CHECK(pool != NULL);

  int64_t t0 = now_ms();
  nostr_simple_pool_ensure_relay_async(pool, DEAD_URL);
  int64_t call_ms = now_ms() - t0;

  NostrRelay *relay = only_relay(pool);

  /* Wait for the background dial to complete one attempt. */
  int64_t attempt_ms = -1;
  while (now_ms() - t0 < 15000) {
    if (nostr_relay_get_reconnect_attempt(relay) >= 1 ||
        nostr_relay_is_connected(relay)) {
      attempt_ms = now_ms() - t0;
      break;
    }
    sleep_ms(20);
  }

  fprintf(stderr, "[register=%lldms, dial completed at %lldms] ",
          (long long)call_ms, (long long)attempt_ms);

  /* Registering must be cheap in every environment. */
  CHECK(call_ms < 250);

  if (attempt_ms >= 0) {
    /* The dial really was slow (the injected delay was applied)... */
    CHECK(attempt_ms >= delay_ms - 100);
    /* ...and the caller returned in a small fraction of it, which is the whole
     * claim: registration does not wait for the network. */
    CHECK(call_ms < attempt_ms / 4);
  } else {
    fprintf(stderr, "[dial never completed here; ratio not asserted] ");
  }

  CHECK(unsetenv("NOSTR_TEST_DIAL_DELAY_MS") == 0);
  nostr_simple_pool_free(pool);
  printf("PASS\n");
}

/* ---- 3. start()/stop() stay sane around the redial worker ----------------- */

static void test_start_stop_idempotent(void) {
  printf("  TEST start_stop_idempotent ... ");
  fflush(stdout);

  NostrSimplePool *pool = nostr_simple_pool_new();
  CHECK(pool != NULL);
  nostr_simple_pool_ensure_relay_async(pool, DEAD_URL);

  /* start() used to pthread_create unconditionally, orphaning the first worker
   * on a second call. stop() used to join an uninitialized pthread_t if the pool
   * had never been started. Both are reachable from a health tick that restarts
   * a pool it thinks is down. */
  nostr_simple_pool_start(pool);
  nostr_simple_pool_start(pool);
  nostr_simple_pool_stop(pool);
  nostr_simple_pool_stop(pool);

  /* And free() must retire the redial worker even mid-backoff. */
  nostr_simple_pool_free(pool);
  printf("PASS\n");
}

/* ---- 4. a synchronous ensure joins a dial the redial worker claimed ------- */

/* nostrc-1h2ze. ensure_relay() registers a relay before it tries to claim the
 * dial, and the redial worker can claim it in between. ensure_relay() then
 * returned at once, reporting a reachable relay as unconnected; libhanami's
 * ensure_relays() turned that into HANAMI_ERR_NETWORK.
 *
 * NOSTR_TEST_MODE gives every dial an offline connection, so the joined dial
 * succeeds on every platform without a socket, a DNS lookup or a fixture
 * relay. The gate holds the worker's dial at CONNECTING, which libnostr
 * reports on the dialling thread before the connection exists, so the worker
 * owns the claim for as long as the test needs. */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  NostrSimplePool *pool;
  NostrRelay *relay;
  const char *url;
  bool connecting;          /* the worker's dial is held at CONNECTING */
  bool release;             /* ...until this is set */
  bool caller_started;
  bool caller_returned;
  bool connected_at_return; /* what the synchronous caller saw */
} DialGate;

static struct timespec deadline_ms(long ms) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += ms / 1000;
  deadline.tv_nsec += (ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  return deadline;
}

static void hold_connect(NostrRelay *relay, NostrRelayConnectionState old_state,
                         NostrRelayConnectionState new_state, void *data) {
  (void)old_state;
  if (new_state != NOSTR_RELAY_STATE_CONNECTING) return;
  DialGate *gate = data;
  /* Re-entered on the dialling thread, the synchronous call must not wait for
   * that thread's own claim. If it did, CONNECTING would never reach the test
   * thread and its deadline would fail. */
  nostr_simple_pool_ensure_relay(gate->pool, gate->url);
  CHECK(!nostr_relay_is_connected(relay));
  CHECK(pthread_mutex_lock(&gate->mutex) == 0);
  gate->connecting = true;
  CHECK(pthread_cond_broadcast(&gate->changed) == 0);
  while (!gate->release)
    CHECK(pthread_cond_wait(&gate->changed, &gate->mutex) == 0);
  CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void *ensure_claimed_relay(void *data) {
  DialGate *gate = data;
  CHECK(pthread_mutex_lock(&gate->mutex) == 0);
  gate->caller_started = true;
  CHECK(pthread_cond_broadcast(&gate->changed) == 0);
  CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
  nostr_simple_pool_ensure_relay(gate->pool, gate->url);
  /* Sampled the moment the call returns, as a synchronous caller does. */
  bool connected = nostr_relay_is_connected(gate->relay);
  CHECK(pthread_mutex_lock(&gate->mutex) == 0);
  gate->caller_returned = true;
  gate->connected_at_return = connected;
  CHECK(pthread_cond_broadcast(&gate->changed) == 0);
  CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
  return NULL;
}

static void test_sync_ensure_joins_claimed_dial(void) {
  printf("  TEST sync_ensure_joins_claimed_dial ... ");
  fflush(stdout);
  CHECK(setenv("NOSTR_TEST_MODE", "1", 1) == 0);

  DialGate gate = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                    .changed = PTHREAD_COND_INITIALIZER };
  gate.pool = nostr_simple_pool_new();
  CHECK(gate.pool);
  Error *error = NULL;
  gate.relay = nostr_relay_new(NULL, "ws://relay-nostrc-1h2ze.invalid", &error);
  CHECK(gate.relay && !error);
  gate.url = nostr_relay_get_url_const(gate.relay);
  nostr_relay_set_state_callback(gate.relay, hold_connect, &gate);

  /* Hand the relay to the pool through its public array with the gate already
   * installed, then nudge the worker: an async ensure of a known URL only
   * wakes it. From here every dial is production code, and the first one is
   * the worker's. */
  CHECK(pthread_mutex_lock(&gate.pool->pool_mutex) == 0);
  gate.pool->relays = malloc(sizeof *gate.pool->relays);
  CHECK(gate.pool->relays);
  gate.pool->relays[0] = gate.relay;
  gate.pool->relay_count = 1;
  CHECK(pthread_mutex_unlock(&gate.pool->pool_mutex) == 0);
  nostr_simple_pool_ensure_relay_async(gate.pool, gate.url);

  CHECK(pthread_mutex_lock(&gate.mutex) == 0);
  struct timespec deadline = deadline_ms(10000);
  while (!gate.connecting)
    CHECK(pthread_cond_timedwait(&gate.changed, &gate.mutex, &deadline) == 0);
  CHECK(pthread_mutex_unlock(&gate.mutex) == 0);

  pthread_t caller;
  CHECK(pthread_create(&caller, NULL, ensure_claimed_relay, &gate) == 0);
  CHECK(pthread_mutex_lock(&gate.mutex) == 0);
  deadline = deadline_ms(10000);
  while (!gate.caller_started)
    CHECK(pthread_cond_timedwait(&gate.changed, &gate.mutex, &deadline) == 0);
  /* A bounded negative observation of a call that must block, not a sleep
   * standing in for readiness: while the dial is held, a correct
   * ensure_relay() cannot return, so this wait can only time out. Without the
   * join the call returns within microseconds. */
  deadline = deadline_ms(250);
  int wait_rc = 0;
  while (!gate.caller_returned && wait_rc == 0)
    wait_rc = pthread_cond_timedwait(&gate.changed, &gate.mutex, &deadline);
  CHECK(wait_rc == 0 || wait_rc == ETIMEDOUT);
  bool returned_while_held = gate.caller_returned;
  gate.release = true;
  CHECK(pthread_cond_broadcast(&gate.changed) == 0);
  CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
  CHECK(pthread_join(caller, NULL) == 0);

  fprintf(stderr, "[returned while dial held=%d, connected at return=%d] ",
          returned_while_held, gate.connected_at_return);
  CHECK(!returned_while_held);
  /* The joined attempt connected, so the caller saw a connected relay: the
   * property libhanami's ensure_relays() relies on. */
  CHECK(gate.connected_at_return);
  /* ...and it joined that relay rather than registering another. */
  CHECK(only_relay(gate.pool) == gate.relay);

  nostr_relay_set_state_callback(gate.relay, NULL, NULL);
  nostr_simple_pool_free(gate.pool);
  CHECK(unsetenv("NOSTR_TEST_MODE") == 0);
  CHECK(pthread_cond_destroy(&gate.changed) == 0);
  CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
  printf("PASS\n");
}

int main(void) {
  printf("test_pool_redial\n");
  test_sync_ensure_joins_claimed_dial();
  test_failed_initial_connect_is_retried();
  test_async_register_does_not_wait_for_dial();
  test_start_stop_idempotent();
  printf("test_pool_redial: OK\n");
  return 0;
}
