#ifndef NSON_H
#define NSON_H

#include <stdint.h>
#include "nostr.h"

#define ID_START 7
#define ID_END 71
#define PUBKEY_START 83
#define PUBKEY_END 147
#define SIG_START 156
#define SIG_END 284
#define CREATED_AT_START 299
#define CREATED_AT_END 309

#define NSON_STRING_START 318
#define NSON_VALUES_START 320

#define NSON_MARKER_START 309
#define NSON_MARKER_END 317

typedef struct {
    char* id;
    char* pubkey;
    char* sig;
    Timestamp created_at;
    int kind;
    char* content;
    Tag* tags;
    int tags_count;
} nson_Event;

int nson_unmarshal(const char* data, nson_Event* event);
char* nson_marshal(const nson_Event* event);
void nson_event_free(nson_Event* event);

#endif // NSON_H
