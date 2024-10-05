#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "nostr.h"

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char* (*serialize)(const struct _NostrEvent *event);
    struct _NostrEvent* (*deserialize)(const char *json_str);
} NostrJsonInterface;

#endif // NOSTR_JSON_H