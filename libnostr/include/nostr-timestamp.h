#ifndef __NOSTR_TIMESTAMP_H__
#define __NOSTR_TIMESTAMP_H__

/* Transitional header for GLib-friendly timestamp API. */

#include <time.h>
#include <stdint.h>
#include "timestamp.h" /* legacy Timestamp, Now, TimestampToTime */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef and function aliases */
typedef Timestamp NostrTimestamp;

#define nostr_timestamp_now         Now
#define nostr_timestamp_to_time     TimestampToTime

/* Potential GI notes:
 * nostr_timestamp_now: Returns: (type int64): Unix epoch seconds
 * nostr_timestamp_to_time: Returns: (type time_t)
 */

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TIMESTAMP_H__ */
