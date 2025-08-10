#include "json.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <jansson.h>

NostrJsonInterface *json_interface = NULL;

void nostr_set_json_interface(NostrJsonInterface *interface) {
    json_interface = interface;
}

void nostr_json_init(void) {
    if (json_interface && json_interface->init) {
        json_interface->init();
    }
}

void nostr_json_cleanup(void) {
    if (json_interface && json_interface->cleanup) {
        json_interface->cleanup();
    }
}

char *nostr_event_serialize(const NostrEvent *event) {
    if (json_interface && json_interface->serialize_event) {
        return json_interface->serialize_event(event);
    }
    return NULL;
}

int nostr_event_deserialize(NostrEvent *event, const char *json_str) {
    if (json_interface && json_interface->deserialize_event) {
        return json_interface->deserialize_event(event, json_str);
    }
    return -1;
}

char *nostr_envelope_serialize(const NostrEnvelope *envelope) {
    if (json_interface && json_interface->serialize_envelope) {
        return json_interface->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json) {
    if (json_interface && json_interface->deserialize_envelope) {
        return json_interface->deserialize_envelope(envelope, json);
    }
    return -1;
}

char *nostr_filter_serialize(const NostrFilter *filter) {
    if (json_interface && json_interface->serialize_filter) {
        return json_interface->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(NostrFilter *filter, const char *json) {
    if (json_interface && json_interface->deserialize_filter) {
        return json_interface->deserialize_filter(filter, json);
    }
    return -1;
}

int nostr_json_get_string_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             char **out_str) {
    if (!json || !object_key || !entry_key || !out_str) return -1;
    *out_str = NULL;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *val = json_object_get(obj, entry_key);
    if (val && json_is_string(val)) {
        const char *s = json_string_value(val);
        if (s) *out_str = strdup(s);
    }
    json_decref(root);
    return *out_str ? 0 : -1;
}

int nostr_json_get_string_array_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   char ***out_array,
                                   size_t *out_count) {
    if (out_array) *out_array = NULL; if (out_count) *out_count = 0;
    if (!json || !object_key || !entry_key || !out_array) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *arr = json_object_get(obj, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    char **vec = NULL;
    if (n > 0) {
        vec = (char **)calloc(n, sizeof(char*));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            if (it && json_is_string(it)) {
                const char *s = json_string_value(it);
                vec[i] = s ? strdup(s) : NULL;
            }
        }
    }
    json_decref(root);
    *out_array = vec; if (out_count) *out_count = n;
    return 0;
}

int nostr_json_get_array_length_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!json || !object_key || !entry_key || !out_len) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *arr = json_object_get(obj, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    *out_len = json_array_size(arr);
    json_decref(root);
    return 0;
}

static json_t *load_and_get_object_in_array(const char *json,
                                            const char *object_key,
                                            const char *entry_key,
                                            size_t index,
                                            json_t **out_root) {
    *out_root = NULL;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return NULL;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return NULL; }
    json_t *arr = json_object_get(obj, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return NULL; }
    json_t *it = json_array_get(arr, index);
    if (!it || !json_is_object(it)) { json_decref(root); return NULL; }
    *out_root = root;
    return it;
}

int nostr_json_get_int_in_object_array_at(const char *json,
                                          const char *object_key,
                                          const char *entry_key,
                                          size_t index,
                                          const char *field_key,
                                          int *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !object_key || !entry_key || !field_key || !out_val) return -1;
    json_t *root = NULL;
    json_t *it = load_and_get_object_in_array(json, object_key, entry_key, index, &root);
    if (!it) return -1;
    json_t *v = json_object_get(it, field_key);
    int rc = -1;
    if (json_is_integer(v)) { *out_val = (int)json_integer_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_string_in_object_array_at(const char *json,
                                             const char *object_key,
                                             const char *entry_key,
                                             size_t index,
                                             const char *field_key,
                                             char **out_str) {
    if (!out_str) return -1; *out_str = NULL;
    if (!json || !object_key || !entry_key || !field_key) return -1;
    json_t *root = NULL;
    json_t *it = load_and_get_object_in_array(json, object_key, entry_key, index, &root);
    if (!it) return -1;
    json_t *v = json_object_get(it, field_key);
    if (json_is_string(v)) {
        const char *s = json_string_value(v);
        if (s) *out_str = strdup(s);
    }
    json_decref(root);
    return *out_str ? 0 : -1;
}

int nostr_json_get_int_array_in_object_array_at(const char *json,
                                                const char *object_key,
                                                const char *entry_key,
                                                size_t index,
                                                const char *field_key,
                                                int **out_items,
                                                size_t *out_count) {
    if (out_items) *out_items = NULL; if (out_count) *out_count = 0;
    if (!json || !object_key || !entry_key || !field_key || !out_items) return -1;
    json_t *root = NULL;
    json_t *it = load_and_get_object_in_array(json, object_key, entry_key, index, &root);
    if (!it) return -1;
    json_t *arr = json_object_get(it, field_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    int *vec = NULL;
    if (n > 0) {
        vec = (int *)calloc(n, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *v = json_array_get(arr, i);
            vec[i] = json_is_integer(v) ? (int)json_integer_value(v) : 0;
        }
    }
    json_decref(root);
    *out_items = vec; if (out_count) *out_count = n;
    return 0;
}

int nostr_json_get_string(const char *json,
                          const char *entry_key,
                          char **out_str) {
    if (!json || !entry_key || !out_str) return -1;
    *out_str = NULL;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *val = json_object_get(root, entry_key);
    if (val && json_is_string(val)) {
        const char *s = json_string_value(val);
        if (s) *out_str = strdup(s);
    }
    json_decref(root);
    return *out_str ? 0 : -1;
}

int nostr_json_get_string_array(const char *json,
                                const char *entry_key,
                                char ***out_array,
                                size_t *out_count) {
    if (out_array) *out_array = NULL; if (out_count) *out_count = 0;
    if (!json || !entry_key || !out_array) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    char **vec = NULL;
    if (n > 0) {
        vec = (char **)calloc(n, sizeof(char*));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            if (it && json_is_string(it)) {
                const char *s = json_string_value(it);
                vec[i] = s ? strdup(s) : NULL;
            }
        }
    }
    json_decref(root);
    *out_array = vec; if (out_count) *out_count = n;
    return 0;
}

int nostr_json_get_int(const char *json,
                       const char *entry_key,
                       int *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = json_object_get(root, entry_key);
    int rc = -1;
    if (json_is_integer(v)) { *out_val = (int)json_integer_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_bool(const char *json,
                        const char *entry_key,
                        bool *out_val) {
    if (out_val) *out_val = false;
    if (!json || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = json_object_get(root, entry_key);
    int rc = -1;
    if (json_is_boolean(v)) { *out_val = json_is_true(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_int_array(const char *json,
                             const char *entry_key,
                             int **out_items,
                             size_t *out_count) {
    if (out_items) *out_items = NULL; if (out_count) *out_count = 0;
    if (!json || !entry_key || !out_items) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    int *vec = NULL;
    if (n > 0) {
        vec = (int *)calloc(n, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            vec[i] = json_is_integer(it) ? (int)json_integer_value(it) : 0;
        }
    }
    json_decref(root);
    *out_items = vec; if (out_count) *out_count = n;
    return 0;
}

int nostr_json_get_int_at(const char *json,
                          const char *object_key,
                          const char *entry_key,
                          int *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !object_key || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *v = json_object_get(obj, entry_key);
    int rc = -1;
    if (json_is_integer(v)) { *out_val = (int)json_integer_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_bool_at(const char *json,
                           const char *object_key,
                           const char *entry_key,
                           bool *out_val) {
    if (out_val) *out_val = false;
    if (!json || !object_key || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *v = json_object_get(obj, entry_key);
    int rc = -1;
    if (json_is_boolean(v)) { *out_val = json_is_true(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_int_array_at(const char *json,
                                const char *object_key,
                                const char *entry_key,
                                int **out_items,
                                size_t *out_count) {
    if (out_items) *out_items = NULL; if (out_count) *out_count = 0;
    if (!json || !object_key || !entry_key || !out_items) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *arr = json_object_get(obj, entry_key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    size_t n = json_array_size(arr);
    int *vec = NULL;
    if (n > 0) {
        vec = (int *)calloc(n, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            vec[i] = json_is_integer(it) ? (int)json_integer_value(it) : 0;
        }
    }
    json_decref(root);
    *out_items = vec; if (out_count) *out_count = n;
    return 0;
}
