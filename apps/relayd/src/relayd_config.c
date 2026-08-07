#include "relayd_config.h"

#include "rate_limit.h"
#include "security_limits_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELAYD_MAX_EVENT_BYTES (2 * 1024 * 1024)
#define RELAYD_MAX_REPLAY_CAPACITY 1000000
#define RELAYD_MAX_SECONDS (7 * 24 * 60 * 60)

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int parse_supported_nips(const char *val, int *out, int *outn) {
  *outn = 0;
  if (!val) return -1;
  const char *p = val;
  while (*p && *p != '[') p++;
  if (*p != '[') return -1;
  p++;
  while (*p && *p != ']') {
    while (isspace((unsigned char)*p) || *p == ',') p++;
    if (*p == ']') break;
    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || v < 0 || v > INT_MAX) return -1;
    if (*outn >= RELAYD_MAX_SUPPORTED_NIPS) return -1;
    out[(*outn)++] = (int)v;
    p = end;
  }
  return *p == ']' ? 0 : -1;
}

static int parse_int(const char *text, int *out) {
  if (!text || !out) return -1;
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    return -1;
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end != '\0') return -1;
  *out = (int)value;
  return 0;
}

static int runtime_int(int64_t value, int fallback) {
  return value > 0 && value <= INT_MAX ? (int)value : fallback;
}

static void apply_defaults(RelaydConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  snprintf(cfg->listen, sizeof(cfg->listen), "%s", "127.0.0.1:4848");
  snprintf(cfg->storage_driver, sizeof(cfg->storage_driver), "%s", "nostrdb");
  cfg->supported_nips[0] = 1;
  cfg->supported_nips[1] = 11;
  cfg->supported_nips[2] = 42;
  cfg->supported_nips[3] = 45;
  cfg->supported_nips_count = 4;

  cfg->max_filters = 10;
  cfg->max_limit = 500;
  cfg->max_subs = 1;
  cfg->max_event_bytes =
      runtime_int(nostr_limit_max_event_size(), 256 * 1024);

  cfg->rate_ops_per_sec = 20;
  cfg->rate_burst = 40;
  cfg->rate_event_cost = 5;

  cfg->replay_cache_capacity = 65536;
  cfg->replay_ttl_seconds = 900;
  cfg->future_skew_seconds = 600;
  cfg->past_skew_seconds = 0;

  cfg->verification_cost = 5;
  cfg->verification_conn_per_sec =
      runtime_int(nostr_limit_verification_conn_per_sec(), 40);
  cfg->verification_conn_burst =
      runtime_int(nostr_limit_verification_conn_burst(), 80);
  cfg->verification_ip_per_sec =
      runtime_int(nostr_limit_verification_ip_per_sec(), 200);
  cfg->verification_ip_burst =
      runtime_int(nostr_limit_verification_ip_burst(), 400);
  cfg->verification_global_per_sec =
      runtime_int(nostr_limit_verification_global_per_sec(), 2000);
  cfg->verification_global_burst =
      runtime_int(nostr_limit_verification_global_burst(), 4000);
  cfg->verification_max_ips =
      runtime_int(nostr_limit_max_verification_ips(), 4096);
  cfg->verification_max_jobs =
      runtime_int(nostr_limit_max_verification_jobs(), 64);
  cfg->verification_max_bytes =
      runtime_int(nostr_limit_max_verification_bytes(), 16 * 1024 * 1024);
  cfg->verification_negative_cache_entries =
      runtime_int(nostr_limit_verification_negative_cache_entries(), 4096);
  cfg->verification_negative_ttl_seconds =
      runtime_int(nostr_limit_verification_negative_ttl_seconds(), 30);

  cfg->negentropy_enabled = 0;
  snprintf(cfg->name, sizeof(cfg->name), "%s", "nostrc-relayd");
  snprintf(cfg->software, sizeof(cfg->software), "%s", "nostrc");
  snprintf(cfg->version, sizeof(cfg->version), "%s", "0.1");
  snprintf(cfg->auth, sizeof(cfg->auth), "%s", "off");
}

static int fail(char *error, size_t size, const char *message) {
  if (error && size > 0) snprintf(error, size, "%s", message);
  return -1;
}

int relayd_config_validate(const RelaydConfig *cfg, char *error,
                           size_t error_size) {
  if (!cfg) return fail(error, error_size, "null config");
  if (cfg->max_filters <= 0 || cfg->max_limit <= 0 || cfg->max_subs <= 0)
    return fail(error, error_size, "basic limits must be positive");
  if (cfg->max_event_bytes < 1024 ||
      cfg->max_event_bytes > RELAYD_MAX_EVENT_BYTES)
    return fail(error, error_size, "max_event_bytes out of range");

  if (cfg->rate_ops_per_sec <= 0 ||
      cfg->rate_ops_per_sec > (int)RATE_LIMIT_MAX_RATE ||
      cfg->rate_burst <= 0 ||
      cfg->rate_burst > (int)RATE_LIMIT_MAX_BURST ||
      cfg->rate_event_cost <= 0 || cfg->rate_event_cost > cfg->rate_burst)
    return fail(error, error_size, "frame rate limits out of range");

  if (cfg->replay_cache_capacity <= 0 ||
      cfg->replay_cache_capacity > RELAYD_MAX_REPLAY_CAPACITY ||
      cfg->replay_ttl_seconds < 0 ||
      cfg->replay_ttl_seconds > RELAYD_MAX_SECONDS ||
      cfg->future_skew_seconds < 0 ||
      cfg->future_skew_seconds > RELAYD_MAX_SECONDS ||
      cfg->past_skew_seconds < 0 ||
      cfg->past_skew_seconds > RELAYD_MAX_SECONDS)
    return fail(error, error_size, "replay or skew limits out of range");

  uint64_t maximum_verification_cost =
      (uint64_t)(cfg->verification_cost > 0 ? cfg->verification_cost : 0) +
      ((uint64_t)cfg->max_event_bytes + 16383u) / 16384u;
  if (cfg->verification_cost <= 0 ||
      maximum_verification_cost > RATE_LIMIT_MAX_BURST ||
      cfg->verification_conn_per_sec <= 0 ||
      cfg->verification_conn_per_sec > (int)RATE_LIMIT_MAX_RATE ||
      cfg->verification_conn_burst <= 0 ||
      (uint64_t)cfg->verification_conn_burst < maximum_verification_cost ||
      cfg->verification_conn_burst > (int)RATE_LIMIT_MAX_BURST ||
      cfg->verification_ip_per_sec < cfg->verification_conn_per_sec ||
      cfg->verification_ip_per_sec > (int)RATE_LIMIT_MAX_RATE ||
      cfg->verification_ip_burst < cfg->verification_conn_burst ||
      cfg->verification_ip_burst > (int)RATE_LIMIT_MAX_BURST ||
      cfg->verification_global_per_sec < cfg->verification_ip_per_sec ||
      cfg->verification_global_per_sec > (int)RATE_LIMIT_MAX_RATE ||
      cfg->verification_global_burst < cfg->verification_ip_burst ||
      cfg->verification_global_burst > (int)RATE_LIMIT_MAX_BURST)
    return fail(error, error_size, "verification token limits out of range");

  if (cfg->verification_max_ips <= 0 ||
      cfg->verification_max_ips > RELAYD_MAX_REPLAY_CAPACITY ||
      cfg->verification_max_jobs <= 0 ||
      cfg->verification_max_jobs > RELAYD_MAX_REPLAY_CAPACITY ||
      cfg->verification_max_bytes < cfg->max_event_bytes ||
      cfg->verification_max_bytes > RELAYD_MAX_EVENT_BYTES * 64 ||
      cfg->verification_negative_cache_entries <= 0 ||
      cfg->verification_negative_cache_entries > RELAYD_MAX_REPLAY_CAPACITY ||
      cfg->verification_negative_ttl_seconds <= 0 ||
      cfg->verification_negative_ttl_seconds > 3600)
    return fail(error, error_size, "verification resource limits out of range");

  if (strcmp(cfg->auth, "off") != 0 && strcmp(cfg->auth, "optional") != 0 &&
      strcmp(cfg->auth, "required") != 0)
    return fail(error, error_size, "auth must be off, optional, or required");
  return 0;
}

int relayd_config_load(const char *path, RelaydConfig *out) {
  if (!out) return -1;
  apply_defaults(out);
  if (!path) return relayd_config_validate(out, NULL, 0);

  FILE *file = fopen(path, "r");
  if (!file) return relayd_config_validate(out, NULL, 0);

  int parse_failed = 0;
  char line[512];
  while (fgets(line, sizeof(line), file)) {
    trim(line);
    if (line[0] == '#' || line[0] == ';' || line[0] == 0) continue;
    char *eq = strchr(line, '=');
    if (!eq) {
      parse_failed = 1;
      break;
    }
    *eq = 0;
    char *key = line;
    char *val = eq + 1;
    trim(key);
    trim(val);
    if (*val == '"') {
      size_t len = strlen(val);
      if (len < 2 || val[len - 1] != '"') {
        parse_failed = 1;
        break;
      }
      val[len - 1] = 0;
      val++;
    }

#define PARSE_FIELD(name, field)                                                \
    else if (strcmp(key, name) == 0) {                                         \
      if (parse_int(val, &out->field) != 0) parse_failed = 1;                  \
    }

    if (strcmp(key, "listen") == 0)
      snprintf(out->listen, sizeof(out->listen), "%s", val);
    else if (strcmp(key, "storage_driver") == 0)
      snprintf(out->storage_driver, sizeof(out->storage_driver), "%s", val);
    else if (strcmp(key, "supported_nips") == 0) {
      int count = 0;
      if (parse_supported_nips(val, out->supported_nips, &count) != 0)
        parse_failed = 1;
      else
        out->supported_nips_count = count;
    }
    PARSE_FIELD("max_filters", max_filters)
    PARSE_FIELD("max_limit", max_limit)
    PARSE_FIELD("max_subs", max_subs)
    PARSE_FIELD("max_event_bytes", max_event_bytes)
    PARSE_FIELD("rate_ops_per_sec", rate_ops_per_sec)
    PARSE_FIELD("rate_burst", rate_burst)
    PARSE_FIELD("rate_event_cost", rate_event_cost)
    PARSE_FIELD("replay_cache_capacity", replay_cache_capacity)
    PARSE_FIELD("replay_ttl_seconds", replay_ttl_seconds)
    PARSE_FIELD("future_skew_seconds", future_skew_seconds)
    PARSE_FIELD("past_skew_seconds", past_skew_seconds)
    PARSE_FIELD("verification_cost", verification_cost)
    PARSE_FIELD("verification_conn_per_sec", verification_conn_per_sec)
    PARSE_FIELD("verification_conn_burst", verification_conn_burst)
    PARSE_FIELD("verification_ip_per_sec", verification_ip_per_sec)
    PARSE_FIELD("verification_ip_burst", verification_ip_burst)
    PARSE_FIELD("verification_global_per_sec", verification_global_per_sec)
    PARSE_FIELD("verification_global_burst", verification_global_burst)
    PARSE_FIELD("verification_max_ips", verification_max_ips)
    PARSE_FIELD("verification_max_jobs", verification_max_jobs)
    PARSE_FIELD("verification_max_bytes", verification_max_bytes)
    PARSE_FIELD("verification_negative_cache_entries",
                verification_negative_cache_entries)
    PARSE_FIELD("verification_negative_ttl_seconds",
                verification_negative_ttl_seconds)
    else if (strcmp(key, "name") == 0)
      snprintf(out->name, sizeof(out->name), "%s", val);
    else if (strcmp(key, "software") == 0)
      snprintf(out->software, sizeof(out->software), "%s", val);
    else if (strcmp(key, "version") == 0)
      snprintf(out->version, sizeof(out->version), "%s", val);
    else if (strcmp(key, "description") == 0)
      snprintf(out->description, sizeof(out->description), "%s", val);
    else if (strcmp(key, "contact") == 0)
      snprintf(out->contact, sizeof(out->contact), "%s", val);
    else if (strcmp(key, "auth") == 0)
      snprintf(out->auth, sizeof(out->auth), "%s", val);
    PARSE_FIELD("negentropy_enabled", negentropy_enabled)

#undef PARSE_FIELD

    if (parse_failed) break;
  }
  fclose(file);
  if (parse_failed) return -1;
  return relayd_config_validate(out, NULL, 0);
}
