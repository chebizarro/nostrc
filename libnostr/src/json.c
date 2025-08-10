#include "json.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"

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

char *nostr_event_serialize(const NostrEvent *event) {
    if (json_interface && json_interface->serialize_event) {
        return json_interface->serialize_event(event);
    }
    return NULL;
}

int nostr_event_deserialize(NostrEvent *event, const char *json_str) {
    if (json_interface && json_interface->deserialize_event) {
        return json_interface->deserialize_event(event, json_str);
    }
    return -1;
}

char *nostr_envelope_serialize(const NostrEnvelope *envelope) {
    if (json_interface && json_interface->serialize_envelope) {
        return json_interface->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json) {
    if (json_interface && json_interface->deserialize_envelope) {
        return json_interface->deserialize_envelope(envelope, json);
    }
    return -1;
}

char *nostr_filter_serialize(const NostrFilter *filter) {
    if (json_interface && json_interface->serialize_filter) {
        return json_interface->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(NostrFilter *filter, const char *json) {
    if (json_interface && json_interface->deserialize_filter) {
        return json_interface->deserialize_filter(filter, json);
    }
    return -1;
}
