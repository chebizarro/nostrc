#include "nostr_cache.h"
#include <assert.h>
#include "../nh_test.h"
#include <stdio.h>
#include <string.h>

static void expect_int(int cond, const char *msg){ if (!cond){ fprintf(stderr, "FAIL: %s\n", msg); NH_CHECK(cond); } }

int main(void){
  nh_cache c;
  /* Use in-memory DB for unit test */
  expect_int(nh_cache_open(&c, ":memory:") == 0, "open cache");
  /* Ensure a primary group */
  const unsigned int gid = 200001u;
  expect_int(nh_cache_ensure_primary_group(&c, "demo", gid) == 0, "ensure primary group insert");

  /* Lookup by name */
  unsigned int got_gid = 0;
  expect_int(nh_cache_group_lookup_name(&c, "demo", &got_gid) == 0, "lookup group by name");
  expect_int(got_gid == gid, "gid matches");

  /* Lookup by gid */
  char name[64];
  expect_int(nh_cache_group_lookup_gid(&c, gid, name, sizeof name) == 0, "lookup group by gid");
  expect_int(strcmp(name, "demo") == 0, "group name matches");

  /* Re-ensuring the same name is idempotent; a different name for an existing
   * gid is refused and must leave the original group untouched. */
  expect_int(nh_cache_ensure_primary_group(&c, "demo", gid) == 0, "ensure primary group idempotent");
  expect_int(nh_cache_ensure_primary_group(&c, "demo2", gid) != 0, "ensure primary group refuses rename");
  name[0] = '\0';
  expect_int(nh_cache_group_lookup_gid(&c, gid, name, sizeof name) == 0, "lookup group by gid after refused rename");
  expect_int(strcmp(name, "demo") == 0, "group name preserved");

  nh_cache_close(&c);
  printf("test_groups: ok (gid=%u name=%s)\n", gid, name);
  return 0;
}
