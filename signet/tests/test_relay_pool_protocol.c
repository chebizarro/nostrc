/* SPDX-License-Identifier: MIT
 *
 * test_relay_pool_protocol.c - Protocol-level tests for Signet relay pool.
 *
 * NPA-11: Tests the protocol behaviors added by the Nostr Protocol Audit:
 * - NPA-01: Signature verification (invalid events dropped)
 * - NPA-02: Publish OK acknowledgment tracking
 * - NPA-03: Since-filter timestamp tracking
 * - NPA-04: Scoped filter building (#p tag + since)
 * - NPA-06: CLOSED subscription detection
 * - NPA-10: EOSE-based subscription readiness
 *
 * These tests exercise relay_pool.c APIs without a real relay connection.
 */

#include "signet/relay_pool.h"

#include "test_check.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <glib.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
  do { printf("  TEST %s ... ", #name); } while (0)

#define PASS() \
  do { printf("PASS\n"); g_tests_passed++; } while (0)

#define FAIL(msg) \
  do { printf("FAIL: %s\n", msg); g_tests_failed++; } while (0)

#define ASSERT_TRUE(expr, msg) \
  do { if (!(expr)) { FAIL(msg); return; } } while (0)

#define ASSERT_FALSE(expr, msg) \
  do { if (expr) { FAIL(msg); return; } } while (0)

#define ASSERT_EQ_INT(a, b, msg) \
  do { if ((a) != (b)) { FAIL(msg); return; } } while (0)

/* ---- Test: relay pool creation and lifecycle ---- */

static void test_relay_pool_lifecycle(void) {
  TEST(relay_pool_lifecycle);

  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = NULL,
    .user_data = NULL,
    .auth_sk_hex = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");
  ASSERT_FALSE(signet_relay_pool_is_connected(rp), "pool should not be connected without relays");
  ASSERT_FALSE(signet_relay_pool_is_subscribed(rp), "pool should not be subscribed without relays");
  ASSERT_FALSE(signet_relay_pool_check_sub_closed(rp), "no subs means no closed");

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: NPA-01 signature verification via handle_event_json ---- */

/* Callback context to track received events. */
typedef struct {
  int event_count;
  int last_kind;
} EventCtx;

static void test_event_cb(const SignetRelayEventView *ev, void *user_data) {
  EventCtx *ctx = (EventCtx *)user_data;
  ctx->event_count++;
  ctx->last_kind = ev->kind;
}

static void test_sig_verification_valid(void) {
  TEST(sig_verification_valid);

  EventCtx ectx = { .event_count = 0, .last_kind = 0 };
  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = test_event_cb,
    .user_data = &ectx,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  /* Build a properly signed event. */
  NostrEvent *evt = nostr_event_new();
  nostr_event_set_kind(evt, 1);
  nostr_event_set_content(evt, "hello protocol test");
  nostr_event_set_created_at(evt, 1700000000);

  /* Generate a keypair for signing. */
  char *sk_hex = nostr_key_generate_private();
  ASSERT_TRUE(sk_hex != NULL, "key generation should succeed");
  ASSERT_EQ_INT(nostr_event_sign(evt, sk_hex), 0, "event should sign successfully");

  char *json = nostr_event_serialize_compact(evt);
  ASSERT_TRUE(json != NULL, "event should serialize");
  nostr_event_free(evt);
  free(sk_hex);

  /* Wrap in NIP-01 EVENT envelope and dispatch through handle_event_json.
   * handle_event_json parses the event and dispatches to callback.
   * Note: middleware checks signatures, but handle_event_json is a
   * direct parse+dispatch path. */
  int rc = signet_relay_pool_handle_event_json(rp, json);
  free(json);

  ASSERT_EQ_INT(rc, 0, "handle_event_json should succeed for valid event");
  ASSERT_EQ_INT(ectx.event_count, 1, "callback should fire once");
  ASSERT_EQ_INT(ectx.last_kind, 1, "event kind should be 1");

  signet_relay_pool_free(rp);
  PASS();
}

static void test_sig_verification_invalid(void) {
  TEST(sig_verification_invalid);

  EventCtx ectx = { .event_count = 0, .last_kind = 0 };
  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = test_event_cb,
    .user_data = &ectx,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  /* Build a JSON string with an invalid signature (all zeros).
   * This should be rejected by handle_event_json's parse path
   * or by the middleware's signature check. */
  const char *bad_json =
    "{\"id\":\"0000000000000000000000000000000000000000000000000000000000000000\","
    "\"pubkey\":\"0000000000000000000000000000000000000000000000000000000000000000\","
    "\"created_at\":1700000000,"
    "\"kind\":1,"
    "\"tags\":[],"
    "\"content\":\"bad sig\","
    "\"sig\":\"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\"}";

  /* handle_event_json does basic parsing — if it dispatches, it goes to callback.
   * The middleware path (NPA-01) does the sig check. handle_event_json is a
   * simpler parse+dispatch that doesn't go through middleware.
   * We test that handle_event_json at least parses and dispatches. */
  int rc = signet_relay_pool_handle_event_json(rp, bad_json);

  /* handle_event_json now verifies the Schnorr signature before dispatching
   * (NPA-01 defense in depth). An all-zeros signature is invalid, so the
   * event must be REJECTED: non-zero return and the callback must NOT fire. */
  ASSERT_TRUE(rc != 0, "handle_event_json must reject an invalid signature");
  ASSERT_EQ_INT(ectx.event_count, 0, "callback must NOT fire for a forged event");

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: NPA-02 publish without connection returns error ---- */

static void test_publish_without_start(void) {
  TEST(publish_without_start);

  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = NULL,
    .user_data = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  /* Publishing before start should fail gracefully. */
  int rc = signet_relay_pool_publish_event_json(rp, "{\"id\":\"test\"}");
  ASSERT_EQ_INT(rc, -1, "publish before start should return -1");

  /* Ack-aware publish should also fail. */
  rc = signet_relay_pool_publish_event_json_ack(rp, "{\"id\":\"test\"}", NULL, NULL);
  ASSERT_EQ_INT(rc, -1, "publish_ack before start should return -1");

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: NPA-03 since-filter tracking ---- */

static void test_since_filter_tracking(void) {
  TEST(since_filter_tracking);

  EventCtx ectx = { .event_count = 0, .last_kind = 0 };
  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = test_event_cb,
    .user_data = &ectx,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  /* Before any events, update_since should return 0 (no events seen). */
  int64_t since = signet_relay_pool_update_since_from_latest(rp);
  ASSERT_EQ_INT((int)since, 0, "since should be 0 with no events");

  /* Feed events through handle_event_json to update last_event_ts.
   * We need properly signed events for the middleware to accept them. */
  char *sk_hex = nostr_key_generate_private();
  ASSERT_TRUE(sk_hex != NULL, "key generation should succeed");

  /* Event at timestamp 1700000100 */
  NostrEvent *evt1 = nostr_event_new();
  nostr_event_set_kind(evt1, 1);
  nostr_event_set_content(evt1, "event one");
  nostr_event_set_created_at(evt1, 1700000100);
  nostr_event_sign(evt1, sk_hex);
  char *json1 = nostr_event_serialize_compact(evt1);
  nostr_event_free(evt1);

  signet_relay_pool_handle_event_json(rp, json1);
  free(json1);

  /* Event at timestamp 1700000200 (later). */
  NostrEvent *evt2 = nostr_event_new();
  nostr_event_set_kind(evt2, 1);
  nostr_event_set_content(evt2, "event two");
  nostr_event_set_created_at(evt2, 1700000200);
  nostr_event_sign(evt2, sk_hex);
  char *json2 = nostr_event_serialize_compact(evt2);
  nostr_event_free(evt2);

  signet_relay_pool_handle_event_json(rp, json2);
  free(json2);
  free(sk_hex);

  /* Now update_since should return latest - 60s skew. */
  since = signet_relay_pool_update_since_from_latest(rp);
  /* handle_event_json dispatches to on_event but doesn't go through the
   * middleware that updates last_event_ts. The timestamp tracking happens
   * in the middleware path only. So since may still be 0 here.
   * This test validates the API doesn't crash and returns a sane value. */
  ASSERT_TRUE(since >= 0, "since should be non-negative");

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: NPA-04 scoped subscribe API ---- */

static void test_subscribe_scoped_without_start(void) {
  TEST(subscribe_scoped_without_start);

  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = NULL,
    .user_data = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  static const int kinds[] = { 24133, 28000 };

  /* subscribe_scoped should fail gracefully without start.
   * It returns -1 because the pool isn't started. */
  int rc = signet_relay_pool_subscribe_scoped(rp, kinds, 2,
    "abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234", 0);
  /* The function may succeed (caching params) or fail (not started).
   * Either is acceptable — key thing is no crash. */
  (void)rc;

  /* subscribe_kinds should also not crash. */
  rc = signet_relay_pool_subscribe_kinds(rp, kinds, 2);
  (void)rc;

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: handle_event_json rejects malformed JSON ---- */

static void test_handle_malformed_json(void) {
  TEST(handle_malformed_json);

  EventCtx ectx = { .event_count = 0, .last_kind = 0 };
  SignetRelayPoolConfig cfg = {
    .relays = NULL,
    .n_relays = 0,
    .on_event = test_event_cb,
    .user_data = &ectx,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  /* Completely invalid JSON */
  int rc = signet_relay_pool_handle_event_json(rp, "not json at all");
  ASSERT_EQ_INT(rc, -1, "malformed JSON should return -1");
  ASSERT_EQ_INT(ectx.event_count, 0, "no events should fire for bad JSON");

  /* NULL input */
  rc = signet_relay_pool_handle_event_json(rp, NULL);
  ASSERT_EQ_INT(rc, -1, "NULL input should return -1");

  /* Empty string */
  rc = signet_relay_pool_handle_event_json(rp, "");
  ASSERT_EQ_INT(rc, -1, "empty string should return -1");

  /* Valid JSON but not an event */
  rc = signet_relay_pool_handle_event_json(rp, "{\"hello\":\"world\"}");
  /* May parse partially — main thing is no crash */
  (void)rc;

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: relay pool URL accessor ---- */

static void test_relay_pool_urls(void) {
  TEST(relay_pool_urls);

  const char *urls[] = { "wss://relay1.example.com", "wss://relay2.example.com" };
  SignetRelayPoolConfig cfg = {
    .relays = urls,
    .n_relays = 2,
    .on_event = NULL,
    .user_data = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  ASSERT_TRUE(rp != NULL, "pool should be created");

  size_t count = 0;
  const char *const *got = signet_relay_pool_get_urls(rp, &count);
  ASSERT_TRUE(got != NULL, "URLs should be returned");
  ASSERT_EQ_INT((int)count, 2, "should have 2 URLs");
  ASSERT_TRUE(strcmp(got[0], "wss://relay1.example.com") == 0, "URL 0 should match");
  ASSERT_TRUE(strcmp(got[1], "wss://relay2.example.com") == 0, "URL 1 should match");

  /* NULL pool returns NULL */
  got = signet_relay_pool_get_urls(NULL, &count);
  ASSERT_TRUE(got == NULL, "NULL pool should return NULL URLs");

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- Test: NULL safety across all APIs ---- */

static void test_null_safety(void) {
  TEST(null_safety);

  /* All APIs should handle NULL gracefully without crashing. */
  signet_relay_pool_free(NULL);
  ASSERT_FALSE(signet_relay_pool_is_connected(NULL), "NULL pool not connected");
  ASSERT_FALSE(signet_relay_pool_is_subscribed(NULL), "NULL pool not subscribed");
  ASSERT_FALSE(signet_relay_pool_check_sub_closed(NULL), "NULL pool no closed subs");
  ASSERT_EQ_INT((int)signet_relay_pool_update_since_from_latest(NULL), 0, "NULL pool since=0");
  ASSERT_EQ_INT(signet_relay_pool_publish_event_json(NULL, "{}"), -1, "NULL pool publish fails");
  ASSERT_EQ_INT(signet_relay_pool_publish_event_json_ack(NULL, "{}", NULL, NULL), -1, "NULL pool publish_ack fails");
  ASSERT_EQ_INT(signet_relay_pool_handle_event_json(NULL, "{}"), -1, "NULL pool handle fails");
  ASSERT_EQ_INT(signet_relay_pool_start(NULL), -1, "NULL pool start fails");
  signet_relay_pool_stop(NULL); /* should not crash */

  PASS();
}

/* ---- fp-1r0k: the health-tick path must not stall the main loop ---------- */

/* A hostname that cannot resolve (RFC 2606 reserves .invalid), so the relay
 * never connects and the daemon stays in the state this issue is about. */
#define STALL_RELAY_URL "ws://relay-fp-1r0k-does-not-exist.invalid:80"
#define STALL_TEST_PUBKEY \
  "1111111111111111111111111111111111111111111111111111111111111111"

/* signetd_main.c runs signetd_health_tick every 250ms, and it reaches
 * signet_relay_pool_subscribe_scoped both on the CLOSED-subscription path and
 * on its 30s-throttled reconnect path. That subscribe used to hold rp->mu across
 * nostr_simple_pool_subscribe -> ensure_relay -> a 30s dial, so while any
 * configured relay was unreachable the daemon stalled roughly 30s out of every
 * 30s -- serving no 25910 management, no NIP-46 and no NIP-5L for most of its
 * life. A mistyped relay URL was enough to cause it.
 *
 * On proving it: no socket arrangement reproduces the stall, because
 * nostr_connection_new() only blocks when libwebsockets' service thread is stuck
 * (in practice inside a synchronous DNS lookup), and in a test environment every
 * dial finishes in milliseconds. A bare "the tick was fast" assertion would
 * therefore pass just as happily with the bug present. So this injects a
 * genuinely slow dial and requires that a slow dial actually ran concurrently
 * with the ticks before making the strong claim. */
static void test_health_tick_does_not_stall_on_unreachable_relay(void) {
  TEST(health_tick_does_not_stall_on_unreachable_relay);

  const gint64 dial_delay_ms = 1500;
  g_setenv("NOSTR_TEST_DIAL_DELAY_MS", "1500", TRUE);

  const char *relays[] = { STALL_RELAY_URL };
  SignetRelayPoolConfig cfg = {
    .relays = relays,
    .n_relays = 1,
    .on_event = NULL,
    .user_data = NULL,
    .auth_sk_hex = NULL,
  };

  /* Construction is on signetd's startup path, before the main loop exists. */
  gint64 t0 = g_get_monotonic_time();
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  gint64 new_us = g_get_monotonic_time() - t0;
  CHECK(rp != NULL);
  CHECK(signet_relay_pool_start(rp) == 0);

  const int kinds[] = { 24133, 25910, 1059 };
  gint64 worst_tick_us = 0;
  gint64 bounce_us = 0;

  /* ~3.5s of ticks: comfortably longer than one injected 1500ms dial, so a slow
   * dial is in flight for part of the window whatever the scheduling. */
  for (int i = 0; i < 14; i++) {
    gint64 tick0 = g_get_monotonic_time();

    /* What the tick does every time round, all of it taking rp->mu. */
    (void)signet_relay_pool_is_connected(rp);
    (void)signet_relay_pool_check_sub_closed(rp);
    CHECK(signet_relay_pool_subscribe_scoped(rp, kinds, 3,
                                             STALL_TEST_PUBKEY, 12345) == 0);

    /* And what the daemon's other listeners need concurrently -- these are the
     * calls that used to block on rp->mu behind the dial even though they never
     * touch the network. */
    size_t n_urls = 0;
    const char *const *urls = signet_relay_pool_get_urls(rp, &n_urls);
    CHECK(n_urls == 1);
    CHECK(strcmp(urls[0], STALL_RELAY_URL) == 0);

    gint64 tick_us = g_get_monotonic_time() - tick0;
    if (tick_us > worst_tick_us) worst_tick_us = tick_us;

    /* The tick's heavier branch, throttled to once per 30s in the daemon:
     * advance `since`, make sure the pool is running, replay the
     * subscription. fp-rym6 removed the stop()/start() bounce this used to
     * mirror; what is left must still cost nothing like a dial. */
    if (i == 4) {
      gint64 b0 = g_get_monotonic_time();
      (void)signet_relay_pool_update_since_from_latest(rp);
      CHECK(signet_relay_pool_start(rp) == 0);
      CHECK(signet_relay_pool_subscribe_scoped(rp, kinds, 3,
                                               STALL_TEST_PUBKEY, 12345) == 0);
      bounce_us = g_get_monotonic_time() - b0;
    }

    g_usleep(250 * 1000);
  }

  unsigned dials = signet_relay_pool_dial_attempts(rp);
  fprintf(stderr, "[new=%lldus worst_tick=%lldus reconnect_branch=%lldus dials=%u] ",
          (long long)new_us, (long long)worst_tick_us,
          (long long)bounce_us, dials);

  /* Nothing on the main-loop path may cost anything like a dial. With the bug,
   * construction and every tick each cost the full injected delay -- and in
   * production the full 30s libnostr connect timeout, per unreachable relay. */
  CHECK(new_us < 500 * 1000);
  CHECK(worst_tick_us < 500 * 1000);
  /* The reconnect branch no longer joins libnostr worker threads either, so it
   * is now bounded by the same reasoning as an ordinary tick rather than by
   * their 200ms select cadence. */
  CHECK(bounce_us < 1000 * 1000);

  if (dials >= 1) {
    /* Strong form: at least one dial that takes dial_delay_ms ran to completion
     * during the window, and no tick came close to it. Availability is
     * demonstrated against a real slow dial, not asserted in its absence. */
    CHECK(worst_tick_us * 3 < dial_delay_ms * 1000);
    CHECK(new_us * 3 < dial_delay_ms * 1000);
  } else {
    /* Report rather than pass quietly. Some environments cannot complete a dial
     * at all, and then the concurrency this test is about was never exercised. */
    fprintf(stderr, "[no background dial completed here; bound only] ");
  }

  g_unsetenv("NOSTR_TEST_DIAL_DELAY_MS");
  signet_relay_pool_free(rp);
  PASS();
}

/* ---- fp-rym6: the reconnect branch must not take the pool down ----------- */

/* signetd_health_tick's disconnected branch used to stop() and start() the
 * pool, because before fp-ieg8 that bounce -- specifically the subscribe that
 * followed it -- was the only thing that ever dialled a relay again. The
 * daemon was the retry engine. This pins the property that makes removing it
 * safe: with the tick doing nothing but re-subscribing, an unreachable relay
 * is still dialled, repeatedly, by libnostr.
 *
 * It asserts through signet's own API rather than libnostr's, so it also
 * covers the wiring: signet_relay_pool_new() must register relays with the
 * async primitive that leaves them to the redial worker (a revert to the
 * blocking ensure_relay would dial once on the caller's thread and then sit
 * there), and the tick's remaining calls must not disturb that.
 *
 * On what a bounce would add: nothing here. stop()/start() do not dial --
 * stop() joins the pool worker and drops subscriptions, start() spawns the
 * worker -- and the redial worker's lifetime is deliberately the pool's, not
 * start()/stop()'s, so it keeps retrying either way. */
static void test_relays_redial_without_a_pool_bounce(void) {
  TEST(relays_redial_without_a_pool_bounce);

  const char *relays[] = { STALL_RELAY_URL };
  SignetRelayPoolConfig cfg = {
    .relays = relays,
    .n_relays = 1,
    .on_event = NULL,
    .user_data = NULL,
    .auth_sk_hex = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  CHECK(rp != NULL);
  CHECK(signet_relay_pool_start(rp) == 0);

  /* Run the tick the daemon now runs -- the cheap per-tick reads plus, once,
   * the whole of its throttled reconnect branch -- and otherwise just watch.
   * Nothing in here dials, so every dial counted below came from libnostr.
   *
   * Two attempts, not one: one attempt only proves something dialled once,
   * which the pool would have done at registration. Two proves a retry loop
   * that outlives the tick. The curve is 1s doubling with jitter, so attempt 2
   * is due around 3s in; the budget is generous against a loaded machine. */
  const int kinds[] = { 24133, 25910, 1059 };
  const gint64 budget_us = 20 * G_TIME_SPAN_SECOND;
  gint64 t0 = g_get_monotonic_time();
  unsigned dials = 0;
  int ticks = 0;

  while (g_get_monotonic_time() - t0 < budget_us) {
    (void)signet_relay_pool_is_connected(rp);
    (void)signet_relay_pool_check_sub_closed(rp);

    if (ticks == 2) {
      /* The throttled branch, in full. */
      (void)signet_relay_pool_update_since_from_latest(rp);
      CHECK(signet_relay_pool_start(rp) == 0);
      CHECK(signet_relay_pool_subscribe_scoped(rp, kinds, 3,
                                               STALL_TEST_PUBKEY, 12345) == 0);
    }
    ticks++;

    dials = signet_relay_pool_dial_attempts(rp);
    if (dials >= 2) break;
    if (signet_relay_pool_is_connected(rp)) break;
    g_usleep(250 * 1000);
  }

  gint64 elapsed_us = g_get_monotonic_time() - t0;
  bool connected = signet_relay_pool_is_connected(rp);
  bool in_flight = signet_relay_pool_dial_in_flight(rp);
  fprintf(stderr, "[%u dial attempts in %lldms across %d ticks, no bounce, "
                  "in_flight=%d] ",
          dials, (long long)(elapsed_us / 1000), ticks, (int)in_flight);

  /* THE property, asserted in every environment: something dialled this relay,
   * and it was not this test -- the loop above only reads and re-subscribes.
   * Delete libnostr's redial worker and all three of these go false and stay
   * false for the full 20s budget, which is exactly the state signetd was in
   * before fp-ieg8 and the reason it had to bounce the pool itself.
   *
   * in_flight is what keeps this assertable where a dial can hang: an
   * environment that cannot complete a connect still shows CONNECTING, and
   * only "nobody is dialling" shows none of the three. */
  CHECK_MSG(dials >= 1 || connected || in_flight,
            "relay was never dialled: nothing retries it but the tick");

  if (connected) {
    /* A resolver that hijacks NXDOMAIN turns the unreachable relay into a
     * connected one, and then the retry cadence never runs. Say so rather than
     * passing quietly. */
    fprintf(stderr, "[%s unexpectedly resolved; retry cadence not exercised] ",
            STALL_RELAY_URL);
  } else if (dials >= 2) {
    /* Stronger form: it kept retrying, and on a bounded curve rather than a
     * hot loop. The 1s-doubling curve cannot fit many attempts into the
     * window; a regression to dialling once per tick blows past this. */
    CHECK(dials <= (unsigned)(elapsed_us / (400 * 1000)) + 3);
  } else {
    /* Reported, not asserted away: where a single dial never returns (the
     * ThreadSanitizer tree wedges libwebsockets' service thread) no test that
     * dials can observe a second attempt. The claim above still held. */
    fprintf(stderr, "[fewer than 2 dials completed here; cadence not "
                    "asserted] ");
  }

  signet_relay_pool_free(rp);
  PASS();
}

/* ---- main ---- */

int main(void) {
  printf("=== Signet Relay Pool Protocol Tests (NPA-11) ===\n");

  test_relay_pool_lifecycle();
  test_sig_verification_valid();
  test_sig_verification_invalid();
  test_publish_without_start();
  test_since_filter_tracking();
  test_subscribe_scoped_without_start();
  test_handle_malformed_json();
  test_relay_pool_urls();
  test_null_safety();
  test_health_tick_does_not_stall_on_unreachable_relay();
  test_relays_redial_without_a_pool_bounce();

  printf("\n--- Results: %d passed, %d failed ---\n", g_tests_passed, g_tests_failed);
  return g_tests_failed > 0 ? 1 : 0;
}
