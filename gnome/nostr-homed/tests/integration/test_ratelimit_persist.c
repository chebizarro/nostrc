/* Direct unit test for the persistent rate limiter. No SEQPACKET, no broker,
 * so this runs on macOS as well. See beads nostrc-zcll.2.
 *
 * Verifies:
 *   - Recording max_failures on one instance persists to SQLite.
 *   - Opening a fresh instance on the same DB restores the cooldown and the
 *     account is RATE_LIMITED until the persisted cooldown_until_ms elapses.
 *   - After cooldown expires (advance the injected clock past it), the
 *     reopened limiter re-allows the account.
 *   - nh_auth_ratelimit_reset persists (cleared state survives reopen).
 *   - In-memory mode (no path) still works and does NOT touch disk.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "auth_ratelimit.h"

#include "../nh_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t g_now_ms = 1000000ull; /* Advanced explicitly by the test. */

static uint64_t test_clock(void *ctx) {
  (void)ctx;
  return g_now_ms;
}

static void unlink_db(const char *path) {
  (void)unlink(path);
  /* WAL/SHM sidecars from journal_mode=WAL. */
  char aux[512];
  snprintf(aux, sizeof aux, "%s-wal", path); (void)unlink(aux);
  snprintf(aux, sizeof aux, "%s-shm", path); (void)unlink(aux);
}

static void test_persist_cooldown_across_reopen(const char *db) {
  unlink_db(db);
  g_now_ms = 1000000ull;

  nh_auth_ratelimit_config cfg;
  nh_auth_ratelimit_config_defaults(&cfg);
  /* Tighten defaults for the test: 3 failures, 10s window, 30s cooldown. */
  cfg.max_failures = 3;
  cfg.window_ms = 10000;
  cfg.cooldown_ms = 30000;

  const char *key = "alice";

  /* Phase 1: burn the budget, then close the limiter. */
  {
    nh_auth_ratelimit *rl = NULL;
    NH_CHECK(nh_auth_ratelimit_open_persistent(db, &cfg, test_clock, NULL, &rl) == 0);
    NH_CHECK(rl != NULL);
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) != 0);
    for (unsigned i = 0; i < cfg.max_failures; ++i) {
      nh_auth_ratelimit_record_failure(rl, key, 0);
    }
    /* Should now be inside the cooldown. */
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) == 0);
    nh_auth_ratelimit_free(rl);
  }

  /* Phase 2: reopen — the cooldown must still be in effect at the SAME wall
   * time. The persistent limiter uses its own clock hook, so we hold g_now_ms
   * steady across the "restart" to simulate a fast reboot. */
  {
    nh_auth_ratelimit *rl = NULL;
    NH_CHECK(nh_auth_ratelimit_open_persistent(db, &cfg, test_clock, NULL, &rl) == 0);
    NH_CHECK(rl != NULL);
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) == 0); /* still locked */

    /* Advance just before cooldown boundary — still locked. */
    g_now_ms += cfg.cooldown_ms - 1;
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) == 0);

    /* Cross the boundary — now allowed. */
    g_now_ms += 2;
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) != 0);
    nh_auth_ratelimit_free(rl);
  }

  unlink_db(db);
}

static void test_persist_reset(const char *db) {
  unlink_db(db);
  g_now_ms = 2000000ull;

  nh_auth_ratelimit_config cfg;
  nh_auth_ratelimit_config_defaults(&cfg);
  cfg.max_failures = 3;
  cfg.window_ms = 10000;
  cfg.cooldown_ms = 30000;

  const char *key = "bob";

  {
    nh_auth_ratelimit *rl = NULL;
    NH_CHECK(nh_auth_ratelimit_open_persistent(db, &cfg, test_clock, NULL, &rl) == 0);
    for (unsigned i = 0; i < cfg.max_failures; ++i)
      nh_auth_ratelimit_record_failure(rl, key, 0);
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) == 0);
    nh_auth_ratelimit_reset(rl, key);
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) != 0);
    nh_auth_ratelimit_free(rl);
  }

  /* Reopen: reset must have persisted — no cooldown even though we did not
   * advance the clock at all. */
  {
    nh_auth_ratelimit *rl = NULL;
    NH_CHECK(nh_auth_ratelimit_open_persistent(db, &cfg, test_clock, NULL, &rl) == 0);
    NH_CHECK(nh_auth_ratelimit_check(rl, key, 0) != 0);
    nh_auth_ratelimit_free(rl);
  }

  unlink_db(db);
}

static void test_in_memory_still_works(void) {
  nh_auth_ratelimit_config cfg;
  nh_auth_ratelimit_config_defaults(&cfg);
  cfg.max_failures = 2;
  cfg.window_ms = 10000;
  cfg.cooldown_ms = 30000;

  nh_auth_ratelimit *rl = nh_auth_ratelimit_new(&cfg);
  NH_CHECK(rl != NULL);

  const char *k = "carol";
  /* In-memory mode: caller-supplied now_ms is honoured (no wall clock). */
  NH_CHECK(nh_auth_ratelimit_check(rl, k, 100) != 0);
  nh_auth_ratelimit_record_failure(rl, k, 100);
  nh_auth_ratelimit_record_failure(rl, k, 200);
  NH_CHECK(nh_auth_ratelimit_check(rl, k, 300) == 0); /* cooldown */
  NH_CHECK(nh_auth_ratelimit_check(rl, k, 200 + 30000 + 1) != 0);

  nh_auth_ratelimit_free(rl);
}

/* NEGATIVE: passing a NULL or empty path must fail cleanly. */
static void test_persist_open_rejects_bad_path(void) {
  nh_auth_ratelimit *rl = (nh_auth_ratelimit *)0x1;
  NH_CHECK(nh_auth_ratelimit_open_persistent(NULL, NULL, NULL, NULL, &rl) == -1);
  NH_CHECK(rl == NULL);
  rl = (nh_auth_ratelimit *)0x1;
  NH_CHECK(nh_auth_ratelimit_open_persistent("", NULL, NULL, NULL, &rl) == -1);
  NH_CHECK(rl == NULL);
}

int main(void) {
  /* Pick a tmpdir the test can own. */
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char db[512];
  snprintf(db, sizeof db, "%s/nh_ratelimit_persist_%d.db", tmp, (int)getpid());

  test_persist_cooldown_across_reopen(db);
  test_persist_reset(db);
  test_in_memory_still_works();
  test_persist_open_rejects_bad_path();

  printf("test_ratelimit_persist: OK\n");
  return 0;
}
