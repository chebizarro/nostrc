/**
 * NIP-13 Proof of Work Example
 *
 * Demonstrates mining an event with a target difficulty.
 */
#include "nip13.h"
#include "nostr-event.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    /* Check difficulty of a known event ID */
    const char *test_id = "000000000e9d97a1ab09fc381030b346cdd7b4a35c17aeeda7ecb7238440d5d0";
    int d = nip13_difficulty(test_id);
    printf("Difficulty of test ID: %d leading zero bits\n", d);

    /* Check against a threshold */
    if (nip13_check(test_id, 36) == 0) {
        printf("Event meets 36-bit PoW requirement\n");
    } else {
        printf("Event does NOT meet 36-bit PoW requirement\n");
    }

    /* Mine an event with a modest difficulty target */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Hello, Nostr with PoW!");
    nostr_event_set_created_at(event, (int64_t)time(NULL));

    int target = 8; /* low target for demo — real usage would be 20+ */
    time_t timeout = 5;
    printf("Mining event with %d-bit difficulty target (timeout %lds)...\n",
           target, (long)timeout);

    int result = nip13_generate(event, target, timeout);
    if (result == 0) {
        char *id = nostr_event_get_id(event);
        printf("PoW success! Event ID: %s\n", id);
        printf("Achieved difficulty: %d bits\n", nip13_difficulty(id));
        free(id);
    } else if (result == NIP13_ERR_GENERATE_TIMEOUT) {
        printf("Timed out — could not reach target difficulty\n");
    } else {
        printf("Error during PoW generation\n");
    }

    nostr_event_free(event);
    return 0;
}
