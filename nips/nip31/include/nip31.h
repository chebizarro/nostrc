#ifndef NOSTR_NIP31_H
#define NOSTR_NIP31_H

#include <nostr/nip31/export.h>

#include "nostr-event.h"

// Function to get the "alt" tag from an event
NOSTR_NIP31_API const char* nostr_get_alt(const nostr_event_t *event);

#endif // NOSTR_NIP31_H
