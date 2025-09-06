#ifndef NOSTR_RELAY_LIMITS_H
#define NOSTR_RELAY_LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct {
  /* Rates */
  uint32_t msgs_per_10s;
  uint32_t burst;
  /* Filters */
  uint32_t max_filters;
  uint32_t max_limit;
  uint32_t max_tag_values;
  uint32_t max_window_days;
} NostrRelayLimits;

/* Reason strings for CLOSED/OK */
const char *nostr_limits_reason_rl_exceeded(void);
const char *nostr_limits_reason_invalid_filter(void);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_RELAY_LIMITS_H */
