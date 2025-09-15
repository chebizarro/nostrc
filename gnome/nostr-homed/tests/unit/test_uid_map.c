#include "nostr_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
  nh_cache c;
  /* Open an in-memory sqlite for test by pointing to :memory: via nh_cache_open, then set policy */
  if (nh_cache_open(&c, ":memory:") != 0){ fprintf(stderr, "open cache failed\n"); return 1; }
  /* Set deterministic policy */
  if (nh_cache_set_uid_policy(&c, 200000u, 1000u) != 0){ fprintf(stderr, "set policy failed\n"); nh_cache_close(&c); return 1; }
  const char *npub_sample = "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqsamp1e";
  uint32_t u1 = nh_cache_map_npub_to_uid(&c, npub_sample);
  uint32_t u2 = nh_cache_map_npub_to_uid(&c, npub_sample);
  if (u1 < 200000u || u1 >= 201000u){ fprintf(stderr, "uid out of range: %u\n", u1); nh_cache_close(&c); return 1; }
  if (u1 != u2) { fprintf(stderr, "uid mismatch: %u != %u\n", u1, u2); nh_cache_close(&c); return 1; }
  /* Different input should likely differ (not guaranteed, but extremely likely) */
  const char *npub_sample2 = "npub1differentkeystringforunittestxxxxxxxxxxxxxxxxxxxx";
  uint32_t u3 = nh_cache_map_npub_to_uid(&c, npub_sample2);
  if (u3 < 200000u || u3 >= 201000u){ fprintf(stderr, "uid2 out of range: %u\n", u3); nh_cache_close(&c); return 1; }
  if (u3 == u1){ fprintf(stderr, "collision observed (unlikely): %u\n", u3); /* do not fail hard */ }
  nh_cache_close(&c);
  printf("test_uid_map: ok (u1=%u u3=%u)\n", u1, u3);
  return 0;
}
