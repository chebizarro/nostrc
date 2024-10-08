#include "nostr.h"
#include <cjson/cJSON.h>

static void cjson_init(void) {
    // No initialization required for cJSON
}

static void cjson_cleanup(void) {
    // No cleanup required for cJSON
}

static char* cjson_serialize(const NostrEvent *event) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", event->id);
    cJSON_AddStringToObject(json, "pubkey", event->pubkey);
    cJSON_AddNumberToObject(json, "kind", event->kind);
    cJSON_AddStringToObject(json, "content", event->content);
    cJSON_AddStringToObject(json, "sig", event->sig);
    
    // Serialize tags
    cJSON *tags = cJSON_CreateArray();
    for (char **tag = event->tags; *tag != NULL; ++tag) {
        cJSON_AddItemToArray(tags, cJSON_CreateString(*tag));
    }
    cJSON_AddItemToObject(json, "tags", tags);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return json_str;
}

static NostrEvent* cjson_deserialize(const char *json_str) {
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        return NULL;
    }

    NostrEvent *event = nostr_event_new();
    event->id = strdup(cJSON_GetObjectItem(json, "id")->valuestring);
    event->pubkey = strdup(cJSON_GetObjectItem(json, "pubkey")->valuestring);
    event->kind = cJSON_GetObjectItem(json, "kind")->valueint;
    event->content = strdup(cJSON_GetObjectItem(json, "content")->valuestring);
    event->sig = strdup(cJSON_GetObjectItem(json, "sig")->valuestring);

    // Deserialize tags
    cJSON *tags = cJSON_GetObjectItem(json, "tags");
    size_t tag_count = cJSON_GetArraySize(tags);
    event->tags = (char **)malloc((tag_count + 1) * sizeof(char *));
    for (size_t i = 0; i < tag_count; ++i) {
        event->tags[i] = strdup(cJSON_GetArrayItem(tags, i)->valuestring);
    }
    event->tags[tag_count] = NULL;

    cJSON_Delete(json);
    return event;
}

static const NostrJsonInterface cjson_interface = {
    .init = cjson_init,
    .cleanup = cjson_cleanup,
    .serialize_event = cjson_serialize,
    .deserialize_event = cjson_deserialize
};
