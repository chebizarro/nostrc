#include "nson.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static void parse_descriptors(const char* data, int* size, uint8_t** values) {
    char nson_size_str[3] = {0};
    strncpy(nson_size_str, data + NSON_STRING_START, 2);
    *size = (int)strtol(nson_size_str, NULL, 16) * 2;
    *values = malloc(*size);
    for (int i = 0; i < *size / 2; i++) {
        sscanf(data + NSON_VALUES_START + i * 2, "%2hhx", &((*values)[i]));
    }
}

int nson_unmarshal(const char* data, nson_Event* event) {
    if (strncmp(data + NSON_MARKER_START, ",\"nson\":", 9) != 0) {
        return -1;
    }

    int nson_size;
    uint8_t* nson_descriptors;
    parse_descriptors(data, &nson_size, &nson_descriptors);

    event->id = strndup(data + ID_START, ID_END - ID_START);
    event->pubkey = strndup(data + PUBKEY_START, PUBKEY_END - PUBKEY_START);
    event->sig = strndup(data + SIG_START, SIG_END - SIG_START);
    event->created_at = strtol(data + CREATED_AT_START, NULL, 10);

    int kind_chars = nson_descriptors[0];
    int kind_start = NSON_VALUES_START + nson_size + 9;
    char kind_str[kind_chars + 1];
    strncpy(kind_str, data + kind_start, kind_chars);
    kind_str[kind_chars] = '\0';
    event->kind = atoi(kind_str);

    int content_chars = (nson_descriptors[1] << 8) + nson_descriptors[2];
    int content_start = kind_start + kind_chars + 12;
    char* content_str = strndup(data + content_start, content_chars);
    event->content = content_str;

    int n_tags = nson_descriptors[3];
    event->tags_count = n_tags;
    event->tags = malloc(n_tags * sizeof(Tag));
    int tags_start = content_start + content_chars + 9;

    int nson_index = 3;
    int tags_index = tags_start;
    for (int t = 0; t < n_tags; t++) {
        nson_index++;
        tags_index += 1;
        int n_items = nson_descriptors[nson_index];
        event->tags[t].count = n_items;
        event->tags[t].elements = malloc(n_items * sizeof(char*));
        for (int n = 0; n < n_items; n++) {
            nson_index++;
            int item_start = tags_index + 2;
            int item_chars = (nson_descriptors[nson_index] << 8) + nson_descriptors[nson_index + 1];
            nson_index++;
            event->tags[t].elements[n] = strndup(data + item_start, item_chars);
            tags_index = item_start + item_chars + 1;
        }
        tags_index += 1;
    }

    free(nson_descriptors);
    return 0;
}

char* nson_marshal(const nson_Event* event) {
    uint8_t nson_buf[256];
    int nson_index = 3;
    nson_buf[3] = event->tags_count;

    char tags_str[1000] = "[";
    for (int t = 0; t < event->tags_count; t++) {
        int n_items = event->tags[t].count;
        nson_index++;
        nson_buf[nson_index] = n_items;

        strcat(tags_str, "[");
        for (int i = 0; i < n_items; i++) {
            char* v = event->tags[t].elements[i];
            nson_index++;
            uint16_t len = strlen(v) - 2;
            nson_buf[nson_index] = len >> 8;
            nson_buf[nson_index + 1] = len & 0xFF;
            nson_index++;
            strcat(tags_str, v);
            if (n_items > i + 1) {
                strcat(tags_str, ",");
            }
        }
        strcat(tags_str, "]");
        if (event->tags_count > t + 1) {
            strcat(tags_str, ",");
        }
    }
    strcat(tags_str, "]}");

    int kind_chars = snprintf(NULL, 0, "%d", event->kind);
    nson_buf[0] = kind_chars;

    char content_str[256];
    snprintf(content_str, sizeof(content_str), "\"%s\"", event->content);
    int content_chars = strlen(content_str) - 2;
    nson_buf[1] = content_chars >> 8;
    nson_buf[2] = content_chars & 0xFF;

    int nson_size_bytes = nson_index + 1;
    char* nson_str = malloc(2 * nson_size_bytes + 1);
    for (int i = 0; i < nson_size_bytes; i++) {
        sprintf(nson_str + i * 2, "%02x", nson_buf[i]);
    }

    char* base = malloc(NSON_VALUES_START + 2 * nson_size_bytes + 9 + kind_chars + 12 + content_chars + 9 + strlen(tags_str) + 2);
    sprintf(base, "{\"id\":\"%s\",\"pubkey\":\"%s\",\"sig\":\"%s\",\"created_at\":%ld,\"nson\":\"%02x%s\",\"kind\":%d,\"content\":%s,\"tags\":%s", 
        event->id, event->pubkey, event->sig, event->created_at, nson_size_bytes, nson_str, event->kind, content_str, tags_str);

    free(nson_str);
    return base;
}

void nson_event_free(nson_Event* event) {
    free(event->id);
    free(event->pubkey);
    free(event->sig);
    free(event->content);

    for (int i = 0; i < event->tags_count; i++) {
        for (int j = 0; j < event->tags[i].count; j++) {
            free(event->tags[i].elements[j]);
        }
        free(event->tags[i].elements);
    }
    free(event->tags);
}
