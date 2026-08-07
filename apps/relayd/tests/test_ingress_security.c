#include "nostr-event.h"
#include "nostr-keys.h"
#include "rate_limit.h"
#include "relay_ingress.h"
#include "relay_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static char *signed_event_json(int64_t created_at, const char *content) {
  const char *sk =
      "0101010101010101010101010101010101010101010101010101010101010101";
  NostrEvent *event = nostr_event_new();
  expect(event != NULL, "allocate event");
  char *pubkey = nostr_key_get_public(sk);
  expect(pubkey != NULL, "derive public key");
  nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_kind(event, 1);
  nostr_event_set_content(event, content);
  expect(nostr_event_sign(event, sk) == 0, "sign event");
  char *json = nostr_event_serialize_compact(event);
  free(pubkey);
  nostr_event_free(event);
  expect(json != NULL, "serialize event");
  return json;
}

static VerificationBudget *make_budget(uint64_t now_ms) {
  VerificationBudgetConfig cfg = {
      .per_ip_rate = 1000,
      .per_ip_burst = 1000,
      .global_rate = 10000,
      .global_burst = 10000,
      .max_ip_entries = 32,
      .max_inflight_jobs = 4,
      .max_inflight_bytes = 1024 * 1024,
      .negative_cache_entries = 32,
      .negative_cache_ttl_ms = 30000,
  };
  VerificationBudget *budget = verification_budget_create(&cfg, now_ms);
  expect(budget != NULL, "create verification budget");
  return budget;
}

static RelayIngressDecision decide(RelayPolicy *policy,
                                   VerificationBudget *budget,
                                   RateLimitBucket *conn_bucket,
                                   const char *json, size_t len,
                                   time_t now_wall, uint64_t now_ms,
                                   RelayIngressResult *result) {
  RelayIngressConfig cfg = {
      .max_event_bytes = 256 * 1024,
      .verification_cost = 5,
  };
  return relay_ingress_validate_and_reserve(
      &cfg, policy, budget, conn_bucket, "192.0.2.1", json, len, now_wall,
      now_ms, result);
}

int main(void) {
  const time_t now = (time_t)1900000000;
  const uint64_t mono = 1000000;
  RelayPolicy *policy = relay_policy_create(4096, 900, 600, 0);
  VerificationBudget *budget = make_budget(mono);
  RateLimitBucket conn;
  rate_limit_bucket_init(&conn, 1000, 1000, mono);
  expect(policy != NULL, "create replay policy");

  char *json = signed_event_json(now, "relay ingress");
  RelayIngressResult result;
  expect(decide(policy, budget, &conn, json, strlen(json), now, mono,
                &result) == RELAY_INGRESS_ACCEPT,
         "valid signed event accepted");
  expect(strlen(result.canonical_id) == 64, "canonical id returned");
  expect(relay_ingress_finish(policy, &result, 1), "commit reservation");
  relay_ingress_result_clear(&result);

  expect(decide(policy, budget, &conn, json, strlen(json), now, mono + 1,
                &result) == RELAY_INGRESS_DUPLICATE,
         "committed event detected as duplicate");
  relay_ingress_result_clear(&result);

  char *retry = signed_event_json(now, "storage retry");
  expect(decide(policy, budget, &conn, retry, strlen(retry), now, mono + 2,
                &result) == RELAY_INGRESS_ACCEPT,
         "new event reserved");
  expect(relay_ingress_finish(policy, &result, 0), "storage failure rollback");
  relay_ingress_result_clear(&result);
  expect(decide(policy, budget, &conn, retry, strlen(retry), now, mono + 3,
                &result) == RELAY_INGRESS_ACCEPT,
         "retry accepted after rollback");
  expect(relay_ingress_finish(policy, &result, 0), "retry rollback");
  relay_ingress_result_clear(&result);

  char *future = signed_event_json(now + 601, "future");
  expect(decide(policy, budget, &conn, future, strlen(future), now, mono + 4,
                &result) == RELAY_INGRESS_REJECT_SKEW,
         "future event beyond 600 seconds rejected");
  relay_ingress_result_clear(&result);

  char *gift_age = signed_event_json(now - 2 * 24 * 60 * 60, "historical");
  expect(decide(policy, budget, &conn, gift_age, strlen(gift_age), now,
                mono + 5, &result) == RELAY_INGRESS_ACCEPT,
         "past skew disabled for historical and gift-wrap compatibility");
  expect(relay_ingress_finish(policy, &result, 0), "historical rollback");
  relay_ingress_result_clear(&result);

  char *forged = strdup(retry);
  expect(forged != NULL, "copy event");
  char *id = strstr(forged, "\"id\":\"");
  expect(id != NULL, "find declared id");
  id += strlen("\"id\":\"");
  id[0] = id[0] == '0' ? '1' : '0';
  expect(decide(policy, budget, &conn, forged, strlen(forged), now, mono + 6,
                &result) == RELAY_INGRESS_REJECT_VALIDATION,
         "declared id mismatch rejected by canonical validator");
  relay_ingress_result_clear(&result);

  RelayIngressConfig tiny = {.max_event_bytes = 32, .verification_cost = 5};
  expect(relay_ingress_validate_and_reserve(
             &tiny, policy, budget, &conn, "192.0.2.1", json, strlen(json),
             now, mono + 7, &result) == RELAY_INGRESS_REJECT_OVERSIZE,
         "oversized event rejected before parsing");
  relay_ingress_result_clear(&result);

  RelayPolicy *large = relay_policy_create(4096, 900, 600, 0);
  expect(large != NULL, "large replay policy");
  unsigned char first[32] = {0};
  expect(relay_policy_reserve(large, first, mono) == RELAY_REPLAY_RESERVED,
         "reserve first binary id");
  expect(relay_policy_commit(large, first, mono), "commit first binary id");
  for (unsigned int i = 1; i <= 1500; ++i) {
    unsigned char current[32] = {0};
    current[0] = (unsigned char)(i >> 8);
    current[1] = (unsigned char)i;
    expect(relay_policy_reserve(large, current, mono + i) ==
               RELAY_REPLAY_RESERVED,
           "reserve intervening id");
    expect(relay_policy_commit(large, current, mono + i), "commit intervening id");
  }
  expect(relay_policy_reserve(large, first, mono + 2000) ==
             RELAY_REPLAY_DUPLICATE,
         "replay found after more than 1024 intervening events");

  relay_policy_destroy(large);

  RelayPolicy *expiring = relay_policy_create(4, 1, 600, 0);
  unsigned char expiring_id[32] = {42};
  expect(expiring != NULL, "expiring replay policy");
  expect(relay_policy_reserve(expiring, expiring_id, 1000) ==
             RELAY_REPLAY_RESERVED,
         "reserve expiring id");
  expect(relay_policy_commit(expiring, expiring_id, 1000), "commit expiring id");
  expect(relay_policy_reserve(expiring, expiring_id, 1999) ==
             RELAY_REPLAY_DUPLICATE,
         "duplicate before TTL expiry");
  expect(relay_policy_reserve(expiring, expiring_id, 2001) ==
             RELAY_REPLAY_RESERVED,
         "TTL expiry permits retry");
  relay_policy_destroy(expiring);
  free(forged);
  free(gift_age);
  free(future);
  free(retry);
  free(json);
  verification_budget_destroy(budget);
  relay_policy_destroy(policy);
  puts("OK");
  return 0;
}
