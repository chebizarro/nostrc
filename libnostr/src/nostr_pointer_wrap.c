#include "nostr-pointer.h"

NostrProfilePointer *nostr_profile_pointer_new(void) {
    return create_profile_pointer();
}

void nostr_profile_pointer_free(NostrProfilePointer *ptr) {
    free_profile_pointer((ProfilePointer *)ptr);
}

NostrEventPointer *nostr_event_pointer_new(void) {
    return create_event_pointer();
}

void nostr_event_pointer_free(NostrEventPointer *ptr) {
    free_event_pointer((EventPointer *)ptr);
}

NostrEntityPointer *nostr_entity_pointer_new(void) {
    return create_entity_pointer();
}

void nostr_entity_pointer_free(NostrEntityPointer *ptr) {
    free_entity_pointer((EntityPointer *)ptr);
}
