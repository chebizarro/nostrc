#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "../include/nostr/nip77/negentropy.h"

static int begin(void *ctx){(void)ctx;return 0;}
static int next(void *ctx, NostrIndexItem *out){(void)ctx;(void)out;return 1;}
static void endi(void *ctx){(void)ctx;}

int main(void) {
  NostrNegDataSource ds = { .ctx=NULL, .begin_iter=begin, .next=next, .end_iter=endi };
  NostrNegSession *s = nostr_neg_session_new(&ds, NULL);
  assert(s!=NULL);
  char *hex = nostr_neg_build_initial_hex(s);
  assert(hex!=NULL);
  free(hex);
  NostrNegStats st; nostr_neg_get_stats(s, &st);
  nostr_neg_session_free(s);
  printf("ok session\n");
  return 0;
}
