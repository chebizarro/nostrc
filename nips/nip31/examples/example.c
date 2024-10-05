#include <stdio.h>
#include "nostr/nip31.h"
#include "nostr/event.h"

int main() {
    // Create an event and add an "alt" tag
    nostr_event_t event;
    nostr_event_init(&event);
    nostr_event_add_tag(&event, "alt", "example-alt-value");

    // Get the "alt" tag value
    const char *alt_value = nostr_get_alt(&event);
    if (alt_value) {
        printf("Alt tag value: %s\n", alt_value);
    } else {
        printf("Alt tag not found\n");
    }

    // Free the event
    nostr_event_free(&event);

    return 0;
}
