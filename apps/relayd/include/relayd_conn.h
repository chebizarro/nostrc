#ifndef RELAYD_CONN_H
#define RELAYD_CONN_H

#include "rate_limit.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NIP-01: a subscription id is a non-empty string of at most 64 chars. */
#define RELAYD_SUBID_MAX 64
/* Hard cap on REQs one connection may have streaming at once; the
 * effective limit is min(cfg.max_subs, this). */
#define RELAYD_MAX_PENDING_SUBS 16

/* A REQ whose stored results are still being sent. The relay has no live
 * fan-out, so a subscription ends at its EOSE. */
typedef struct {
  void *it; /* storage iterator (from query or search); released by query_free */
  char subid[RELAYD_SUBID_MAX + 1];
} RelaydPendingSub;

typedef struct {
  /* FIFO: subs[0] is streamed by on_writable until its EOSE, then the rest
   * follow in arrival order. */
  RelaydPendingSub subs[RELAYD_MAX_PENDING_SUBS];
  size_t nsubs;
  int authed;
  int need_auth_chal;
  char auth_chal[64];
  char authed_pubkey[128];
  char peer_ip[64];
  char *rx_buffer;
  size_t rx_length;
  size_t rx_capacity;

  RateLimitBucket frame_rate;
  RateLimitBucket byte_rate;
  RateLimitBucket verification_rate;

  void *neg_state;
  char neg_subid[128];
} ConnState;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CONN_H */
