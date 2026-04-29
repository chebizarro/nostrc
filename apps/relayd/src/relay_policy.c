#include "relay_policy.h"

#include <stddef.h>
#include <string.h>

#define SEEN_ID_CAPACITY 65536
#define DEFAULT_REPLAY_TTL_SECONDS 300

typedef struct {
  char id[65];
  time_t seen_at;
} SeenIdEntry;

static int g_seen_id_ttl_seconds = DEFAULT_REPLAY_TTL_SECONDS;
static SeenIdEntry g_seen_ids[SEEN_ID_CAPACITY];
static size_t g_seen_cursor = 0;

static int g_future_skew_seconds = 0;
static int g_past_skew_seconds = 0;

static inline int ids_equal64(const char *a, const char *b) {
  for (int i = 0; i < 64; i++) {
    if (a[i] != b[i]) return 0;
  }
  return 1;
}

void relay_policy_set_replay_ttl(int seconds) {
  g_seen_id_ttl_seconds = seconds > 0 ? seconds : 0;
}

int relay_policy_get_replay_ttl(void) {
  return g_seen_id_ttl_seconds;
}

void relay_policy_set_skew(int future_seconds, int past_seconds) {
  g_future_skew_seconds = future_seconds > 0 ? future_seconds : 0;
  g_past_skew_seconds = past_seconds > 0 ? past_seconds : 0;
}

void relay_policy_get_skew(int *future_seconds, int *past_seconds) {
  if (future_seconds) *future_seconds = g_future_skew_seconds;
  if (past_seconds) *past_seconds = g_past_skew_seconds;
}

int relay_policy_seen_id_check_and_add(const char *id_hex, time_t now) {
  size_t pos = 0;
  if (!id_hex || id_hex[0] == '\0') return 0;

  if (g_seen_id_ttl_seconds > 0) {
    size_t scan = SEEN_ID_CAPACITY < 1024 ? SEEN_ID_CAPACITY : 1024;
    for (size_t i = 0; i < scan; i++) {
      size_t idx = (g_seen_cursor + SEEN_ID_CAPACITY - 1 - i) % SEEN_ID_CAPACITY;
      if (g_seen_ids[idx].id[0] && (now - g_seen_ids[idx].seen_at) <= g_seen_id_ttl_seconds) {
        if (ids_equal64(g_seen_ids[idx].id, id_hex)) return 1;
      }
    }
  }

  pos = g_seen_cursor % SEEN_ID_CAPACITY;
  for (int i = 0; i < 64; i++) g_seen_ids[pos].id[i] = id_hex[i] ? id_hex[i] : '0';
  g_seen_ids[pos].id[64] = '\0';
  g_seen_ids[pos].seen_at = now;
  g_seen_cursor = (g_seen_cursor + 1) % SEEN_ID_CAPACITY;
  return 0;
}

int relay_policy_created_at_out_of_range(int64_t created_at, time_t now) {
  if (created_at <= 0) return 0;
  if (g_future_skew_seconds > 0 && (created_at - (int64_t)now) > g_future_skew_seconds) return 1;
  if (g_past_skew_seconds > 0 && ((int64_t)now - created_at) > g_past_skew_seconds) return 1;
  return 0;
}

void nostr_relay_set_replay_ttl(int seconds) {
  relay_policy_set_replay_ttl(seconds);
}

void nostr_relay_set_skew(int future_seconds, int past_seconds) {
  relay_policy_set_skew(future_seconds, past_seconds);
}

int nostr_relay_get_replay_ttl(void) {
  return relay_policy_get_replay_ttl();
}

void nostr_relay_get_skew(int *future_seconds, int *past_seconds) {
  relay_policy_get_skew(future_seconds, past_seconds);
}
