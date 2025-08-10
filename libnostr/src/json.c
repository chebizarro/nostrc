#include "json.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <jansson.h>

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

int nostr_json_get_string_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             char **out_str) {
    if (!json || !object_key || !entry_key || !out_str) return -1;
    *out_str = NULL;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *val = json_object_get(obj, entry_key);
    if (val && json_is_string(val)) {
        const char *s = json_string_value(val);
        if (s) *out_str = strdup(s);
    }
    json_decref(root);
    return *out_str ? 0 : -1;
}

int nostr_json_get_string_array_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   char ***out_array,
                                   size_t *out_count) {
    if (out_array) *out_array = NULL; if (out_count) *out_count = 0;
    if (!json || !object_key || !entry_key || !out_array) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *arr = json_object_get(obj, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    char **vec = NULL;
    if (n > 0) {
        vec = (char **)calloc(n, sizeof(char*));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            if (it && json_is_string(it)) {
                const char *s = json_string_value(it);
                vec[i] = s ? strdup(s) : NULL;
            }
        }
    }
    json_decref(root);
    *out_array = vec; if (out_count) *out_count = n;
    return 0;
}
