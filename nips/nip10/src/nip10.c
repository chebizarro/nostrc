#include "nostr/nip10.h"
#include <string.h>

NostrTag* get_thread_root(NostrTags* tags) {
    if (!tags) return NULL;
    NostrTag* first_e = NULL;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag* tag = tags->data[i];
        if (!tag || tag->size == 0) continue;
        if (strcmp(tag->data[0], "e") != 0) continue;
        if (!first_e) first_e = tag;
        if (tag->size >= 4 && strcmp(tag->data[3], "root") == 0) {
            return tag;
        }
    }
    return first_e;
}

NostrTag* get_immediate_reply(NostrTags* tags) {
    if (!tags) return NULL;
    NostrTag* root = NULL;
    NostrTag* last_e = NULL;

    for (size_t i = 0; i < tags->count; i++) {
        NostrTag* tag = tags->data[i];
        if (!tag || tag->size < 2) continue;

        if (strcmp(tag->data[0], "e") != 0 && strcmp(tag->data[0], "a") != 0) continue;

        if (tag->size >= 4) {
            if (strcmp(tag->data[3], "reply") == 0) {
                return tag;
            }
            if (strcmp(tag->data[3], "root") == 0) {
                root = tag;
                continue;
            }
            if (strcmp(tag->data[3], "mention") == 0) {
                continue;
            }
        }

        if (strcmp(tag->data[0], "e") == 0) {
            last_e = tag;
        }
    }

    if (root != NULL) return root;
    return last_e;
}
