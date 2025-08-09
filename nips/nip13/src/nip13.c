#include "nostr/nip13.h"
#include "nostr/event.h"
#include "nostr-event.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int leading_zeros(uint8_t byte) {
    static const int lookup[16] = {4, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    return lookup[byte >> 4] + (byte == 0);
}

int nip13_difficulty(const char *event_id) {
    if (strlen(event_id) != 64) {
        return -1;
    }

    int zeros = 0;
    uint8_t byte;

    for (size_t i = 0; i < 64; i += 2) {
        if (sscanf(event_id + i, "%2hhx", &byte) != 1) {
            return -1;
        }

        int lz = leading_zeros(byte);
        zeros += lz;

        if (lz < 8) {
            break;
        }
    }

    return zeros;
}

int nip13_check(const char *event_id, int min_difficulty) {
    return nip13_difficulty(event_id) >= min_difficulty ? 0 : NIP13_ERR_DIFFICULTY_TOO_LOW;
}

int nip13_generate(Event *event, int target_difficulty, time_t timeout) {
    char *tag[2];
    tag[0] = "nonce";
    tag[1] = malloc(21); // Enough to hold uint64 max value
    if (!tag[1]) {
        return -1;
    }

    snprintf(tag[1], 21, "%d", target_difficulty);
    event_add_tag(event, tag, 2);

    uint64_t nonce = 0;
    time_t start = time(NULL);

    while (1) {
        nonce++;
        snprintf(tag[1], 21, "%llu", (unsigned long long)nonce);
        event->created_at = time(NULL);

        if (nip13_difficulty(nostr_event_get_id((NostrEvent*)event)) >= target_difficulty) {
            free(tag[1]);
            return 0;
        }

        if (nonce % 1000 == 0 && difftime(time(NULL), start) > timeout) {
            free(tag[1]);
            return NIP13_ERR_GENERATE_TIMEOUT;
        }
    }
}
