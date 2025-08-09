#include "nostr-event-extra.h"
#include "event.h"  // internal struct definition for NostrEvent
#include <jansson.h>
#include <string.h>

// nostr_event_set_extra sets an out-of-the-spec value under the given key into the event object.
void nostr_event_set_extra(NostrEvent *event, const char *key, void *value) {
    if (!event->extra) {
        event->extra = json_object();
    }
    if (value) json_incref((json_t *)value);
    json_object_set_new(event->extra, key, (json_t *)value);
}

// nostr_event_remove_extra removes an out-of-the-spec value under the given key from the event object.
void nostr_event_remove_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return;
    }
    json_object_del(event->extra, key);
}

// nostr_event_get_extra tries to get a value under the given key that may be present in the event object
void *nostr_event_get_extra(NostrEvent *event, const char *key) {
    if (!event->extra) {
        return NULL;
    }
    return (void *)json_object_get(event->extra, key);
}

// nostr_event_get_extra_string returns a newly-allocated copy of string value or NULL.
char *nostr_event_get_extra_string(NostrEvent *event, const char *key) {
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (!json_is_string(value)) {
        return NULL;
    }
    const char *s = json_string_value(value);
    return s ? strdup(s) : NULL;
}

// nostr_event_get_extra_number writes number to out and returns true on success.
bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out) {
    if (!out) return false;
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (json_is_number(value)) {
        *out = json_number_value(value);
        return true;
    }
    if (json_is_integer(value)) {
        *out = (double)json_integer_value(value);
        return true;
    }
    return false;
}

// nostr_event_get_extra_bool writes boolean to out and returns true on success.
bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out) {
    if (!out) return false;
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (!json_is_boolean(value)) {
        return false;
    }
    *out = json_boolean_value(value);
    return true;
}
