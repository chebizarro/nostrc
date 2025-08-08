#include "event_extra.h"
#include <jansson.h>
#include <string.h>

// SetExtra sets an out-of-the-spec value under the given key into the event object.
void set_extra(NostrEvent *event, const char *key, json_t *value) {
    if (!event->extra) {
        event->extra = json_object();
    }
    json_object_set_new(event->extra, key, value);
}

// RemoveExtra removes an out-of-the-spec value under the given key from the event object.
void remove_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return;
    }
    json_object_del(event->extra, key);
}

// GetExtra tries to get a value under the given key that may be present in the event object
json_t *get_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return NULL;
    }
    return json_object_get(event->extra, key);
}

// GetExtraString is like [GetExtra], but only works if the value is a string,
// otherwise returns the zero-value.
const char *get_extra_string(NostrEvent *event, const char *key) {
    json_t *value = get_extra(event, key);
    if (!json_is_string(value)) {
        return "";
    }
    return json_string_value(value);
}

// GetExtraNumber is like [GetExtra], but only works if the value is a float64,
// otherwise returns the zero-value.
double get_extra_number(NostrEvent *event, const char *key) {
    json_t *value = get_extra(event, key);
    if (json_is_number(value)) {
        return json_number_value(value);
    }
    if (json_is_integer(value)) {
        return (double)json_integer_value(value);
    }
    return 0.0;
}

// GetExtraBoolean is like [GetExtra], but only works if the value is a boolean,
// otherwise returns the zero-value.
bool get_extra_boolean(NostrEvent *event, const char *key) {
    json_t *value = get_extra(event, key);
    if (!json_is_boolean(value)) {
        return false;
    }
    return json_boolean_value(value);
}
