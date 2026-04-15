#include "nip13.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int leading_zeros(uint8_t byte) {
    if (byte == 0) return 8;
    int count = 0;
    while ((byte & 0x80) == 0) {
        count++;
        byte <<= 1;
    }
    return count;
}

int nip13_difficulty(const char *event_id) {
    if (!event_id || strlen(event_id) != 64)
        return -1;

    int zeros = 0;
    uint8_t byte;

    for (size_t i = 0; i < 64; i += 2) {
        if (sscanf(event_id + i, "%2hhx", &byte) != 1)
            return -1;

        int lz = leading_zeros(byte);
        zeros += lz;

        if (lz < 8)
            break;
    }

    return zeros;
}

int nip13_check(const char *event_id, int min_difficulty) {
    int d = nip13_difficulty(event_id);
    if (d < 0) return -1;
    return d >= min_difficulty ? 0 : NIP13_ERR_DIFFICULTY_TOO_LOW;
}

int nip13_generate(NostrEvent *event, int target_difficulty, time_t timeout) {
    if (!event) return -1;

    /* NIP-13 requires a 3-element nonce tag: ["nonce", <nonce>, <target>] */
    char target_str[21];
    snprintf(target_str, sizeof(target_str), "%d", target_difficulty);

    /* Create initial nonce tag with placeholder value */
    NostrTag *nonce_tag = nostr_tag_new("nonce", "0", target_str, NULL);
    if (!nonce_tag)
        return -1;

    /* Append to event tags (or create tags if none) */
    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags) {
        tags = nostr_tags_new(1, nonce_tag);
        nostr_event_set_tags(event, tags);
    } else {
        nostr_tags_append(tags, nonce_tag);
    }

    /* Find the nonce tag index so we can update the value in-place */
    size_t nonce_idx = nostr_tags_size(tags) - 1;

    uint64_t nonce = 0;
    time_t start = time(NULL);
    char nonce_str[21];

    while (1) {
        nonce++;
        snprintf(nonce_str, sizeof(nonce_str), "%llu", (unsigned long long)nonce);

        /* Update the nonce value (index 1) in the existing tag */
        NostrTag *tag = nostr_tags_get(tags, nonce_idx);
        if (tag)
            nostr_tag_set(tag, 1, nonce_str);

        nostr_event_set_created_at(event, (int64_t)time(NULL));

        char *id = nostr_event_get_id(event);
        if (!id) continue;

        int difficulty = nip13_difficulty(id);
        free(id);

        if (difficulty >= target_difficulty)
            return 0;

        if (nonce % 1000 == 0 && difftime(time(NULL), start) > timeout)
            return NIP13_ERR_GENERATE_TIMEOUT;
    }
}
