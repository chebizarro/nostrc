#ifndef __NOSTR_TIMESTAMP_H__
#define __NOSTR_TIMESTAMP_H__

/* Public header for GI-friendly timestamp API. */

#include <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef and API */
typedef int64_t NostrTimestamp;

int64_t nostr_timestamp_now(void);
time_t  nostr_timestamp_to_time(NostrTimestamp t);

/* Potential GI notes:
 * nostr_timestamp_now: Returns: (type int64): Unix epoch seconds
 * nostr_timestamp_to_time: Returns: (type time_t)
 */

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TIMESTAMP_H__ */
