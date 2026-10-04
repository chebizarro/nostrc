#ifndef RELAYD_CTX_H
#define RELAYD_CTX_H

#include "nostr-storage.h"
#include "rate_limit.h"
#include "relay_policy.h"
#include "relayd_config.h"
#include "relayd_async_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ConnState;
typedef struct {
  NostrStorage *storage;
  const RelaydAsyncStorageOps *async_storage;
  unsigned int ack_timeout_ms;
  struct ConnState *clients;
  RelaydConfig cfg;
  RelayPolicy *policy;
  VerificationBudget *verification_budget;
} RelaydCtx;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CTX_H */
