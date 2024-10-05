#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr_jansson.h"
#include <jansson.h>

// Initializes the JSON interface (if needed)
void nostr_json_init(void) {
    // Initialize if necessary, Jansson doesn't require special init
}

// Cleans up the JSON interface (if needed)
void nostr_json_cleanup(void) {
    // Clean up if needed, Jansson doesn't require special cleanup
}

// Serialize NostrEvent to a JSON string
char *nostr_json_serialize(const NostrEvent *event) {
    if (!event) return NULL;

    // Create a new JSON object
    json_t *json_obj = json_object();
	char kind_str[12];
	snprintf(kind_str, sizeof(kind_str), "%d", event->kind);  // if 'kind' is int
	json_object_set_new(json_obj, "kind", json_string(kind_str));
    json_object_set_new(json_obj, "id", json_string(event->id));
    json_object_set_new(json_obj, "pubkey", json_string(event->pubkey));
    json_object_set_new(json_obj, "created_at", json_integer(event->created_at));
    json_object_set_new(json_obj, "content", json_string(event->content));

    // Convert JSON object to string
    char *json_str = json_dumps(json_obj, JSON_COMPACT);

    // Free the JSON object
    json_decref(json_obj);

    return json_str;
}

// Deserialize a JSON string to NostrEvent
NostrEvent *nostr_json_deserialize(const char *json_str) {
    if (!json_str) return NULL;

    // Parse the JSON string
    json_error_t error;
    json_t *json_obj = json_loads(json_str, 0, &error);
    if (!json_obj) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return NULL;
    }

    // Allocate memory for NostrEvent
    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event) {
        json_decref(json_obj);
        return NULL;
    }

    // Extract fields
    json_t *json_id = json_object_get(json_obj, "id");
    json_t *json_pubkey = json_object_get(json_obj, "pubkey");
    json_t *json_created_at = json_object_get(json_obj, "created_at");
    json_t *json_kind = json_object_get(json_obj, "kind");
    json_t *json_content = json_object_get(json_obj, "content");

    event->id = strdup(json_string_value(json_id));
    event->pubkey = strdup(json_string_value(json_pubkey));
    event->created_at = json_integer_value(json_created_at);
    event->kind = atoi(json_string_value(json_kind));
    event->content = strdup(json_string_value(json_content));

    // Free the JSON object
    json_decref(json_obj);

    return event;
}

// Implement the interface
NostrJsonInterface jansson_struct = {
    .init = nostr_json_init,
    .cleanup = nostr_json_cleanup,
    .serialize = nostr_json_serialize,
    .deserialize = nostr_json_deserialize
};

NostrJsonInterface *jansson_impl = &jansson_struct;
