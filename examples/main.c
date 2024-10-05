#include "nostr.h"
#include <stdio.h>

int main() {
    // Set the JSON interface for the library
    nostr_set_json_interface(&cjson_interface);

    // Initialize the JSON interface
    nostr_json_init();

    // Create an example event
    NostrEvent event;
    event.id = "event-id";
    event.pubkey = "public-key";
    event.kind = 1;
    event.content = "Hello, Nostr!";
    event.sig = "signature";
    event.tags = (char *[]){"tag1", "tag2", NULL};

    // Serialize the event
    char *json_str = nostr_event_serialize(&event);
    if (json_str) {
        printf("Serialized JSON: %s\n", json_str);
        g_free(json_str);
    }

    // Deserialize the event
    const char *json_input = "{\"id\":\"event-id\",\"pubkey\":\"public-key\",\"kind\":1,\"content\":\"Hello, Nostr!\",\"sig\":\"signature\",\"tags\":[\"tag1\",\"tag2\"]}";
    NostrEvent *deserialized_event = nostr_event_deserialize(json_input);
    if (deserialized_event) {
        printf("Deserialized Event: %s\n", deserialized_event->content);
        nostr_event_free(deserialized_event);
    }

    // Cleanup the JSON interface
    nostr_json_cleanup();

    return 0;
}
