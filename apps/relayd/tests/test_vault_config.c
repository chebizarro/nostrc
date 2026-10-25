#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "relayd_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f);
  assert(fputs(text, f) >= 0);
  assert(fclose(f) == 0);
}

int main(void) {
  const char *pk = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  char dir[] = "/tmp/relayd-vsp01-XXXXXX";
  assert(mkdtemp(dir));
  char config[256], list[256], text[1024];
  snprintf(config, sizeof config, "%s/relay.toml", dir);
  snprintf(list, sizeof list, "%s/members", dir);
  snprintf(text, sizeof text, "auth = \"optional\"\nrelay_url = \"wss://vault.example.com/\"\nvault_allowlist = \"%s\"\n", list);
  write_file(config, text);
  RelaydConfig cfg;
  assert(relayd_config_load(NULL, &cfg) == 0 && !cfg.vault_enabled);
  assert(relayd_config_load(config, &cfg) != 0); /* Missing is not public. */
  write_file(list, "# deny all\n\n");
  assert(relayd_config_load(config, &cfg) == 0 && cfg.vault_enabled);
  assert(!relayd_config_vault_allows(&cfg, pk));
  write_file(list, pk);
  assert(relayd_config_load(config, &cfg) == 0);
  assert(relayd_config_vault_allows(&cfg, pk));
  assert(!relayd_config_vault_allows(&cfg, "bad"));
  assert(!relayd_config_vault_allows(&cfg, NULL));
  strcpy(cfg.auth, "off");
  assert(relayd_config_validate(&cfg, NULL, 0) != 0);
  strcpy(cfg.auth, "optional");
  cfg.relay_url[0] = 0;
  assert(relayd_config_validate(&cfg, NULL, 0) != 0);
  write_file(list, "bad\n");
  assert(relayd_config_load(config, &cfg) != 0);
  write_file(list, "79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798\n");
  assert(relayd_config_load(config, &cfg) != 0);
  FILE *f = fopen(list, "w");
  assert(f);
  for (int i = 0; i <= RELAYD_MAX_VAULT_PUBKEYS; i++) fprintf(f, "%s\n", pk);
  assert(fclose(f) == 0);
  assert(relayd_config_load(config, &cfg) != 0);
  unlink(config); unlink(list); rmdir(dir);
  puts("PASS VSP-01 allowlist config: opt-in, deny-empty, reject missing/malformed/oversized and missing auth/url");
  return 0;
}
