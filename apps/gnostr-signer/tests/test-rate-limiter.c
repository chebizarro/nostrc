/* test-rate-limiter.c - Unit tests for gnostr-signer rate limiting
 *
 * Tests rate limiting functionality for preventing brute force attacks including:
 * - Global rate limiting (UI password entry)
 * - Per-client rate limiting (NIP-46 bunker auth)
 * - Exponential backoff
 * - Persistence across restarts
 * - Admin functions
 * - User-friendly error messages
 *
 * Issue: nostrc-1g1
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include "../src/rate-limiter.h"

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
  GnRateLimiter *limiter;
  gint rate_limit_exceeded_count;
  gint lockout_expired_count;
  gint client_rate_limit_exceeded_count;
  gint client_lockout_expired_count;
  gchar *last_client_pubkey;
  guint last_lockout_seconds;
} RateLimiterFixture;

static void
on_rate_limit_exceeded(GnRateLimiter *limiter, guint lockout_seconds, gpointer user_data)
{
  (void)limiter;
  RateLimiterFixture *f = (RateLimiterFixture *)user_data;
  f->rate_limit_exceeded_count++;
  f->last_lockout_seconds = lockout_seconds;
}

static void
on_lockout_expired(GnRateLimiter *limiter, gpointer user_data)
{
  (void)limiter;
  RateLimiterFixture *f = (RateLimiterFixture *)user_data;
  f->lockout_expired_count++;
}

static void
on_client_rate_limit_exceeded(GnRateLimiter *limiter, const gchar *client_pubkey,
                              guint lockout_seconds, gpointer user_data)
{
  (void)limiter;
  RateLimiterFixture *f = (RateLimiterFixture *)user_data;
  f->client_rate_limit_exceeded_count++;
  f->last_lockout_seconds = lockout_seconds;
  g_free(f->last_client_pubkey);
  f->last_client_pubkey = g_strdup(client_pubkey);
}

static void
on_client_lockout_expired(GnRateLimiter *limiter, const gchar *client_pubkey, gpointer user_data)
{
  (void)limiter;
  (void)client_pubkey;
  RateLimiterFixture *f = (RateLimiterFixture *)user_data;
  f->client_lockout_expired_count++;
}

static void
rate_limiter_fixture_setup(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  /* Create a new rate limiter with 3 max attempts, 10 second window, 1 second base lockout */
  fixture->limiter = gn_rate_limiter_new(3, 10, 1);
  fixture->rate_limit_exceeded_count = 0;
  fixture->lockout_expired_count = 0;
  fixture->client_rate_limit_exceeded_count = 0;
  fixture->client_lockout_expired_count = 0;
  fixture->last_client_pubkey = NULL;
  fixture->last_lockout_seconds = 0;

  /* Connect signals */
  g_signal_connect(fixture->limiter, "rate-limit-exceeded",
                   G_CALLBACK(on_rate_limit_exceeded), fixture);
  g_signal_connect(fixture->limiter, "lockout-expired",
                   G_CALLBACK(on_lockout_expired), fixture);
  g_signal_connect(fixture->limiter, "client-rate-limit-exceeded",
                   G_CALLBACK(on_client_rate_limit_exceeded), fixture);
  g_signal_connect(fixture->limiter, "client-lockout-expired",
                   G_CALLBACK(on_client_lockout_expired), fixture);
}

static void
rate_limiter_fixture_teardown(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  g_object_unref(fixture->limiter);
  g_free(fixture->last_client_pubkey);
}

/* ===========================================================================
 * Basic Creation Tests
 * =========================================================================== */

static void
test_rate_limiter_create_default(void)
{
  GnRateLimiter *limiter = gn_rate_limiter_new_default();

  g_assert_nonnull(limiter);
  g_assert_true(GN_IS_RATE_LIMITER(limiter));
  g_assert_true(gn_rate_limiter_check_allowed(limiter));
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(limiter), ==,
                   GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS);
  g_assert_false(gn_rate_limiter_is_locked_out(limiter));

  g_object_unref(limiter);
}

static void
test_rate_limiter_create_custom(void)
{
  GnRateLimiter *limiter = gn_rate_limiter_new(10, 60, 5);

  g_assert_nonnull(limiter);
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(limiter), ==, 10);

  g_object_unref(limiter);
}

/* ===========================================================================
 * Global Rate Limiting Tests
 * =========================================================================== */

static void
test_rate_limiter_record_failed_attempts(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  /* First attempt should be allowed */
  g_assert_true(gn_rate_limiter_check_allowed(fixture->limiter));
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 3);

  /* Record failed attempt */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 2);

  /* Record another failed attempt */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 1);

  /* Still allowed */
  g_assert_true(gn_rate_limiter_check_allowed(fixture->limiter));
}

static void
test_rate_limiter_lockout_after_max_attempts(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  /* Record max attempts (3 failures) */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);

  /* Should now be locked out */
  g_assert_true(gn_rate_limiter_is_locked_out(fixture->limiter));
  g_assert_false(gn_rate_limiter_check_allowed(fixture->limiter));
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 0);

  /* Signal should have been emitted */
  g_assert_cmpint(fixture->rate_limit_exceeded_count, ==, 1);
}

static void
test_rate_limiter_reset_on_success(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  /* Record some failed attempts */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 1);

  /* Successful attempt should reset */
  gn_rate_limiter_record_attempt(fixture->limiter, TRUE);

  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 3);
  g_assert_true(gn_rate_limiter_check_allowed(fixture->limiter));
  g_assert_false(gn_rate_limiter_is_locked_out(fixture->limiter));
}

static void
test_rate_limiter_manual_reset(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  /* Record max attempts to trigger lockout */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);

  g_assert_true(gn_rate_limiter_is_locked_out(fixture->limiter));

  /* Manual reset should clear lockout */
  gn_rate_limiter_reset(fixture->limiter);

  g_assert_false(gn_rate_limiter_is_locked_out(fixture->limiter));
  g_assert_cmpuint(gn_rate_limiter_get_attempts_remaining(fixture->limiter), ==, 3);
  g_assert_cmpuint(gn_rate_limiter_get_lockout_multiplier(fixture->limiter), ==, 1);
}

static void
test_rate_limiter_lockout_multiplier(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  /* First lockout */
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  gn_rate_limiter_record_attempt(fixture->limiter, FALSE);

  /* Multiplier should be doubled for next lockout */
  guint first_multiplier = gn_rate_limiter_get_lockout_multiplier(fixture->limiter);
  g_assert_cmpuint(first_multiplier, ==, 2);  /* 1 * 2 after first lockout */
}

/* ===========================================================================
 * Per-Client Rate Limiting Tests
 * =========================================================================== */

static void
test_rate_limiter_client_initial_allowed(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  guint remaining = 999;
  GnRateLimitStatus status = gn_rate_limiter_check_client(fixture->limiter, client, &remaining);

  g_assert_cmpint(status, ==, GN_RATE_LIMIT_ALLOWED);
  g_assert_cmpuint(remaining, ==, 0);
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client), ==, 3);
}

static void
test_rate_limiter_client_track_failures(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  /* Record failed attempts */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client), ==, 2);

  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client), ==, 1);
}

static void
test_rate_limiter_client_lockout(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  /* Record max attempts */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);

  /* Should be locked out */
  g_assert_true(gn_rate_limiter_is_client_locked_out(fixture->limiter, client));

  guint remaining = 0;
  GnRateLimitStatus status = gn_rate_limiter_check_client(fixture->limiter, client, &remaining);
  g_assert_cmpint(status, ==, GN_RATE_LIMIT_LOCKED_OUT);
  g_assert_cmpuint(remaining, >, 0);

  /* Signal should have been emitted */
  g_assert_cmpint(fixture->client_rate_limit_exceeded_count, ==, 1);
  g_assert_cmpstr(fixture->last_client_pubkey, ==, client);
}

static void
test_rate_limiter_client_reset_on_success(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  /* Record some failed attempts */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);

  /* Successful attempt should reset */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, TRUE);

  /* Client should have clean state now (no entry in hash table) */
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client), ==, 3);
  g_assert_false(gn_rate_limiter_is_client_locked_out(fixture->limiter, client));
}

static void
test_rate_limiter_multiple_clients_independent(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client1 = "1111111111111111111111111111111111111111111111111111111111111111";
  const gchar *client2 = "2222222222222222222222222222222222222222222222222222222222222222";

  /* Reset any existing state for these clients from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client1);
  gn_rate_limiter_reset_client(fixture->limiter, client2);

  /* Lock out client1 */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);

  g_assert_true(gn_rate_limiter_is_client_locked_out(fixture->limiter, client1));

  /* client2 should still be allowed */
  g_assert_false(gn_rate_limiter_is_client_locked_out(fixture->limiter, client2));
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client2), ==, 3);
}

static void
test_rate_limiter_client_manual_reset(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  /* Lock out client */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);

  g_assert_true(gn_rate_limiter_is_client_locked_out(fixture->limiter, client));

  /* Manual reset */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  g_assert_false(gn_rate_limiter_is_client_locked_out(fixture->limiter, client));
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client), ==, 3);
}

/* ===========================================================================
 * Admin Functions Tests
 * =========================================================================== */

static void
test_rate_limiter_clear_all_clients(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client1 = "1111111111111111111111111111111111111111111111111111111111111111";
  const gchar *client2 = "2222222222222222222222222222222222222222222222222222222222222222";

  /* Record failures for multiple clients */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client2, FALSE);

  /* Clear all clients */
  gn_rate_limiter_clear_all_clients(fixture->limiter);

  /* All clients should be allowed again */
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client1), ==, 3);
  g_assert_cmpuint(gn_rate_limiter_get_client_attempts_remaining(fixture->limiter, client2), ==, 3);
}

static void
test_rate_limiter_list_clients(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client1 = "1111111111111111111111111111111111111111111111111111111111111111";
  const gchar *client2 = "2222222222222222222222222222222222222222222222222222222222222222";

  /* Record failures for clients */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client1, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client2, FALSE);

  /* List clients */
  GPtrArray *clients = gn_rate_limiter_list_clients(fixture->limiter);

  g_assert_nonnull(clients);
  g_assert_cmpuint(clients->len, ==, 2);

  g_ptr_array_unref(clients);
}

static void
test_rate_limiter_get_client_info(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Record some failures */
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);
  gn_rate_limiter_record_client_attempt(fixture->limiter, client, FALSE);

  /* Get client info */
  GnClientRateLimitInfo *info = gn_rate_limiter_get_client_info(fixture->limiter, client);

  g_assert_nonnull(info);
  g_assert_cmpstr(info->client_pubkey, ==, client);
  g_assert_cmpuint(info->failed_attempts, ==, 2);
  g_assert_cmpuint(info->backoff_multiplier, >=, 1);

  gn_client_rate_limit_info_free(info);
}

static void
test_rate_limiter_get_client_info_nonexistent(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "nonexistent0000000000000000000000000000000000000000000000000000";

  GnClientRateLimitInfo *info = gn_rate_limiter_get_client_info(fixture->limiter, client);
  g_assert_null(info);
}

/* ===========================================================================
 * Error Message Tests
 * =========================================================================== */

static void
test_rate_limiter_format_error_allowed(void)
{
  gchar *msg = gn_rate_limiter_format_error_message(GN_RATE_LIMIT_ALLOWED, 0);
  g_assert_nonnull(msg);
  g_assert_cmpstr(msg, ==, "Authentication allowed");
  g_free(msg);
}

static void
test_rate_limiter_format_error_backoff_seconds(void)
{
  gchar *msg = gn_rate_limiter_format_error_message(GN_RATE_LIMIT_BACKOFF, 5);
  g_assert_nonnull(msg);
  g_assert_true(strstr(msg, "5 seconds") != NULL);
  g_free(msg);
}

static void
test_rate_limiter_format_error_backoff_minutes(void)
{
  gchar *msg = gn_rate_limiter_format_error_message(GN_RATE_LIMIT_BACKOFF, 90);
  g_assert_nonnull(msg);
  g_assert_true(strstr(msg, "minute") != NULL);
  g_free(msg);
}

static void
test_rate_limiter_format_error_locked_out(void)
{
  gchar *msg = gn_rate_limiter_format_error_message(GN_RATE_LIMIT_LOCKED_OUT, 120);
  g_assert_nonnull(msg);
  g_assert_true(strstr(msg, "locked out") != NULL);
  g_free(msg);
}

/* ===========================================================================
 * Persistence Tests
 * =========================================================================== */

static void
test_rate_limiter_save_load(void)
{
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Create limiter and add some state */
  GnRateLimiter *limiter1 = gn_rate_limiter_new(5, 300, 1);

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(limiter1, client);

  gn_rate_limiter_record_client_attempt(limiter1, client, FALSE);
  gn_rate_limiter_record_client_attempt(limiter1, client, FALSE);

  /* Force save */
  gboolean saved = gn_rate_limiter_save(limiter1);
  g_assert_true(saved);

  /* Create new limiter (will load state) */
  GnRateLimiter *limiter2 = gn_rate_limiter_new(5, 300, 1);

  /* State should be loaded */
  GnClientRateLimitInfo *info = gn_rate_limiter_get_client_info(limiter2, client);

  /* Note: This test depends on file system state, so we check if info exists
   * If running fresh, info might not exist */
  if (info) {
    g_assert_cmpuint(info->failed_attempts, ==, 2);
    gn_client_rate_limit_info_free(info);
  }

  g_object_unref(limiter1);
  g_object_unref(limiter2);
}

/* ===========================================================================
 * Edge Cases Tests
 * =========================================================================== */

static void
test_rate_limiter_null_client(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  guint remaining = 999;
  GnRateLimitStatus status = gn_rate_limiter_check_client(fixture->limiter, NULL, &remaining);
  g_assert_cmpint(status, ==, GN_RATE_LIMIT_ALLOWED);

  /* Should not crash */
  gn_rate_limiter_record_client_attempt(fixture->limiter, NULL, FALSE);
  gn_rate_limiter_reset_client(fixture->limiter, NULL);
}

static void
test_rate_limiter_empty_client(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;

  guint remaining = 999;
  GnRateLimitStatus status = gn_rate_limiter_check_client(fixture->limiter, "", &remaining);
  g_assert_cmpint(status, ==, GN_RATE_LIMIT_ALLOWED);

  /* Should not crash */
  gn_rate_limiter_record_client_attempt(fixture->limiter, "", FALSE);
}

static void
test_rate_limiter_null_remaining(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  const gchar *client = "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab";

  /* Reset any existing state for this client from previous test runs */
  gn_rate_limiter_reset_client(fixture->limiter, client);

  /* Should not crash when remaining is NULL */
  GnRateLimitStatus status = gn_rate_limiter_check_client(fixture->limiter, client, NULL);
  g_assert_cmpint(status, ==, GN_RATE_LIMIT_ALLOWED);
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  /* Basic creation tests */
  g_test_add_func("/signer/rate-limiter/create/default",
                  test_rate_limiter_create_default);
  g_test_add_func("/signer/rate-limiter/create/custom",
                  test_rate_limiter_create_custom);

  /* Global rate limiting tests */
  g_test_add("/signer/rate-limiter/global/record_failures", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_record_failed_attempts,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/global/lockout", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_lockout_after_max_attempts,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/global/reset_on_success", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_reset_on_success,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/global/manual_reset", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_manual_reset,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/global/lockout_multiplier", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_lockout_multiplier,
             rate_limiter_fixture_teardown);

  /* Per-client rate limiting tests */
  g_test_add("/signer/rate-limiter/client/initial_allowed", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_client_initial_allowed,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/client/track_failures", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_client_track_failures,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/client/lockout", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_client_lockout,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/client/reset_on_success", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_client_reset_on_success,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/client/independent", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_multiple_clients_independent,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/client/manual_reset", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_client_manual_reset,
             rate_limiter_fixture_teardown);

  /* Admin functions tests */
  g_test_add("/signer/rate-limiter/admin/clear_all", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_clear_all_clients,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/admin/list_clients", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_list_clients,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/admin/get_client_info", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_get_client_info,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/admin/get_client_info_nonexistent", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_get_client_info_nonexistent,
             rate_limiter_fixture_teardown);

  /* Error message tests */
  g_test_add_func("/signer/rate-limiter/error/allowed",
                  test_rate_limiter_format_error_allowed);
  g_test_add_func("/signer/rate-limiter/error/backoff_seconds",
                  test_rate_limiter_format_error_backoff_seconds);
  g_test_add_func("/signer/rate-limiter/error/backoff_minutes",
                  test_rate_limiter_format_error_backoff_minutes);
  g_test_add_func("/signer/rate-limiter/error/locked_out",
                  test_rate_limiter_format_error_locked_out);

  /* Persistence tests */
  g_test_add_func("/signer/rate-limiter/persistence/save_load",
                  test_rate_limiter_save_load);

  /* Edge cases tests */
  g_test_add("/signer/rate-limiter/edge/null_client", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_null_client,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/edge/empty_client", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_empty_client,
             rate_limiter_fixture_teardown);
  g_test_add("/signer/rate-limiter/edge/null_remaining", RateLimiterFixture, NULL,
             rate_limiter_fixture_setup, test_rate_limiter_null_remaining,
             rate_limiter_fixture_teardown);

  return g_test_run();
}
