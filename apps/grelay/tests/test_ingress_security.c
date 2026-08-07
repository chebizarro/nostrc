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

static char *make_event(int64_t created_at, const char *content) {
  const char *sk =
      "0202020202020202020202020202020202020202020202020202020202020202";
  NostrEvent *event = nostr_event_new();
  char *pubkey = nostr_key_get_public(sk);
  expect(event && pubkey, "event setup");
  nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_kind(event, 1);
  nostr_event_set_content(event, content);
  expect(nostr_event_sign(event, sk) == 0, "event signing");
  char *json = nostr_event_serialize_compact(event);
  free(pubkey);
  nostr_event_free(event);
  expect(json != NULL, "event serialization");
  return json;
}

int main(void) {
  const time_t now = (time_t)1900000000;
  const uint64_t mono = 5000;
  RelayPolicy *policy = relay_policy_create(128, 900, 600, 0);
  VerificationBudgetConfig budget_cfg = {
      .per_ip_rate = 100,
      .per_ip_burst = 100,
      .global_rate = 1000,
      .global_burst = 1000,
      .max_ip_entries = 16,
      .max_inflight_jobs = 2,
      .max_inflight_bytes = 512 * 1024,
      .negative_cache_entries = 16,
      .negative_cache_ttl_ms = 30000,
  };
  VerificationBudget *budget =
      verification_budget_create(&budget_cfg, mono);
  RateLimitBucket conn;
  rate_limit_bucket_init(&conn, 100, 100, mono);
  expect(policy && budget, "grelay security state");

  RelayIngressConfig ingress_cfg = {
      .max_event_bytes = 256 * 1024,
      .verification_cost = 5,
  };
  char *json = make_event(now, "grelay parity");
  RelayIngressResult result;
  expect(relay_ingress_validate_and_reserve(
             &ingress_cfg, policy, budget, &conn, "198.51.100.4", json,
             strlen(json), now, mono, &result) == RELAY_INGRESS_ACCEPT,
         "grelay accepts canonical signed event before storage");
  char canonical[65];
  memcpy(canonical, result.canonical_id, sizeof(canonical));
  expect(relay_ingress_finish(policy, &result, 1), "grelay store commit");
  relay_ingress_result_clear(&result);

  expect(relay_ingress_validate_and_reserve(
             &ingress_cfg, policy, budget, &conn, "198.51.100.4", json,
             strlen(json), now, mono + 1, &result) ==
             RELAY_INGRESS_DUPLICATE,
         "grelay replay rejected before storage and fanout");
  expect(strcmp(result.canonical_id, canonical) == 0,
         "duplicate acknowledgement uses canonical id");
  relay_ingress_result_clear(&result);

  char *bad_sig = make_event(now, "bad sig");
  char *sig = strstr(bad_sig, "\"sig\":\"");
  expect(sig != NULL, "find signature");
  sig += strlen("\"sig\":\"");
  sig[0] = sig[0] == '0' ? '1' : '0';
  expect(relay_ingress_validate_and_reserve(
             &ingress_cfg, policy, budget, &conn, "198.51.100.4", bad_sig,
             strlen(bad_sig), now, mono + 2, &result) ==
             RELAY_INGRESS_REJECT_VALIDATION,
         "grelay rejects invalid signature before storage and fanout");
  relay_ingress_result_clear(&result);

  char *future = make_event(now + 601, "future");
  expect(relay_ingress_validate_and_reserve(
             &ingress_cfg, policy, budget, &conn, "198.51.100.4", future,
             strlen(future), now, mono + 3, &result) ==
             RELAY_INGRESS_REJECT_SKEW,
         "grelay applies documented future skew");
  relay_ingress_result_clear(&result);

  RelayIngressConfig tiny = {.max_event_bytes = 16, .verification_cost = 5};
  expect(relay_ingress_validate_and_reserve(
             &tiny, policy, budget, &conn, "198.51.100.4", json,
             strlen(json), now, mono + 4, &result) ==
             RELAY_INGRESS_REJECT_OVERSIZE,
         "grelay enforces max event size before parsing");
  relay_ingress_result_clear(&result);

  free(future);
  free(bad_sig);
  free(json);
  verification_budget_destroy(budget);
  relay_policy_destroy(policy);
  puts("OK");
  return 0;
}
