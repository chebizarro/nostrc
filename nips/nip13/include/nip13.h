#ifndef NOSTR_NIP13_H
#define NOSTR_NIP13_H

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIP13_ERR_DIFFICULTY_TOO_LOW 1
#define NIP13_ERR_GENERATE_TIMEOUT   2

/**
 * nip13_difficulty:
 * @event_id: 64-character hex event ID
 *
 * Counts the number of leading zero bits in the event ID.
 *
 * Returns: number of leading zero bits, or -1 on invalid input
 */
int nip13_difficulty(const char *event_id);

/**
 * nip13_check:
 * @event_id: 64-character hex event ID
 * @min_difficulty: minimum required leading zero bits
 *
 * Checks whether the event ID meets the minimum proof-of-work difficulty.
 *
 * Returns: 0 if sufficient, NIP13_ERR_DIFFICULTY_TOO_LOW if not, -1 on error
 */
int nip13_check(const char *event_id, int min_difficulty);

/**
 * nip13_generate:
 * @event: event to mine (tags and created_at will be modified)
 * @target_difficulty: number of leading zero bits required
 * @timeout: maximum seconds to mine before giving up
 *
 * Performs proof of work by iterating nonce values until the event ID
 * has at least @target_difficulty leading zero bits, or @timeout expires.
 * Appends a ["nonce", "<value>", "<target>"] tag per NIP-13 spec.
 *
 * Returns: 0 on success, NIP13_ERR_GENERATE_TIMEOUT on timeout, -1 on error
 */
int nip13_generate(NostrEvent *event, int target_difficulty, time_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP13_H */
