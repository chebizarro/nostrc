/* test_budget.c - per-app daily budget accounting: limits, reservations,
 * commit/release, local-day rollover (incl. mid-flight), persistence.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-budget.h"

#include <glib.h>
#include <glib/gstdio.h>

typedef struct {
  GDateTime *now;
} FakeClock;

static GDateTime *
fake_now(gpointer data)
{
  FakeClock *c = data;
  return g_date_time_ref(c->now);
}

static void
clock_set(FakeClock *c, gint y, gint m, gint d, gint h, gint min)
{
  g_clear_pointer(&c->now, g_date_time_unref);
  g_autoptr(GTimeZone) tz = g_time_zone_new_utc();
  c->now = g_date_time_new(tz, y, m, d, h, min, 0);
}

#define APP "org.example.Zapper"

static void
test_limits_and_reservations(void)
{
  FakeClock clk = { 0 };
  clock_set(&clk, 2026, 9, 26, 10, 0);
  NwaBudgetStore *s = nwa_budget_store_new(NULL, fake_now, &clk);
  NwaBudgetInfo bi;

  nwa_budget_store_get(s, APP, &bi);
  g_assert_false(bi.known);
  g_assert_cmpuint(bi.remaining_msat, ==, 0);
  /* no budget: nothing fits unless forced (user approved) */
  g_assert_cmpuint(nwa_budget_store_reserve(s, APP, 1000, FALSE), ==, 0);

  nwa_budget_store_set_limit(s, APP, 10000000); /* 10k sats */
  guint r1 = nwa_budget_store_reserve(s, APP, 6000000, FALSE);
  g_assert_cmpuint(r1, !=, 0);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 6000000); /* in-flight counts */
  g_assert_cmpuint(bi.remaining_msat, ==, 4000000);

  /* concurrent request that no longer fits is refused */
  g_assert_cmpuint(nwa_budget_store_reserve(s, APP, 5000000, FALSE), ==, 0);
  guint r2 = nwa_budget_store_reserve(s, APP, 4000000, FALSE);
  g_assert_cmpuint(r2, !=, 0);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.remaining_msat, ==, 0);

  /* r1 succeeds with 3 sat fees; r2 fails definitively and is refunded */
  nwa_budget_store_commit(s, r1, 6003000);
  nwa_budget_store_release(s, r2);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 6003000);
  g_assert_cmpuint(bi.remaining_msat, ==, 3997000);

  /* user-approved payment may exceed the budget */
  guint r3 = nwa_budget_store_reserve(s, APP, 50000000, TRUE);
  g_assert_cmpuint(r3, !=, 0);
  nwa_budget_store_commit(s, r3, 50000000);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 56003000);
  g_assert_cmpuint(bi.remaining_msat, ==, 0); /* saturates, never wraps */

  /* unknown reservation ids are ignored */
  nwa_budget_store_commit(s, 9999, 1);
  nwa_budget_store_release(s, 9999);
  g_assert_cmpuint(nwa_budget_store_reserve(s, APP, 0, TRUE), ==, 0);

  nwa_budget_store_free(s);
  g_date_time_unref(clk.now);
}

static void
test_day_rollover(void)
{
  FakeClock clk = { 0 };
  clock_set(&clk, 2026, 9, 26, 23, 50);
  NwaBudgetStore *s = nwa_budget_store_new(NULL, fake_now, &clk);
  NwaBudgetInfo bi;
  nwa_budget_store_set_limit(s, APP, 1000000);

  guint r = nwa_budget_store_reserve(s, APP, 900000, FALSE);
  nwa_budget_store_commit(s, r, 900000);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.remaining_msat, ==, 100000);

  /* in flight across midnight: reserved on the 26th ... */
  guint inflight = nwa_budget_store_reserve(s, APP, 100000, FALSE);
  g_assert_cmpuint(inflight, !=, 0);

  clock_set(&clk, 2026, 9, 27, 0, 5);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 0);          /* fresh day */
  g_assert_cmpuint(bi.remaining_msat, ==, 1000000);
  g_assert_cmpuint(bi.limit_msat_per_day, ==, 1000000);   /* limit persists */

  /* ... completes on the 27th: charged to the new day */
  nwa_budget_store_commit(s, inflight, 100500);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 100500);

  /* a reservation refunded after rollover must not go negative */
  guint r2 = nwa_budget_store_reserve(s, APP, 1000, FALSE);
  clock_set(&clk, 2026, 9, 28, 9, 0);
  nwa_budget_store_release(s, r2);
  nwa_budget_store_get(s, APP, &bi);
  g_assert_cmpuint(bi.spent_today_msat, ==, 0);

  nwa_budget_store_free(s);
  g_date_time_unref(clk.now);
}

static void
test_persistence(void)
{
  FakeClock clk = { 0 };
  clock_set(&clk, 2026, 9, 26, 12, 0);
  g_autofree gchar *dir = g_dir_make_tmp("nwa-budget-XXXXXX", NULL);
  g_autofree gchar *path = g_build_filename(dir, "sub", "budgets.json", NULL);

  NwaBudgetStore *s = nwa_budget_store_new(path, fake_now, &clk);
  g_assert_true(nwa_budget_store_load(s, NULL)); /* missing file is fine */
  nwa_budget_store_set_limit(s, APP, 2100000);
  nwa_budget_store_set_allow_read(s, "org.example.Reader", TRUE);
  guint r = nwa_budget_store_reserve(s, APP, 21000, FALSE);
  (void)r; /* crash before commit: the reservation is already on disk */
  nwa_budget_store_free(s);

  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);

  s = nwa_budget_store_new(path, fake_now, &clk);
  g_assert_true(nwa_budget_store_load(s, NULL));
  NwaBudgetInfo bi;
  nwa_budget_store_get(s, APP, &bi);
  g_assert_true(bi.known);
  g_assert_cmpuint(bi.limit_msat_per_day, ==, 2100000);
  g_assert_cmpuint(bi.spent_today_msat, ==, 21000);
  g_assert_false(bi.allow_read);
  nwa_budget_store_get(s, "org.example.Reader", &bi);
  g_assert_true(bi.allow_read);
  g_assert_cmpuint(bi.limit_msat_per_day, ==, 0);
  nwa_budget_store_free(s);

  /* corrupt file: moved aside, store starts empty (fail closed) */
  g_assert_true(g_file_set_contents(path, "{not json", -1, NULL));
  s = nwa_budget_store_new(path, fake_now, &clk);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*not a valid budget file*");
  g_assert_true(nwa_budget_store_load(s, NULL));
  g_test_assert_expected_messages();
  nwa_budget_store_get(s, APP, &bi);
  g_assert_false(bi.known);
  g_autofree gchar *bad = g_strconcat(path, ".corrupt", NULL);
  g_assert_true(g_file_test(bad, G_FILE_TEST_EXISTS));
  nwa_budget_store_free(s);

  g_remove(bad);
  g_remove(path);
  g_autofree gchar *sub = g_path_get_dirname(path);
  g_rmdir(sub);
  g_rmdir(dir);
  g_date_time_unref(clk.now);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/budget/limits-and-reservations", test_limits_and_reservations);
  g_test_add_func("/budget/day-rollover", test_day_rollover);
  g_test_add_func("/budget/persistence", test_persistence);
  return g_test_run();
}
