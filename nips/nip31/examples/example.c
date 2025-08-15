#include <stdio.h>
#include <stdlib.h>

#include "nostr-event.h"
#include "nostr/nip31/nip31.h"

int main(void) {
    // Create an event and set an "alt" tag using canonical API
    NostrEvent *ev = nostr_event_new();
    if (!ev) return 1;

    if (nostr_nip31_set_alt(ev, "example-alt-value") != 0) {
        fprintf(stderr, "failed to set alt\n");
        nostr_event_free(ev);
        return 1;
    }

    // Get the "alt" tag value (malloc'd)
    char *alt_value = NULL;
    if (nostr_nip31_get_alt(ev, &alt_value) == 0 && alt_value) {
        printf("Alt tag value: %s\n", alt_value);
        free(alt_value);
    } else {
        printf("Alt tag not found\n");
    }

    // Cleanup
    nostr_event_free(ev);
    return 0;
}
