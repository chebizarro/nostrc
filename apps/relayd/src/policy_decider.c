#include "protocol_nip01.h"

#include "relay_ingress.h"

#include <string.h>
#include <time.h>

int relayd_nip01_ingress_decide_json(
    RelayPolicy *policy, VerificationBudget *verification_budget,
    RateLimitBucket *connection_verification_bucket, const char *peer_ip,
    const char *event_json, size_t event_json_len, size_t max_event_bytes,
    uint32_t verification_cost, int64_t now_wall, uint64_t now_mono_ms,
    char canonical_id_out[65], const char **out_reason) {
  RelayIngressConfig config = {
      .max_event_bytes = max_event_bytes,
      .verification_cost = verification_cost,
  };
  RelayIngressResult result;
  RelayIngressDecision decision = relay_ingress_validate_and_reserve(
      &config, policy, verification_budget,
      connection_verification_bucket, peer_ip, event_json, event_json_len,
      now_wall > 0 ? (time_t)now_wall : time(NULL), now_mono_ms, &result);

  if (canonical_id_out) {
    memcpy(canonical_id_out, result.canonical_id, sizeof(result.canonical_id));
  }
  if (out_reason) *out_reason = result.reason;

  int rc = -1;
  if (decision == RELAY_INGRESS_DUPLICATE) {
    rc = 0;
  } else if (decision == RELAY_INGRESS_ACCEPT) {
    /* The test-only decision helper never stores, so it must not poison replay. */
    (void)relay_ingress_finish(policy, &result, 0);
    rc = 1;
  }

  relay_ingress_result_clear(&result);
  return rc;
}
