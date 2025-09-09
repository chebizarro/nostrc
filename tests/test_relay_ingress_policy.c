#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "apps/relayd/include/protocol_nip01.h"
#include "nostr-event.h"

static void expect(int cond, const char *msg) {
  if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

/* Build a minimal signed event JSON with provided created_at and pubkey/signature omitted (we rely on check_signature to fail only if truly invalid). */
static char *build_unsigned_event_json(int64_t created_at) {
  /* Easiest: construct minimal object with fields we need. Signature check will fail; we'll skip it by pre-signing via libnostr.
   * To ensure signature passes, we construct via libnostr. */
  NostrEvent *ev = nostr_event_new();
  expect(ev != NULL, "alloc event");
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, "hello");
  /* Deterministic key: 0101.. */
  char sk_hex[65]; for (int i=0;i<64;i++) sk_hex[i] = (i%2==0)?'0':'1'; sk_hex[64]='\0';
  extern char *nostr_key_get_public(const char *sk);
  char *pk = nostr_key_get_public(sk_hex);
  expect(pk && strlen(pk)==64, "derive pk");
  nostr_event_set_pubkey(ev, pk);
  free(pk);
  int rc = nostr_event_sign(ev, sk_hex);
  expect(rc==0, "sign");
  char *json = nostr_event_serialize_compact(ev);
  expect(json != NULL, "serialize");
  nostr_event_free(ev);
  return json;
}

int main(void) {
  /* Enable policies */
  nostr_relay_set_replay_ttl(900);
  nostr_relay_set_skew(600, 86400);

  time_t now = time(NULL);

  /* 1) Accept normal event */
  char *j1 = build_unsigned_event_json((int64_t)now);
  const char *reason = NULL;
  int dec = relayd_nip01_ingress_decide_json(j1, now, &reason);
  expect(dec == 1, "store ok");
  free(j1);

  /* 2) Duplicate within TTL -> accept but don't store */
  char *j2 = build_unsigned_event_json((int64_t)now);
  dec = relayd_nip01_ingress_decide_json(j2, now, &reason);
  expect(dec == 0 && reason && strcmp(reason, "duplicate")==0, "duplicate ok not stored");
  free(j2);

  /* 3) Future skew reject */
  char *j3 = build_unsigned_event_json((int64_t)now + 3600);
  dec = relayd_nip01_ingress_decide_json(j3, now, &reason);
  expect(dec == -1 && reason && strstr(reason, "out of range")!=NULL, "future skew rejects");
  free(j3);

  /* 4) Past skew reject */
  char *j4 = build_unsigned_event_json((int64_t)now - (86400*2));
  dec = relayd_nip01_ingress_decide_json(j4, now, &reason);
  expect(dec == -1 && reason && strstr(reason, "out of range")!=NULL, "past skew rejects");
  free(j4);

  printf("OK\n");
  return 0;
}
