#include "nostr/nip31.h"
#include <string.h>

const char* nostr_get_alt(const nostr_event_t *event) {
    for (size_t i = 0; i < event->tags_len; ++i) {
        if (event->tags[i].key && strcmp(event->tags[i].key, "alt") == 0) {
            return event->tags[i].value;
        }
    }
    return NULL;
}
