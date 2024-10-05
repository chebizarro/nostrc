#include "pointer.h"
#include <assert.h>

int main() {
    // Test ProfilePointer
    ProfilePointer *profile_ptr = create_profile_pointer();
    assert(profile_ptr != NULL);
    profile_ptr->public_key = strdup("test_public_key");
    profile_ptr->relays = (char **)malloc(2 * sizeof(char *));
    profile_ptr->relays[0] = strdup("relay1");
    profile_ptr->relays[1] = strdup("relay2");
    profile_ptr->relays_count = 2;
    free_profile_pointer(profile_ptr);

    // Test EventPointer
    EventPointer *event_ptr = create_event_pointer();
    assert(event_ptr != NULL);
    event_ptr->id = strdup("test_id");
    event_ptr->relays = (char **)malloc(2 * sizeof(char *));
    event_ptr->relays[0] = strdup("relay1");
    event_ptr->relays[1] = strdup("relay2");
    event_ptr->relays_count = 2;
    event_ptr->author = strdup("test_author");
    event_ptr->kind = 1;
    free_event_pointer(event_ptr);

    // Test EntityPointer
    EntityPointer *entity_ptr = create_entity_pointer();
    assert(entity_ptr != NULL);
    entity_ptr->public_key = strdup("test_public_key");
    entity_ptr->identifier = strdup("test_identifier");
    entity_ptr->relays = (char **)malloc(2 * sizeof(char *));
    entity_ptr->relays[0] = strdup("relay1");
    entity_ptr->relays[1] = strdup("relay2");
    entity_ptr->relays_count = 2;
    entity_ptr->kind = 1;
    free_entity_pointer(entity_ptr);

    return 0;
}