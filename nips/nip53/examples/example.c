#include <stdio.h>
#include "nip53.h"

int main() {
    const char *event_json = "{\"kind\":31923,\"tags\":[[\"d\",\"event-id\"],[\"title\",\"Live Event Title\"],[\"summary\",\"This is a summary.\"],[\"image\",\"http://example.com/image.png\"],[\"status\",\"ongoing\"],[\"start\",\"1633072800\"],[\"end\",\"1633076400\"],[\"streaming\",\"http://example.com/stream\"],[\"recording\",\"http://example.com/record\"],[\"p\",\"pubkey1\",\"relay1\",\"host\"],[\"p\",\"pubkey2\",\"relay2\",\"guest\"],[\"t\",\"hashtag1\"],[\"current_participants\",\"100\"],[\"total_participants\",\"500\"],[\"relay\",\"wss://relay.example.com\"]]}";

    LiveEvent *event = parse_live_event(event_json);
    if (!event) {
        fprintf(stderr, "Failed to parse live event\n");
        return -1;
    }

    char *json_str = live_event_to_json(event);
    if (json_str) {
        printf("Serialized JSON: %s\n", json_str);
        free(json_str);
    } else {
        fprintf(stderr, "Failed to serialize live event\n");
    }

    Participant *host = get_host(event);
    if (host) {
        printf("Host Public Key: %s\n", host->pub_key);
    } else {
        printf("No host found\n");
    }

    free_live_event(event);

    return 0;
}
