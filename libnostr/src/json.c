#include "json.h"
#include "envelope.h"
#include "event.h"
#include "filter.h"

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

char *nostr_envelope_serialize(const Envelope *envelope) {
    if (json_interface && json_interface->serialize_envelope) {
        return json_interface->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(Envelope *envelope, const char *json) {
    if (json_interface && json_interface->deserialize_envelope) {
        return json_interface->deserialize_envelope(envelope, json);
    }
    return -1;
}

char *nostr_filter_serialize(const Filter *filter) {
    if (json_interface && json_interface->serialize_filter) {
        return json_interface->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(Filter *filter, const char *json) {
    if (json_interface && json_interface->deserialize_filter) {
        return json_interface->deserialize_filter(filter, json);
    }
    return -1;
}
