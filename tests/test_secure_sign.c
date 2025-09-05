#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/rand.h>
#include <nostr-event.h>
#include <secure_buf.h>

int main(void){
  // Build a minimal event
  NostrEvent *ev = nostr_event_new();
  if (!ev) { fprintf(stderr, "nostr_event_new failed\n"); return 1; }
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, "hello signed world");

  // Generate a dummy 32-byte secret
  unsigned char sk[32];
  if (RAND_bytes(sk, sizeof(sk)) != 1) { fprintf(stderr, "RAND_bytes failed\n"); nostr_event_free(ev); return 2; }
  nostr_secure_buf sb = secure_alloc(32);
  if (!sb.ptr) { fprintf(stderr, "secure_alloc failed\n"); nostr_event_free(ev); return 3; }
  memcpy(sb.ptr, sk, 32);
  volatile unsigned char *p = sk; for (size_t i=0;i<sizeof sk;i++) p[i]=0; // wipe stack copy

  // Sign using secure API
  if (nostr_event_sign_secure(ev, &sb) != 0) {
    fprintf(stderr, "nostr_event_sign_secure failed\n");
    secure_free(&sb); nostr_event_free(ev); return 4;
  }
  secure_free(&sb);

  // Basic assertions: id and sig should be present, and signature should verify
  if (!ev->id || !*ev->id || !ev->sig || !*ev->sig) {
    fprintf(stderr, "missing id/sig\n"); nostr_event_free(ev); return 5;
  }
  // We can't verify without a public key set here; treat presence as success for this smoke test
  // (Full verification path is covered elsewhere.)

  nostr_event_free(ev);
  return 0;
}
