#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "envelope.h"
#include "event.h"
#include "filter.h"

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char *(*serialize_event)(const NostrEvent *event);
    int (*deserialize_event)(NostrEvent *event, const char *json_str);
    char *(*serialize_envelope)(const Envelope *envelope);
    int (*deserialize_envelope)(Envelope *envelope, const char *json_str);
    char *(*serialize_filter)(const Filter *filter);
    int (*deserialize_filter)(Filter *filter, const char *json_str);

} NostrJsonInterface;

extern NostrJsonInterface *json_interface;
void nostr_set_json_interface(NostrJsonInterface *interface);
void nostr_json_init(void);
void nostr_json_cleanup(void);
char *nostr_event_serialize(const NostrEvent *event);
int nostr_event_deserialize(NostrEvent *event, const char *json);
char *nostr_envelope_serialize(const Envelope *envelope);
int nostr_envelope_deserialize(Envelope *envelope, const char *json);
char *nostr_filter_serialize(const Filter *filter);
int nostr_filter_deserialize(Filter *filter, const char *json);

#endif // NOSTR_JSON_H