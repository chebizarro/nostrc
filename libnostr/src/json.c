#include "json.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <jansson.h>

NostrJsonInterface *json_interface = NULL;

/* --- Minimal default JSON backend (jansson) --- */
static void default_json_init(void) {}
static void default_json_cleanup(void) {}

static char *default_serialize_filter(const NostrFilter *filter) {
    if (!filter) return strdup("{}");
    json_t *obj = json_object();
    if (!obj) return NULL;

    /* ids */
    size_t n = nostr_filter_ids_len(filter);
    if (n > 0) {
        json_t *arr = json_array();
        for (size_t i = 0; i < n; ++i) {
            const char *s = nostr_filter_ids_get(filter, i);
            if (s) json_array_append_new(arr, json_string(s));
        }
        json_object_set_new(obj, "ids", arr);
    }

    /* kinds */
    n = nostr_filter_kinds_len(filter);
    if (n > 0) {
        json_t *arr = json_array();
        for (size_t i = 0; i < n; ++i) {
            int k = nostr_filter_kinds_get(filter, i);
            json_array_append_new(arr, json_integer(k));
        }
        json_object_set_new(obj, "kinds", arr);
    }

    /* authors */
    n = nostr_filter_authors_len(filter);
    if (n > 0) {
        json_t *arr = json_array();
        for (size_t i = 0; i < n; ++i) {
            const char *s = nostr_filter_authors_get(filter, i);
            if (s) json_array_append_new(arr, json_string(s));
        }
        json_object_set_new(obj, "authors", arr);
    }

    /* since / until */
    int64_t since = nostr_filter_get_since_i64(filter);
    if (since > 0) json_object_set_new(obj, "since", json_integer(since));
    int64_t until = nostr_filter_get_until_i64(filter);
    if (until > 0) json_object_set_new(obj, "until", json_integer(until));

    /* limit */
    int lim = nostr_filter_get_limit(filter);
    if (lim > 0) json_object_set_new(obj, "limit", json_integer(lim));

    /* search */
    const char *search = nostr_filter_get_search(filter);
    if (search && *search) json_object_set_new(obj, "search", json_string(search));

    char *out = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    return out;
}

static int default_deserialize_event(NostrEvent *event, const char *json_str) {
    if (!event || !json_str) return 0;
    json_error_t err; json_t *root = json_loads(json_str, 0, &err);
    if (!root) return 0;
    json_t *val = NULL;

    /* id */
    val = json_object_get(root, "id");
    if (val && json_is_string(val)) event->id = strdup(json_string_value(val));

    /* pubkey */
    val = json_object_get(root, "pubkey");
    if (val && json_is_string(val)) event->pubkey = strdup(json_string_value(val));

    /* created_at */
    val = json_object_get(root, "created_at");
    if (val && json_is_integer(val)) event->created_at = (int64_t)json_integer_value(val);

    /* kind */
    val = json_object_get(root, "kind");
    if (val && json_is_integer(val)) event->kind = (int)json_integer_value(val);

    /* content */
    val = json_object_get(root, "content");
    if (val && json_is_string(val)) event->content = strdup(json_string_value(val));

    /* sig */
    val = json_object_get(root, "sig");
    if (val && json_is_string(val)) event->sig = strdup(json_string_value(val));

    /* tags: array of arrays of strings */
    val = json_object_get(root, "tags");
    if (val && json_is_array(val)) {
        size_t tn = json_array_size(val);
        NostrTags *tags = nostr_tags_new(0);
        for (size_t i = 0; i < tn; ++i) {
            json_t *tag = json_array_get(val, i);
            if (!tag || !json_is_array(tag)) continue;
            const char *a = NULL, *b = NULL, *c = NULL;
            json_t *e0 = json_array_get(tag, 0);
            json_t *e1 = json_array_get(tag, 1);
            json_t *e2 = json_array_get(tag, 2);
            if (e0 && json_is_string(e0)) a = json_string_value(e0);
            if (e1 && json_is_string(e1)) b = json_string_value(e1);
            if (e2 && json_is_string(e2)) c = json_string_value(e2);
            NostrTag *nt = nostr_tag_new(a ? a : "", b, c, NULL);
            nostr_tags_append(tags, nt);
        }
        event->tags = tags;
    }

    json_decref(root);
    return 1;
}

static char *default_serialize_event(const NostrEvent *event) {
    if (!event) return NULL;
    json_t *obj = json_object();
    if (!obj) return NULL;

    /* id (optional) */
    if (event->id && *event->id)
        json_object_set_new(obj, "id", json_string(event->id));

    /* pubkey */
    if (event->pubkey && *event->pubkey)
        json_object_set_new(obj, "pubkey", json_string(event->pubkey));

    /* created_at */
    if (event->created_at > 0)
        json_object_set_new(obj, "created_at", json_integer(event->created_at));

    /* kind */
    json_object_set_new(obj, "kind", json_integer(event->kind));

    /* tags: array of arrays of strings */
    if (event->tags) {
        size_t tn = nostr_tags_size(event->tags);
        if (tn > 0) {
            json_t *tags = json_array();
            for (size_t i = 0; i < tn; ++i) {
                NostrTag *t = nostr_tags_get(event->tags, i);
                if (!t) continue;
                size_t m = nostr_tag_size(t);
                json_t *arr = json_array();
                for (size_t j = 0; j < m; ++j) {
                    const char *s = nostr_tag_get(t, j);
                    json_array_append_new(arr, s ? json_string(s) : json_string(""));
                }
                json_array_append_new(tags, arr);
            }
            json_object_set_new(obj, "tags", tags);
        }
    }

    /* content */
    if (event->content)
        json_object_set_new(obj, "content", json_string(event->content));

    /* sig (optional) */
    if (event->sig && *event->sig)
        json_object_set_new(obj, "sig", json_string(event->sig));

    char *out = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    return out;
}
static char *default_serialize_envelope(const NostrEnvelope *envelope) { (void)envelope; return NULL; }
static int default_deserialize_envelope(NostrEnvelope *envelope, const char *json) { (void)envelope; (void)json; return -1; }

static void ensure_default_json(void) {
    if (json_interface) return;
    static NostrJsonInterface default_iface;
    default_iface.init = default_json_init;
    default_iface.cleanup = default_json_cleanup;
    default_iface.serialize_event = default_serialize_event;
    default_iface.deserialize_event = default_deserialize_event;
    default_iface.serialize_envelope = default_serialize_envelope;
    default_iface.deserialize_envelope = default_deserialize_envelope;
    default_iface.serialize_filter = default_serialize_filter;
    default_iface.deserialize_filter = NULL;
    json_interface = &default_iface;
}

void nostr_set_json_interface(NostrJsonInterface *interface) {
    json_interface = interface;
}

void nostr_json_init(void) {
    ensure_default_json();
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
    ensure_default_json();
    if (json_interface && json_interface->serialize_event) {
        return json_interface->serialize_event(event);
    }
    return NULL;
}

int nostr_event_deserialize(NostrEvent *event, const char *json_str) {
    ensure_default_json();
    if (json_interface && json_interface->deserialize_event) {
        return json_interface->deserialize_event(event, json_str);
    }
    return -1;
}

char *nostr_envelope_serialize(const NostrEnvelope *envelope) {
    ensure_default_json();
    if (json_interface && json_interface->serialize_envelope) {
        return json_interface->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json) {
    ensure_default_json();
    if (json_interface && json_interface->deserialize_envelope) {
        return json_interface->deserialize_envelope(envelope, json);
    }
    return -1;
}

char *nostr_filter_serialize(const NostrFilter *filter) {
    ensure_default_json();
    if (json_interface && json_interface->serialize_filter) {
        return json_interface->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(NostrFilter *filter, const char *json) {
    ensure_default_json();
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
