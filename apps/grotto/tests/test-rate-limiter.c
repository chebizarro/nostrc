/* Tests for UI password rate limiting. */
#include <glib.h>

#include "../src/rate-limiter.h"

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
  GnRateLimiter *limiter;
  gint rate_limit_exceeded_count;
  gint lockout_expired_count;
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
rate_limiter_fixture_setup(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  /* Create a new rate limiter with 3 max attempts, 10 second window, 1 second base lockout */
  fixture->limiter = gn_rate_limiter_new(3, 10, 1);
  fixture->rate_limit_exceeded_count = 0;
  fixture->lockout_expired_count = 0;
  fixture->last_lockout_seconds = 0;

  /* Connect signals */
  g_signal_connect(fixture->limiter, "rate-limit-exceeded",
                   G_CALLBACK(on_rate_limit_exceeded), fixture);
  g_signal_connect(fixture->limiter, "lockout-expired",
                   G_CALLBACK(on_lockout_expired), fixture);
}

static void
rate_limiter_fixture_teardown(RateLimiterFixture *fixture, gconstpointer data)
{
  (void)data;
  g_object_unref(fixture->limiter);
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
  guint first_lockout = fixture->last_lockout_seconds;
  guint notifications = fixture->rate_limit_exceeded_count;
  for (guint i = 0; i < 3; i++)
    gn_rate_limiter_record_attempt(fixture->limiter, FALSE);
  g_assert_cmpuint(fixture->rate_limit_exceeded_count, ==, notifications + 1);
  g_assert_cmpuint(fixture->last_lockout_seconds, ==, first_lockout);
}

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

  return g_test_run();
}
