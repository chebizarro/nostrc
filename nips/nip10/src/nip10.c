#include "nostr/nip10.h"
#include <string.h>

Tag* get_thread_root(Tags* tags) {
    for (size_t i = 0; i < tags->count; i++) {
        Tag* tag = tags->items[i];
        if (tag->count >= 4 && strcmp(tag->items[0], "e") == 0 && strcmp(tag->items[3], "root") == 0) {
            return tag;
        }
    }

    return tags_get_first(tags, "e");
}

Tag* get_immediate_reply(Tags* tags) {
    Tag* root = NULL;
    Tag* last_e = NULL;

    for (size_t i = 0; i < tags->count; i++) {
        Tag* tag = tags->items[i];

        if (tag->count < 2) {
            continue;
        }
        if (strcmp(tag->items[0], "e") != 0 && strcmp(tag->items[0], "a") != 0) {
            continue;
        }

        if (tag->count >= 4) {
            if (strcmp(tag->items[3], "reply") == 0) {
                return tag;
            }
            if (strcmp(tag->items[3], "root") == 0) {
                root = tag;
                continue;
            }
            if (strcmp(tag->items[3], "mention") == 0) {
                continue;
            }
        }

        last_e = tag;
    }

    if (root != NULL) {
        return root;
    }

    return last_e;
}
