#ifndef EVENT_EXTRA_H
#define EVENT_EXTRA_H

#include "nostr.h"
#include <stdbool.h>
#include <stdlib.h>

// Function prototypes for handling extra fields in NostrEvent
void set_extra(NostrEvent *event, const char *key, json_t *value);
void remove_extra(NostrEvent *event, const char *key);
json_t *get_extra(NostrEvent *event, const char *key);
const char *get_extra_string(NostrEvent *event, const char *key);
double get_extra_number(NostrEvent *event, const char *key);
bool get_extra_boolean(NostrEvent *event, const char *key);

#endif // EVENT_EXTRA_H