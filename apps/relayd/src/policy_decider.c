#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "protocol_nip01.h"
#include "relay_policy.h"
#include "nostr-event.h"

/* Keep relay_policy.c in apps/relayd/src without touching build files outside bucket scope. */
#include "relay_policy.c"
#include "json.h"

/* Standalone, test-focused policy evaluator that does not depend on libwebsockets or NIP-42/50. */

/* Testable ingress decision (no sockets). Returns -1 reject, 0 accept-but-don't-store, 1 store. */
int relayd_nip01_ingress_decide_json(const char *event_json, int64_t now_override, const char **out_reason) {
  if (out_reason) *out_reason = NULL;
  if (!event_json) return -1;
  int rc_store = -1; const char *reason = NULL; char *id = NULL;
  char *ebuf = strdup(event_json);
  if (!ebuf) { if (out_reason) *out_reason = "oom"; return -1; }
  NostrEvent *ev = nostr_event_new(); int ok_parse = 0;
  if (ev) {
    if (nostr_event_deserialize_compact(ev, ebuf, NULL)) ok_parse = 1;
    else ok_parse = (nostr_event_deserialize(ev, ebuf) == 0);
  }
  if (!ok_parse || !ev) { free(ebuf); if (ev) nostr_event_free(ev); if (out_reason) *out_reason = "parse error"; return -1; }
  time_t now = now_override > 0 ? (time_t)now_override : time(NULL);
  int64_t created_at = nostr_event_get_created_at(ev);
  if (relay_policy_created_at_out_of_range(created_at, now)) {
    reason = "invalid: created_at out of range";
    rc_store = -1;
    goto out;
  }
  if (!nostr_event_check_signature(ev)) { reason = "invalid: bad signature"; rc_store = -1; goto out; }
  id = nostr_event_get_id(ev);
  if (id && strlen(id) == 64 && relay_policy_get_replay_ttl() > 0) {
    if (relay_policy_seen_id_check_and_add(id, now)) { rc_store = 0; reason = "duplicate"; goto out; }
  }
  rc_store = 1; reason = "ok";
out:
  if (out_reason) *out_reason = reason;
  if (id) free(id);
  nostr_event_free(ev); free(ebuf);
  return rc_store;
}
