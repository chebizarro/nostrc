#ifndef NOSTR_NIP31_H
#define NOSTR_NIP31_H

#include "nostr-event.h"

// Function to get the "alt" tag from an event
const char* nostr_get_alt(const nostr_event_t *event);

#endif // NOSTR_NIP31_H
