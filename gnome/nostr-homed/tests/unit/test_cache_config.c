#include <assert.h>
#include "../nh_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nostr_cache.h"

static const char *write_temp_conf(void){
  static char path[256];
  snprintf(path, sizeof path, "/tmp/nss_nostr_test_%d.conf", (int)getpid());
  FILE *f = fopen(path, "w");
  NH_CHECK(f);
  fputs("db_path=:memory:\n", f);
  fputs("uid_base=200000\n", f);
  fputs("uid_range=5000\n", f);
  fclose(f);
  return path;
}

int main(void){
  const char *conf = write_temp_conf();
  nh_cache c; int rc = nh_cache_open_configured(&c, conf);
  if (rc != 0) { fprintf(stderr, "nh_cache_open_configured failed: %d\n", rc); remove(conf); return 1; }
  NH_CHECK(rc == 0);
  /* Policy should be applied */
  NH_CHECK(c.uid_base == 200000);
  NH_CHECK(c.uid_range == 5000);
  /* Mapping should fall in configured range */
  const char *npub = "npub1xyz";
  unsigned int uid = nh_cache_map_npub_to_uid(&c, npub);
  if (!(uid >= 200000 && uid < 205000)) { fprintf(stderr, "uid out of range: %u\n", uid); nh_cache_close(&c); remove(conf); return 1; }
  NH_CHECK(uid >= 200000 && uid < 205000);
  nh_cache_close(&c);
  remove(conf);
  puts("ok");
  return 0;
}
