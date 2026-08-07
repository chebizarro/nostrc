#ifndef RELAYD_CONN_H
#define RELAYD_CONN_H

#include "rate_limit.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *it;
  char subid[128];
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
