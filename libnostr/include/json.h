#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "nostr.h"

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char* (*serialize_event)(const struct _NostrEvent *event);
    struct _NostrEvent* (*deserialize_event)(const char *json_str);
} NostrJsonInterface;

#endif // NOSTR_JSON_H