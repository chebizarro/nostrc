#include "nostr-event-extra.h"
#include "nostr-event.h"  // internal struct definition for NostrEvent
#include "json.h"         // NostrJsonInterface helpers
#include <string.h>

// nostr_event_set_extra sets an out-of-the-spec value under the given key into the event object.
void nostr_event_set_extra(NostrEvent *event, const char *key, void *value) {
    if (!event->extra) {
        event->extra = nostr_json_object_new();
    }
    if (value) nostr_json_value_incref((NostrJsonValue)value);
    nostr_json_object_set(event->extra, key, (NostrJsonValue)value);
}

// nostr_event_remove_extra removes an out-of-the-spec value under the given key from the event object.
void nostr_event_remove_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return;
    }
    nostr_json_object_del(event->extra, key);
}

// nostr_event_get_extra tries to get a value under the given key that may be present in the event object
void *nostr_event_get_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return NULL;
    }
    return nostr_json_object_get(event->extra, key);
}

// nostr_event_get_extra_string returns a newly-allocated copy of string value or NULL.
char *nostr_event_get_extra_string(NostrEvent *event, const char *key) {
    NostrJsonValue value = (NostrJsonValue)nostr_event_get_extra(event, key);
    if (!nostr_json_value_is_string(value)) {
        return NULL;
    }
    const char *s = nostr_json_value_string(value);
    return s ? strdup(s) : NULL;
}

// nostr_event_get_extra_number writes number to out and returns true on success.
bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out) {
    if (!out) return false;
    NostrJsonValue value = (NostrJsonValue)nostr_event_get_extra(event, key);
    if (nostr_json_value_is_number(value)) {
        *out = nostr_json_value_number(value);
        return true;
    }
    if (nostr_json_value_is_integer(value)) {
        *out = (double)nostr_json_value_integer(value);
        return true;
    }
    return false;
}

// nostr_event_get_extra_bool writes boolean to out and returns true on success.
bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out) {
    if (!out) return false;
    NostrJsonValue value = (NostrJsonValue)nostr_event_get_extra(event, key);
    if (!nostr_json_value_is_boolean(value)) {
        return false;
    }
    *out = nostr_json_value_boolean(value);
    return true;
}
