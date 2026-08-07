#include "relay_ingress.h"

#include <string.h>

void relay_ingress_result_init(RelayIngressResult *result) {
  if (!result) return;
  memset(result, 0, sizeof(*result));
  result->decision = RELAY_INGRESS_REJECT_PARSE;
  result->validation_status = NOSTR_EVENT_VALIDATION_NULL;
  result->budget_status = VERIFICATION_BUDGET_ERROR;
  result->reason = "invalid: bad event";
}

RelayIngressDecision relay_ingress_validate_and_reserve(
    const RelayIngressConfig *config, RelayPolicy *policy,
    VerificationBudget *verification_budget,
    RateLimitBucket *connection_verification_bucket, const char *peer_ip,
    const char *event_json, size_t event_json_len, time_t now_wall,
    uint64_t now_mono_ms, RelayIngressResult *result) {
  if (!result) return RELAY_INGRESS_REJECT_PARSE;
  relay_ingress_result_init(result);

  if (!config || !policy || !verification_budget ||
      !connection_verification_bucket || !event_json) {
    return result->decision;
  }
  if (event_json_len == 0 || event_json_len > config->max_event_bytes) {
    result->decision = RELAY_INGRESS_REJECT_OVERSIZE;
    result->reason = "invalid: event too large";
    return result->decision;
  }

  if (verification_budget_is_negative(verification_budget, event_json,
                                      event_json_len, now_mono_ms)) {
    result->decision = RELAY_INGRESS_REJECT_CACHED_INVALID;
    result->reason = "invalid: cached validation failure";
    return result->decision;
  }

  result->event = nostr_event_new();
  if (!result->event) {
    result->reason = "error: out of memory";
    return result->decision;
  }

  result->validation_status =
      nostr_event_deserialize_signed(result->event, event_json, NULL);
  if (result->validation_status != NOSTR_EVENT_VALIDATION_OK) {
    result->decision = RELAY_INGRESS_REJECT_PARSE;
    result->reason = "invalid: malformed signed event";
    return result->decision;
  }

  if (relay_policy_created_at_out_of_range(
          policy, nostr_event_get_created_at(result->event), now_wall)) {
    result->decision = RELAY_INGRESS_REJECT_SKEW;
    result->reason = "invalid: created_at out of range";
    return result->decision;
  }

  uint32_t size_cost = (uint32_t)((event_json_len + 16383u) / 16384u);
  uint32_t weighted_cost = config->verification_cost > UINT32_MAX - size_cost
                               ? UINT32_MAX
                               : config->verification_cost + size_cost;
  result->budget_status = verification_budget_acquire(
      verification_budget, connection_verification_bucket, peer_ip, now_mono_ms,
      event_json_len, weighted_cost);
  if (result->budget_status != VERIFICATION_BUDGET_OK) {
    result->decision = RELAY_INGRESS_REJECT_VERIFY_BUDGET;
    result->reason = "rate-limited: verification budget";
    return result->decision;
  }

  result->validation_status =
      nostr_event_validate(result->event, result->canonical_id);
  verification_budget_release(verification_budget, event_json_len);
  if (result->validation_status != NOSTR_EVENT_VALIDATION_OK) {
    verification_budget_record_negative(verification_budget, event_json,
                                        event_json_len, now_mono_ms);
    result->decision = RELAY_INGRESS_REJECT_VALIDATION;
    result->reason = "invalid: id or signature validation failed";
    return result->decision;
  }

  if (!relay_policy_hex_to_id(result->canonical_id, result->binary_id)) {
    result->decision = RELAY_INGRESS_REJECT_VALIDATION;
    result->validation_status = NOSTR_EVENT_VALIDATION_BAD_ID;
    result->reason = "invalid: canonical id";
    return result->decision;
  }

  RelayReplayStatus replay =
      relay_policy_reserve(policy, result->binary_id, now_mono_ms);
  if (replay == RELAY_REPLAY_DUPLICATE) {
    result->decision = RELAY_INGRESS_DUPLICATE;
    result->reason = "duplicate";
    return result->decision;
  }
  if (replay == RELAY_REPLAY_IN_PROGRESS) {
    result->decision = RELAY_INGRESS_REJECT_REPLAY_ERROR;
    result->reason = "duplicate: storage in progress";
    return result->decision;
  }
  if (replay == RELAY_REPLAY_ERROR) {
    result->decision = RELAY_INGRESS_REJECT_REPLAY_ERROR;
    result->reason = "error: replay policy unavailable";
    return result->decision;
  }

  result->replay_reserved = replay == RELAY_REPLAY_RESERVED;
  result->reservation_now_ms = now_mono_ms;
  result->decision = RELAY_INGRESS_ACCEPT;
  result->reason = "";
  return result->decision;
}

int relay_ingress_finish(RelayPolicy *policy, RelayIngressResult *result,
                         int storage_succeeded) {
  if (!result || !result->replay_reserved) return 1;
  int ok = storage_succeeded
               ? relay_policy_commit(policy, result->binary_id,
                                     rate_limit_now_ms())
               : relay_policy_rollback(policy, result->binary_id);
  result->replay_reserved = 0;
  return ok;
}

void relay_ingress_result_clear(RelayIngressResult *result) {
  if (!result) return;
  if (result->event) nostr_event_free(result->event);
  result->event = NULL;
  result->replay_reserved = 0;
}

const char *relay_ingress_decision_string(RelayIngressDecision decision) {
  switch (decision) {
    case RELAY_INGRESS_ACCEPT: return "accept";
    case RELAY_INGRESS_DUPLICATE: return "duplicate";
    case RELAY_INGRESS_REJECT_OVERSIZE: return "oversize";
    case RELAY_INGRESS_REJECT_PARSE: return "parse";
    case RELAY_INGRESS_REJECT_SKEW: return "skew";
    case RELAY_INGRESS_REJECT_VERIFY_BUDGET: return "verification-budget";
    case RELAY_INGRESS_REJECT_CACHED_INVALID: return "cached-invalid";
    case RELAY_INGRESS_REJECT_VALIDATION: return "validation";
    case RELAY_INGRESS_REJECT_REPLAY_ERROR: return "replay-error";
    default: return "unknown";
  }
}
