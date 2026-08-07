#include "rate_limit.h"
#include "relayd_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static VerificationBudget *budget_with(size_t jobs, size_t bytes,
                                       uint32_t ip_burst,
                                       uint32_t global_burst) {
  VerificationBudgetConfig cfg = {
      .per_ip_rate = 1000,
      .per_ip_burst = ip_burst,
      .global_rate = 10000,
      .global_burst = global_burst,
      .max_ip_entries = 8,
      .max_inflight_jobs = jobs,
      .max_inflight_bytes = bytes,
      .negative_cache_entries = 8,
      .negative_cache_ttl_ms = 1000,
  };
  return verification_budget_create(&cfg, 1000);
}

int main(void) {
  RateLimitBucket fast;
  rate_limit_bucket_init(&fast, 5000, 5000, 1000);
  expect(rate_limit_bucket_allow(&fast, 1000, 5000),
         "rate above 1000/sec consumes without division by zero");
  expect(!rate_limit_bucket_allow(&fast, 1000, 1), "empty fast bucket");
  expect(rate_limit_bucket_allow(&fast, 1001, 5),
         "fixed-point millisecond refill at high rate");

  RateLimitBucket backward;
  rate_limit_bucket_init(&backward, 10, 1, 1000);
  expect(rate_limit_bucket_allow(&backward, 1000, 1), "consume initial token");
  expect(!rate_limit_bucket_allow(&backward, 500, 1),
         "backward clock does not refill");
  expect(!rate_limit_bucket_allow(&backward, 1099, 1),
         "partial token not rounded up");
  expect(rate_limit_bucket_allow(&backward, 1100, 1),
         "monotonic elapsed time refills");

  RateLimitBucket weighted;
  rate_limit_bucket_init(&weighted, 10, 10, 0);
  expect(rate_limit_bucket_allow(&weighted, 0, 7), "weighted cost allowed");
  expect(!rate_limit_bucket_allow(&weighted, 0, 4),
         "weighted cost exhausts bucket");

  VerificationBudget *ip_budget = budget_with(4, 4096, 5, 100);
  expect(ip_budget != NULL, "create per-IP budget");
  RateLimitBucket conn_a, conn_b, conn_c;
  rate_limit_bucket_init(&conn_a, 100, 100, 1000);
  rate_limit_bucket_init(&conn_b, 100, 100, 1000);
  rate_limit_bucket_init(&conn_c, 100, 100, 1000);
  expect(verification_budget_acquire(ip_budget, &conn_a, "192.0.2.9", 1000,
                                     100, 5) == VERIFICATION_BUDGET_OK,
         "first IP verification allowed");
  verification_budget_release(ip_budget, 100);
  expect(verification_budget_acquire(ip_budget, &conn_b, "192.0.2.9", 1000,
                                     100, 5) ==
             VERIFICATION_BUDGET_IP_RATE,
         "second connection cannot evade per-IP quota");
  expect(verification_budget_acquire(ip_budget, &conn_c, "198.51.100.7", 1000,
                                     100, 5) == VERIFICATION_BUDGET_OK,
         "different IP has independent quota");
  verification_budget_release(ip_budget, 100);
  verification_budget_destroy(ip_budget);

  VerificationBudgetConfig churn_cfg = {
      .per_ip_rate = 1,
      .per_ip_burst = 1,
      .global_rate = 10000,
      .global_burst = 10000,
      .max_ip_entries = 2,
      .max_inflight_jobs = 4,
      .max_inflight_bytes = 4096,
      .negative_cache_entries = 2,
      .negative_cache_ttl_ms = 1000,
  };
  VerificationBudget *churn = verification_budget_create(&churn_cfg, 1000);
  expect(churn != NULL, "create full-capacity churn budget");
  char churn_ip[32];
  uint64_t churn_now = 1000;
  for (unsigned int i = 0; i < 24; ++i) {
    snprintf(churn_ip, sizeof(churn_ip), "198.51.100.%u", i + 1);
    rate_limit_bucket_init(&conn_a, 10000, 10000, churn_now);
    expect(verification_budget_acquire(churn, &conn_a, churn_ip, churn_now,
                                       1, 1) == VERIFICATION_BUDGET_OK,
           "idle IP entry is reinserted on its valid probe chain");
    verification_budget_release(churn, 1);
    expect(verification_budget_acquire(churn, &conn_a, churn_ip, churn_now,
                                       1, 1) == VERIFICATION_BUDGET_IP_RATE,
           "reinserted IP entry remains discoverable");
    churn_now += 10ull * 60ull * 1000ull + 1ull;
  }
  verification_budget_destroy(churn);

  churn_cfg.max_ip_entries = 1;
  VerificationBudget *single = verification_budget_create(&churn_cfg, 1000);
  expect(single != NULL, "create single-entry churn budget");
  churn_now = 1000;
  for (unsigned int i = 0; i < 24; ++i) {
    snprintf(churn_ip, sizeof(churn_ip), "203.0.113.%u", i + 1);
    rate_limit_bucket_init(&conn_a, 10000, 10000, churn_now);
    expect(verification_budget_acquire(single, &conn_a, churn_ip, churn_now,
                                       1, 1) == VERIFICATION_BUDGET_OK,
           "single idle IP entry can be replaced");
    verification_budget_release(single, 1);
    expect(verification_budget_acquire(single, &conn_a, "192.0.2.250",
                                       churn_now, 1, 1) ==
               VERIFICATION_BUDGET_ERROR,
           "active IP buckets never exceed max_ip_entries");
    churn_now += 10ull * 60ull * 1000ull + 1ull;
  }
  verification_budget_destroy(single);

  VerificationBudget *global_budget = budget_with(4, 4096, 100, 5);
  expect(global_budget != NULL, "create global budget");
  rate_limit_bucket_init(&conn_a, 100, 100, 1000);
  rate_limit_bucket_init(&conn_b, 100, 100, 1000);
  expect(verification_budget_acquire(global_budget, &conn_a, "192.0.2.10",
                                     1000, 100, 5) == VERIFICATION_BUDGET_OK,
         "first global verification allowed");
  verification_budget_release(global_budget, 100);
  expect(verification_budget_acquire(global_budget, &conn_b,
                                     "198.51.100.10", 1000, 100, 5) ==
             VERIFICATION_BUDGET_GLOBAL_RATE,
         "rotating IP cannot evade global quota");
  verification_budget_destroy(global_budget);

  VerificationBudget *bounded = budget_with(1, 128, 100, 100);
  expect(bounded != NULL, "create bounded budget");
  RateLimitBucket conn_d, conn_e;
  rate_limit_bucket_init(&conn_d, 100, 100, 1000);
  rate_limit_bucket_init(&conn_e, 100, 100, 1000);
  expect(verification_budget_acquire(bounded, &conn_d, "203.0.113.1", 1000,
                                     128, 1) == VERIFICATION_BUDGET_OK,
         "first in-flight job allowed");
  expect(verification_budget_inflight_jobs(bounded) == 1 &&
             verification_budget_inflight_bytes(bounded) == 128,
         "in-flight resources accounted");
  expect(verification_budget_acquire(bounded, &conn_e, "203.0.113.2", 1000,
                                     1, 1) == VERIFICATION_BUDGET_MAX_JOBS,
         "job bound rejects excess work");
  verification_budget_release(bounded, 128);
  expect(verification_budget_acquire(bounded, &conn_e, "203.0.113.2", 1000,
                                     129, 1) == VERIFICATION_BUDGET_MAX_BYTES,
         "byte bound rejects oversized retained work");

  const char invalid[] = "{invalid}";
  expect(!verification_budget_is_negative(bounded, invalid, strlen(invalid),
                                          1000),
         "negative cache initially empty");
  verification_budget_record_negative(bounded, invalid, strlen(invalid), 1000);
  expect(verification_budget_is_negative(bounded, invalid, strlen(invalid),
                                         1001),
         "negative cache suppresses repeated invalid input");
  expect(!verification_budget_is_negative(bounded, invalid, strlen(invalid),
                                          2001),
         "negative cache expires");
  verification_budget_destroy(bounded);

  RelaydConfig cfg;
  expect(relayd_config_load(NULL, &cfg) == 0, "default config valid");
  cfg.rate_ops_per_sec = 5000;
  cfg.rate_burst = 5000;
  expect(relayd_config_validate(&cfg, NULL, 0) == 0,
         "configuration above 1000 ops/sec is safe");
  cfg.rate_ops_per_sec = 0;
  expect(relayd_config_validate(&cfg, NULL, 0) != 0,
         "zero rate rejected");
  cfg.rate_ops_per_sec = 20;
  cfg.verification_max_bytes = cfg.max_event_bytes - 1;
  expect(relayd_config_validate(&cfg, NULL, 0) != 0,
         "verification byte bound cannot be below max event size");
  expect(relayd_config_load(NULL, &cfg) == 0, "reload default config");
  cfg.verification_cost = INT_MAX;
  expect(relayd_config_validate(&cfg, NULL, 0) != 0,
         "verification cost overflow is rejected safely");

  puts("OK");
  return 0;
}
