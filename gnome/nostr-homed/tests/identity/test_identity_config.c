#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "nostr_identity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file); assert(fputs(text, file) >= 0); assert(fclose(file) == 0);
}

int main(void) {
  char path[] = "/tmp/nostr-identity-config-XXXXXX";
  nh_identity_config config;
  int fd = mkstemp(path); assert(fd >= 0); close(fd);
  write_text(path,
    "authority_path=/tmp/authority.db\nprojection_path=/tmp/nss.db\n"
    "home_root=/tmp/home\ndefault_shell=/bin/sh\n"
    "uid_min=200000\nuid_max=299999\n"
    "domain_default_min=1000000\ndomain_default_max=1099999\n"
    "domain_rid_min=1100000\ndomain_rid_max=1999999\n"
    "standalone_smb_min=500000\nstandalone_smb_max=599999\n");
  assert(nh_identity_config_load(path, &config) == NH_IDENTITY_OK);
  assert(strcmp(config.authority_path, "/tmp/authority.db") == 0);
  write_text(path, "uid_min=200000\nuid_min=200001\n");
  assert(nh_identity_config_load(path, &config) == NH_IDENTITY_INVALID);
  write_text(path, "unknown_security_option=yes\n");
  assert(nh_identity_config_load(path, &config) == NH_IDENTITY_INVALID);
  nh_identity_config_defaults(&config);
  config.uid_min = 500000; config.uid_max = 550000;
  assert(nh_identity_config_validate(&config) == NH_IDENTITY_INVALID);
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof(config.authority_path), "relative.db");
  assert(nh_identity_config_validate(&config) == NH_IDENTITY_INVALID);
  unlink(path);
  puts("identity config tests: PASS");
  return 0;
}
