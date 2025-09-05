#ifndef NOSTR_RATE_LIMITER_H
#define NOSTR_RATE_LIMITER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Simple token-bucket rate limiter for per-connection/IP controls.
 * rate: tokens per second added
 * burst: maximum bucket size (max tokens)
 * cost: tokens consumed per event (e.g., bytes or frames)
 */

typedef struct nostr_token_bucket {
  double tokens;
  double rate;
  double burst;
  double last_ts; /* seconds since some monotonic epoch */
} nostr_token_bucket;

/* Initialize the token bucket with rate tokens/sec and burst capacity. */
void tb_init(nostr_token_bucket *tb, double rate, double burst);

/* Update the bucket based on current time. If cost <= available tokens, consume and return true.
 * Otherwise return false (caller should reject or defer).
 */
bool tb_allow(nostr_token_bucket *tb, double cost);

/* Helper to set the monotonic timestamp (seconds). If not set, tb_allow uses clock_gettime. */
void tb_set_now(nostr_token_bucket *tb, double now);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_RATE_LIMITER_H */
