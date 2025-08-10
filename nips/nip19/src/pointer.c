// NIP-19 pointer implementations (moved from core)
#include "nostr-pointer.h"
#include <stdlib.h>

NostrProfilePointer *nostr_profile_pointer_new(void) {
    NostrProfilePointer *ptr = (NostrProfilePointer *)malloc(sizeof(NostrProfilePointer));
    if (!ptr) return NULL;
    ptr->public_key = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;
    return ptr;
}

void nostr_profile_pointer_free(NostrProfilePointer *ptr) {
    if (!ptr) return;
    free(ptr->public_key);
    for (size_t i = 0; i < ptr->relays_count; i++) free(ptr->relays[i]);
    free(ptr->relays);
    free(ptr);
}

NostrEventPointer *nostr_event_pointer_new(void) {
    NostrEventPointer *ptr = (NostrEventPointer *)malloc(sizeof(NostrEventPointer));
    if (!ptr) return NULL;
    ptr->id = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;
    ptr->author = NULL;
    ptr->kind = 0;
    return ptr;
}

void nostr_event_pointer_free(NostrEventPointer *ptr) {
    if (!ptr) return;
    free(ptr->id);
    for (size_t i = 0; i < ptr->relays_count; i++) free(ptr->relays[i]);
    free(ptr->relays);
    free(ptr->author);
    free(ptr);
}

NostrEntityPointer *nostr_entity_pointer_new(void) {
    NostrEntityPointer *ptr = (NostrEntityPointer *)malloc(sizeof(NostrEntityPointer));
    if (!ptr) return NULL;
    ptr->public_key = NULL;
    ptr->kind = 0;
    ptr->identifier = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;
    return ptr;
}

void nostr_entity_pointer_free(NostrEntityPointer *ptr) {
    if (!ptr) return;
    free(ptr->public_key);
    free(ptr->identifier);
    for (size_t i = 0; i < ptr->relays_count; i++) free(ptr->relays[i]);
    free(ptr->relays);
    free(ptr);
}
