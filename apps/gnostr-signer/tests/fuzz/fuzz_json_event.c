/* fuzz_json_event.c - Fuzz testing for Nostr event JSON parsing
 *
 * This fuzz target tests the JSON event parsing used in bunker_service
 * and related components against malformed input to find crashes,
 * memory bugs, and edge cases.
 *
 * Build with: clang -fsanitize=fuzzer,address fuzz_json_event.c -o fuzz_json_event ...
 *
 * Issue: nostrc-p7f6
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <json-glib/json-glib.h>

/* Nostr event structure for parsing validation */
typedef struct {
    char *id;           /* 32-byte hex event ID */
    char *pubkey;       /* 32-byte hex public key */
    int64_t created_at; /* Unix timestamp */
    int kind;           /* Event kind */
    char **tags;        /* Array of tag arrays (simplified) */
    size_t n_tags;
    char *content;      /* Event content */
    char *sig;          /* 64-byte hex signature */
} FuzzNostrEvent;

static void fuzz_event_free(FuzzNostrEvent *event) {
    if (!event) return;
    free(event->id);
    free(event->pubkey);
    if (event->tags) {
        for (size_t i = 0; i < event->n_tags; i++) {
            free(event->tags[i]);
        }
        free(event->tags);
    }
    free(event->content);
    free(event->sig);
    free(event);
}

/* Parse Nostr event JSON - mimics the parsing in bunker_service.c */
static FuzzNostrEvent *parse_nostr_event(const char *json_str) {
    if (!json_str || !*json_str) return NULL;

    g_autoptr(JsonParser) parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
        if (error) g_error_free(error);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);
    FuzzNostrEvent *event = calloc(1, sizeof(FuzzNostrEvent));
    if (!event) {
        return NULL;
    }

    /* Parse id */
    if (json_object_has_member(obj, "id")) {
        const char *id = json_object_get_string_member(obj, "id");
        if (id) event->id = strdup(id);
    }

    /* Parse pubkey */
    if (json_object_has_member(obj, "pubkey")) {
        const char *pubkey = json_object_get_string_member(obj, "pubkey");
        if (pubkey) event->pubkey = strdup(pubkey);
    }

    /* Parse created_at */
    if (json_object_has_member(obj, "created_at")) {
        event->created_at = json_object_get_int_member(obj, "created_at");
    }

    /* Parse kind */
    if (json_object_has_member(obj, "kind")) {
        event->kind = (int)json_object_get_int_member(obj, "kind");
    }

    /* Parse content */
    if (json_object_has_member(obj, "content")) {
        const char *content = json_object_get_string_member(obj, "content");
        if (content) event->content = strdup(content);
    }

    /* Parse sig */
    if (json_object_has_member(obj, "sig")) {
        const char *sig = json_object_get_string_member(obj, "sig");
        if (sig) event->sig = strdup(sig);
    }

    /* Parse tags array */
    if (json_object_has_member(obj, "tags")) {
        JsonNode *tags_node = json_object_get_member(obj, "tags");
        if (tags_node && JSON_NODE_HOLDS_ARRAY(tags_node)) {
            JsonArray *tags_array = json_node_get_array(tags_node);
            guint n_tags = json_array_get_length(tags_array);

            event->tags = calloc(n_tags, sizeof(char *));
            event->n_tags = n_tags;

            for (guint i = 0; i < n_tags && event->tags; i++) {
                JsonNode *tag_node = json_array_get_element(tags_array, i);
                if (tag_node && JSON_NODE_HOLDS_ARRAY(tag_node)) {
                    /* Convert tag array to string representation */
                    g_autoptr(JsonGenerator) gen = json_generator_new();
                    json_generator_set_root(gen, tag_node);
                    event->tags[i] = json_generator_to_data(gen, NULL);
                }
            }
        }
    }

    return event;
}

/* Extract kind from JSON - mimics bunker_service.c simple parsing */
static int extract_kind_simple(const char *json_str) {
    if (!json_str) return -1;

    const char *p = strstr(json_str, "\"kind\"");
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;

    p++;
    while (*p == ' ') p++;

    char *end = NULL;
    long kind = strtol(p, &end, 10);
    if (end == p) return -1;

    return (int)kind;
}

/* Extract content preview - mimics bunker_service.c */
static char *extract_content_preview(const char *json_str, size_t max_len) {
    if (!json_str) return NULL;

    const char *content_start = strstr(json_str, "\"content\"");
    if (!content_start) return NULL;

    content_start = strchr(content_start, ':');
    if (!content_start) return NULL;

    content_start++;
    while (*content_start == ' ' || *content_start == '"') content_start++;

    const char *content_end = content_start;
    while (*content_end && *content_end != '"') content_end++;

    size_t len = (size_t)(content_end - content_start);
    if (len > max_len) len = max_len;

    char *preview = malloc(len + 1);
    if (!preview) return NULL;

    memcpy(preview, content_start, len);
    preview[len] = '\0';
    return preview;
}

/* Define entry point based on whether we're using libFuzzer or standalone */
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

/* libFuzzer entry point */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;  /* Need at least a mode byte and some data */
    }

    /* Use first byte to select test mode */
    uint8_t mode = data[0] % 5;
    const uint8_t *input = data + 1;
    size_t input_size = size - 1;

    switch (mode) {
    case 0: {
        /* Test full event JSON parsing */
        if (input_size == 0) return 0;

        /* Create null-terminated string from fuzz input */
        char *json = malloc(input_size + 1);
        if (!json) return 0;
        memcpy(json, input, input_size);
        json[input_size] = '\0';

        FuzzNostrEvent *event = parse_nostr_event(json);
        if (event) {
            fuzz_event_free(event);
        }

        free(json);
        break;
    }

    case 1: {
        /* Test simple kind extraction */
        if (input_size == 0) return 0;

        char *json = malloc(input_size + 1);
        if (!json) return 0;
        memcpy(json, input, input_size);
        json[input_size] = '\0';

        int kind = extract_kind_simple(json);
        (void)kind;

        free(json);
        break;
    }

    case 2: {
        /* Test content preview extraction */
        if (input_size == 0) return 0;

        char *json = malloc(input_size + 1);
        if (!json) return 0;
        memcpy(json, input, input_size);
        json[input_size] = '\0';

        char *preview = extract_content_preview(json, 100);
        if (preview) {
            free(preview);
        }

        free(json);
        break;
    }

    case 3: {
        /* Test with JSON-like structure but malformed content */
        /* Build a semi-valid JSON object from fuzz input */
        char json_buf[4096];
        int offset = 0;

        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "{");

        /* Add fuzzed fields */
        if (input_size > 0) {
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                             "\"kind\":%d", (int)(input[0]));
        }
        if (input_size > 64) {
            char content[65];
            memcpy(content, input + 1, 64);
            content[64] = '\0';
            /* Escape any quotes in content */
            for (int i = 0; i < 64; i++) {
                if (content[i] == '"' || content[i] == '\\') {
                    content[i] = '_';
                }
                if (content[i] < 32) {
                    content[i] = ' ';
                }
            }
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                             ",\"content\":\"%s\"", content);
        }
        if (input_size > 65) {
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                             ",\"created_at\":%lld",
                             (long long)(input[65] * 1000000000LL));
        }

        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "}");

        FuzzNostrEvent *event = parse_nostr_event(json_buf);
        if (event) {
            fuzz_event_free(event);
        }
        break;
    }

    case 4: {
        /* Test deeply nested JSON structures */
        if (input_size < 2) return 0;

        int depth = (input[0] % 20) + 1;  /* 1-20 levels of nesting */
        int array_depth = (input[1] % 10) + 1;

        char *json = malloc(2048);
        if (!json) return 0;

        int offset = 0;
        offset += snprintf(json + offset, 2048 - offset, "{\"tags\":");

        /* Create nested arrays */
        for (int i = 0; i < depth && offset < 1900; i++) {
            offset += snprintf(json + offset, 2048 - offset, "[");
        }
        for (int i = 0; i < depth && offset < 1990; i++) {
            offset += snprintf(json + offset, 2048 - offset, "]");
            if (i < depth - 1) {
                offset += snprintf(json + offset, 2048 - offset, ",");
            }
        }

        offset += snprintf(json + offset, 2048 - offset, "}");

        FuzzNostrEvent *event = parse_nostr_event(json);
        if (event) {
            fuzz_event_free(event);
        }

        free(json);
        break;
    }
    }

    return 0;
}

#else /* Standalone test harness for AFL or manual testing */

#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input-file>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return 1;
    }

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return 1;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Call the fuzzer entry point */
    extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
    int result = LLVMFuzzerTestOneInput(data, (size_t)size);

    free(data);
    return result;
}

/* Define the fuzzer function even in standalone mode */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif /* FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */
