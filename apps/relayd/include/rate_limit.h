#ifndef RELAYD_RATE_LIMIT_H
#define RELAYD_RATE_LIMIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RATE_LIMIT_MAX_RATE 100000000u
#define RATE_LIMIT_MAX_BURST 100000000u

typedef struct {
  uint64_t tokens_fp;
  uint64_t last_ms;
  uint32_t rate_per_sec;
  uint32_t burst;
} RateLimitBucket;

void rate_limit_bucket_init(RateLimitBucket *bucket, uint32_t rate_per_sec,
                            uint32_t burst, uint64_t now_ms);
int rate_limit_bucket_allow(RateLimitBucket *bucket, uint64_t now_ms,
                            uint32_t cost);
uint64_t rate_limit_now_ms(void);

typedef struct VerificationBudget VerificationBudget;

typedef struct {
  uint32_t per_ip_rate;
  uint32_t per_ip_burst;
  uint32_t global_rate;
  uint32_t global_burst;
  size_t max_ip_entries;
  size_t max_inflight_jobs;
  size_t max_inflight_bytes;
  size_t negative_cache_entries;
  uint64_t negative_cache_ttl_ms;
} VerificationBudgetConfig;

typedef enum {
  VERIFICATION_BUDGET_OK = 0,
  VERIFICATION_BUDGET_CONN_RATE,
  VERIFICATION_BUDGET_IP_RATE,
  VERIFICATION_BUDGET_GLOBAL_RATE,
  VERIFICATION_BUDGET_MAX_JOBS,
  VERIFICATION_BUDGET_MAX_BYTES,
  VERIFICATION_BUDGET_ERROR
} VerificationBudgetStatus;

VerificationBudget *verification_budget_create(
    const VerificationBudgetConfig *config, uint64_t now_ms);
void verification_budget_destroy(VerificationBudget *budget);

VerificationBudgetStatus verification_budget_acquire(
    VerificationBudget *budget, RateLimitBucket *connection_bucket,
    const char *peer_ip, uint64_t now_ms, size_t retained_bytes,
    uint32_t weighted_cost);
void verification_budget_release(VerificationBudget *budget,
                                 size_t retained_bytes);

int verification_budget_is_negative(VerificationBudget *budget,
                                    const void *data, size_t len,
                                    uint64_t now_ms);
void verification_budget_record_negative(VerificationBudget *budget,
                                         const void *data, size_t len,
                                         uint64_t now_ms);

size_t verification_budget_inflight_jobs(const VerificationBudget *budget);
size_t verification_budget_inflight_bytes(const VerificationBudget *budget);
const char *verification_budget_status_string(VerificationBudgetStatus status);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RATE_LIMIT_H */
