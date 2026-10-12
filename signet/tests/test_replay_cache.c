/* SPDX-License-Identifier: MIT
 *
 * test_replay_cache.c - Replay cache tests
 */

#include "signet/replay_cache.h"

#include "test_check.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <glib.h>

static void test_replay_basic(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 10,
    .ttl_seconds = 60,
    .skew_seconds = 5,
  };

  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  CHECK(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id1 = "event_id_1";
  const char *id2 = "event_id_2";

  /* First check should succeed */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id1, now, now);
  CHECK(r1 == SIGNET_REPLAY_OK);

  /* Second check of same ID should be duplicate */
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id1, now, now);
  CHECK(r2 == SIGNET_REPLAY_DUPLICATE);

  /* Different ID should succeed */
  SignetReplayResult r3 = signet_replay_check_and_mark(cache, id2, now, now);
  CHECK(r3 == SIGNET_REPLAY_OK);

  signet_replay_cache_free(cache);
  printf("test_replay_basic: PASS\n");
}

static void test_replay_ttl(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 10,
    .ttl_seconds = 2,
    .skew_seconds = 1,
  };

  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  CHECK(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id = "event_id_ttl";

  /* Mark event */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id, now, now);
  CHECK(r1 == SIGNET_REPLAY_OK);

  /* Should be duplicate immediately */
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id, now, now);
  CHECK(r2 == SIGNET_REPLAY_DUPLICATE);

  /* After TTL expires, should be rejected as too old */
  int64_t future = now + 10;
  SignetReplayResult r3 = signet_replay_check_and_mark(cache, id, now, future);
  CHECK(r3 == SIGNET_REPLAY_TOO_OLD);

  signet_replay_cache_free(cache);
  printf("test_replay_ttl: PASS\n");
}

static void test_replay_skew(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 10,
    .ttl_seconds = 60,
    .skew_seconds = 5,
  };

  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  CHECK(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id = "event_id_future";

  /* Event from future within skew should succeed */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id, now + 3, now);
  CHECK(r1 == SIGNET_REPLAY_OK);

  /* Event from far future should be rejected */
  const char *id2 = "event_id_far_future";
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id2, now + 100, now);
  CHECK(r2 == SIGNET_REPLAY_TOO_FAR_IN_FUTURE);

  signet_replay_cache_free(cache);
  printf("test_replay_skew: PASS\n");
}

/* A scope at its quota is refused, never evicted, and cannot touch another
 * scope's ids; a full refuse_when_full cache drops nothing unexpired. */
static void test_replay_scoped_refusal(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 6,
    .ttl_seconds = 60,
    .skew_seconds = 5,
    .max_entries_per_scope = 3,
    .refuse_when_full = true,
  };
  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  CHECK(cache != NULL);
  int64_t now = 2000000000;

  CHECK(signet_replay_check_and_mark_scoped(cache, "victim", "v-1", now, now) ==
        SIGNET_REPLAY_OK);
  char id[32];
  for (int i = 0; i < 3; i++) {
    g_snprintf(id, sizeof(id), "f-%d", i);
    CHECK(signet_replay_check_and_mark_scoped(cache, "flooder", id, now, now) ==
          SIGNET_REPLAY_OK);
  }
  for (int i = 3; i < 50; i++) {
    g_snprintf(id, sizeof(id), "f-%d", i);
    CHECK(signet_replay_check_and_mark_scoped(cache, "flooder", id, now, now) ==
          SIGNET_REPLAY_FULL);
  }
  /* The victim's id survived the flood and still replays as a duplicate. */
  CHECK(signet_replay_check_and_mark_scoped(cache, "victim", "v-1", now, now) ==
        SIGNET_REPLAY_DUPLICATE);
  /* Ids are unique across scopes. */
  CHECK(signet_replay_check_and_mark_scoped(cache, "other", "v-1", now, now) ==
        SIGNET_REPLAY_DUPLICATE);

  /* Global bound: refuse rather than evict. */
  CHECK(signet_replay_check_and_mark_scoped(cache, "b", "b-1", now, now) ==
        SIGNET_REPLAY_OK);
  CHECK(signet_replay_check_and_mark_scoped(cache, "c", "c-1", now, now) ==
        SIGNET_REPLAY_OK);
  CHECK(signet_replay_check_and_mark_scoped(cache, "d", "d-1", now, now) ==
        SIGNET_REPLAY_FULL);
  CHECK(signet_replay_check_and_mark_scoped(cache, "victim", "v-1", now, now) ==
        SIGNET_REPLAY_DUPLICATE);

  /* Once the ids expire the quota frees up; the stale id is then refused by
   * the skew check rather than executed again. */
  int64_t later = now + 61;
  CHECK(signet_replay_check_and_mark_scoped(cache, "flooder", "f-new", later,
                                            later) == SIGNET_REPLAY_OK);
  CHECK(signet_replay_check_and_mark_scoped(cache, "victim", "v-1", now,
                                            later) == SIGNET_REPLAY_TOO_OLD);
  signet_replay_cache_free(cache);
  printf("test_replay_scoped_refusal: PASS\n");
}

/* TTL is raised to 2 * skew: an id whose created_at can still pass the skew
 * check must still be cached. */
static void test_replay_ttl_covers_skew(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 10,
    .ttl_seconds = 10,
    .skew_seconds = 100,
  };
  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  CHECK(cache != NULL);
  int64_t now = 2000000000;
  CHECK(signet_replay_check_and_mark(cache, "x", now + 100, now) ==
        SIGNET_REPLAY_OK);
  /* 199 s later created_at is still within skew; it must be a duplicate. */
  CHECK(signet_replay_check_and_mark(cache, "x", now + 100, now + 199) ==
        SIGNET_REPLAY_DUPLICATE);
  signet_replay_cache_free(cache);
  printf("test_replay_ttl_covers_skew: PASS\n");
}

int main(void) {
  test_replay_basic();
  test_replay_ttl();
  test_replay_skew();
  test_replay_scoped_refusal();
  test_replay_ttl_covers_skew();
  printf("All replay cache tests passed!\n");
  return 0;
}
