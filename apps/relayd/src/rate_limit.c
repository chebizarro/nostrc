#include "rate_limit.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>

#define RATE_FP_SCALE 1000000ull
#define EMPTY_HASH 0ull
#define TOMBSTONE_HASH UINT64_MAX
#define IP_IDLE_TTL_MS (10ull * 60ull * 1000ull)

typedef struct {
  uint64_t hash;
  char ip[64];
  RateLimitBucket bucket;
  uint64_t last_seen_ms;
} IpBudgetEntry;

typedef struct {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  uint64_t expires_ms;
  uint64_t last_seen_ms;
  int state; /* 0 empty, 1 occupied, 2 tombstone */
} NegativeEntry;

struct VerificationBudget {
  VerificationBudgetConfig cfg;
  RateLimitBucket global_bucket;
  IpBudgetEntry *ip_entries;
  size_t ip_table_size;
  size_t ip_count;
  NegativeEntry *negative_entries;
  size_t inflight_jobs;
  size_t inflight_bytes;
};

static uint64_t clamp_mul(uint64_t a, uint64_t b) {
  if (a != 0 && b > UINT64_MAX / a) return UINT64_MAX;
  return a * b;
}

static uint64_t bucket_capacity_fp(const RateLimitBucket *bucket) {
  return clamp_mul(bucket ? bucket->burst : 0, RATE_FP_SCALE);
}

static void bucket_refill(RateLimitBucket *bucket, uint64_t now_ms) {
  if (!bucket || bucket->rate_per_sec == 0 || now_ms <= bucket->last_ms) return;

  uint64_t capacity = bucket_capacity_fp(bucket);
  if (bucket->tokens_fp >= capacity) {
    bucket->tokens_fp = capacity;
    bucket->last_ms = now_ms;
    return;
  }

  uint64_t elapsed = now_ms - bucket->last_ms;
  uint64_t per_second_fp = clamp_mul(bucket->rate_per_sec, RATE_FP_SCALE);
  uint64_t missing = capacity - bucket->tokens_fp;
  uint64_t full_ms = (clamp_mul(missing, 1000ull) + per_second_fp - 1) /
                     per_second_fp;
  if (elapsed >= full_ms) {
    bucket->tokens_fp = capacity;
  } else {
    uint64_t added = clamp_mul(elapsed, per_second_fp) / 1000ull;
    bucket->tokens_fp = added >= missing ? capacity : bucket->tokens_fp + added;
  }
  bucket->last_ms = now_ms;
}

void rate_limit_bucket_init(RateLimitBucket *bucket, uint32_t rate_per_sec,
                            uint32_t burst, uint64_t now_ms) {
  if (!bucket) return;
  if (rate_per_sec > RATE_LIMIT_MAX_RATE) rate_per_sec = RATE_LIMIT_MAX_RATE;
  if (burst > RATE_LIMIT_MAX_BURST) burst = RATE_LIMIT_MAX_BURST;
  bucket->rate_per_sec = rate_per_sec;
  bucket->burst = burst;
  bucket->tokens_fp = clamp_mul(burst, RATE_FP_SCALE);
  bucket->last_ms = now_ms;
}

int rate_limit_bucket_allow(RateLimitBucket *bucket, uint64_t now_ms,
                            uint32_t cost) {
  if (!bucket) return 0;
  if (cost == 0) return 1;
  if (bucket->rate_per_sec == 0 || bucket->burst == 0) return 0;

  bucket_refill(bucket, now_ms);
  uint64_t needed = clamp_mul(cost, RATE_FP_SCALE);
  if (needed > bucket->tokens_fp) return 0;
  bucket->tokens_fp -= needed;
  return 1;
}

uint64_t rate_limit_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ull +
         (uint64_t)(ts.tv_nsec / 1000000ull);
}

static uint64_t hash_bytes(const unsigned char *data, size_t len) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return (hash == EMPTY_HASH || hash == TOMBSTONE_HASH) ? 1ull : hash;
}

static size_t next_pow2(size_t value) {
  size_t out = 1;
  while (out < value && out <= SIZE_MAX / 2) out <<= 1;
  return out < value ? 0 : out;
}

static IpBudgetEntry *ip_entry_get(VerificationBudget *budget,
                                   const char *peer_ip, uint64_t now_ms) {
  if (!budget || !budget->ip_entries || budget->ip_table_size == 0) return NULL;
  const char *ip = (peer_ip && *peer_ip) ? peer_ip : "unknown";
  size_t ip_len = strnlen(ip, sizeof(((IpBudgetEntry *)0)->ip) - 1);
  uint64_t hash = hash_bytes((const unsigned char *)ip, ip_len);
  size_t mask = budget->ip_table_size - 1;
  size_t free_pos = SIZE_MAX;
  size_t tombstone_pos = SIZE_MAX;

  for (size_t probe = 0; probe < budget->ip_table_size; ++probe) {
    size_t pos = ((size_t)hash + probe) & mask;
    IpBudgetEntry *entry = &budget->ip_entries[pos];
    if (entry->hash == EMPTY_HASH) {
      free_pos = tombstone_pos != SIZE_MAX ? tombstone_pos : pos;
      break;
    }
    if (entry->hash == TOMBSTONE_HASH) {
      if (tombstone_pos == SIZE_MAX) tombstone_pos = pos;
      continue;
    }
    if (entry->hash == hash && strcmp(entry->ip, ip) == 0) {
      entry->last_seen_ms = now_ms;
      return entry;
    }
  }

  if (free_pos == SIZE_MAX || budget->ip_count >= budget->cfg.max_ip_entries) {
    uint64_t oldest = UINT64_MAX;
    size_t oldest_pos = SIZE_MAX;
    for (size_t i = 0; i < budget->ip_table_size; ++i) {
      IpBudgetEntry *candidate = &budget->ip_entries[i];
      if (candidate->hash != EMPTY_HASH &&
          candidate->hash != TOMBSTONE_HASH &&
          now_ms >= candidate->last_seen_ms &&
          now_ms - candidate->last_seen_ms >= IP_IDLE_TTL_MS &&
          candidate->last_seen_ms < oldest) {
        oldest = candidate->last_seen_ms;
        oldest_pos = i;
      }
    }
    if (oldest_pos == SIZE_MAX) return NULL; /* fail closed under active churn */
    memset(&budget->ip_entries[oldest_pos], 0,
           sizeof(budget->ip_entries[oldest_pos]));
    budget->ip_entries[oldest_pos].hash = TOMBSTONE_HASH;

    /* Reinsert on the new key's probe chain; never write at the evicted slot
     * unless that slot is a valid insertion point for this key. */
    free_pos = SIZE_MAX;
    tombstone_pos = SIZE_MAX;
    for (size_t probe = 0; probe < budget->ip_table_size; ++probe) {
      size_t pos = ((size_t)hash + probe) & mask;
      IpBudgetEntry *candidate = &budget->ip_entries[pos];
      if (candidate->hash == TOMBSTONE_HASH && tombstone_pos == SIZE_MAX) {
        tombstone_pos = pos;
        continue;
      }
      if (candidate->hash == EMPTY_HASH) {
        free_pos = tombstone_pos != SIZE_MAX ? tombstone_pos : pos;
        break;
      }
    }
    if (free_pos == SIZE_MAX) free_pos = tombstone_pos;
    if (free_pos == SIZE_MAX) return NULL;
  } else {
    budget->ip_count++;
  }

  IpBudgetEntry *entry = &budget->ip_entries[free_pos];
  memset(entry, 0, sizeof(*entry));
  entry->hash = hash;
  memcpy(entry->ip, ip, ip_len);
  entry->ip[ip_len] = '\0';
  entry->last_seen_ms = now_ms;
  rate_limit_bucket_init(&entry->bucket, budget->cfg.per_ip_rate,
                         budget->cfg.per_ip_burst, now_ms);
  return entry;
}

VerificationBudget *verification_budget_create(
    const VerificationBudgetConfig *config, uint64_t now_ms) {
  if (!config || config->per_ip_rate == 0 || config->per_ip_burst == 0 ||
      config->global_rate == 0 || config->global_burst == 0 ||
      config->max_ip_entries == 0 || config->max_inflight_jobs == 0 ||
      config->max_inflight_bytes == 0) {
    return NULL;
  }

  VerificationBudget *budget = calloc(1, sizeof(*budget));
  if (!budget) return NULL;
  budget->cfg = *config;
  size_t wanted = config->max_ip_entries > SIZE_MAX / 2
                      ? 0
                      : config->max_ip_entries * 2;
  budget->ip_table_size = next_pow2(wanted);
  if (budget->ip_table_size == 0) {
    free(budget);
    return NULL;
  }
  budget->ip_entries = calloc(budget->ip_table_size, sizeof(*budget->ip_entries));
  if (!budget->ip_entries) {
    free(budget);
    return NULL;
  }
  if (config->negative_cache_entries > 0) {
    budget->negative_entries =
        calloc(config->negative_cache_entries, sizeof(*budget->negative_entries));
    if (!budget->negative_entries) {
      free(budget->ip_entries);
      free(budget);
      return NULL;
    }
  }
  rate_limit_bucket_init(&budget->global_bucket, config->global_rate,
                         config->global_burst, now_ms);
  return budget;
}

void verification_budget_destroy(VerificationBudget *budget) {
  if (!budget) return;
  free(budget->negative_entries);
  free(budget->ip_entries);
  free(budget);
}

VerificationBudgetStatus verification_budget_acquire(
    VerificationBudget *budget, RateLimitBucket *connection_bucket,
    const char *peer_ip, uint64_t now_ms, size_t retained_bytes,
    uint32_t weighted_cost) {
  if (!budget || !connection_bucket || weighted_cost == 0)
    return VERIFICATION_BUDGET_ERROR;
  if (budget->inflight_jobs >= budget->cfg.max_inflight_jobs)
    return VERIFICATION_BUDGET_MAX_JOBS;
  if (retained_bytes > budget->cfg.max_inflight_bytes ||
      budget->inflight_bytes >
          budget->cfg.max_inflight_bytes - retained_bytes)
    return VERIFICATION_BUDGET_MAX_BYTES;

  IpBudgetEntry *ip_entry = ip_entry_get(budget, peer_ip, now_ms);
  if (!ip_entry) return VERIFICATION_BUDGET_ERROR;

  RateLimitBucket connection_trial = *connection_bucket;
  RateLimitBucket ip_trial = ip_entry->bucket;
  RateLimitBucket global_trial = budget->global_bucket;

  if (!rate_limit_bucket_allow(&connection_trial, now_ms, weighted_cost))
    return VERIFICATION_BUDGET_CONN_RATE;
  if (!rate_limit_bucket_allow(&ip_trial, now_ms, weighted_cost))
    return VERIFICATION_BUDGET_IP_RATE;
  if (!rate_limit_bucket_allow(&global_trial, now_ms, weighted_cost))
    return VERIFICATION_BUDGET_GLOBAL_RATE;

  *connection_bucket = connection_trial;
  ip_entry->bucket = ip_trial;
  budget->global_bucket = global_trial;
  budget->inflight_jobs++;
  budget->inflight_bytes += retained_bytes;
  return VERIFICATION_BUDGET_OK;
}

void verification_budget_release(VerificationBudget *budget,
                                 size_t retained_bytes) {
  if (!budget) return;
  if (budget->inflight_jobs > 0) budget->inflight_jobs--;
  if (retained_bytes >= budget->inflight_bytes)
    budget->inflight_bytes = 0;
  else
    budget->inflight_bytes -= retained_bytes;
}

static void digest_data(const void *data, size_t len,
                        unsigned char digest[SHA256_DIGEST_LENGTH]) {
  SHA256((const unsigned char *)data, len, digest);
}

int verification_budget_is_negative(VerificationBudget *budget,
                                    const void *data, size_t len,
                                    uint64_t now_ms) {
  if (!budget || !data || !budget->negative_entries ||
      budget->cfg.negative_cache_entries == 0)
    return 0;

  unsigned char digest[SHA256_DIGEST_LENGTH];
  digest_data(data, len, digest);
  size_t start = (size_t)hash_bytes(digest, sizeof(digest)) %
                 budget->cfg.negative_cache_entries;
  for (size_t probe = 0; probe < budget->cfg.negative_cache_entries; ++probe) {
    size_t pos = (start + probe) % budget->cfg.negative_cache_entries;
    NegativeEntry *entry = &budget->negative_entries[pos];
    if (entry->state == 0) return 0;
    if (entry->state == 2) continue;
    if (entry->expires_ms <= now_ms) {
      entry->state = 2;
      continue;
    }
    if (memcmp(entry->digest, digest, sizeof(digest)) == 0) {
      entry->last_seen_ms = now_ms;
      return 1;
    }
  }
  return 0;
}

void verification_budget_record_negative(VerificationBudget *budget,
                                         const void *data, size_t len,
                                         uint64_t now_ms) {
  if (!budget || !data || !budget->negative_entries ||
      budget->cfg.negative_cache_entries == 0 ||
      budget->cfg.negative_cache_ttl_ms == 0)
    return;

  unsigned char digest[SHA256_DIGEST_LENGTH];
  digest_data(data, len, digest);
  size_t start = (size_t)hash_bytes(digest, sizeof(digest)) %
                 budget->cfg.negative_cache_entries;
  size_t chosen = SIZE_MAX;
  size_t oldest_pos = SIZE_MAX;
  uint64_t oldest = UINT64_MAX;

  for (size_t probe = 0; probe < budget->cfg.negative_cache_entries; ++probe) {
    size_t pos = (start + probe) % budget->cfg.negative_cache_entries;
    NegativeEntry *entry = &budget->negative_entries[pos];
    if (entry->state != 1 || entry->expires_ms <= now_ms) {
      chosen = pos;
      break;
    }
    if (memcmp(entry->digest, digest, sizeof(digest)) == 0) {
      chosen = pos;
      break;
    }
    if (entry->last_seen_ms < oldest) {
      oldest = entry->last_seen_ms;
      oldest_pos = pos;
    }
  }
  if (chosen == SIZE_MAX) chosen = oldest_pos;
  if (chosen == SIZE_MAX) return;

  NegativeEntry *entry = &budget->negative_entries[chosen];
  memcpy(entry->digest, digest, sizeof(digest));
  entry->expires_ms =
      now_ms > UINT64_MAX - budget->cfg.negative_cache_ttl_ms
          ? UINT64_MAX
          : now_ms + budget->cfg.negative_cache_ttl_ms;
  entry->last_seen_ms = now_ms;
  entry->state = 1;
}

size_t verification_budget_inflight_jobs(const VerificationBudget *budget) {
  return budget ? budget->inflight_jobs : 0;
}

size_t verification_budget_inflight_bytes(const VerificationBudget *budget) {
  return budget ? budget->inflight_bytes : 0;
}

const char *verification_budget_status_string(VerificationBudgetStatus status) {
  switch (status) {
    case VERIFICATION_BUDGET_OK: return "ok";
    case VERIFICATION_BUDGET_CONN_RATE: return "connection-rate";
    case VERIFICATION_BUDGET_IP_RATE: return "ip-rate";
    case VERIFICATION_BUDGET_GLOBAL_RATE: return "global-rate";
    case VERIFICATION_BUDGET_MAX_JOBS: return "max-jobs";
    case VERIFICATION_BUDGET_MAX_BYTES: return "max-bytes";
    default: return "error";
  }
}
