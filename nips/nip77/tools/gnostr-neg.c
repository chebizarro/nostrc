#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/nostr/nip77/negentropy.h"

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  NostrNegDataSource ds = {0};
  NostrNegOptions opts = { .max_ranges = 32, .max_idlist_items = 256, .max_round_trips = 8 };
  NostrNegSession *s = nostr_neg_session_new(&ds, &opts);
  if (!s) { fprintf(stderr, "failed to init session\n"); return 1; }
  char *hex = nostr_neg_build_initial_hex(s);
  printf("initial: %s\n", hex ? hex : "(null)");
  free(hex);
  nostr_neg_session_free(s);
  return 0;
}
