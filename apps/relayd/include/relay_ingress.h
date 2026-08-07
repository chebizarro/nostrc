#ifndef RELAYD_RELAY_INGRESS_H
#define RELAYD_RELAY_INGRESS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nostr-event.h"
#include "rate_limit.h"
#include "relay_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  RELAY_INGRESS_ACCEPT = 0,
  RELAY_INGRESS_DUPLICATE,
  RELAY_INGRESS_REJECT_OVERSIZE,
  RELAY_INGRESS_REJECT_PARSE,
  RELAY_INGRESS_REJECT_SKEW,
  RELAY_INGRESS_REJECT_VERIFY_BUDGET,
  RELAY_INGRESS_REJECT_CACHED_INVALID,
  RELAY_INGRESS_REJECT_VALIDATION,
  RELAY_INGRESS_REJECT_REPLAY_ERROR
} RelayIngressDecision;

typedef struct {
  size_t max_event_bytes;
  uint32_t verification_cost;
} RelayIngressConfig;

typedef struct {
  RelayIngressDecision decision;
  NostrEventValidationStatus validation_status;
  VerificationBudgetStatus budget_status;
  NostrEvent *event;
  char canonical_id[65];
  unsigned char binary_id[32];
  int replay_reserved;
  uint64_t reservation_now_ms;
  const char *reason;
} RelayIngressResult;

void relay_ingress_result_init(RelayIngressResult *result);
RelayIngressDecision relay_ingress_validate_and_reserve(
    const RelayIngressConfig *config, RelayPolicy *policy,
    VerificationBudget *verification_budget,
    RateLimitBucket *connection_verification_bucket, const char *peer_ip,
    const char *event_json, size_t event_json_len, time_t now_wall,
    uint64_t now_mono_ms, RelayIngressResult *result);

/* Commit the replay reservation only after durable storage succeeds. */
int relay_ingress_finish(RelayPolicy *policy, RelayIngressResult *result,
                         int storage_succeeded);
void relay_ingress_result_clear(RelayIngressResult *result);
const char *relay_ingress_decision_string(RelayIngressDecision decision);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RELAY_INGRESS_H */
