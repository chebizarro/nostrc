# Nostr C Library API Documentation

## NSON

### Overview

NSON is a compact representation of Nostr events. This section covers the implementation of NSON in the Nostr C library, providing functions to marshal and unmarshal Nostr events to and from NSON format.

#### Structures

```c
typedef struct {
    char id[65];
    char pubkey[65];
    char sig[129];
    long created_at;
    int kind;
    char* content;
    int tags_count;
    nostr_Tag* tags;
} nson_Event;

typedef struct {
    int fields_count;
    char* fields[4]; // Adjust the size as needed
} nostr_Tag;
```

#### Functions

##### `nson_unmarshal`

Unmarshals an NSON string into an `nson_Event` structure.

```c
int nson_unmarshal(const char* data, nson_Event* evt);
```

- **Parameters:**
  - `data`: The NSON string to be unmarshaled.
  - `evt`: Pointer to the `nson_Event` structure where the unmarshaled data will be stored.

- **Returns:**
  - `0` on success.
  - `-1` if the input is not a valid NSON string.
  - Other negative values for specific errors.

##### `nson_marshal`

Marshals an `nson_Event` structure into an NSON string.

```c
char* nson_marshal(const nson_Event* evt);
```

- **Parameters:**
  - `evt`: Pointer to the `nson_Event` structure to be marshaled.

- **Returns:**
  - A dynamically allocated NSON string on success.
  - `NULL` on failure.

- **Note:** The returned string must be freed using `free`.

##### `nson_event_free`

Frees the memory allocated for an `nson_Event` structure.

```c
void nson_event_free(nson_Event* evt);
```

- **Parameters:**
  - `evt`: Pointer to the `nson_Event` structure whose memory is to be freed.

##### Example Usage

###### Unmarshaling an Event

```c
#include "nson.h"
#include <stdio.h>

int main() {
    const char* nson_data = "{\"id\":\"abc123\",\"pubkey\":\"def456\",\"sig\":\"ghi789\",\"created_at\":1627845443,\"nson\":\"...\",\"kind\":1,\"content\":\"hello world\",\"tags\":[...]}";

    nson_Event event;
    if (nson_unmarshal(nson_data, &event) == 0) {
        printf("Event ID: %s\n", event.id);
        printf("Event PubKey: %s\n", event.pubkey);
        printf("Event Signature: %s\n", event.sig);
        printf("Event Created At: %ld\n", event.created_at);
        printf("Event Kind: %d\n", event.kind);
        printf("Event Content: %s\n", event.content);

        for (int i = 0; i < event.tags_count; ++i) {
            printf("Tag %d:\n", i);
            for (int j = 0; j < event.tags[i].fields_count; ++j) {
                printf("  Field %d: %s\n", j, event.tags[i].fields[j]);
            }
        }

        nson_event_free(&event);
    } else {
        printf("Failed to unmarshal NSON data.\n");
    }

    return 0;
}
```

###### Marshaling an Event

```c
#include "nson.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    nson_Event new_event = {
        .id = "abc123",
        .pubkey = "def456",
        .sig = "ghi789",
        .created_at = 1627845443,
        .kind = 1,
        .content = strdup("hello world"),
        .tags_count = 1,
        .tags = malloc(sizeof(nostr_Tag))
    };
    new_event.tags[0].fields_count = 2;
    new_event.tags[0].fields[0] = strdup("tag1");
    new_event.tags[0].fields[1] = strdup("tag2");

    char* nson_string = nson_marshal(&new_event);
    if (nson_string != NULL) {
        printf("NSON String: %s\n", nson_string);
        free(nson_string);
    } else {
        printf("Failed to marshal event to NSON.\n");
    }

    nson_event_free(&new_event);
    return 0;
}
```

##### Memory Management

The `nson` module provides comprehensive memory management functions to handle dynamic allocations and deallocations. Always use `nson_event_free` to free the memory allocated for an `nson_Event` structure to avoid memory leaks.

### Summary

The `nson` module is designed to provide efficient and flexible serialization and deserialization of Nostr events, with robust memory management to support various use cases, from IoT devices to full-fledged desktop applications.
