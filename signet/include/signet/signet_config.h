/* SPDX-License-Identifier: MIT
 *
 * signet_config.h - Signet configuration model and loader.
 *
 * Load order:
 *   1. Defaults applied via signet_config_init()
 *   2. Config file loaded (GKeyFile INI/TOML-like format)
 *   3. SIGNET_-prefixed env vars override any field
 *
 * Required environment variables (Signet refuses to start without these):
 *   SIGNET_DB_KEY      - master key for SQLCipher (min 32 bytes, base64/hex)
 *   SIGNET_BUNKER_NSEC - Signet's own bunker identity nsec (bech32 or hex)
 *
 * Optional env overrides:
 *   SIGNET_RELAYS         - comma-separated relay URLs
 *   SIGNET_LOG_LEVEL      - debug|info|warn|error
 *   SIGNET_DB_PATH        - SQLCipher database path
 *   SIGNET_HEALTH_PORT    - health endpoint port (0 to disable)
 *   SIGNET_AUDIT_PATH     - audit log path (empty = stdout)
 *   SIGNET_POLICY_PATH    - policy file path
 *   SIGNET_PROVISIONER_PUBKEYS - comma-separated hex pubkeys
 */

#ifndef SIGNET_SIGNET_CONFIG_H
#define SIGNET_SIGNET_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGNET_MAX_STR            256
#define SIGNET_MAX_LISTEN_LEN     128
#define SIGNET_MAX_HEX_32         64
#define SIGNET_MAX_HEX_32_STRLEN  65 /* 64 hex + NUL */

typedef enum {
  SIGNET_LOG_ERROR = 0,
  SIGNET_LOG_WARN  = 1,
  SIGNET_LOG_INFO  = 2,
  SIGNET_LOG_DEBUG = 3,
} SignetLogLevel;

typedef struct {
  /* --- [server] --- */
  SignetLogLevel log_level;
  int health_port;                /* 0 = disabled */

  /* --- [store] --- */
  char db_path[SIGNET_MAX_STR];
  /* Master key injected via env: SIGNET_DB_KEY (required, no default) */

  /* --- [nostr] --- */
  char **relays;
  size_t n_relays;

  /* Bunker identity (derived from SIGNET_BUNKER_NSEC at startup). */
  char remote_signer_pubkey_hex[SIGNET_MAX_HEX_32_STRLEN];
  char remote_signer_secret_key_hex[SIGNET_MAX_HEX_32_STRLEN];

  uint32_t reconnect_interval_s;

  /* Pubkeys authorized to send management events (hex). */
  char **provisioner_pubkeys;
  size_t n_provisioner_pubkeys;

  /* Logical identity name (used to scope policy paths). */
  char identity[SIGNET_MAX_STR];

  /* --- [policy_defaults] --- */
  char policy_file_path[SIGNET_MAX_STR];
  char policy_default_decision[SIGNET_MAX_STR]; /* "deny" or "allow" */
  int *allowed_kinds;
  size_t n_allowed_kinds;
  uint32_t rate_limit_rpm;   /* 0 = unlimited */

  /* --- [replay] --- */
  size_t replay_max_entries;
  uint32_t replay_ttl_seconds;
  uint32_t replay_skew_seconds;

  /* --- [audit] --- */
  char audit_path[SIGNET_MAX_STR];
  bool audit_stdout;

  /* --- [bootstrap] --- */
  int bootstrap_port;              /* 0 = disabled (default) */

  /* --- [dbus] --- */
  bool dbus_unix_enabled;          /* system D-Bus (default false) */
  bool dbus_tcp_enabled;           /* TCP D-Bus (default false) */
  int  dbus_tcp_port;              /* default 47472 */

  /* --- [nip5l] --- */
  bool nip5l_enabled;              /* NIP-5L socket (default false) */
  char nip5l_socket_path[SIGNET_MAX_STR];

  /* --- [ssh_agent] --- */
  bool ssh_agent_enabled;          /* SSH agent socket (default false) */
  char ssh_agent_socket_path[SIGNET_MAX_STR];
} SignetConfig;

/* Initialize cfg with safe defaults. */
void signet_config_init(SignetConfig *cfg);

/* Free heap allocations inside cfg and zero sensitive fields. Safe to call multiple times. */
void signet_config_clear(SignetConfig *cfg);

/* Load configuration from path. If path is NULL or missing, defaults are applied.
 * Environment variable overrides are always applied last.
 * Returns 0 on success, -1 on hard failure (e.g., out-of-memory). */
int signet_config_load(const char *path, SignetConfig *out_cfg);

/* Validate that required fields are present. Returns 0 if valid, -1 if not.
 * On failure, writes a human-readable message to err_buf (if non-NULL). */
int signet_config_validate(const SignetConfig *cfg, char *err_buf, size_t err_buf_len);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SIGNET_CONFIG_H */