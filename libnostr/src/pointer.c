#include "pointer.h"
#include <stdlib.h>
#include <string.h>

// Create ProfilePointer
ProfilePointer *create_profile_pointer(void) {
    ProfilePointer *ptr = (ProfilePointer *)malloc(sizeof(ProfilePointer));
    if (!ptr)
        return NULL;

    ptr->public_key = NULL;
    ptr->relays = NULL;
    ptr->relays_count = 0;

    return ptr;
}

// Free ProfilePointer
void free_profile_pointer(ProfilePointer *ptr) {
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
EventPointer *create_event_pointer(void) {
    EventPointer *ptr = (EventPointer *)malloc(sizeof(EventPointer));
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
void free_event_pointer(EventPointer *ptr) {
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
EntityPointer *create_entity_pointer(void) {
    EntityPointer *ptr = (EntityPointer *)malloc(sizeof(EntityPointer));
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
void free_entity_pointer(EntityPointer *ptr) {
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
