#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nostr_cache.h"

int main(void){
  nh_cache c; int rc = nh_cache_open(&c, ":memory:");
  if (rc != 0) { fprintf(stderr, "nh_cache_open failed: %d\n", rc); return 1; }
  assert(rc == 0);
  rc = nh_cache_set_uid_policy(&c, 200000, 1000);
  if (rc != 0) { fprintf(stderr, "nh_cache_set_uid_policy failed: %d\n", rc); nh_cache_close(&c); return 1; }
  assert(rc == 0);
  const char *npub = "npub1deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
  uint32_t uid = nh_cache_map_npub_to_uid(&c, npub);
  if (!(uid >= 200000 && uid < 201000)) { fprintf(stderr, "uid out of range: %u\n", uid); nh_cache_close(&c); return 1; }
  assert(uid >= 200000 && uid < 201000);
  nh_cache_close(&c);
  puts("ok");
  return 0;
}
