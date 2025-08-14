#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned char pubkey[32];
  char *relay;    /* optional (NULL/empty ok) */
  char *petname;  /* optional (NULL/empty ok) */
} NostrFollowEntry;

typedef struct {
  NostrFollowEntry *entries;
  size_t count;
} NostrFollowList;

/* Build a kind:3 follow list event (replaces entire list). */
int nostr_nip02_build_follow_list(NostrEvent *ev,
                                  const unsigned char author_pk[32],
                                  const NostrFollowList *list,
                                  uint32_t created_at);

/* Parse kind:3 into a follow list (caller frees with free_follow_list). */
int  nostr_nip02_parse_follow_list(const NostrEvent *ev, NostrFollowList *out);
void nostr_nip02_free_follow_list(NostrFollowList *list);

/* Append new follows to the end (dedup by pubkey). */
int nostr_nip02_append(NostrEvent *ev, const NostrFollowEntry *add, size_t add_n);

#ifdef __cplusplus
}
#endif
