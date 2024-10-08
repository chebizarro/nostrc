#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "event.h"
#include "envelope.h"
#include "filter.h"

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char *(*serialize_event)(const NostrEvent *event);
    NostrEvent *(*deserialize_event)(const char *json_str);
    char *(*serialize_envelope)(const Envelope *envelope);
    Envelope *(*deserialize_envelope)(const char *json_str);
    char *(*serialize_filter)(const Filter *filter);
    Filter *(*deserialize_filter)(const char *json_str);

} NostrJsonInterface;

extern NostrJsonInterface *json_interface;
void nostr_set_json_interface(NostrJsonInterface *interface);

#endif // NOSTR_JSON_H