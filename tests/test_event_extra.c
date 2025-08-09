#include "nostr-event-extra.h"
#include "nostr-event.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <jansson.h>

int main() {
    NostrEvent *event = nostr_event_new();
    assert(event != NULL);

    // Test setting extra fields
    nostr_event_set_extra(event, "test_key_string", json_string("test_value"));
    nostr_event_set_extra(event, "test_key_number", json_real(42.0));
    nostr_event_set_extra(event, "test_key_boolean", json_true());

    // Test getting extra fields
    char *string_value = nostr_event_get_extra_string(event, "test_key_string");
    assert(string_value != NULL && strcmp(string_value, "test_value") == 0);
    free(string_value);

    double number_value = 0.0;
    assert(nostr_event_get_extra_number(event, "test_key_number", &number_value));
    assert(number_value == 42.0);

    bool boolean_value = false;
    assert(nostr_event_get_extra_bool(event, "test_key_boolean", &boolean_value));
    assert(boolean_value == true);

    // Test removing extra fields
    nostr_event_remove_extra(event, "test_key_string");
    string_value = nostr_event_get_extra_string(event, "test_key_string");
    assert(string_value == NULL);

    nostr_event_free(event);
    printf("All tests passed!\n");
    return 0;
}