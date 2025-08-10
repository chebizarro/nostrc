#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "nostr-envelope.h"
#include "event.h"
#include "nostr-filter.h"

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char *(*serialize_event)(const NostrEvent *event);
    int (*deserialize_event)(NostrEvent *event, const char *json_str);
    char *(*serialize_envelope)(const NostrEnvelope *envelope);
    int (*deserialize_envelope)(NostrEnvelope *envelope, const char *json_str);
    char *(*serialize_filter)(const NostrFilter *filter);
    int (*deserialize_filter)(NostrFilter *filter, const char *json_str);

} NostrJsonInterface;

extern NostrJsonInterface *json_interface;
void nostr_set_json_interface(NostrJsonInterface *interface);
void nostr_json_init(void);
void nostr_json_cleanup(void);
char *nostr_event_serialize(const NostrEvent *event);
int nostr_event_deserialize(NostrEvent *event, const char *json);
char *nostr_envelope_serialize(const NostrEnvelope *envelope);
int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json);
char *nostr_filter_serialize(const NostrFilter *filter);
int nostr_filter_deserialize(NostrFilter *filter, const char *json);

/* Generic helpers (backend-agnostic) for simple nested lookups.
 * These parse the input JSON per call and return newly allocated results. */
/* Get string at top_object[entry_key] where top_object is at object_key. */
int nostr_json_get_string_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             char **out_str);

/* Get array of strings at top_object[entry_key] where top_object is at object_key. */
int nostr_json_get_string_array_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   char ***out_array,
                                   size_t *out_count);

#endif // NOSTR_JSON_H
