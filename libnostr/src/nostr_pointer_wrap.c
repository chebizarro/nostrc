#include "nostr-pointer.h"
#include <stdlib.h>

NostrProfilePointer *nostr_profile_pointer_new(void) {
    // Opaque blob; size can be minimal since API is opaque
    return (NostrProfilePointer *)malloc(1);
}

void nostr_profile_pointer_free(NostrProfilePointer *ptr) {
    free(ptr);
}

NostrEventPointer *nostr_event_pointer_new(void) {
    return (NostrEventPointer *)malloc(1);
}

void nostr_event_pointer_free(NostrEventPointer *ptr) {
    free(ptr);
}

NostrEntityPointer *nostr_entity_pointer_new(void) {
    return (NostrEntityPointer *)malloc(1);
}

void nostr_entity_pointer_free(NostrEntityPointer *ptr) {
    free(ptr);
}
