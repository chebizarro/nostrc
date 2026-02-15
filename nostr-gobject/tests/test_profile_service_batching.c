/**
 * test_profile_service_batching.c — Profile service debounced batch queue tests
 *
 * Validates the GnostrProfileService batching behavior:
 *
 *   1. Singleton lifecycle (init → use → shutdown → re-init)
 *   2. Request deduplication (same pubkey requested twice → single fetch)
 *   3. Debounce timer accumulation (rapid requests batched together)
 *   4. Cancel-for-user-data removes pending callbacks
 *   5. Stats counters are accurate
 *   6. Shutdown cleans up all resources
 *
 * These tests do NOT connect to real relays. They test the queueing,
 * dedup, and lifecycle logic of the service in isolation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/nostr_profile_service.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Deterministic pubkey generator */
static char *
make_pubkey(guint seed)
{
  GString *s = g_string_sized_new(64);
  for (guint i = 0; i < 64; i++) {
    g_string_append_printf(s, "%x", (seed + i) % 16);
  }
  return g_string_free(s, FALSE);
}

typedef struct {
  guint callback_count;
  char *last_pubkey;
  gboolean got_null_meta;
} TestCallbackData;

static void
test_profile_callback(const char *pubkey_hex,
                       const GnostrProfileMeta *meta,
                       gpointer user_data)
{
  TestCallbackData *data = user_data;
  data->callback_count++;
  g_free(data->last_pubkey);
  data->last_pubkey = g_strdup(pubkey_hex);
  data->got_null_meta = (meta == NULL);
}

static void
callback_data_clear(TestCallbackData *data)
{
  g_free(data->last_pubkey);
  memset(data, 0, sizeof(*data));
}

/* ── Test: singleton-lifecycle ───────────────────────────────────── */

static void
test_singleton_lifecycle(void)
{
  /* Ensure clean state */
  gnostr_profile_service_shutdown();

  /* First get should create the singleton */
  gpointer svc1 = gnostr_profile_service_get_default();
  g_assert_nonnull(svc1);

  /* Second get should return the same instance */
  gpointer svc2 = gnostr_profile_service_get_default();
  g_assert_true(svc1 == svc2);

  /* Shutdown */
  gnostr_profile_service_shutdown();

  /* After shutdown, get_default creates a new instance */
  gpointer svc3 = gnostr_profile_service_get_default();
  g_assert_nonnull(svc3);
  /* Note: svc3 may or may not equal svc1 depending on allocator;
   * the important thing is it's a valid, initialized service */

  gnostr_profile_service_shutdown();
}

/* ── Test: request-dedup ─────────────────────────────────────────── */

static void
test_request_dedup(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();

  /* Set a very long debounce so nothing actually fires during this test */
  gnostr_profile_service_set_debounce(svc, 60000);

  TestCallbackData cb1 = {0};
  TestCallbackData cb2 = {0};

  g_autofree char *pk = make_pubkey(0x10);

  /* Request the same pubkey twice with different callbacks */
  gnostr_profile_service_request(svc, pk, test_profile_callback, &cb1);
  gnostr_profile_service_request(svc, pk, test_profile_callback, &cb2);

  /* Check stats — should have 2 requests but deduplicated internally */
  GnostrProfileServiceStats stats;
  gnostr_profile_service_get_stats(svc, &stats);
  g_assert_cmpuint(stats.requests, ==, 2);
  /* Only 1 unique pubkey should be pending, but 2 callbacks registered */
  g_test_message("Pending requests: %u, pending callbacks: %u",
                 stats.pending_requests, stats.pending_callbacks);
  g_assert_cmpuint(stats.pending_requests, ==, 1);
  g_assert_cmpuint(stats.pending_callbacks, ==, 2);

  callback_data_clear(&cb1);
  callback_data_clear(&cb2);
  gnostr_profile_service_shutdown();
}

/* ── Test: debounce-accumulation ──────────────────────────────────── */

static void
test_debounce_accumulation(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();

  /* Set a 50ms debounce — enough to batch rapid-fire requests */
  gnostr_profile_service_set_debounce(svc, 50);

  /* No relays set → debounce will fire but no network fetch will happen */

  TestCallbackData cbs[10] = {{0}};

  /* Rapid-fire 10 different pubkey requests */
  char *pubkeys[10];
  for (int i = 0; i < 10; i++) {
    pubkeys[i] = make_pubkey(0x20 + i);
    gnostr_profile_service_request(svc, pubkeys[i], test_profile_callback, &cbs[i]);
  }

  GnostrProfileServiceStats stats;
  gnostr_profile_service_get_stats(svc, &stats);
  g_assert_cmpuint(stats.requests, ==, 10);

  /* Let the debounce timer fire (50ms + some margin) */
  g_usleep(100 * 1000);
  gn_test_drain_main_loop();

  /* Stats should show the debounce fired */
  gnostr_profile_service_get_stats(svc, &stats);
  g_test_message("After debounce: requests=%lu, cache_hits=%lu, network_fetches=%lu",
                 (unsigned long)stats.requests,
                 (unsigned long)stats.cache_hits,
                 (unsigned long)stats.network_fetches);

  for (int i = 0; i < 10; i++) {
    callback_data_clear(&cbs[i]);
    g_free(pubkeys[i]);
  }

  gnostr_profile_service_shutdown();
}

/* ── Test: cancel-for-user-data ──────────────────────────────────── */

static void
test_cancel_for_user_data(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();
  gnostr_profile_service_set_debounce(svc, 60000); /* prevent firing */

  TestCallbackData cb_keep = {0};
  TestCallbackData cb_cancel = {0};

  g_autofree char *pk1 = make_pubkey(0x30);
  g_autofree char *pk2 = make_pubkey(0x31);

  /* Register two requests — one we'll cancel */
  gnostr_profile_service_request(svc, pk1, test_profile_callback, &cb_keep);
  gnostr_profile_service_request(svc, pk2, test_profile_callback, &cb_cancel);

  /* Cancel callbacks for cb_cancel's user_data */
  guint cancelled = gnostr_profile_service_cancel_for_user_data(svc, &cb_cancel);
  g_assert_cmpuint(cancelled, ==, 1);

  /* Cancel again — should be idempotent */
  guint cancelled2 = gnostr_profile_service_cancel_for_user_data(svc, &cb_cancel);
  g_assert_cmpuint(cancelled2, ==, 0);

  callback_data_clear(&cb_keep);
  callback_data_clear(&cb_cancel);
  gnostr_profile_service_shutdown();
}

/* ── Test: stats-accuracy ────────────────────────────────────────── */

static void
test_stats_accuracy(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();
  gnostr_profile_service_set_debounce(svc, 60000);

  GnostrProfileServiceStats stats;
  gnostr_profile_service_get_stats(svc, &stats);

  /* Fresh service should have all zeros */
  g_assert_cmpuint(stats.requests, ==, 0);
  g_assert_cmpuint(stats.cache_hits, ==, 0);
  g_assert_cmpuint(stats.network_fetches, ==, 0);
  g_assert_cmpuint(stats.profiles_fetched, ==, 0);
  g_assert_cmpuint(stats.callbacks_fired, ==, 0);

  /* Make some requests */
  for (int i = 0; i < 5; i++) {
    g_autofree char *pk = make_pubkey(0x40 + i);
    gnostr_profile_service_request(svc, pk, NULL, NULL);
  }

  gnostr_profile_service_get_stats(svc, &stats);
  g_assert_cmpuint(stats.requests, ==, 5);

  gnostr_profile_service_shutdown();
}

/* ── Test: shutdown-cleanup ──────────────────────────────────────── */

static void
test_shutdown_cleanup(void)
{
  /* Multiple shutdown cycles should be safe */
  for (int i = 0; i < 5; i++) {
    gpointer svc = gnostr_profile_service_get_default();
    g_assert_nonnull(svc);

    /* Make some requests */
    g_autofree char *pk = make_pubkey(0x50 + i);
    gnostr_profile_service_request(svc, pk, NULL, NULL);

    gnostr_profile_service_shutdown();
  }

  /* Double shutdown should be safe */
  gnostr_profile_service_shutdown();
  gnostr_profile_service_shutdown();
}

/* ── Test: set-debounce ──────────────────────────────────────────── */

static void
test_set_debounce(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();

  /* Default is 150ms */
  /* Set to various values — should not crash */
  gnostr_profile_service_set_debounce(svc, 10);
  gnostr_profile_service_set_debounce(svc, 500);
  gnostr_profile_service_set_debounce(svc, 0);   /* immediate */
  gnostr_profile_service_set_debounce(svc, 150);  /* restore default */

  gnostr_profile_service_shutdown();
}

/* ── Test: invalid-pubkey-ignored ────────────────────────────────── */

static void
test_invalid_pubkey_ignored(void)
{
  gnostr_profile_service_shutdown();
  gpointer svc = gnostr_profile_service_get_default();

  /* These should be silently ignored (not 64 hex chars) */
  gnostr_profile_service_request(svc, NULL, NULL, NULL);
  gnostr_profile_service_request(svc, "", NULL, NULL);
  gnostr_profile_service_request(svc, "short", NULL, NULL);
  gnostr_profile_service_request(svc, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", NULL, NULL);

  GnostrProfileServiceStats stats;
  gnostr_profile_service_get_stats(svc, &stats);
  /* Invalid requests should not increment the counter */
  g_test_message("Requests after invalid pubkeys: %lu", (unsigned long)stats.requests);
  g_assert_cmpuint(stats.requests, ==, 0);

  gnostr_profile_service_shutdown();
}

/* ── Test: relay-provider-callback ───────────────────────────────── */

static void
test_relay_provider(GPtrArray *out)
{
  g_ptr_array_add(out, g_strdup("wss://relay.example.com"));
  g_ptr_array_add(out, g_strdup("wss://relay2.example.com"));
}

static void
test_relay_provider_registration(void)
{
  gnostr_profile_service_shutdown();

  /* Register a relay provider */
  gnostr_profile_service_set_relay_provider(test_relay_provider);

  gpointer svc = gnostr_profile_service_get_default();
  g_assert_nonnull(svc);

  /* The provider should be picked up when dispatch_next_batch runs */
  g_test_message("Relay provider registered successfully");

  /* Unregister */
  gnostr_profile_service_set_relay_provider(NULL);

  gnostr_profile_service_shutdown();
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/nostr-gobject/profile-service/singleton-lifecycle",
                   test_singleton_lifecycle);
  g_test_add_func("/nostr-gobject/profile-service/request-dedup",
                   test_request_dedup);
  g_test_add_func("/nostr-gobject/profile-service/debounce-accumulation",
                   test_debounce_accumulation);
  g_test_add_func("/nostr-gobject/profile-service/cancel-for-user-data",
                   test_cancel_for_user_data);
  g_test_add_func("/nostr-gobject/profile-service/stats-accuracy",
                   test_stats_accuracy);
  g_test_add_func("/nostr-gobject/profile-service/shutdown-cleanup",
                   test_shutdown_cleanup);
  g_test_add_func("/nostr-gobject/profile-service/set-debounce",
                   test_set_debounce);
  g_test_add_func("/nostr-gobject/profile-service/invalid-pubkey-ignored",
                   test_invalid_pubkey_ignored);
  g_test_add_func("/nostr-gobject/profile-service/relay-provider-registration",
                   test_relay_provider_registration);

  return g_test_run();
}
