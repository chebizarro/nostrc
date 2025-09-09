#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "nostr-event.h"
#include "nostr-utils.h"
#include "keys.h"

static void expect(int cond, const char *msg) {
  if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

int main(void) {
  // Construct event
  NostrEvent *ev = nostr_event_new();
  expect(ev != NULL, "alloc event");
  // Prepare deterministic private key and derive matching pubkey (x-only)
  char sk_hex[65];
  for (int i = 0; i < 64; i++) sk_hex[i] = (i%2==0)?'0':'1';
  sk_hex[64] = '\0';
  char *pk_hex = nostr_key_get_public(sk_hex);
  expect(pk_hex && strlen(pk_hex)==64, "derive pubkey");
  nostr_event_set_pubkey(ev, pk_hex);
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, (int64_t)1700000000);
  nostr_event_set_content(ev, "hello world");

  // Sign with deterministic privkey (32 bytes of 0x01)

  int rc = nostr_event_sign(ev, sk_hex);
  expect(rc == 0, "sign ok");
  expect(nostr_event_check_signature(ev), "verify ok");

  // Compute canonical id
  char *id = nostr_event_get_id(ev);
  expect(id && strlen(id) == 64, "id hex len");

  // Tamper with event->id and ensure verification still succeeds (since we recompute)
  if (ev->id) { free(ev->id); ev->id = NULL; }
  ev->id = strdup("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  expect(nostr_event_check_signature(ev), "verify ignores provided id and recomputes");

  // Tamper with content changes and ensure verification fails
  nostr_event_set_content(ev, "tampered");
  expect(!nostr_event_check_signature(ev), "verify fails on tamper");

  free(id);
  nostr_event_free(ev);
  fprintf(stdout, "OK\n");
  return 0;
}
