#include "nson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    // Example NSON data
    const char* nson_data = "{\"id\":\"abc123\",\"pubkey\":\"def456\",\"sig\":\"ghi789\",\"created_at\":1627845443,\"nson\":\"...\",\"kind\":1,\"content\":\"hello world\",\"tags\":[...]}";

    // Unmarshal NSON data
    nson_Event event;
    if (nson_unmarshal(nson_data, &event) == 0) {
        printf("Event ID: %s\n", event.id);
        printf("Event PubKey: %s\n", event.pubkey);
        printf("Event Signature: %s\n", event.sig);
        printf("Event Created At: %ld\n", event.created_at);
        printf("Event Kind: %d\n", event.kind);
        printf("Event Content: %s\n", event.content);
        nson_event_free(&event);
    } else {
        printf("Failed to unmarshal NSON data.\n");
    }

    // Create an event
    nson_Event new_event = {
        .id = strdup("abc123"),
        .pubkey = strdup("def456"),
        .sig = strdup("ghi789"),
        .created_at = 1627845443,
        .kind = 1,
        .content = strdup("hello world"),
        .tags_count = 1,
        .tags = malloc(sizeof(Tag))
    };
    new_event.tags[0].count = 2;
    new_event.tags[0].elements = malloc(2 * sizeof(char*));
    new_event.tags[0].elements[0] = strdup("tag1");
    new_event.tags[0].elements[1] = strdup("tag2");

    // Marshal the event to NSON
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
