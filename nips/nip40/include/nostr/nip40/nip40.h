#ifndef NIPS_NIP40_NOSTR_NIP40_NIP40_H
#define NIPS_NIP40_NOSTR_NIP40_NIP40_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * NIP-40: Expiration Timestamp
 *
 * Events can include an "expiration" tag with a UNIX timestamp indicating
 * when the event should be considered expired. Relays MAY discard expired
 * events, and clients SHOULD hide them.
 *
 * Tag format: ["expiration", "<unix-timestamp>"]
 */

/**
 * nostr_nip40_get_expiration:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @out_timestamp: (out): UNIX timestamp of expiration if found
 *
 * Extracts the expiration timestamp from the event's "expiration" tag.
 *
 * Returns: 0 on success, -ENOENT if no expiration tag, -EINVAL on error
 */
int nostr_nip40_get_expiration(const NostrEvent *ev, int64_t *out_timestamp);

/**
 * nostr_nip40_is_expired:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Checks whether an event's expiration timestamp has passed.
 * Events without an expiration tag are never considered expired.
 *
 * Returns: true if the event has expired, false otherwise
 */
bool nostr_nip40_is_expired(const NostrEvent *ev);

/**
 * nostr_nip40_is_expired_at:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @now: UNIX timestamp to check against (useful for testing)
 *
 * Checks whether an event's expiration timestamp has passed relative to
 * the given time. Events without an expiration tag are never expired.
 *
 * Returns: true if the event has expired at time @now, false otherwise
 */
bool nostr_nip40_is_expired_at(const NostrEvent *ev, int64_t now);

/**
 * nostr_nip40_set_expiration:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @timestamp: UNIX timestamp when the event should expire
 *
 * Sets or replaces the "expiration" tag on the event.
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip40_set_expiration(NostrEvent *ev, int64_t timestamp);

/**
 * nostr_nip40_set_expiration_in:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @seconds_from_now: number of seconds from now until expiration
 *
 * Convenience function: sets the expiration tag to (now + seconds_from_now).
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip40_set_expiration_in(NostrEvent *ev, int64_t seconds_from_now);

/**
 * nostr_nip40_should_relay_accept:
 * @ev: (in) (transfer none) (not nullable): incoming event
 *
 * Relay-side check: returns false if the event has an expiration timestamp
 * that has already passed (relay should reject with "event expired").
 * Events without an expiration tag are always accepted.
 *
 * Returns: true if the relay should accept the event, false if expired
 */
bool nostr_nip40_should_relay_accept(const NostrEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP40_NOSTR_NIP40_NIP40_H */
