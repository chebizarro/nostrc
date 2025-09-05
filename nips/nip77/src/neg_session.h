#ifndef NOSTR_NIP77_NEG_SESSION_H
#define NOSTR_NIP77_NEG_SESSION_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../include/nostr/nip77/negentropy.h"
#include "neg_message.h"

struct NostrNegSession {
  NostrNegDataSource ds;
  NostrNegOptions opts;
  NostrNegStats stats;
  unsigned rounds;
  /* pending next message to send (binary buffer) */
  unsigned char *pending_msg;
  size_t pending_len;
  /* queued child ranges to send in subsequent rounds due to max_ranges cap */
  neg_bound_t pending_ranges[64];
  size_t pending_ranges_len;
};

#endif
