#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "protocol_nip01.h"
#include "nostr-event.h"
#include "json.h"

/* Standalone, test-focused policy evaluator that does not depend on libwebsockets or NIP-42/50. */

/* Replay cache (simple fixed-size ring with TTL) */
#define SEEN_ID_CAPACITY 65536
static int g_seen_id_ttl_seconds_pd = 0;
typedef struct { char id[65]; time_t seen_at; } SeenIdEntryPD;
static SeenIdEntryPD g_seen_ids_pd[SEEN_ID_CAPACITY];
static size_t g_seen_cursor_pd = 0;

static inline int ids_equal64_pd(const char *a, const char *b) {
  for (int i = 0; i < 64; i++) if (a[i] != b[i]) return 0; return 1;
}

static int seen_ids_check_and_add_pd(const char *id_hex, time_t now) {
  if (!id_hex || id_hex[0] == '\0') return 0;
  size_t scan = SEEN_ID_CAPACITY < 1024 ? SEEN_ID_CAPACITY : 1024;
  if (g_seen_id_ttl_seconds_pd <= 0) goto insert;
  for (size_t i = 0; i < scan; i++) {
    size_t idx = (g_seen_cursor_pd + SEEN_ID_CAPACITY - 1 - i) % SEEN_ID_CAPACITY;
    if (g_seen_ids_pd[idx].id[0] && (now - g_seen_ids_pd[idx].seen_at) <= g_seen_id_ttl_seconds_pd) {
      if (ids_equal64_pd(g_seen_ids_pd[idx].id, id_hex)) return 1;
    }
  }
insert:
  size_t pos = g_seen_cursor_pd % SEEN_ID_CAPACITY;
  for (int i = 0; i < 64; i++) g_seen_ids_pd[pos].id[i] = id_hex[i] ? id_hex[i] : '0';
  g_seen_ids_pd[pos].id[64] = '\0';
  g_seen_ids_pd[pos].seen_at = now;
  g_seen_cursor_pd = (g_seen_cursor_pd + 1) % SEEN_ID_CAPACITY;
  return 0;
}

/* Skew policy */
static int g_future_skew_seconds_pd = 0;
static int g_past_skew_seconds_pd = 0;

/* Public setters/getters (mirror protocol_nip01.h API) */
void nostr_relay_set_replay_ttl(int seconds) { g_seen_id_ttl_seconds_pd = seconds > 0 ? seconds : 0; }
void nostr_relay_set_skew(int future_seconds, int past_seconds) {
  g_future_skew_seconds_pd = future_seconds > 0 ? future_seconds : 0;
  g_past_skew_seconds_pd = past_seconds > 0 ? past_seconds : 0;
}
int  nostr_relay_get_replay_ttl(void) { return g_seen_id_ttl_seconds_pd; }
void nostr_relay_get_skew(int *future_seconds, int *past_seconds) {
  if (future_seconds) *future_seconds = g_future_skew_seconds_pd;
  if (past_seconds) *past_seconds = g_past_skew_seconds_pd;
}

/* Testable ingress decision (no sockets). Returns -1 reject, 0 accept-but-don't-store, 1 store. */
int relayd_nip01_ingress_decide_json(const char *event_json, int64_t now_override, const char **out_reason) {
  if (out_reason) *out_reason = NULL;
  if (!event_json) return -1;
  int rc_store = -1; const char *reason = NULL; char *id = NULL;
  char *ebuf = strdup(event_json);
  if (!ebuf) { if (out_reason) *out_reason = "oom"; return -1; }
  NostrEvent *ev = nostr_event_new(); int ok_parse = 0;
  if (ev) {
    if (nostr_event_deserialize_compact(ev, ebuf)) ok_parse = 1;
    else ok_parse = (nostr_event_deserialize(ev, ebuf) == 0);
  }
  if (!ok_parse || !ev) { free(ebuf); if (ev) nostr_event_free(ev); if (out_reason) *out_reason = "parse error"; return -1; }
  time_t now = now_override > 0 ? (time_t)now_override : time(NULL);
  int64_t created_at = nostr_event_get_created_at(ev);
  if (g_future_skew_seconds_pd > 0 || g_past_skew_seconds_pd > 0) {
    if (created_at > 0 &&
        (((g_future_skew_seconds_pd > 0) && ((created_at - (int64_t)now) > g_future_skew_seconds_pd)) ||
         ((g_past_skew_seconds_pd > 0) && (((int64_t)now - created_at) > g_past_skew_seconds_pd)))) {
      reason = "invalid: created_at out of range";
      rc_store = -1;
      goto out;
    }
  }
  if (!nostr_event_check_signature(ev)) { reason = "invalid: bad signature"; rc_store = -1; goto out; }
  id = nostr_event_get_id(ev);
  if (id && strlen(id) == 64 && g_seen_id_ttl_seconds_pd > 0) {
    if (seen_ids_check_and_add_pd(id, now)) { rc_store = 0; reason = "duplicate"; goto out; }
  }
  rc_store = 1; reason = "ok";
out:
  if (out_reason) *out_reason = reason;
  if (id) free(id);
  nostr_event_free(ev); free(ebuf);
  return rc_store;
}
