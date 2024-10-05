#include <stdio.h>
#include "nip52.h"

int main() {
    const char *event_json = "{\"kind\":31923,\"tags\":[[\"d\",\"event-id\"],[\"title\",\"Event Title\"],[\"image\",\"http://example.com/image.png\"],[\"start\",\"1633072800\"],[\"end\",\"1633076400\"],[\"location\",\"Location 1\"],[\"g\",\"geohash1\"],[\"p\",\"pubkey1\",\"relay1\",\"role1\"],[\"r\",\"reference1\"],[\"t\",\"hashtag1\"]]}";

    CalendarEvent *event = parse_calendar_event(event_json);
    if (!event) {
        fprintf(stderr, "Failed to parse calendar event\n");
        return -1;
    }

    char *json_str = calendar_event_to_json(event);
    if (json_str) {
        printf("Serialized JSON: %s\n", json_str);
        free(json_str);
    } else {
        fprintf(stderr, "Failed to serialize calendar event\n");
    }

    free_calendar_event(event);

    return 0;
}
