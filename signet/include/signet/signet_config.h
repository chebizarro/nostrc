/* SPDX-License-Identifier: MIT
 *
 * signet_config.h - Signet configuration model and loader.
 *
 * This module owns configuration parsing and defaults. It is intentionally
 * small and follows the monorepo's "defaults + load from file if present"
 * pattern (see apps/relayd/src/relayd_config.c).
 *
 * Phase 1: loader is a stub that applies defaults; later phases implement
 * TOML-style parsing and full validation.
 */

#ifndef SIGNET_SIGNET_CONFIG_H
#define SIGNET_SIGNET_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define SIGNET_MAX_STR            256
#define SIGNET_MAX_LISTEN_LEN     128
#define SIGNET_MAX_HEX_32         64
#define SIGNET_MAX_HEX_32_STRLEN  65 /* 64 hex + NUL */

typedef struct {
  /* Relay list (heap allocated array of heap allocated strings). */
  char **relays;
  size_t n_relays;

  /* Logical identity name (used to scope policy + Vault paths). */
  char identity[SIGNET_MAX_STR];

  /* NIP-46 transport keypair (remote signer key). */
  char remote_signer_pubkey_hex[SIGNET_MAX_HEX_32_STRLEN];
  char remote_signer_secret_key_hex[SIGNET_MAX_HEX_32_STRLEN];

  /* Vault settings (KV v2). */
  char vault_url[SIGNET_MAX_STR];
  char vault_kv_mount[SIGNET_MAX_STR];
  char vault_key_prefix[SIGNET_MAX_STR];
  char vault_policy_prefix[SIGNET_MAX_STR];
  char vault_token_file[SIGNET_MAX_STR];
  char vault_namespace[SIGNET_MAX_STR];
  char vault_ca_bundle[SIGNET_MAX_STR];
  uint32_t vault_timeout_ms;

  /* Policy settings. */
  char policy_backend[SIGNET_MAX_STR];     /* "file" or "vault" */
  char policy_file_path[SIGNET_MAX_STR];
  char policy_default_decision[SIGNET_MAX_STR]; /* "deny" or "allow" */

  /* Audit settings. */
  char audit_path[SIGNET_MAX_STR];
  int audit_stdout; /* boolean (0/1) */

  /* Replay protection. */
  size_t replay_max_entries;
  uint32_t replay_ttl_seconds;
  uint32_t replay_skew_seconds;

  /* Management settings. */
  char **admin_pubkeys;
  size_t n_admin_pubkeys;

  /* Health endpoint. */
  int health_enable; /* boolean (0/1) */
  char health_listen[SIGNET_MAX_LISTEN_LEN];
} SignetConfig;

/* Initialize cfg with safe defaults. */
void signet_config_init(SignetConfig *cfg);

/* Free heap allocations inside cfg and zero sensitive fields. Safe to call multiple times. */
void signet_config_clear(SignetConfig *cfg);

/* Load configuration from path. If path is NULL or missing, defaults are applied.
 * Returns 0 on success, -1 on hard failure (e.g., out-of-memory). */
int signet_config_load(const char *path, SignetConfig *out_cfg);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SIGNET_CONFIG_H */