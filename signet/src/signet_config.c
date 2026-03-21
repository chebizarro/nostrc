/* SPDX-License-Identifier: MIT
 *
 * signet_config.c - Signet configuration loader.
 *
 * Load order: defaults → GKeyFile (INI/TOML-like) → env var overrides.
 * Secrets are ONLY loaded from env vars; never from config files.
 */

#include "signet/signet_config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

/* libnostr: nsec decode + pubkey derivation */
#include <nostr-keys.h>
#include <nostr/nip19/nip19.h>
#include <secure_buf.h>

/* ------------------------------ helpers ---------------------------------- */

static void signet_free_strv(char **v, size_t n) {
  if (!v) return;
  for (size_t i = 0; i < n; i++) free(v[i]);
  free(v);
}

static void signet_free_intv(int *v) {
  free(v);
}

/* Parse a comma/semicolon separated string into a string array.
 * Also handles GKeyFile-style "val1;val2;val3" lists. */
static char **signet_parse_csv(const char *raw, size_t *out_count) {
  *out_count = 0;
  if (!raw || !raw[0]) return NULL;

  gchar **parts = g_strsplit_set(raw, ",;", -1);
  if (!parts) return NULL;

  /* Count non-empty parts */
  size_t count = 0;
  for (int i = 0; parts[i]; i++) {
    g_strstrip(parts[i]);
    /* Strip surrounding quotes */
    size_t len = strlen(parts[i]);
    if (len >= 2 && parts[i][0] == '"' && parts[i][len-1] == '"') {
      parts[i][len-1] = '\0';
      memmove(parts[i], parts[i]+1, len-1);
    }
    if (parts[i][0]) count++;
  }

  if (count == 0) {
    g_strfreev(parts);
    return NULL;
  }

  char **result = (char **)calloc(count, sizeof(char *));
  if (!result) {
    g_strfreev(parts);
    return NULL;
  }

  size_t j = 0;
  for (int i = 0; parts[i] && j < count; i++) {
    if (parts[i][0]) {
      result[j++] = strdup(parts[i]);
    }
  }

  g_strfreev(parts);
  *out_count = count;
  return result;
}

/* Parse a comma-separated list of integers. */
static int *signet_parse_int_csv(const char *raw, size_t *out_count) {
  *out_count = 0;
  if (!raw || !raw[0]) return NULL;

  gchar **parts = g_strsplit_set(raw, ",;", -1);
  if (!parts) return NULL;

  /* Count valid integers */
  size_t count = 0;
  for (int i = 0; parts[i]; i++) {
    g_strstrip(parts[i]);
    if (parts[i][0]) count++;
  }

  if (count == 0) {
    g_strfreev(parts);
    return NULL;
  }

  int *result = (int *)calloc(count, sizeof(int));
  if (!result) {
    g_strfreev(parts);
    return NULL;
  }

  size_t j = 0;
  for (int i = 0; parts[i] && j < count; i++) {
    g_strstrip(parts[i]);
    if (!parts[i][0]) continue;
    char *end = NULL;
    long v = strtol(parts[i], &end, 10);
    if (end && *end == '\0') {
      result[j++] = (int)v;
    }
  }

  g_strfreev(parts);
  *out_count = j;
  if (j == 0) { free(result); return NULL; }
  return result;
}

static SignetLogLevel signet_parse_log_level(const char *s) {
  if (!s) return SIGNET_LOG_INFO;
  if (g_ascii_strcasecmp(s, "debug") == 0) return SIGNET_LOG_DEBUG;
  if (g_ascii_strcasecmp(s, "warn") == 0) return SIGNET_LOG_WARN;
  if (g_ascii_strcasecmp(s, "error") == 0) return SIGNET_LOG_ERROR;
  return SIGNET_LOG_INFO;
}

/* Safe strncpy that always NUL-terminates. */
static void signet_strlcpy(char *dst, const char *src, size_t dst_sz) {
  if (!dst || dst_sz == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t len = strlen(src);
  if (len >= dst_sz) len = dst_sz - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/* ----------------------------- hex helpers -------------------------------- */

static void signet_bytes_to_hex(const uint8_t *in, size_t in_len, char *out, size_t out_sz) {
  static const char hex[] = "0123456789abcdef";
  if (!in || !out || out_sz < (in_len * 2 + 1)) { if (out && out_sz) out[0] = '\0'; return; }
  for (size_t i = 0; i < in_len; i++) {
    out[i*2]   = hex[(in[i] >> 4) & 0xF];
    out[i*2+1] = hex[in[i] & 0xF];
  }
  out[in_len * 2] = '\0';
}

static bool signet_is_hex(const char *s, size_t expected_len) {
  if (!s) return false;
  for (size_t i = 0; i < expected_len; i++) {
    char c = s[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return s[expected_len] == '\0';
}

/* ------------------------------ init/clear -------------------------------- */

void signet_config_init(SignetConfig *cfg) {
  if (!cfg) return;

  memset(cfg, 0, sizeof(*cfg));

  cfg->log_level = SIGNET_LOG_INFO;
  cfg->health_port = 8080;

  signet_strlcpy(cfg->db_path, "/data/signet.db", sizeof(cfg->db_path));

  cfg->reconnect_interval_s = 30;

  signet_strlcpy(cfg->identity, "default", sizeof(cfg->identity));

  signet_strlcpy(cfg->policy_file_path, "", sizeof(cfg->policy_file_path));
  signet_strlcpy(cfg->policy_default_decision, "deny", sizeof(cfg->policy_default_decision));
  cfg->rate_limit_rpm = 0;

  cfg->replay_max_entries = 50000;
  cfg->replay_ttl_seconds = 600;
  cfg->replay_skew_seconds = 120;

  signet_strlcpy(cfg->audit_path, "", sizeof(cfg->audit_path));
  cfg->audit_stdout = true;
}

void signet_config_clear(SignetConfig *cfg) {
  if (!cfg) return;

  signet_free_strv(cfg->relays, cfg->n_relays);
  cfg->relays = NULL;
  cfg->n_relays = 0;

  signet_free_strv(cfg->provisioner_pubkeys, cfg->n_provisioner_pubkeys);
  cfg->provisioner_pubkeys = NULL;
  cfg->n_provisioner_pubkeys = 0;

  signet_free_intv(cfg->allowed_kinds);
  cfg->allowed_kinds = NULL;
  cfg->n_allowed_kinds = 0;

  /* Wipe sensitive fields. */
  secure_wipe(cfg->remote_signer_secret_key_hex, sizeof(cfg->remote_signer_secret_key_hex));

  signet_config_init(cfg);
}

/* ----------------------------- file loader -------------------------------- */

static void signet_config_load_keyfile(GKeyFile *kf, SignetConfig *cfg) {
  gchar *val = NULL;

  /* [server] */
  val = g_key_file_get_string(kf, "server", "log_level", NULL);
  if (val) { cfg->log_level = signet_parse_log_level(val); g_free(val); }

  if (g_key_file_has_key(kf, "server", "health_port", NULL)) {
    cfg->health_port = g_key_file_get_integer(kf, "server", "health_port", NULL);
  }

  /* [store] */
  val = g_key_file_get_string(kf, "store", "db_path", NULL);
  if (val) { signet_strlcpy(cfg->db_path, val, sizeof(cfg->db_path)); g_free(val); }

  /* [nostr] */
  val = g_key_file_get_string(kf, "nostr", "relays", NULL);
  if (val) {
    signet_free_strv(cfg->relays, cfg->n_relays);
    cfg->relays = signet_parse_csv(val, &cfg->n_relays);
    g_free(val);
  }

  if (g_key_file_has_key(kf, "nostr", "reconnect_interval_s", NULL)) {
    cfg->reconnect_interval_s = (uint32_t)g_key_file_get_integer(kf, "nostr", "reconnect_interval_s", NULL);
  }

  val = g_key_file_get_string(kf, "nostr", "provisioner_pubkeys", NULL);
  if (val) {
    signet_free_strv(cfg->provisioner_pubkeys, cfg->n_provisioner_pubkeys);
    cfg->provisioner_pubkeys = signet_parse_csv(val, &cfg->n_provisioner_pubkeys);
    g_free(val);
  }

  val = g_key_file_get_string(kf, "nostr", "identity", NULL);
  if (val) { signet_strlcpy(cfg->identity, val, sizeof(cfg->identity)); g_free(val); }

  /* [policy_defaults] */
  val = g_key_file_get_string(kf, "policy_defaults", "default_decision", NULL);
  if (val) { signet_strlcpy(cfg->policy_default_decision, val, sizeof(cfg->policy_default_decision)); g_free(val); }

  val = g_key_file_get_string(kf, "policy_defaults", "policy_file", NULL);
  if (val) { signet_strlcpy(cfg->policy_file_path, val, sizeof(cfg->policy_file_path)); g_free(val); }

  val = g_key_file_get_string(kf, "policy_defaults", "allowed_kinds", NULL);
  if (val) {
    signet_free_intv(cfg->allowed_kinds);
    cfg->allowed_kinds = signet_parse_int_csv(val, &cfg->n_allowed_kinds);
    g_free(val);
  }

  if (g_key_file_has_key(kf, "policy_defaults", "rate_limit_rpm", NULL)) {
    cfg->rate_limit_rpm = (uint32_t)g_key_file_get_integer(kf, "policy_defaults", "rate_limit_rpm", NULL);
  }

  /* [replay] */
  if (g_key_file_has_key(kf, "replay", "max_entries", NULL))
    cfg->replay_max_entries = (size_t)g_key_file_get_integer(kf, "replay", "max_entries", NULL);
  if (g_key_file_has_key(kf, "replay", "ttl_seconds", NULL))
    cfg->replay_ttl_seconds = (uint32_t)g_key_file_get_integer(kf, "replay", "ttl_seconds", NULL);
  if (g_key_file_has_key(kf, "replay", "skew_seconds", NULL))
    cfg->replay_skew_seconds = (uint32_t)g_key_file_get_integer(kf, "replay", "skew_seconds", NULL);

  /* [audit] */
  val = g_key_file_get_string(kf, "audit", "path", NULL);
  if (val) { signet_strlcpy(cfg->audit_path, val, sizeof(cfg->audit_path)); g_free(val); }

  if (g_key_file_has_key(kf, "audit", "stdout", NULL))
    cfg->audit_stdout = g_key_file_get_boolean(kf, "audit", "stdout", NULL);

  /* [bootstrap] */
  if (g_key_file_has_key(kf, "bootstrap", "port", NULL))
    cfg->bootstrap_port = g_key_file_get_integer(kf, "bootstrap", "port", NULL);

  /* [dbus] */
  if (g_key_file_has_key(kf, "dbus", "unix_enabled", NULL))
    cfg->dbus_unix_enabled = g_key_file_get_boolean(kf, "dbus", "unix_enabled", NULL);
  if (g_key_file_has_key(kf, "dbus", "tcp_enabled", NULL))
    cfg->dbus_tcp_enabled = g_key_file_get_boolean(kf, "dbus", "tcp_enabled", NULL);
  if (g_key_file_has_key(kf, "dbus", "tcp_port", NULL))
    cfg->dbus_tcp_port = g_key_file_get_integer(kf, "dbus", "tcp_port", NULL);
  else
    cfg->dbus_tcp_port = 47472;

  /* [nip5l] */
  if (g_key_file_has_key(kf, "nip5l", "enabled", NULL))
    cfg->nip5l_enabled = g_key_file_get_boolean(kf, "nip5l", "enabled", NULL);
  val = g_key_file_get_string(kf, "nip5l", "socket_path", NULL);
  if (val) { signet_strlcpy(cfg->nip5l_socket_path, val, sizeof(cfg->nip5l_socket_path)); g_free(val); }

  /* [ssh_agent] */
  if (g_key_file_has_key(kf, "ssh_agent", "enabled", NULL))
    cfg->ssh_agent_enabled = g_key_file_get_boolean(kf, "ssh_agent", "enabled", NULL);
  val = g_key_file_get_string(kf, "ssh_agent", "socket_path", NULL);
  if (val) { signet_strlcpy(cfg->ssh_agent_socket_path, val, sizeof(cfg->ssh_agent_socket_path)); g_free(val); }
}

/* -------------------------- env var overrides ----------------------------- */

static void signet_config_apply_env(SignetConfig *cfg) {
  const char *val = NULL;

  /* Non-secret overrides */
  val = g_getenv("SIGNET_LOG_LEVEL");
  if (val) cfg->log_level = signet_parse_log_level(val);

  val = g_getenv("SIGNET_HEALTH_PORT");
  if (val) cfg->health_port = atoi(val);

  val = g_getenv("SIGNET_DB_PATH");
  if (val) signet_strlcpy(cfg->db_path, val, sizeof(cfg->db_path));

  val = g_getenv("SIGNET_RELAYS");
  if (val) {
    signet_free_strv(cfg->relays, cfg->n_relays);
    cfg->relays = signet_parse_csv(val, &cfg->n_relays);
  }

  val = g_getenv("SIGNET_POLICY_PATH");
  if (val) signet_strlcpy(cfg->policy_file_path, val, sizeof(cfg->policy_file_path));

  val = g_getenv("SIGNET_AUDIT_PATH");
  if (val) signet_strlcpy(cfg->audit_path, val, sizeof(cfg->audit_path));

  val = g_getenv("SIGNET_PROVISIONER_PUBKEYS");
  if (val) {
    signet_free_strv(cfg->provisioner_pubkeys, cfg->n_provisioner_pubkeys);
    cfg->provisioner_pubkeys = signet_parse_csv(val, &cfg->n_provisioner_pubkeys);
  }

  /* Secret: SIGNET_BUNKER_NSEC → derive seckey hex + pubkey hex */
  val = g_getenv("SIGNET_BUNKER_NSEC");
  if (val && val[0]) {
    if (g_ascii_strncasecmp(val, "nsec1", 5) == 0) {
      /* Bech32 nsec — decode to 32 bytes, then hex encode */
      uint8_t sk[32];
      if (nostr_nip19_decode_nsec(val, sk) == 0) {
        signet_bytes_to_hex(sk, 32, cfg->remote_signer_secret_key_hex,
                            sizeof(cfg->remote_signer_secret_key_hex));
        secure_wipe(sk, 32);
      }
    } else if (signet_is_hex(val, 64)) {
      /* Raw hex secret key */
      signet_strlcpy(cfg->remote_signer_secret_key_hex, val,
                      sizeof(cfg->remote_signer_secret_key_hex));
    }

    /* Derive public key */
    if (cfg->remote_signer_secret_key_hex[0]) {
      char *pub = nostr_key_get_public(cfg->remote_signer_secret_key_hex);
      if (pub) {
        signet_strlcpy(cfg->remote_signer_pubkey_hex, pub,
                        sizeof(cfg->remote_signer_pubkey_hex));
        free(pub);
      }
    }
  }

  /* SIGNET_DB_KEY is read at SQLCipher open time, not stored in config struct. */

  /* v2 transport overrides */
  val = g_getenv("SIGNET_BOOTSTRAP_PORT");
  if (val) cfg->bootstrap_port = atoi(val);

  val = g_getenv("SIGNET_DBUS_UNIX");
  if (val) cfg->dbus_unix_enabled = (atoi(val) != 0 || g_ascii_strcasecmp(val, "true") == 0);

  val = g_getenv("SIGNET_DBUS_TCP");
  if (val) cfg->dbus_tcp_enabled = (atoi(val) != 0 || g_ascii_strcasecmp(val, "true") == 0);

  val = g_getenv("SIGNET_DBUS_TCP_PORT");
  if (val) cfg->dbus_tcp_port = atoi(val);

  val = g_getenv("SIGNET_NIP5L");
  if (val) cfg->nip5l_enabled = (atoi(val) != 0 || g_ascii_strcasecmp(val, "true") == 0);

  val = g_getenv("SIGNET_NIP5L_SOCKET");
  if (val) signet_strlcpy(cfg->nip5l_socket_path, val, sizeof(cfg->nip5l_socket_path));

  val = g_getenv("SIGNET_SSH_AGENT");
  if (val) cfg->ssh_agent_enabled = (atoi(val) != 0 || g_ascii_strcasecmp(val, "true") == 0);

  val = g_getenv("SIGNET_SSH_AGENT_SOCKET");
  if (val) signet_strlcpy(cfg->ssh_agent_socket_path, val, sizeof(cfg->ssh_agent_socket_path));
}

/* ------------------------------ public API -------------------------------- */

int signet_config_load(const char *path, SignetConfig *out_cfg) {
  if (!out_cfg) return -1;

  signet_config_init(out_cfg);

  /* Load config file if specified and exists. */
  if (path && path[0]) {
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
      signet_config_load_keyfile(kf, out_cfg);
    } else {
      /* File missing or parse error — proceed with defaults + env. */
      if (err) g_error_free(err);
    }

    g_key_file_free(kf);
  }

  /* Environment variables always override. */
  signet_config_apply_env(out_cfg);

  return 0;
}

int signet_config_validate(const SignetConfig *cfg, char *err_buf, size_t err_buf_len) {
  if (!cfg) {
    if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "config is NULL");
    return -1;
  }

  /* SIGNET_BUNKER_NSEC must have been provided. */
  if (!cfg->remote_signer_secret_key_hex[0]) {
    if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "SIGNET_BUNKER_NSEC not set or invalid");
    return -1;
  }

  /* At least one relay required. */
  if (cfg->n_relays == 0) {
    if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "no relays configured (set [nostr] relays or SIGNET_RELAYS)");
    return -1;
  }

  /* DB path must be non-empty. */
  if (!cfg->db_path[0]) {
    if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "db_path is empty");
    return -1;
  }

  return 0;
}