#ifndef RELAYD_CTX_H
#define RELAYD_CTX_H

#include "nostr-storage.h"
#include "rate_limit.h"
#include "relay_policy.h"
#include "relayd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  NostrStorage *storage;
  RelaydConfig cfg;
  RelayPolicy *policy;
  VerificationBudget *verification_budget;
} RelaydCtx;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CTX_H */
