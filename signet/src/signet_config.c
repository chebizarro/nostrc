/* SPDX-License-Identifier: MIT
 *
 * signet_config.c - Signet configuration loader (Phase 1 stub).
 */

#include "signet/signet_config.h"

#include <stdlib.h>
#include <string.h>

static void signet_free_strv(char **v, size_t n) {
  if (!v) return;
  for (size_t i = 0; i < n; i++) {
    free(v[i]);
  }
  free(v);
}

void signet_config_init(SignetConfig *cfg) {
  if (!cfg) return;

  memset(cfg, 0, sizeof(*cfg));

  /* Defaults mirror "use defaults if file missing" pattern from relayd_config.c */
  cfg->relays = NULL;
  cfg->n_relays = 0;

  strncpy(cfg->identity, "default", sizeof(cfg->identity) - 1);

  /* Transport keys default empty in Phase 1; user must configure in real deployments. */
  cfg->remote_signer_pubkey_hex[0] = '\0';
  cfg->remote_signer_secret_key_hex[0] = '\0';

  strncpy(cfg->vault_url, "http://127.0.0.1:8200", sizeof(cfg->vault_url) - 1);
  strncpy(cfg->vault_kv_mount, "secret", sizeof(cfg->vault_kv_mount) - 1);
  strncpy(cfg->vault_key_prefix, "signet/keys", sizeof(cfg->vault_key_prefix) - 1);
  strncpy(cfg->vault_policy_prefix, "signet/policies", sizeof(cfg->vault_policy_prefix) - 1);
  strncpy(cfg->vault_token_file, "", sizeof(cfg->vault_token_file) - 1);
  strncpy(cfg->vault_namespace, "", sizeof(cfg->vault_namespace) - 1);
  strncpy(cfg->vault_ca_bundle, "", sizeof(cfg->vault_ca_bundle) - 1);
  cfg->vault_timeout_ms = 5000;

  strncpy(cfg->policy_backend, "file", sizeof(cfg->policy_backend) - 1);
  strncpy(cfg->policy_file_path, "/var/lib/signet/policy.json", sizeof(cfg->policy_file_path) - 1);
  strncpy(cfg->policy_default_decision, "deny", sizeof(cfg->policy_default_decision) - 1);

  strncpy(cfg->audit_path, "/var/log/signet/audit.jsonl", sizeof(cfg->audit_path) - 1);
  cfg->audit_stdout = 0;

  cfg->replay_max_entries = 50000;
  cfg->replay_ttl_seconds = 600;
  cfg->replay_skew_seconds = 120;

  cfg->admin_pubkeys = NULL;
  cfg->n_admin_pubkeys = 0;

  cfg->health_enable = 1;
  strncpy(cfg->health_listen, "127.0.0.1:9486", sizeof(cfg->health_listen) - 1);
}

void signet_config_clear(SignetConfig *cfg) {
  if (!cfg) return;

  signet_free_strv(cfg->relays, cfg->n_relays);
  cfg->relays = NULL;
  cfg->n_relays = 0;

  signet_free_strv(cfg->admin_pubkeys, cfg->n_admin_pubkeys);
  cfg->admin_pubkeys = NULL;
  cfg->n_admin_pubkeys = 0;

  /* Best-effort wipe of sensitive fields. */
  memset(cfg->remote_signer_secret_key_hex, 0, sizeof(cfg->remote_signer_secret_key_hex));

  /* Keep structure in a safe, default-initialized state. */
  signet_config_init(cfg);
}

int signet_config_load(const char *path, SignetConfig *out_cfg) {
  (void)path;

  if (!out_cfg) return -1;

  /* Phase 1: stub loader always applies defaults.
   * Later phases parse TOML-style config and validate required fields. */
  signet_config_init(out_cfg);
  return 0;
}