// nostr.c
#include "nostr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simdjson.h"

// Global context for simdjson
simdjson::dom::parser parser;

// Initializes the JSON interface (if needed)
void nostr_json_init(void) {
    // You can initialize the simdjson parser or any other structures here
}

// Cleans up the JSON interface (if needed)
void nostr_json_cleanup(void) {
    // Free any allocated resources if necessary
}

// Serialize NostrEvent to a JSON string
char *nostr_json_serialize(const NostrEvent *event) {
    if (!event) return NULL;

    // Serialize the NostrEvent into a JSON string
    size_t json_len = snprintf(NULL, 0, 
        "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%ld,\"kind\":\"%s\",\"content\":\"%s\"}",
        event->id, event->pubkey, event->created_at, event->kind, event->content);
    
    char *json_str = (char *)malloc(json_len + 1);
    snprintf(json_str, json_len + 1, 
        "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%ld,\"kind\":\"%s\",\"content\":\"%s\"}",
        event->id, event->pubkey, event->created_at, event->kind, event->content);

    return json_str;
}

// Deserialize a JSON string to NostrEvent
NostrEvent *nostr_json_deserialize(const char *json_str) {
    if (!json_str) return NULL;

    // Parse the JSON using simdjson
    simdjson::dom::element doc;
    auto error = parser.parse(json_str).get(doc);
    if (error) return NULL;

    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event) return NULL;

    // Extract fields
    event->id = strdup(doc["id"].get_c_str().value_or(""));
    event->pubkey = strdup(doc["pubkey"].get_c_str().value_or(""));
    event->created_at = doc["created_at"].get_int64().value_or(0);
    event->kind = strdup(doc["kind"].get_c_str().value_or(""));
    event->content = strdup(doc["content"].get_c_str().value_or(""));

    return event;
}

// Implement the interface
NostrJsonInterface nostr_json = {
    .init = nostr_json_init,
    .cleanup = nostr_json_cleanup,
    .serialize = nostr_json_serialize,
    .deserialize = nostr_json_deserialize
};
