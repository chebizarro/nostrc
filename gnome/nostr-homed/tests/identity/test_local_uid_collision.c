/* Files-only UID collision regression: never modify the host's accounts. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "../nh_test.h"
#include "nostr_identity.h"
#include <unistd.h>
#include <string.h>

static nh_identity_ownership_result available(void *ctx, const char *name,
                                              uint32_t uid, uint32_t gid) {
  (void)ctx; (void)name; (void)uid; (void)gid;
  return NH_IDENTITY_OWNERSHIP_FREE;
}
int main(void) {
  FILE *passwd = fopen("/etc/passwd", "r");
  NH_CHECK(passwd);
  char *line = NULL;
  size_t capacity = 0;
  uint32_t occupied = 0;
  while (getline(&line, &capacity, passwd) >= 0) {
    char *name_end = strchr(line, ':');
    char *password_end = name_end ? strchr(name_end + 1, ':') : NULL;
    if (!password_end || password_end[1] < '0' || password_end[1] > '9') continue;
    char *end;
    unsigned long uid = strtoul(password_end + 1, &end, 10);
    if (*end == ':' && uid > 0 && uid < 200000) {
      occupied = (uint32_t)uid;
      break;
    }
  }
  free(line);
  fclose(passwd);
  NH_CHECK(occupied);
  char dir[] = "/tmp/nh-local-uid-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  config.uid_min = occupied;
  config.uid_max = occupied; /* Exhaustion proves the callback cannot bypass files. */
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  nh_identity_enroll_request request = {0};
  request.username = "n_collision";
  request.pubkey_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  request.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  NH_CHECK(nh_identity_operation_begin_enroll(store,
      "00000000-0000-4000-8000-0000000000f1", &request, &state) == NH_IDENTITY_RANGE_EXHAUSTED);
  nh_identity_store_close(store);
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm", "authority.lock"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, files[i]); unlink(path);
  }
  NH_CHECK(rmdir(dir) == 0);
  puts("local passwd UID cannot be allocated even with an always-free ownership probe");
}
