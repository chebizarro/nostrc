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
void nostr_json_init(void);
void nostr_json_cleanup(void);
char * nostr_event_serialize(const NostrEvent * event);
NostrEvent * nostr_event_deserialize(const char * event);
char * nostr_envelope_serialize(const Envelope * envelope);
Envelope * nostr_envelope_deserialize(const char * json);
char * nostr_filter_serialize(const Filter * filter);
Filter * nostr_filter_deserialize(const char * json);

#endif // NOSTR_JSON_H