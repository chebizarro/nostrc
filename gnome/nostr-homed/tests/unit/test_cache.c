#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nostr_cache.h"

int main(void){
  nh_cache c; int rc = nh_cache_open(&c, ":memory:");
  assert(rc == 0);
  rc = nh_cache_set_uid_policy(&c, 200000, 1000);
  assert(rc == 0);
  const char *npub = "npub1deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
  uint32_t uid = nh_cache_map_npub_to_uid(&c, npub);
  assert(uid >= 200000 && uid < 201000);
  nh_cache_close(&c);
  puts("ok");
  return 0;
}
