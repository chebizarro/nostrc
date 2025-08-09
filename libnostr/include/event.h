#ifndef NOSTR_EVENT_H
#define NOSTR_EVENT_H

#include "tag.h"

// Define the NostrEvent structure
typedef struct _NostrEvent {
    char *id;
    char *pubkey;
    int64_t created_at;
    int kind;
    Tags *tags;
    char *content;
    char *sig;
    void *extra; // Extra fields
} NostrEvent;

/* Legacy function prototypes removed. Use `nostr-event.h` for the public API. */

#endif // NOSTR_EVENT_H
