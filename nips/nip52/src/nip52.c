#include "nip52.h"
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

CalendarEvent *parse_calendar_event(const char *event_json) {
    CalendarEvent *event = malloc(sizeof(CalendarEvent));
    memset(event, 0, sizeof(CalendarEvent));

    json_t *root;
    json_error_t error;
    root = json_loads(event_json, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return NULL;
    }

    json_t *kind = json_object_get(root, "kind");
    event->kind = (CalendarEventKind)json_integer_value(kind);

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
        } else if (strcmp(key, "image") == 0) {
            event->image = strdup(value);
        } else if (strcmp(key, "start") == 0 || strcmp(key, "end") == 0) {
            struct tm tm = {0};
            if (event->kind == TIME_BASED) {
                time_t timestamp = (time_t)atoll(value);
                if (strcmp(key, "start") == 0) {
                    event->start = timestamp;
                } else {
                    event->end = timestamp;
                }
            } else if (event->kind == DATE_BASED) {
                strptime(value, DATE_FORMAT, &tm);
                time_t date = mktime(&tm);
                if (strcmp(key, "start") == 0) {
                    event->start = date;
                } else {
                    event->end = date;
                }
            }
        } else if (strcmp(key, "location") == 0) {
            event->locations = realloc(event->locations, sizeof(char *) * (index + 1));
            event->locations[index] = strdup(value);
        } else if (strcmp(key, "g") == 0) {
            event->geohashes = realloc(event->geohashes, sizeof(char *) * (index + 1));
            event->geohashes[index] = strdup(value);
        } else if (strcmp(key, "p") == 0 && is_valid_hex(value)) {
            event->participants = realloc(event->participants, sizeof(Participant) * (index + 1));
            event->participants[index].pub_key = strdup(value);
            if (json_array_size(tag) > 2) {
                event->participants[index].relay = strdup(json_string_value(json_array_get(tag, 2)));
                if (json_array_size(tag) > 3) {
                    event->participants[index].role = strdup(json_string_value(json_array_get(tag, 3)));
                }
            }
        } else if (strcmp(key, "r") == 0) {
            event->references = realloc(event->references, sizeof(char *) * (index + 1));
            event->references[index] = strdup(value);
        } else if (strcmp(key, "t") == 0) {
            event->hashtags = realloc(event->hashtags, sizeof(char *) * (index + 1));
            event->hashtags[index] = strdup(value);
        } else if (strcmp(key, "start_tzid") == 0) {
            event->start_tzid = strdup(value);
        } else if (strcmp(key, "end_tzid") == 0) {
            event->end_tzid = strdup(value);
        }
    }

    json_decref(root);
    return event;
}

char *calendar_event_to_json(CalendarEvent *event) {
    json_t *root = json_object();
    json_t *tags = json_array();

    json_object_set_new(root, "kind", json_integer(event->kind));
    json_object_set_new(root, "tags", tags);

    json_array_append_new(tags, json_pack("[s, s]", "d", event->identifier));
    json_array_append_new(tags, json_pack("[s, s]", "title", event->title));
    if (event->image) {
        json_array_append_new(tags, json_pack("[s, s]", "image", event->image));
    }
    if (event->kind == TIME_BASED) {
        json_array_append_new(tags, json_pack("[s, s]", "start", time(NULL), event->start));
        if (event->end != 0) {
            json_array_append_new(tags, json_pack("[s, s]", "end", time(NULL), event->end));
        }
    } else if (event->kind == DATE_BASED) {
        char start_str[11];
        strftime(start_str, sizeof(start_str), DATE_FORMAT, localtime(&event->start));
        json_array_append_new(tags, json_pack("[s, s]", "start", start_str));
        if (event->end != 0) {
            char end_str[11];
            strftime(end_str, sizeof(end_str), DATE_FORMAT, localtime(&event->end));
            json_array_append_new(tags, json_pack("[s, s]", "end", end_str));
        }
    }

    for (int i = 0; event->locations && event->locations[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "location", event->locations[i]));
    }
    for (int i = 0; event->geohashes && event->geohashes[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "g", event->geohashes[i]));
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
    for (int i = 0; event->references && event->references[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "r", event->references[i]));
    }
    for (int i = 0; event->hashtags && event->hashtags[i]; i++) {
        json_array_append_new(tags, json_pack("[s, s]", "t", event->hashtags[i]));
    }

    char *json_str = json_dumps(root, 0);
    json_decref(root);
    return json_str;
}

void free_calendar_event(CalendarEvent *event) {
    if (event) {
        free(event->identifier);
        free(event->title);
        free(event->image);
        for (int i = 0; event->locations && event->locations[i]; i++) {
            free(event->locations[i]);
        }
        free(event->locations);
        for (int i = 0; event->geohashes && event->geohashes[i]; i++) {
            free(event->geohashes[i]);
        }
        free(event->geohashes);
        for (int i = 0; event->participants && event->participants[i].pub_key; i++) {
            free(event->participants[i].pub_key);
            free(event->participants[i].relay);
            free(event->participants[i].role);
        }
        free(event->participants);
        for (int i = 0; event->references && event->references[i]; i++) {
            free(event->references[i]);
        }
        free(event->references);
        for (int i = 0; event->hashtags && event->hashtags[i]; i++) {
            free(event->hashtags[i]);
        }
        free(event->hashtags);
        free(event->start_tzid);
        free(event->end_tzid);
        free(event);
    }
}
