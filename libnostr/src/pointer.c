#include "pointer.h"
#include <stdlib.h>
#include <string.h>

// Create ProfilePointer
NostrProfilePointer *nostr_profile_pointer_new(void) {
    NostrProfilePointer *ptr = (NostrProfilePointer *)malloc(sizeof(NostrProfilePointer));
    if (!ptr)
        return NULL;

    ptr->public_key = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;

    return ptr;
}

// Free ProfilePointer
void nostr_profile_pointer_free(NostrProfilePointer *ptr) {
    if (ptr) {
        free(ptr->public_key);
        for (size_t i = 0; i < ptr->relays_count; i++) {
            free(ptr->relays[i]);
        }
        free(ptr->relays);
        free(ptr);
    }
}

// Create EventPointer
NostrEventPointer *nostr_event_pointer_new(void) {
    NostrEventPointer *ptr = (NostrEventPointer *)malloc(sizeof(NostrEventPointer));
    if (!ptr)
        return NULL;

    ptr->id = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;
    ptr->author = NULL;
    ptr->kind = 0;

    return ptr;
}

// Free EventPointer
void nostr_event_pointer_free(NostrEventPointer *ptr) {
    if (ptr) {
        free(ptr->id);
        for (size_t i = 0; i < ptr->relays_count; i++) {
            free(ptr->relays[i]);
        }
        free(ptr->relays);
        free(ptr->author);
        free(ptr);
    }
}

// Create EntityPointer
NostrEntityPointer *nostr_entity_pointer_new(void) {
    NostrEntityPointer *ptr = (NostrEntityPointer *)malloc(sizeof(NostrEntityPointer));
    if (!ptr)
        return NULL;

    ptr->public_key = NULL;
    ptr->kind = 0;
    ptr->identifier = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;

    return ptr;
}

// Free EntityPointer
void nostr_entity_pointer_free(NostrEntityPointer *ptr) {
    if (ptr) {
        free(ptr->public_key);
        free(ptr->identifier);
        for (size_t i = 0; i < ptr->relays_count; i++) {
            free(ptr->relays[i]);
        }
        free(ptr->relays);
        free(ptr);
    }
}
