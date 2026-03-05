/* SPDX-License-Identifier: MIT
 *
 * test_replay_cache.c - Replay cache tests
 */

#include "signet/replay_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void test_replay_basic(void) {
  SignetReplayCacheConfig cfg = {
    .max_entries = 10,
    .ttl_seconds = 60,
    .skew_seconds = 5,
  };

  SignetReplayCache *cache = signet_replay_cache_new(&cfg);
  assert(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id1 = "event_id_1";
  const char *id2 = "event_id_2";

  /* First check should succeed */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id1, now, now);
  assert(r1 == SIGNET_REPLAY_OK);

  /* Second check of same ID should be duplicate */
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id1, now, now);
  assert(r2 == SIGNET_REPLAY_DUPLICATE);

  /* Different ID should succeed */
  SignetReplayResult r3 = signet_replay_check_and_mark(cache, id2, now, now);
  assert(r3 == SIGNET_REPLAY_OK);

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
  assert(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id = "event_id_ttl";

  /* Mark event */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id, now, now);
  assert(r1 == SIGNET_REPLAY_OK);

  /* Should be duplicate immediately */
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id, now, now);
  assert(r2 == SIGNET_REPLAY_DUPLICATE);

  /* After TTL expires, should be rejected as too old */
  int64_t future = now + 10;
  SignetReplayResult r3 = signet_replay_check_and_mark(cache, id, now, future);
  assert(r3 == SIGNET_REPLAY_TOO_OLD);

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
  assert(cache != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *id = "event_id_future";

  /* Event from future within skew should succeed */
  SignetReplayResult r1 = signet_replay_check_and_mark(cache, id, now + 3, now);
  assert(r1 == SIGNET_REPLAY_OK);

  /* Event from far future should be rejected */
  const char *id2 = "event_id_far_future";
  SignetReplayResult r2 = signet_replay_check_and_mark(cache, id2, now + 100, now);
  assert(r2 == SIGNET_REPLAY_TOO_FAR_IN_FUTURE);

  signet_replay_cache_free(cache);
  printf("test_replay_skew: PASS\n");
}

int main(void) {
  test_replay_basic();
  test_replay_ttl();
  test_replay_skew();
  printf("All replay cache tests passed!\n");
  return 0;
}
