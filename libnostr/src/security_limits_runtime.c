#include "security_limits_runtime.h"
#include "security_limits.h"
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

static int is_space_ch(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/*
 * Robustly parse a signed decimal environment variable.
 *
 * Rejects (returns fallback) on:
 *   - unset / empty / all-whitespace values
 *   - no digits consumed
 *   - trailing junk after the number (e.g. "123abc")
 *   - overflow/underflow reported by strtoll via errno == ERANGE
 *     (e.g. a 21-digit value like 999999999999999999999)
 *   - non-positive results (limits must be >= 1)
 *
 * A successfully parsed value is clamped to [1, INT64_MAX].
 */
static long long read_ll_env(const char *name, long long fallback) {
  const char *s = getenv(name);
  if (!s) return fallback;

  /* Skip leading whitespace so an all-blank value is treated as empty. */
  const char *p = s;
  while (is_space_ch((unsigned char)*p)) p++;
  if (*p == '\0') return fallback;

  errno = 0;
  char *end = NULL;
  long long v = strtoll(p, &end, 10);
  if (end == p) return fallback;              /* no digits consumed */
  if (errno == ERANGE) return fallback;       /* overflow / underflow */

  /* Only trailing whitespace is allowed after the number. */
  while (is_space_ch((unsigned char)*end)) end++;
  if (*end != '\0') return fallback;          /* trailing junk, e.g. "123abc" */

  if (v <= 0) return fallback;                /* limits must be positive */

  /* Clamp to a sane upper bound. int64_t is the public return type. */
#if LLONG_MAX > INT64_MAX
  if (v > (long long)INT64_MAX) return (long long)INT64_MAX;
#endif
  return v;
}

/*
 * All limits are resolved exactly once, guarded by pthread_once. This avoids
 * the first-call data race that the previous per-getter function-local static
 * caches had (two threads could observe/write a partially-initialized value).
 */
typedef struct {
  int64_t max_frame_len;
  int64_t max_frames_per_sec;
  int64_t max_bytes_per_sec;
  int64_t max_event_size;
  int64_t max_tags_per_event;
  int64_t max_tag_depth;
  int64_t max_ids_per_filter;
  int64_t max_filters_per_req;
  int64_t invalidsig_window_seconds;
  int64_t invalidsig_threshold;
  int64_t invalidsig_ban_seconds;
  int64_t ws_read_timeout_seconds;
  int64_t ws_progress_window_ms;
  int64_t ws_min_bytes_per_window;
} nostr_limits_t;

static nostr_limits_t g_limits;
static pthread_once_t g_limits_once = PTHREAD_ONCE_INIT;

static void limits_init_once(void) {
  g_limits.max_frame_len              = read_ll_env("NOSTR_MAX_FRAME_LEN_BYTES", NOSTR_MAX_FRAME_LEN_BYTES);
  g_limits.max_frames_per_sec         = read_ll_env("NOSTR_MAX_FRAMES_PER_SEC", NOSTR_MAX_FRAMES_PER_SEC);
  g_limits.max_bytes_per_sec          = read_ll_env("NOSTR_MAX_BYTES_PER_SEC", NOSTR_MAX_BYTES_PER_SEC);
  g_limits.max_event_size             = read_ll_env("NOSTR_MAX_EVENT_SIZE_BYTES", NOSTR_MAX_EVENT_SIZE_BYTES);
  g_limits.max_tags_per_event         = read_ll_env("NOSTR_MAX_TAGS_PER_EVENT", NOSTR_MAX_TAGS_PER_EVENT);
  g_limits.max_tag_depth              = read_ll_env("NOSTR_MAX_TAG_DEPTH", NOSTR_MAX_TAG_DEPTH);
  g_limits.max_ids_per_filter         = read_ll_env("NOSTR_MAX_IDS_PER_FILTER", NOSTR_MAX_IDS_PER_FILTER);
  g_limits.max_filters_per_req        = read_ll_env("NOSTR_MAX_FILTERS_PER_REQ", NOSTR_MAX_FILTERS_PER_REQ);
  g_limits.invalidsig_window_seconds  = read_ll_env("NOSTR_INVALID_SIG_WINDOW_SECONDS", NOSTR_INVALID_SIG_WINDOW_SECONDS);
  g_limits.invalidsig_threshold       = read_ll_env("NOSTR_INVALID_SIG_THRESHOLD", NOSTR_INVALID_SIG_THRESHOLD);
  g_limits.invalidsig_ban_seconds     = read_ll_env("NOSTR_INVALID_SIG_BAN_SECONDS", NOSTR_INVALID_SIG_BAN_SECONDS);
  g_limits.ws_read_timeout_seconds    = read_ll_env("NOSTR_WS_READ_TIMEOUT_SECONDS", 120);   /* default 120s */
  g_limits.ws_progress_window_ms      = read_ll_env("NOSTR_WS_PROGRESS_WINDOW_MS", 30000);    /* default 30s window */
  g_limits.ws_min_bytes_per_window    = read_ll_env("NOSTR_WS_MIN_BYTES_PER_WINDOW", 1);      /* default 1B */
}

static inline void limits_ensure_init(void) {
  pthread_once(&g_limits_once, limits_init_once);
}

int64_t nostr_limit_max_frame_len(void) {
  limits_ensure_init();
  return g_limits.max_frame_len;
}

int64_t nostr_limit_max_frames_per_sec(void) {
  limits_ensure_init();
  return g_limits.max_frames_per_sec;
}

int64_t nostr_limit_max_bytes_per_sec(void) {
  limits_ensure_init();
  return g_limits.max_bytes_per_sec;
}

int64_t nostr_limit_max_event_size(void) {
  limits_ensure_init();
  return g_limits.max_event_size;
}

int64_t nostr_limit_max_tags_per_event(void) {
  limits_ensure_init();
  return g_limits.max_tags_per_event;
}

int64_t nostr_limit_max_tag_depth(void) {
  limits_ensure_init();
  return g_limits.max_tag_depth;
}

int64_t nostr_limit_max_ids_per_filter(void) {
  limits_ensure_init();
  return g_limits.max_ids_per_filter;
}

int64_t nostr_limit_max_filters_per_req(void) {
  limits_ensure_init();
  return g_limits.max_filters_per_req;
}

int64_t nostr_limit_invalidsig_window_seconds(void) {
  limits_ensure_init();
  return g_limits.invalidsig_window_seconds;
}

int64_t nostr_limit_invalidsig_threshold(void) {
  limits_ensure_init();
  return g_limits.invalidsig_threshold;
}

int64_t nostr_limit_invalidsig_ban_seconds(void) {
  limits_ensure_init();
  return g_limits.invalidsig_ban_seconds;
}

/* WebSocket slowloris/timeouts (no compile-time defaults yet; use literals) */
int64_t nostr_limit_ws_read_timeout_seconds(void) {
  limits_ensure_init();
  return g_limits.ws_read_timeout_seconds;
}

int64_t nostr_limit_ws_progress_window_ms(void) {
  limits_ensure_init();
  return g_limits.ws_progress_window_ms;
}

int64_t nostr_limit_ws_min_bytes_per_window(void) {
  limits_ensure_init();
  return g_limits.ws_min_bytes_per_window;
}
