#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* --- Default JSON backend (jansson) implementations --- */

void default_json_init(void) {}
void default_json_cleanup(void) {}

char *default_serialize_filter(const NostrFilter *filter) {
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

int default_deserialize_event(NostrEvent *event, const char *json_str) {
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
            size_t elem_count = json_array_size(tag);
            if (elem_count == 0) continue;
            /* First element is tag key */
            json_t *e0 = json_array_get(tag, 0);
            const char *key = (e0 && json_is_string(e0)) ? json_string_value(e0) : "";
            NostrTag *nt = nostr_tag_new(key, NULL);
            if (!nt) continue;
            /* Append all remaining elements (NIP-10 markers are at index 3) */
            for (size_t j = 1; j < elem_count; ++j) {
                json_t *ej = json_array_get(tag, j);
                const char *s = (ej && json_is_string(ej)) ? json_string_value(ej) : NULL;
                nostr_tag_append(nt, s ? s : "");
            }
            nostr_tags_append(tags, nt);
        }
        event->tags = tags;
    }

    json_decref(root);
    return 1;
}

char *default_serialize_event(const NostrEvent *event) {
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

char *default_serialize_envelope(const NostrEnvelope *envelope) { (void)envelope; return NULL; }
int default_deserialize_envelope(NostrEnvelope *envelope, const char *json) { (void)envelope; (void)json; return -1; }

/* ---- Backend implementations of facade generic helpers ---- */

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
            json_t *it = json_array_get(arr, i);
            if (!json_is_number(it)) { json_decref(root); free(vec); return -1; }
            if (json_is_integer(it)) {
                vec[i] = (int)json_integer_value(it);
            } else {
                vec[i] = (int)json_number_value(it);
            }
        }
    }
    json_decref(root);
    *out_items = vec; if (out_count) *out_count = n;
    return 0;
}

int nostr_json_get_string(const char *json,
                          const char *entry_key,
                          char **out_str) {
    if (!json || !entry_key || !out_str) return -1; *out_str = NULL;
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
    if (n == 0) {
        vec = (int *)calloc(1, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
    } else {
        vec = (int *)calloc(n, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            if (!json_is_number(it)) { json_decref(root); free(vec); return -1; }
            if (json_is_integer(it)) {
                vec[i] = (int)json_integer_value(it);
            } else {
                vec[i] = (int)json_number_value(it);
            }
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
    /* Always return a non-NULL buffer (caller must free), even when n == 0 */
    if (n == 0) {
        vec = (int *)calloc(1, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
    } else {
        vec = (int *)calloc(n, sizeof(int));
        if (!vec) { json_decref(root); return -1; }
        for (size_t i = 0; i < n; i++) {
            json_t *it = json_array_get(arr, i);
            /* Strict type check: every entry must be a number */
            if (!json_is_number(it)) {
                json_decref(root);
                free(vec);
                return -1;
            }
            if (json_is_integer(it)) {
                vec[i] = (int)json_integer_value(it);
            } else {
                /* Truncate real numbers to int */
                vec[i] = (int)json_number_value(it);
            }
        }
    }
    json_decref(root);
    *out_items = vec; if (out_count) *out_count = n;
    return 0;
}

/* =========================================================================
 * In-memory JSON object manipulation helpers (jansson backend)
 * ========================================================================= */

NostrJsonValue nostr_json_object_new(void) {
    return (NostrJsonValue)json_object();
}

void nostr_json_value_free(NostrJsonValue val) {
    if (val) json_decref((json_t *)val);
}

NostrJsonValue nostr_json_value_incref(NostrJsonValue val) {
    if (val) json_incref((json_t *)val);
    return val;
}

int nostr_json_object_set(NostrJsonValue obj, const char *key, NostrJsonValue val) {
    if (!obj || !key) return -1;
    /* json_object_set_new takes ownership of val */
    return json_object_set_new((json_t *)obj, key, (json_t *)val);
}

NostrJsonValue nostr_json_object_get(NostrJsonValue obj, const char *key) {
    if (!obj || !key) return NULL;
    /* Returns a borrowed reference */
    return (NostrJsonValue)json_object_get((json_t *)obj, key);
}

int nostr_json_object_del(NostrJsonValue obj, const char *key) {
    if (!obj || !key) return -1;
    return json_object_del((json_t *)obj, key);
}

bool nostr_json_value_is_string(NostrJsonValue val) {
    return val && json_is_string((json_t *)val);
}

bool nostr_json_value_is_number(NostrJsonValue val) {
    return val && json_is_number((json_t *)val);
}

bool nostr_json_value_is_integer(NostrJsonValue val) {
    return val && json_is_integer((json_t *)val);
}

bool nostr_json_value_is_boolean(NostrJsonValue val) {
    return val && json_is_boolean((json_t *)val);
}

const char *nostr_json_value_string(NostrJsonValue val) {
    if (!val || !json_is_string((json_t *)val)) return NULL;
    return json_string_value((json_t *)val);
}

double nostr_json_value_number(NostrJsonValue val) {
    if (!val) return 0.0;
    if (json_is_number((json_t *)val)) return json_number_value((json_t *)val);
    if (json_is_integer((json_t *)val)) return (double)json_integer_value((json_t *)val);
    return 0.0;
}

int64_t nostr_json_value_integer(NostrJsonValue val) {
    if (!val || !json_is_integer((json_t *)val)) return 0;
    return json_integer_value((json_t *)val);
}

bool nostr_json_value_boolean(NostrJsonValue val) {
    if (!val || !json_is_boolean((json_t *)val)) return false;
    return json_boolean_value((json_t *)val);
}
