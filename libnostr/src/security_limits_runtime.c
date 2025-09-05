#include "security_limits_runtime.h"
#include "security_limits.h"
#include <stdlib.h>
#include <stdint.h>

static long long read_ll_env(const char *name, long long fallback) {
  const char *s = getenv(name);
  if (!s || !*s) return fallback;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  if (end == s) return fallback;
  if (v <= 0) return fallback;
  return v;
}

int64_t nostr_limit_max_frame_len(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_FRAME_LEN_BYTES", NOSTR_MAX_FRAME_LEN_BYTES);
  return val;
}

int64_t nostr_limit_max_frames_per_sec(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_FRAMES_PER_SEC", NOSTR_MAX_FRAMES_PER_SEC);
  return val;
}

int64_t nostr_limit_max_bytes_per_sec(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_BYTES_PER_SEC", NOSTR_MAX_BYTES_PER_SEC);
  return val;
}

int64_t nostr_limit_max_event_size(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_EVENT_SIZE_BYTES", NOSTR_MAX_EVENT_SIZE_BYTES);
  return val;
}

int64_t nostr_limit_max_tags_per_event(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_TAGS_PER_EVENT", NOSTR_MAX_TAGS_PER_EVENT);
  return val;
}

int64_t nostr_limit_max_tag_depth(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_TAG_DEPTH", NOSTR_MAX_TAG_DEPTH);
  return val;
}

int64_t nostr_limit_max_ids_per_filter(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_IDS_PER_FILTER", NOSTR_MAX_IDS_PER_FILTER);
  return val;
}

int64_t nostr_limit_max_filters_per_req(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_MAX_FILTERS_PER_REQ", NOSTR_MAX_FILTERS_PER_REQ);
  return val;
}

int64_t nostr_limit_invalidsig_window_seconds(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_INVALID_SIG_WINDOW_SECONDS", NOSTR_INVALID_SIG_WINDOW_SECONDS);
  return val;
}

int64_t nostr_limit_invalidsig_threshold(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_INVALID_SIG_THRESHOLD", NOSTR_INVALID_SIG_THRESHOLD);
  return val;
}

int64_t nostr_limit_invalidsig_ban_seconds(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_INVALID_SIG_BAN_SECONDS", NOSTR_INVALID_SIG_BAN_SECONDS);
  return val;
}

/* WebSocket slowloris/timeouts (no compile-time defaults yet; use literals) */
int64_t nostr_limit_ws_read_timeout_seconds(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_WS_READ_TIMEOUT_SECONDS", 60); /* default 60s */
  return val;
}

int64_t nostr_limit_ws_progress_window_ms(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_WS_PROGRESS_WINDOW_MS", 5000); /* default 5s */
  return val;
}

int64_t nostr_limit_ws_min_bytes_per_window(void) {
  static long long val = -1;
  if (val < 0) val = read_ll_env("NOSTR_WS_MIN_BYTES_PER_WINDOW", 256); /* default 256B */
  return val;
}
