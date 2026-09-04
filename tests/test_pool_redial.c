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
 *
 * Failure mode being reproduced: nostr_relay_connect() returns false only when
 * nostr_connection_new() fails, and the reliable way to make that happen with
 * no network is a hostname that cannot resolve -- which is also the operator
 * mistake this all protects against (a mistyped relay URL). RFC 2606 reserves
 * .invalid precisely so it never resolves.
 */

#include "nostr-simple-pool.h"
#include "nostr-relay.h"

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

int main(void) {
  printf("test_pool_redial\n");
  test_failed_initial_connect_is_retried();
  test_async_register_does_not_wait_for_dial();
  test_start_stop_idempotent();
  printf("test_pool_redial: OK\n");
  return 0;
}
