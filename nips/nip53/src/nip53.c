#include "nip53.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <jansson.h>
#include <time.h>

static int is_valid_hex(const char *str) {
    while (*str) {
        if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'f') || (*str >= 'A' && *str <= 'F'))) {
            return 0;
        }
        str++;
    }
    return 1;
}

LiveEvent *parse_live_event(const char *event_json) {
    LiveEvent *event = malloc(sizeof(LiveEvent));
    memset(event, 0, sizeof(LiveEvent));

    json_t *root;
    json_error_t error;
    root = json_loads(event_json, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return NULL;
    }

    json_t *tags = json_object_get(root, "tags");
    size_t index;
    json_t *tag;
    json_array_foreach(tags, index, tag) {
        const char *key = json_string_value(json_array_get(tag, 0));
        const char *value = json_string_value(json_array_get(tag, 1));

        if (strcmp(key, "d") == 0) {
            event->identifier = strdup(value);
        } else if (strcmp(key, "title") == 0) {
            event->title = strdup(value);
        } else if (strcmp(key, "summary") == 0) {
            event->summary = strdup(value);
        } else if (strcmp(key, "image") == 0) {
            event->image = strdup(value);
        } else if (strcmp(key, "status") == 0) {
            event->status = strdup(value);
        } else if (strcmp(key, "start") == 0 || strcmp(key, "end") == 0) {
            time_t timestamp = (time_t)atoll(value);
            if (strcmp(key, "start") == 0) {
                event->starts = timestamp;
            } else {
                event->ends = timestamp;
            }
        } else if (strcmp(key, "streaming") == 0) {
            event->streaming = realloc(event->streaming, sizeof(char *) * (index + 1));
            event->streaming[index] = strdup(value);
        } else if (strcmp(key, "recording") == 0) {
            event->recording = realloc(event->recording, sizeof(char *) * (index + 1));
            event->recording[index] = strdup(value);
        } else if (strcmp(key, "p") == 0 && is_valid_hex(value)) {
            event->participants = realloc(event->participants, sizeof(Participant) * (index + 1));
            event->participants[index].pub_key = strdup(value);
            if (json_array_size(tag) > 2) {
                event->participants[index].relay = strdup(json_string_value(json_array_get(tag, 2)));
                if (json_array_size(tag) > 3) {
                    event->participants[index].role = strdup(json_string_value(json_array_get(tag, 3)));
                }
            }
        } else if (strcmp(key, "relay") == 0) {
            event->relays = realloc(event->relays, sizeof(char *) * (index + 1));
            event->relays[index] = strdup(value);
        } else if (strcmp(key, "t") == 0) {
            event->hashtags = realloc(event->hashtags, sizeof(char *) * (index + 1));
            event->hashtags[index] = strdup(value);
        } else if (strcmp(key, "current_participants") == 0) {
            event->current_participants = atoi(value);
        } else if (strcmp(key, "total_participants") == 0) {
            event->total_participants = atoi(value);
        }
    }

    json_decref(root);
    return event;
}

char *live_event_to_json(LiveEvent *event) {
    json_t *root = json_object();
    json_t *tags = json_array();

    json_object_set_new(root, "tags", tags);

    json_array_append_new(tags, json_pack("[s, s]", "d", event->identifier));
    json_array_append_new(tags, json_pack("[s, s]", "title", event->title));
    if (event->summary) {
        json_array_append_new(tags, json_pack("[s, s]", "summary", event->summary));
    }
    if (event->image) {
        json_array_append_new(tags, json_pack("[s, s]", "image", event->image));
    }
    if (event->status) {
        json_array_append_new(tags, json_pack("[s, s]", "status", event->status));
    }
    json_array_append_new(tags, json_pack("[s, s]", "start", time(NULL), event->starts));
    if (event->ends != 0) {
        json_array_append_new(tags, json_pack("[s, s]", "end", time(NULL), event->ends));
    }

    for (int i = 0; event->streaming && event->streaming[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "streaming", event->streaming[i]));
    }
    for (int i = 0; event->recording && event->recording[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "recording", event->recording[i]));
    }
    for (int i = 0; event->participants && event->participants[i].pub_key; i++) {
        json_t *p = json_pack("[s, s]", "p", event->participants[i].pub_key);
        if (event->participants[i].relay) {
            json_array_append_new(p, json_string(event->participants[i].relay));
        }
        if (event->participants[i].role) {
            json_array_append_new(p, json_string(event->participants[i].role));
        }
        json_array_append(tags, p);
    }
    for (int i = 0; event->hashtags && event->hashtags[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "t", event->hashtags[i]));
    }
    if (event->current_participants != 0) {
        json_array_append_new(tags, json_pack("[s, s]", "current_participants", event->current_participants));
    }
    if (event->total_participants != 0) {
        json_array_append_new(tags, json_pack("[s, s]", "total_participants", event->total_participants));
    }
    for (int i = 0; event->relays && event->relays[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "relay", event->relays[i]));
    }

    char *json_str = json_dumps(root, 0);
    json_decref(root);
    return json_str;
}

Participant *get_host(LiveEvent *event) {
    for (int i = 0; event->participants && event->participants[i].pub_key; i++) {
        if (event->participants[i].role && strcmp(event->participants[i].role, "host") == 0) {
            return &event->participants[i];
        }
    }
    return NULL;
}

void free_live_event(LiveEvent *event) {
    if (event) {
        free(event->identifier);
        free(event->title);
        free(event->summary);
        free(event->image);
        free(event->status);
        for (int i = 0; event->streaming && event->streaming[i]; i++) {
            free(event->streaming[i]);
        }
        free(event->streaming);
        for (int i = 0; event->recording && event->recording[i]; i++) {
            free(event->recording[i]);
        }
        free(event->recording);
        for (int i = 0; event->participants && event->participants[i].pub_key; i++) {
            free(event->participants[i].pub_key);
            free(event->participants[i].relay);
            free(event->participants[i].role);
        }
        free(event->participants);
        for (int i = 0; event->hashtags && event->hashtags[i]; i++) {
            free(event->hashtags[i]);
        }
        free(event->hashtags);
        for (int i = 0; event->relays && event->relays[i]; i++) {
            free(event->relays[i]);
        }
        free(event->relays);
        free(event);
    }
}
