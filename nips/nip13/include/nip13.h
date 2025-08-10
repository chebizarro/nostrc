#ifndef NOSTR_NIP13_H
#define NOSTR_NIP13_H

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NIP13_ERR_DIFFICULTY_TOO_LOW 1
#define NIP13_ERR_GENERATE_TIMEOUT 2

// Difficulty counts the number of leading zero bits in an event ID.
int nip13_difficulty(const char *event_id);

// Check reports whether the event ID demonstrates a sufficient proof of work difficulty.
int nip13_check(const char *event_id, int min_difficulty);

// Generate performs proof of work on the specified event until either the target
// difficulty is reached or the function runs for longer than the timeout.
int nip13_generate(Event *event, int target_difficulty, time_t timeout);

#endif // NOSTR_NIP13_H
