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

// NostrEvent management
NostrEvent *create_event(void);
void free_event(NostrEvent *event);
char *event_serialize(NostrEvent *event);
char *event_get_id(NostrEvent *event);
bool event_check_signature(NostrEvent *event);
int event_sign(NostrEvent *event, const char *private_key);

bool event_is_regular(NostrEvent *event);

#endif // NOSTR_EVENT_H
