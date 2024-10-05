#include "nostr.h"
#include <stdlib.h>
#include <string.h>

NostrJsonInterface *json_interface = NULL;

void nostr_set_json_interface(NostrJsonInterface *interface) {
    json_interface = interface;
}

void nostr_json_init(void) {
    if (json_interface && json_interface->init) {
        json_interface->init();
    }
}

void nostr_json_cleanup(void) {
    if (json_interface && json_interface->cleanup) {
        json_interface->cleanup();
    }
}

char* nostr_event_serialize(const NostrEvent *event) {
    if (json_interface && json_interface->serialize) {
        return json_interface->serialize(event);
    }
    return NULL;
}
