#ifndef NOSTR_TAG_H
#define NOSTR_TAG_H

#include "go.h"
#include <stdbool.h>
#include <stdlib.h>

typedef StringArray Tag;

typedef struct _Tags {
    Tag **data;
    size_t count;
} Tags;

#endif // NOSTR_TAG_H
