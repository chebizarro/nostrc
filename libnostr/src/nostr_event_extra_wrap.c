#include <stdbool.h>
#include "event_extra.h"

void nostr_event_set_extra(NostrEvent *event, const char *key, json_t *value) {
    set_extra(event, key, value);
}

void nostr_event_remove_extra(NostrEvent *event, const char *key) {
    remove_extra(event, key);
}

json_t *nostr_event_get_extra(NostrEvent *event, const char *key) {
    return get_extra(event, key);
}

char *nostr_event_get_extra_string(NostrEvent *event, const char *key) {
    return (char *)get_extra_string(event, key);
}

bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out) {
    if (!out) return false;
    *out = get_extra_number(event, key);
    return true;
}

bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out) {
    if (!out) return false;
    *out = get_extra_boolean(event, key);
    return true;
}
