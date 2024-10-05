#include "event_extra.h"
#include <assert.h>
#include <stdio.h>
#include <jansson.h>

int main() {
    NostrEvent *event = create_event();
    assert(event != NULL);

    // Test setting extra fields
    set_extra(event, "test_key_string", json_string("test_value"));
    set_extra(event, "test_key_number", json_real(42.0));
    set_extra(event, "test_key_boolean", json_true());

    // Test getting extra fields
    const char *string_value = get_extra_string(event, "test_key_string");
    assert(strcmp(string_value, "test_value") == 0);

    double number_value = get_extra_number(event, "test_key_number");
    assert(number_value == 42.0);

    bool boolean_value = get_extra_boolean(event, "test_key_boolean");
    assert(boolean_value == true);

    // Test removing extra fields
    remove_extra(event, "test_key_string");
    string_value = get_extra_string(event, "test_key_string");
    assert(strcmp(string_value, "") == 0);

    free_event(event);
    printf("All tests passed!\n");
    return 0;
}