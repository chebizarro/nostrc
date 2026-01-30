#include "go.h"
#include "nostr_jansson.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-event-extra.h"
#include "nostr-tag.h"
#include "json.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

char *jansson_event_serialize(const NostrEvent *event);
int jansson_event_deserialize(NostrEvent *event, const char *json_str);
int _deserialize_event(NostrEvent *event, json_t *json_obj);
char *jansson_envelope_serialize(const NostrEnvelope *envelope);
int jansson_envelope_deserialize(NostrEnvelope *envelope, const char *json_str);

int jansson_filter_deserialize(NostrFilter *filter, json_t *json_obj);
json_t *jansson_filter_serialize(const NostrFilter *filter);
json_t *jansson_tag_serialize(const NostrTag *tag);
json_t *jansson_tags_serialize(const NostrTags *tags);
int jansson_tag_deserialize(NostrTag *tag, json_t *json);
NostrTags *jansson_tags_deserialize(json_t *json_array);
json_t *string_array_serialize(const StringArray *array);
json_t *int_array_serialize(const IntArray *array);
int string_array_deserialize(StringArray *array, json_t *json_array);
int int_array_deserialize(IntArray *array, json_t *json_array);

// Initializes the JSON interface (if needed)
void jansson_init(void) {
    // Initialize if necessary, Jansson doesn't require special init
}

int nostr_json_get_raw(const char *json,
                       const char *entry_key,
                       char **out_raw) {
    if (out_raw) *out_raw = NULL;
    if (!json || !entry_key || !out_raw) return -1;
    json_error_t err; memset(&err, 0, sizeof(err));
    json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    if (!json_is_object(root)) { json_decref(root); return -1; }
    json_t *v = json_object_get(root, entry_key);
    if (!v) { json_decref(root); return -1; }
    char *dump = json_dumps(v, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!dump) { json_decref(root); return -1; }
    *out_raw = strdup(dump);
    free(dump);
    json_decref(root);
    return *out_raw ? 0 : -1;
}

// Cleans up the JSON interface (if needed)
void jansson_cleanup(void) {
    // Clean up if needed, Jansson doesn't require special cleanup
}

// Serialize NostrEvent to a JSON string
char *jansson_event_serialize(const NostrEvent *event) {
    if (!event)
        return NULL;

    // Create a new JSON object
    json_t *json_obj = json_object();
    // NIP-01 uses numeric kind
    json_object_set_new(json_obj, "kind", json_integer(event->kind));
    if (event->id)
        json_object_set_new(json_obj, "id", json_string(event->id));
    if (event->pubkey)
        json_object_set_new(json_obj, "pubkey", json_string(event->pubkey));
    json_object_set_new(json_obj, "created_at", json_integer((json_int_t)event->created_at));
    if (event->content)
        json_object_set_new(json_obj, "content", json_string(event->content));
    if (event->sig)
        json_object_set_new(json_obj, "sig", json_string(event->sig));

    // tags (array of arrays) if present
    if (event->tags && event->tags->count > 0) {
        json_t *tags_json = jansson_tags_serialize(event->tags);
        if (tags_json) json_object_set_new(json_obj, "tags", tags_json);
    }

    // Convert JSON object to string
    char *json_str = json_dumps(json_obj, JSON_COMPACT);

    // Free the JSON object
    json_decref(json_obj);

    return json_str;
}

int jansson_event_deserialize(NostrEvent *event, const char *json_str) {
    // Parse the JSON string
    json_error_t error;
    json_t *json_obj = json_loads(json_str, 0, &error);
    if (!json_obj) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return -1;
    }
    int err = _deserialize_event(event, json_obj);
    // Free the JSON object
    json_decref(json_obj);
    return err;
}

int _deserialize_event(NostrEvent *event, json_t *json_obj) {
    // Extract fields with validation
    json_t *json_id = json_object_get(json_obj, "id");
    json_t *json_pubkey = json_object_get(json_obj, "pubkey");
    json_t *json_created_at = json_object_get(json_obj, "created_at");
    json_t *json_kind = json_object_get(json_obj, "kind");
    json_t *json_content = json_object_get(json_obj, "content");
    json_t *json_sig = json_object_get(json_obj, "sig");
    json_t *json_tags = json_object_get(json_obj, "tags");

    // id
    if (json_is_string(json_id)) {
        event->id = strdup(json_string_value(json_id));
    } else {
        event->id = NULL;
    }

    // pubkey
    if (json_is_string(json_pubkey)) {
        event->pubkey = strdup(json_string_value(json_pubkey));
    } else {
        event->pubkey = NULL;
    }

    // created_at (integer)
    if (json_is_integer(json_created_at)) {
        event->created_at = (int64_t)json_integer_value(json_created_at);
    } else if (json_is_string(json_created_at)) {
        const char *s = json_string_value(json_created_at);
        event->created_at = s ? (int64_t)atoll(s) : 0;
    } else {
        event->created_at = 0;
    }

    // kind (prefer integer, fallback string)
    if (json_is_integer(json_kind)) {
        event->kind = (int)json_integer_value(json_kind);
    } else if (json_is_string(json_kind)) {
        const char *ks = json_string_value(json_kind);
        event->kind = ks ? atoi(ks) : 0;
    } else {
        event->kind = 0;
    }

    // content
    if (json_is_string(json_content)) {
        event->content = strdup(json_string_value(json_content));
    } else {
        event->content = NULL;
    }

    // sig
    if (json_is_string(json_sig)) {
        event->sig = strdup(json_string_value(json_sig));
    } else {
        event->sig = NULL;
    }

    // tags
    if (json_is_array(json_tags)) {
        NostrTags *t = jansson_tags_deserialize(json_tags);
        if (t) {
            // free existing default empty tags if present
            if (event->tags) nostr_tags_free(event->tags);
            event->tags = t;
        }
    }

    return 0;
}

char *jansson_envelope_serialize(const NostrEnvelope *envelope) {
    if (!envelope) return NULL;
    json_t *arr = json_array();
    if (!arr) return NULL;
    switch (envelope->type) {
    case NOSTR_ENVELOPE_EVENT: {
        NostrEventEnvelope *env = (NostrEventEnvelope *)envelope;
        json_array_append_new(arr, json_string("EVENT"));
        if (env->subscription_id) json_array_append_new(arr, json_string(env->subscription_id));
        if (env->event) {
            // Reuse event serializer to get json_t
            char *evs = jansson_event_serialize(env->event);
            if (evs) {
                json_error_t e; json_t *ev = json_loads(evs, 0, &e);
                free(evs);
                if (ev) { json_array_append_new(arr, ev); }
            }
        }
        break;
    }
    case NOSTR_ENVELOPE_NOTICE: {
        NostrNoticeEnvelope *env = (NostrNoticeEnvelope *)envelope;
        json_array_append_new(arr, json_string("NOTICE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case NOSTR_ENVELOPE_EOSE: {
        NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)envelope;
        json_array_append_new(arr, json_string("EOSE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case NOSTR_ENVELOPE_CLOSE: {
        NostrCloseEnvelope *env = (NostrCloseEnvelope *)envelope;
        json_array_append_new(arr, json_string("CLOSE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case NOSTR_ENVELOPE_CLOSED: {
        NostrClosedEnvelope *env = (NostrClosedEnvelope *)envelope;
        json_array_append_new(arr, json_string("CLOSED"));
        json_array_append_new(arr, json_string(env->subscription_id ? env->subscription_id : ""));
        json_array_append_new(arr, json_string(env->reason ? env->reason : ""));
        break;
    }
    case NOSTR_ENVELOPE_OK: {
        NostrOKEnvelope *env = (NostrOKEnvelope *)envelope;
        json_array_append_new(arr, json_string("OK"));
        json_array_append_new(arr, json_string(env->event_id ? env->event_id : ""));
        json_array_append_new(arr, env->ok ? json_true() : json_false());
        if (env->reason) json_array_append_new(arr, json_string(env->reason));
        break;
    }
    case NOSTR_ENVELOPE_AUTH: {
        NostrAuthEnvelope *env = (NostrAuthEnvelope *)envelope;
        json_array_append_new(arr, json_string("AUTH"));
        if (env->event) {
            char *evs = jansson_event_serialize(env->event);
            if (evs) {
                json_error_t e; json_t *ev = json_loads(evs, 0, &e);
                free(evs);
                if (ev) { json_array_append_new(arr, ev); }
            }
        } else if (env->challenge) {
            json_array_append_new(arr, json_string(env->challenge));
        }
        break;
    }
    default:
        json_decref(arr);
        return NULL;
    }
    char *s = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    return s;
}

// Deserialize a JSON string to NostrEvent
int jansson_envelope_deserialize(NostrEnvelope *envelope, const char *json_str) {
    if (!json_str)
        return -1;

    char *first_comma = strchr(json_str, ',');
    if (!first_comma)
        return -1;

    char label[16];
    strncpy(label, json_str, first_comma - json_str);
    label[first_comma - json_str] = '\0';

    // Parse the JSON string
    json_error_t error;
    json_t *json_obj = json_loads(json_str, 0, &error);
    if (!json_obj) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return -1;
    }
    if (!json_is_array(json_obj)) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Error: root is not an array\n");
        json_decref(json_obj);
        return -1;
    }

    // Process according to envelope type
    switch (envelope->type) {
    case NOSTR_ENVELOPE_EVENT: {
        NostrEventEnvelope *env = (NostrEventEnvelope *)envelope;
        size_t n = json_array_size(json_obj);
        if (n < 2) { json_decref(json_obj); return -1; }
        json_t *json_evt = json_array_get(json_obj, n-1);
        if (!json_is_object(json_evt)) { json_decref(json_obj); return -1; }
        if (n == 3) {
            json_t *json_id = json_array_get(json_obj, 1);
            if (!json_is_string(json_id)) { json_decref(json_obj); return -1; }
            env->subscription_id = strdup(json_string_value(json_id));
        }
        env->event = nostr_event_new();
        if (!env->event) { json_decref(json_obj); return -1; }
        (void)_deserialize_event(env->event, json_evt);
        break;
    }
    case NOSTR_ENVELOPE_REQ: {
        if (json_array_size(json_obj) < 3) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode REQ envelope: missing filters\n");
            break;
        }
        NostrReqEnvelope *env = (NostrReqEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        if (!json_is_string(json_id)) { break; }
        env->subscription_id = strdup(json_string_value(json_id));
        NostrFilters *fs = nostr_filters_new();
        if (!fs) { break; }
        for (size_t i = 2; i < json_array_size(json_obj); i++) {
            json_t *json_filter = json_array_get(json_obj, i);
            NostrFilter f = {0};
            if (jansson_filter_deserialize(&f, json_filter) != 0) {
                /* ensure no leaks on error */
                nostr_filter_clear(&f);
                continue;
            }
            nostr_filters_add(fs, &f);
        }
        env->filters = fs;
        break;
    }
    case NOSTR_ENVELOPE_COUNT: {
        if (json_array_size(json_obj) < 4) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode COUNT envelope: missing filters\n");
            break;
        }
        NostrCountEnvelope *env = (NostrCountEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        json_t *json_count = json_array_get(json_obj, 2);
        if (!json_is_string(json_id) || !json_is_object(json_count)) { break; }
        env->subscription_id = strdup(json_string_value(json_id));
        // Default to 0 if missing or non-integer
        env->count = 0;
        json_t *count_value = json_object_get(json_count, "count");
        if (json_is_integer(count_value)) {
            env->count = (int)json_integer_value(count_value);
        }
        NostrFilters *fs = nostr_filters_new();
        if (!fs) { break; }
        for (size_t i = 3; i < json_array_size(json_obj); i++) {
            json_t *json_filter = json_array_get(json_obj, i);
            NostrFilter f = {0};
            if (jansson_filter_deserialize(&f, json_filter) != 0) {
                /* ensure no leaks on error */
                nostr_filter_clear(&f);
                continue;
            }
            nostr_filters_add(fs, &f);
        }
        env->filters = fs;
        break;
    }
    case NOSTR_ENVELOPE_NOTICE: {
        if (json_array_size(json_obj) < 2) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode NOTICE envelope\n");
            break;
        }
        NostrNoticeEnvelope *env = (NostrNoticeEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case NOSTR_ENVELOPE_EOSE: {
        if (json_array_size(json_obj) < 2) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode EOSE envelope\n");
            break;
        }
        NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case NOSTR_ENVELOPE_CLOSE: {
        if (json_array_size(json_obj) < 2) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode CLOSE envelope\n");
            break;
        }
        NostrCloseEnvelope *env = (NostrCloseEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case NOSTR_ENVELOPE_CLOSED: {
        if (json_array_size(json_obj) < 3) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode CLOSED envelope\n");
            break;
        }
        NostrClosedEnvelope *env = (NostrClosedEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        json_t *json_message = json_array_get(json_obj, 2);
        if (!json_is_string(json_id) || !json_is_string(json_message)) { break; }
        env->subscription_id = strdup(json_string_value(json_id));
        env->reason = strdup(json_string_value(json_message));
        break;
    }
    case NOSTR_ENVELOPE_OK: {
        if (json_array_size(json_obj) < 3) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode OK envelope\n");
            break;
        }
        NostrOKEnvelope *env = (NostrOKEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        json_t *json_ok = json_array_get(json_obj, 2);
        if (!json_is_string(json_id) || !(json_is_true(json_ok) || json_is_false(json_ok))) { break; }
        env->event_id = strdup(json_string_value(json_id));
        env->ok = json_is_true(json_ok);
        if (json_array_size(json_obj) >= 4) {
            json_t *json_reason = json_array_get(json_obj, 3);
            if (json_is_string(json_reason)) env->reason = strdup(json_string_value(json_reason));
        }
        break;
    }
    case NOSTR_ENVELOPE_AUTH: {
        if (json_array_size(json_obj) < 2) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "Failed to decode AUTH envelope\n");
            break;
        }
        NostrAuthEnvelope *env = (NostrAuthEnvelope *)envelope;
        json_t *json_challenge = json_array_get(json_obj, 1);
        if (json_is_object(json_challenge)) {
            env->event = nostr_event_new();
            if (env->event) _deserialize_event(env->event, json_challenge);
        } else if (json_is_string(json_challenge)) {
            env->challenge = strdup(json_string_value(json_challenge));
        } else {
            // Invalid; neither object nor string
        }
        break;
    }
    default:
        break;
    }

    json_decref(json_obj);
    return 0;
}

int jansson_filter_deserialize(NostrFilter *filter, json_t *json_obj) {
    if (!filter || !json_is_object(json_obj)) {
        return -1;
    }

    // Ensure arrays/tags are initialized if caller passed a zeroed struct
    if (filter->ids.data == NULL && filter->ids.capacity == 0) {
        string_array_init(&filter->ids);
    }
    if (filter->kinds.data == NULL && filter->kinds.capacity == 0) {
        int_array_init(&filter->kinds);
    }
    if (filter->authors.data == NULL && filter->authors.capacity == 0) {
        string_array_init(&filter->authors);
    }
    if (filter->tags == NULL) {
        filter->tags = nostr_tags_new(0);
    }

    // Deserialize the `ids`
    json_t *ids_json = json_object_get(json_obj, "ids");
    if (ids_json && string_array_deserialize(&filter->ids, ids_json) != 0) {
        return -1;
    }

    // Deserialize the `kinds`
    json_t *kinds_json = json_object_get(json_obj, "kinds");
    if (kinds_json && int_array_deserialize(&filter->kinds, kinds_json) != 0) {
        return -1;
    }

    // Deserialize the `authors`
    json_t *authors_json = json_object_get(json_obj, "authors");
    if (authors_json && string_array_deserialize(&filter->authors, authors_json) != 0) {
        return -1;
    }

    // Deserialize the `tags` (optional explicit array-of-arrays)
    json_t *tags_json = json_object_get(json_obj, "tags");
    if (tags_json) {
        NostrTags *t = jansson_tags_deserialize(tags_json);
        if (t) {
            if (filter->tags) nostr_tags_free(filter->tags);
            filter->tags = t;
        }
    }

    // Also support NIP-01 dynamic tag filter keys: "#e": [..], "#p": [..], etc.
    // For each key that starts with '#' and has a one-character name, create tag entries [name, value].
    // If any array element is non-string, treat input as invalid and fail.
    const char *key;
    json_t *val;
    json_object_foreach(json_obj, key, val) {
        if (key && key[0] == '#' && key[1] && !key[2] && json_is_array(val)) {
            char tag_name[2] = { key[1], '\0' };
            size_t i, n = json_array_size(val);
            for (i = 0; i < n; i++) {
                json_t *elt = json_array_get(val, i);
                if (!json_is_string(elt)) {
                    return -1; // invalid type in #tag array
                }
                const char *v = json_string_value(elt);
                NostrTag *tag = new_string_array(0);
                if (!tag) continue;
                nostr_tag_add(tag, tag_name);
                nostr_tag_add(tag, v);
                NostrTags *nt = nostr_tags_append_unique(filter->tags, tag);
                if (nt) {
                    filter->tags = nt;
                } else {
                    nostr_tag_free(tag);
                }
            }
        }
    }

    // Deserialize `since` and `until` as integer timestamps
    json_t *since_json = json_object_get(json_obj, "since");
    if (json_is_integer(since_json)) {
        filter->since = (NostrTimestamp)json_integer_value(since_json);
    }

    json_t *until_json = json_object_get(json_obj, "until");
    if (json_is_integer(until_json)) {
        filter->until = (NostrTimestamp)json_integer_value(until_json);
    }

    // Deserialize `limit`
    json_t *limit_json = json_object_get(json_obj, "limit");
    if (json_is_integer(limit_json)) {
        filter->limit = (int)json_integer_value(limit_json);
    }

    // Deserialize `search`
    json_t *search_json = json_object_get(json_obj, "search");
    if (json_is_string(search_json)) {
        filter->search = strdup(json_string_value(search_json)); // Make a copy of the string
    }

    // Deserialize `limit_zero`
    json_t *limit_zero_json = json_object_get(json_obj, "limit_zero");
    filter->limit_zero = json_is_true(limit_zero_json);

    return 0; // Success
}

json_t *jansson_filter_serialize(const NostrFilter *filter) {
    if (!filter) {
        return NULL;
    }

    // Create a JSON object
    json_t *json_obj = json_object();

    // NIP-01: include only recognized keys and only when present
    if (string_array_size((StringArray *)&filter->ids) > 0) {
        json_t *ids_json = string_array_serialize(&filter->ids);
        json_object_set_new(json_obj, "ids", ids_json);
    }

    if (int_array_size((IntArray *)&filter->kinds) > 0) {
        json_t *kinds_json = int_array_serialize(&filter->kinds);
        json_object_set_new(json_obj, "kinds", kinds_json);
    }

    if (string_array_size((StringArray *)&filter->authors) > 0) {
        json_t *authors_json = string_array_serialize(&filter->authors);
        json_object_set_new(json_obj, "authors", authors_json);
    }

    // Tags: encode as NIP-01 dynamic keys like "#e": [..], "#p": [..]
    if (filter->tags && filter->tags->count > 0) {
        // Aggregate values by single-letter tag name
        for (size_t i = 0; i < filter->tags->count; i++) {
            NostrTag *t = filter->tags->data[i];
            if (!t || nostr_tag_size(t) < 2) continue;
            const char *name = nostr_tag_get(t, 0);
            const char *value = nostr_tag_get(t, 1);
            if (!name || !value) continue;
            // Only map single-character names to dynamic keys
            if (name[0] == '\0' || name[1] != '\0') continue;
            char keybuf[3] = {'#', name[0], '\0'};
            json_t *arr = json_object_get(json_obj, keybuf);
            if (!arr) {
                arr = json_array();
                json_object_set_new(json_obj, keybuf, arr);
            }
            // Append value
            json_array_append_new(arr, json_string(value));
        }
    }

    if (filter->since > 0) {
        json_object_set_new(json_obj, "since", json_integer((json_int_t)filter->since));
    }
    if (filter->until > 0) {
        json_object_set_new(json_obj, "until", json_integer((json_int_t)filter->until));
    }

    if (filter->limit > 0) {
        json_object_set_new(json_obj, "limit", json_integer(filter->limit));
    }

    if (filter->search) {
        json_object_set_new(json_obj, "search", json_string(filter->search));
    }

    // Do NOT include non-standard fields like 'limit_zero'.

    return json_obj;
}

json_t *jansson_tag_serialize(const NostrTag *tag) {
    if (!tag)
        return NULL;
    return string_array_serialize(tag);
}

// Serialize a collection of Tags into a JSON array of arrays
json_t *jansson_tags_serialize(const NostrTags *tags) {
    if (!tags)
        return NULL;

    json_t *json_a = json_array();
    for (size_t i = 0; i < tags->count; i++) {
        json_t *tag_json = jansson_tag_serialize(tags->data[i]);
        json_array_append_new(json_a, tag_json);
    }
    return json_a;
}

// Deserialize a JSON array into a single Tag
int jansson_tag_deserialize(NostrTag *tag, json_t *json_array) {
    if (!json_is_array(json_array))
        return -1;

    return string_array_deserialize(tag, json_array);
}

// Deserialize a JSON array of arrays into a Tags collection
NostrTags *jansson_tags_deserialize(json_t *json_array) {
    if (!json_is_array(json_array))
        return NULL;

    NostrTags *tags = malloc(sizeof(NostrTags));
    if (!tags)
        return NULL;

    tags->data = NULL;
    tags->count = 0;

    size_t index;
    json_t *value;
    json_array_foreach(json_array, index, value) {
        NostrTag *tag = new_string_array(0);
        if (!tag) {
            nostr_tags_free(tags);
            return NULL;
        }
        if (jansson_tag_deserialize(tag, value) != 0) {
            nostr_tag_free(tag);
            nostr_tags_free(tags);
            return NULL;
        }
        NostrTags *new_tags = nostr_tags_append_unique(tags, tag);
        if (!new_tags) {
            // on failure, free constructed tag and existing tags
            nostr_tag_free(tag);
            nostr_tags_free(tags);
            return NULL;
        }
        tags = new_tags;
    }

    return tags;
}

json_t *string_array_serialize(const StringArray *array) {
    if (array == NULL) {
        return NULL;
    }

    json_t *json_a = json_array();
    if (!json_a) {
        return NULL;
    }

    for (size_t i = 0; i < array->size; i++) {
        json_array_append_new(json_a, json_string(array->data[i]));
    }

    return json_a;
}

int string_array_deserialize(StringArray *array, json_t *json_array) {
    if (!json_is_array(json_array)) {
        return -1; // Return an error code if the input is not a JSON array
    }
    size_t array_size = json_array_size(json_array);
    for (size_t i = 0; i < array_size; i++) {
        json_t *json_value = json_array_get(json_array, i);
        if (!json_is_string(json_value)) {
            return -1; // Return error if an element is not a string
        }
        const char *str_value = json_string_value(json_value);
        string_array_add(array, str_value);
    }

    return 0; // Success
}

json_t *int_array_serialize(const IntArray *array) {
    if (array == NULL) {
        return NULL;
    }

    json_t *int_array = json_array();
    if (!int_array) {
        return NULL;
    }

    for (size_t i = 0; i < array->size; i++) {
        json_array_append_new(int_array, json_integer(array->data[i]));
    }

    return int_array;
}

int int_array_deserialize(IntArray *array, json_t *json_array) {
    if (!json_is_array(json_array)) {
        return -1; // Return an error code if the input is not a JSON array
    }

    size_t array_size = json_array_size(json_array);
    for (size_t i = 0; i < array_size; i++) {
        json_t *json_value = json_array_get(json_array, i);
        if (!json_is_integer(json_value)) {
            return -1; // Return error if an element is not an integer
        }

        int int_value = (int) json_integer_value(json_value);
        // Add the integer to IntArray (assumes you have a function to add)
        int_array_add(array, int_value);
    }

    return 0; // Success
}

// Implement the interface
NostrJsonInterface jansson_struct = {
    .init = jansson_init,
    .cleanup = jansson_cleanup,
    .serialize_event = jansson_event_serialize,
    .deserialize_event = jansson_event_deserialize,
    .serialize_envelope = jansson_envelope_serialize,
    .deserialize_envelope = jansson_envelope_deserialize,
    .serialize_filter = NULL, /* set below */
    .deserialize_filter = NULL};

NostrJsonInterface *jansson_impl = &jansson_struct;

/* String-based wrappers for Filter serialize/deserialize to match interface */
static char *jansson_filter_serialize_str(const NostrFilter *filter) {
    json_t *obj = jansson_filter_serialize(filter);
    if (!obj) return NULL;
    char *s = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    return s;
}

static int jansson_filter_deserialize_str(NostrFilter *filter, const char *json_str) {
    if (!json_str) return -1;
    json_error_t error;
    json_t *obj = json_loads(json_str, 0, &error);
    if (!obj) return -1;
    int rc = jansson_filter_deserialize(filter, obj);
    json_decref(obj);
    return rc;
}

__attribute__((constructor)) static void _init_interface_funcs(void) {
    jansson_impl->serialize_filter = jansson_filter_serialize_str;
    jansson_impl->deserialize_filter = jansson_filter_deserialize_str;
}

// Auto-register this JSON implementation as the default for libnostr when linked
__attribute__((constructor)) static void _register_default_interface(void) {
    // Provided by libnostr/src/json.c
    extern void nostr_set_json_interface(NostrJsonInterface *interface);
    nostr_set_json_interface(jansson_impl);
}

/* === Event extra helpers (JSON-backed) =================================== */
void nostr_event_set_extra(NostrEvent *event, const char *key, void *value) {
    if (!event || !key) return;
    if (!event->extra) {
        event->extra = json_object();
    }
    if (value) json_incref((json_t *)value);
    json_object_set_new((json_t *)event->extra, key, (json_t *)value);
}

void nostr_event_remove_extra(NostrEvent *event, const char *key) {
    if (!event || !event->extra || !key) return;
    json_object_del((json_t *)event->extra, key);
}

void *nostr_event_get_extra(NostrEvent *event, const char *key) {
    if (!event || !event->extra || !key) return NULL;
    return (void *)json_object_get((json_t *)event->extra, key);
}

char *nostr_event_get_extra_string(NostrEvent *event, const char *key) {
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (!json_is_string(value)) return NULL;
    const char *s = json_string_value(value);
    return s ? strdup(s) : NULL;
}

bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out) {
    if (!out) return false;
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (json_is_number(value)) { *out = json_number_value(value); return true; }
    if (json_is_integer(value)) { *out = (double)json_integer_value(value); return true; }
    return false;
}

bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out) {
    if (!out) return false;
    json_t *value = (json_t *)nostr_event_get_extra(event, key);
    if (!json_is_boolean(value)) return false;
    *out = json_boolean_value(value);
    return true;
}

/* === Backend implementations of facade generic helpers =================== */
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
            json_t *eit = json_array_get(arr, i);
            if (!json_is_number(eit)) { json_decref(root); free(vec); return -1; }
            if (json_is_integer(eit)) {
                vec[i] = (int)json_integer_value(eit);
            } else {
                vec[i] = (int)json_number_value(eit);
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

/* ===========================================================================
 * Extended JSON Interface Implementation (nostrc-3nj)
 * ===========================================================================
 */

/* ---- In-memory JSON value manipulation ---- */

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
    return json_object_set_new((json_t *)obj, key, (json_t *)val);
}

NostrJsonValue nostr_json_object_get(NostrJsonValue obj, const char *key) {
    if (!obj || !key) return NULL;
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
    return (int64_t)json_integer_value((json_t *)val);
}

bool nostr_json_value_boolean(NostrJsonValue val) {
    if (!val || !json_is_boolean((json_t *)val)) return false;
    return json_boolean_value((json_t *)val);
}

/* ---- 64-bit Integer and Double Getters ---- */

int nostr_json_get_int64(const char *json,
                         const char *entry_key,
                         int64_t *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = json_object_get(root, entry_key);
    int rc = -1;
    if (json_is_integer(v)) { *out_val = (int64_t)json_integer_value(v); rc = 0; }
    else if (json_is_real(v)) { *out_val = (int64_t)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_int64_at(const char *json,
                            const char *object_key,
                            const char *entry_key,
                            int64_t *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !object_key || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *v = json_object_get(obj, entry_key);
    int rc = -1;
    if (json_is_integer(v)) { *out_val = (int64_t)json_integer_value(v); rc = 0; }
    else if (json_is_real(v)) { *out_val = (int64_t)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_double(const char *json,
                          const char *entry_key,
                          double *out_val) {
    if (out_val) *out_val = 0.0;
    if (!json || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = json_object_get(root, entry_key);
    int rc = -1;
    if (json_is_number(v)) { *out_val = json_number_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_double_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             double *out_val) {
    if (out_val) *out_val = 0.0;
    if (!json || !object_key || !entry_key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }
    json_t *v = json_object_get(obj, entry_key);
    int rc = -1;
    if (json_is_number(v)) { *out_val = json_number_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

/* ---- Key Existence and Type Checking ---- */

static NostrJsonType jansson_type_to_nostr_type(json_t *v) {
    if (!v) return NOSTR_JSON_INVALID;
    if (json_is_null(v)) return NOSTR_JSON_NULL;
    if (json_is_boolean(v)) return NOSTR_JSON_BOOL;
    if (json_is_integer(v)) return NOSTR_JSON_INTEGER;
    if (json_is_real(v)) return NOSTR_JSON_REAL;
    if (json_is_string(v)) return NOSTR_JSON_STRING;
    if (json_is_array(v)) return NOSTR_JSON_ARRAY;
    if (json_is_object(v)) return NOSTR_JSON_OBJECT;
    return NOSTR_JSON_INVALID;
}

bool nostr_json_has_key(const char *json, const char *key) {
    if (!json || !key) return false;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return false;
    json_t *v = json_object_get(root, key);
    bool exists = (v != NULL);
    json_decref(root);
    return exists;
}

bool nostr_json_has_key_at(const char *json, const char *object_key, const char *key) {
    if (!json || !object_key || !key) return false;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return false;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return false; }
    json_t *v = json_object_get(obj, key);
    bool exists = (v != NULL);
    json_decref(root);
    return exists;
}

NostrJsonType nostr_json_get_type(const char *json, const char *key) {
    if (!json || !key) return NOSTR_JSON_INVALID;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return NOSTR_JSON_INVALID;
    json_t *v = json_object_get(root, key);
    NostrJsonType t = jansson_type_to_nostr_type(v);
    json_decref(root);
    return t;
}

NostrJsonType nostr_json_get_type_at(const char *json, const char *object_key, const char *key) {
    if (!json || !object_key || !key) return NOSTR_JSON_INVALID;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return NOSTR_JSON_INVALID;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return NOSTR_JSON_INVALID; }
    json_t *v = json_object_get(obj, key);
    NostrJsonType t = jansson_type_to_nostr_type(v);
    json_decref(root);
    return t;
}

/* ---- Deep Path Access ---- */

/* Helper to navigate to a JSON value via dot-notation path */
static json_t *navigate_path(json_t *root, const char *path) {
    if (!root || !path) return NULL;

    json_t *current = root;
    char *path_copy = strdup(path);
    if (!path_copy) return NULL;

    char *token = strtok(path_copy, ".");
    while (token && current) {
        if (json_is_object(current)) {
            current = json_object_get(current, token);
        } else if (json_is_array(current)) {
            /* Try to parse token as array index */
            char *endptr;
            long idx = strtol(token, &endptr, 10);
            if (*endptr == '\0' && idx >= 0 && (size_t)idx < json_array_size(current)) {
                current = json_array_get(current, (size_t)idx);
            } else {
                current = NULL;
            }
        } else {
            current = NULL;
        }
        token = strtok(NULL, ".");
    }

    free(path_copy);
    return current;
}

int nostr_json_get_string_path(const char *json, const char *path, char **out_str) {
    if (out_str) *out_str = NULL;
    if (!json || !path || !out_str) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    int rc = -1;
    if (v && json_is_string(v)) {
        const char *s = json_string_value(v);
        if (s) { *out_str = strdup(s); rc = 0; }
    }
    json_decref(root);
    return rc;
}

int nostr_json_get_int_path(const char *json, const char *path, int *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !path || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    int rc = -1;
    if (v && json_is_integer(v)) { *out_val = (int)json_integer_value(v); rc = 0; }
    else if (v && json_is_real(v)) { *out_val = (int)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_int64_path(const char *json, const char *path, int64_t *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !path || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    int rc = -1;
    if (v && json_is_integer(v)) { *out_val = (int64_t)json_integer_value(v); rc = 0; }
    else if (v && json_is_real(v)) { *out_val = (int64_t)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_double_path(const char *json, const char *path, double *out_val) {
    if (out_val) *out_val = 0.0;
    if (!json || !path || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    int rc = -1;
    if (v && json_is_number(v)) { *out_val = json_number_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_bool_path(const char *json, const char *path, bool *out_val) {
    if (out_val) *out_val = false;
    if (!json || !path || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    int rc = -1;
    if (v && json_is_boolean(v)) { *out_val = json_is_true(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_raw_path(const char *json, const char *path, char **out_raw) {
    if (out_raw) *out_raw = NULL;
    if (!json || !path || !out_raw) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *v = navigate_path(root, path);
    if (!v) { json_decref(root); return -1; }
    char *dump = json_dumps(v, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!dump) { json_decref(root); return -1; }
    *out_raw = strdup(dump);
    free(dump);
    json_decref(root);
    return *out_raw ? 0 : -1;
}

/* ---- Array Helpers ---- */

int nostr_json_get_array_length(const char *json, const char *key, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!json || !key || !out_len) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }
    *out_len = json_array_size(arr);
    json_decref(root);
    return 0;
}

int nostr_json_get_array_string(const char *json, const char *key, size_t index, char **out_str) {
    if (out_str) *out_str = NULL;
    if (!json || !key || !out_str) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, key);
    if (!arr || !json_is_array(arr) || index >= json_array_size(arr)) { json_decref(root); return -1; }
    json_t *v = json_array_get(arr, index);
    int rc = -1;
    if (v && json_is_string(v)) {
        const char *s = json_string_value(v);
        if (s) { *out_str = strdup(s); rc = 0; }
    }
    json_decref(root);
    return rc;
}

int nostr_json_get_array_int(const char *json, const char *key, size_t index, int *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, key);
    if (!arr || !json_is_array(arr) || index >= json_array_size(arr)) { json_decref(root); return -1; }
    json_t *v = json_array_get(arr, index);
    int rc = -1;
    if (v && json_is_integer(v)) { *out_val = (int)json_integer_value(v); rc = 0; }
    else if (v && json_is_real(v)) { *out_val = (int)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

int nostr_json_get_array_int64(const char *json, const char *key, size_t index, int64_t *out_val) {
    if (out_val) *out_val = 0;
    if (!json || !key || !out_val) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, key);
    if (!arr || !json_is_array(arr) || index >= json_array_size(arr)) { json_decref(root); return -1; }
    json_t *v = json_array_get(arr, index);
    int rc = -1;
    if (v && json_is_integer(v)) { *out_val = (int64_t)json_integer_value(v); rc = 0; }
    else if (v && json_is_real(v)) { *out_val = (int64_t)json_real_value(v); rc = 0; }
    json_decref(root);
    return rc;
}

/* ---- Object Key Enumeration ---- */

int nostr_json_get_object_keys(const char *json,
                               char ***out_keys,
                               size_t *out_count) {
    if (out_keys) *out_keys = NULL;
    if (out_count) *out_count = 0;
    if (!json || !out_keys || !out_count) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root || !json_is_object(root)) { if (root) json_decref(root); return -1; }

    size_t n = json_object_size(root);
    if (n == 0) {
        *out_keys = NULL;
        *out_count = 0;
        json_decref(root);
        return 0;
    }

    char **keys = (char **)calloc(n, sizeof(char *));
    if (!keys) { json_decref(root); return -1; }

    const char *key;
    json_t *val;
    size_t i = 0;
    json_object_foreach(root, key, val) {
        keys[i++] = strdup(key);
    }

    *out_keys = keys;
    *out_count = n;
    json_decref(root);
    return 0;
}

int nostr_json_get_object_keys_at(const char *json,
                                  const char *object_key,
                                  char ***out_keys,
                                  size_t *out_count) {
    if (out_keys) *out_keys = NULL;
    if (out_count) *out_count = 0;
    if (!json || !object_key || !out_keys || !out_count) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }

    size_t n = json_object_size(obj);
    if (n == 0) {
        *out_keys = NULL;
        *out_count = 0;
        json_decref(root);
        return 0;
    }

    char **keys = (char **)calloc(n, sizeof(char *));
    if (!keys) { json_decref(root); return -1; }

    const char *key;
    json_t *val;
    size_t i = 0;
    json_object_foreach(obj, key, val) {
        keys[i++] = strdup(key);
    }

    *out_keys = keys;
    *out_count = n;
    json_decref(root);
    return 0;
}

/* ---- Object Iteration (Callback-based) ---- */

int nostr_json_object_foreach(const char *json,
                              NostrJsonObjectIterCb callback,
                              void *user_data) {
    if (!json || !callback) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root || !json_is_object(root)) { if (root) json_decref(root); return -1; }

    const char *key;
    json_t *val;
    json_object_foreach(root, key, val) {
        char *val_json = json_dumps(val, JSON_COMPACT | JSON_ENCODE_ANY);
        if (val_json) {
            bool cont = callback(key, val_json, user_data);
            free(val_json);
            if (!cont) break;
        }
    }

    json_decref(root);
    return 0;
}

int nostr_json_object_foreach_at(const char *json,
                                 const char *object_key,
                                 NostrJsonObjectIterCb callback,
                                 void *user_data) {
    if (!json || !object_key || !callback) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *obj = json_object_get(root, object_key);
    if (!obj || !json_is_object(obj)) { json_decref(root); return -1; }

    const char *key;
    json_t *val;
    json_object_foreach(obj, key, val) {
        char *val_json = json_dumps(val, JSON_COMPACT | JSON_ENCODE_ANY);
        if (val_json) {
            bool cont = callback(key, val_json, user_data);
            free(val_json);
            if (!cont) break;
        }
    }

    json_decref(root);
    return 0;
}

/* ---- Array Iteration (Callback-based) ---- */

int nostr_json_array_foreach(const char *json,
                             const char *key,
                             NostrJsonArrayIterCb callback,
                             void *user_data) {
    if (!json || !key || !callback) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *arr = json_object_get(root, key);
    if (!arr || !json_is_array(arr)) { json_decref(root); return -1; }

    size_t index;
    json_t *val;
    json_array_foreach(arr, index, val) {
        char *val_json = json_dumps(val, JSON_COMPACT | JSON_ENCODE_ANY);
        if (val_json) {
            bool cont = callback(index, val_json, user_data);
            free(val_json);
            if (!cont) break;
        }
    }

    json_decref(root);
    return 0;
}

int nostr_json_array_foreach_root(const char *json,
                                  NostrJsonArrayIterCb callback,
                                  void *user_data) {
    if (!json || !callback) return -1;
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root || !json_is_array(root)) { if (root) json_decref(root); return -1; }

    size_t index;
    json_t *val;
    json_array_foreach(root, index, val) {
        char *val_json = json_dumps(val, JSON_COMPACT | JSON_ENCODE_ANY);
        if (val_json) {
            bool cont = callback(index, val_json, user_data);
            free(val_json);
            if (!cont) break;
        }
    }

    json_decref(root);
    return 0;
}

/* ---- JSON Building ---- */

/* Builder state enum */
typedef enum {
    BUILDER_STATE_EMPTY,
    BUILDER_STATE_OBJECT,
    BUILDER_STATE_ARRAY
} BuilderState;

/* Builder stack entry */
typedef struct BuilderStackEntry {
    json_t *container;
    BuilderState state;
    char *pending_key;    /* For objects: key waiting for value */
    struct BuilderStackEntry *prev;
} BuilderStackEntry;

struct NostrJsonBuilder {
    BuilderStackEntry *stack;
    json_t *result;
};

NostrJsonBuilder *nostr_json_builder_new(void) {
    NostrJsonBuilder *b = (NostrJsonBuilder *)calloc(1, sizeof(NostrJsonBuilder));
    return b;
}

void nostr_json_builder_free(NostrJsonBuilder *builder) {
    if (!builder) return;
    /* Free stack */
    while (builder->stack) {
        BuilderStackEntry *e = builder->stack;
        builder->stack = e->prev;
        if (e->pending_key) free(e->pending_key);
        if (e->container) json_decref(e->container);
        free(e);
    }
    if (builder->result) json_decref(builder->result);
    free(builder);
}

static void builder_push(NostrJsonBuilder *b, json_t *container, BuilderState state) {
    BuilderStackEntry *e = (BuilderStackEntry *)calloc(1, sizeof(BuilderStackEntry));
    e->container = container;
    e->state = state;
    e->prev = b->stack;
    b->stack = e;
}

static BuilderStackEntry *builder_pop(NostrJsonBuilder *b) {
    if (!b->stack) return NULL;
    BuilderStackEntry *e = b->stack;
    b->stack = e->prev;
    return e;
}

static int builder_add_value(NostrJsonBuilder *b, json_t *val) {
    if (!b || !val) return -1;

    if (!b->stack) {
        /* Top-level value */
        if (b->result) json_decref(b->result);
        b->result = val;
        return 0;
    }

    BuilderStackEntry *top = b->stack;
    if (top->state == BUILDER_STATE_OBJECT) {
        if (!top->pending_key) {
            json_decref(val);
            return -1;  /* Need key before value in object */
        }
        json_object_set_new(top->container, top->pending_key, val);
        free(top->pending_key);
        top->pending_key = NULL;
    } else if (top->state == BUILDER_STATE_ARRAY) {
        json_array_append_new(top->container, val);
    } else {
        json_decref(val);
        return -1;
    }

    return 0;
}

int nostr_json_builder_begin_object(NostrJsonBuilder *builder) {
    if (!builder) return -1;
    json_t *obj = json_object();
    if (!obj) return -1;

    if (builder->stack) {
        /* Nested: add to parent first, then push */
        BuilderStackEntry *top = builder->stack;
        if (top->state == BUILDER_STATE_OBJECT) {
            if (!top->pending_key) {
                json_decref(obj);
                return -1;
            }
            json_object_set_new(top->container, top->pending_key, obj);
            free(top->pending_key);
            top->pending_key = NULL;
            /* Borrow reference for stack - incref */
            json_incref(obj);
        } else if (top->state == BUILDER_STATE_ARRAY) {
            json_array_append_new(top->container, obj);
            json_incref(obj);
        }
    }

    builder_push(builder, obj, BUILDER_STATE_OBJECT);
    return 0;
}

int nostr_json_builder_end_object(NostrJsonBuilder *builder) {
    if (!builder || !builder->stack || builder->stack->state != BUILDER_STATE_OBJECT) return -1;
    BuilderStackEntry *e = builder_pop(builder);
    if (!builder->stack) {
        /* This was the root */
        if (builder->result) json_decref(builder->result);
        builder->result = e->container;
    } else {
        json_decref(e->container);
    }
    if (e->pending_key) free(e->pending_key);
    free(e);
    return 0;
}

int nostr_json_builder_begin_array(NostrJsonBuilder *builder) {
    if (!builder) return -1;
    json_t *arr = json_array();
    if (!arr) return -1;

    if (builder->stack) {
        BuilderStackEntry *top = builder->stack;
        if (top->state == BUILDER_STATE_OBJECT) {
            if (!top->pending_key) {
                json_decref(arr);
                return -1;
            }
            json_object_set_new(top->container, top->pending_key, arr);
            free(top->pending_key);
            top->pending_key = NULL;
            json_incref(arr);
        } else if (top->state == BUILDER_STATE_ARRAY) {
            json_array_append_new(top->container, arr);
            json_incref(arr);
        }
    }

    builder_push(builder, arr, BUILDER_STATE_ARRAY);
    return 0;
}

int nostr_json_builder_end_array(NostrJsonBuilder *builder) {
    if (!builder || !builder->stack || builder->stack->state != BUILDER_STATE_ARRAY) return -1;
    BuilderStackEntry *e = builder_pop(builder);
    if (!builder->stack) {
        if (builder->result) json_decref(builder->result);
        builder->result = e->container;
    } else {
        json_decref(e->container);
    }
    free(e);
    return 0;
}

int nostr_json_builder_set_key(NostrJsonBuilder *builder, const char *key) {
    if (!builder || !key || !builder->stack || builder->stack->state != BUILDER_STATE_OBJECT) return -1;
    if (builder->stack->pending_key) free(builder->stack->pending_key);
    builder->stack->pending_key = strdup(key);
    return builder->stack->pending_key ? 0 : -1;
}

int nostr_json_builder_add_string(NostrJsonBuilder *builder, const char *value) {
    if (!builder) return -1;
    return builder_add_value(builder, json_string(value ? value : ""));
}

int nostr_json_builder_add_int(NostrJsonBuilder *builder, int value) {
    if (!builder) return -1;
    return builder_add_value(builder, json_integer(value));
}

int nostr_json_builder_add_int64(NostrJsonBuilder *builder, int64_t value) {
    if (!builder) return -1;
    return builder_add_value(builder, json_integer((json_int_t)value));
}

int nostr_json_builder_add_double(NostrJsonBuilder *builder, double value) {
    if (!builder) return -1;
    return builder_add_value(builder, json_real(value));
}

int nostr_json_builder_add_bool(NostrJsonBuilder *builder, bool value) {
    if (!builder) return -1;
    return builder_add_value(builder, value ? json_true() : json_false());
}

int nostr_json_builder_add_null(NostrJsonBuilder *builder) {
    if (!builder) return -1;
    return builder_add_value(builder, json_null());
}

int nostr_json_builder_add_raw(NostrJsonBuilder *builder, const char *raw_json) {
    if (!builder || !raw_json) return -1;
    json_error_t err;
    json_t *val = json_loads(raw_json, 0, &err);
    if (!val) return -1;
    return builder_add_value(builder, val);
}

char *nostr_json_builder_finish(NostrJsonBuilder *builder) {
    if (!builder) return NULL;

    /* Close any open containers */
    while (builder->stack) {
        if (builder->stack->state == BUILDER_STATE_OBJECT) {
            nostr_json_builder_end_object(builder);
        } else {
            nostr_json_builder_end_array(builder);
        }
    }

    if (!builder->result) return NULL;

    char *s = json_dumps(builder->result, JSON_COMPACT);
    json_decref(builder->result);
    builder->result = NULL;
    return s;
}

/* ---- Convenience Builders ---- */

char *nostr_json_build_object(const char *key, ...) {
    if (!key) return NULL;

    json_t *obj = json_object();
    if (!obj) return NULL;

    va_list args;
    va_start(args, key);

    const char *k = key;
    while (k) {
        const char *v = va_arg(args, const char *);
        if (v) {
            json_object_set_new(obj, k, json_string(v));
        }
        k = va_arg(args, const char *);
    }

    va_end(args);

    char *s = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    return s;
}

char *nostr_json_build_string_array(const char *first, ...) {
    json_t *arr = json_array();
    if (!arr) return NULL;

    if (first) {
        json_array_append_new(arr, json_string(first));

        va_list args;
        va_start(args, first);
        const char *s;
        while ((s = va_arg(args, const char *)) != NULL) {
            json_array_append_new(arr, json_string(s));
        }
        va_end(args);
    }

    char *result = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    return result;
}

char *nostr_json_build_int_array(const int *values, size_t count) {
    json_t *arr = json_array();
    if (!arr) return NULL;

    for (size_t i = 0; i < count; i++) {
        json_array_append_new(arr, json_integer(values[i]));
    }

    char *result = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    return result;
}

/* ---- Validation ---- */

bool nostr_json_is_valid(const char *json) {
    if (!json) return false;
    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    if (!root) return false;
    json_decref(root);
    return true;
}

bool nostr_json_is_object_str(const char *json) {
    if (!json) return false;
    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    if (!root) return false;
    bool is_obj = json_is_object(root);
    json_decref(root);
    return is_obj;
}

bool nostr_json_is_array_str(const char *json) {
    if (!json) return false;
    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    if (!root) return false;
    bool is_arr = json_is_array(root);
    json_decref(root);
    return is_arr;
}

/* ---- Transformation ---- */

char *nostr_json_prettify(const char *json) {
    if (!json) return NULL;
    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    if (!root) return NULL;
    char *result = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    return result;
}

char *nostr_json_compact(const char *json) {
    if (!json) return NULL;
    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    if (!root) return NULL;
    char *result = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return result;
}

char *nostr_json_merge_objects(const char *base, const char *overlay) {
    if (!base || !overlay) return NULL;
    json_error_t err;
    json_t *base_obj = json_loads(base, 0, &err);
    if (!base_obj || !json_is_object(base_obj)) { if (base_obj) json_decref(base_obj); return NULL; }
    json_t *overlay_obj = json_loads(overlay, 0, &err);
    if (!overlay_obj || !json_is_object(overlay_obj)) {
        json_decref(base_obj);
        if (overlay_obj) json_decref(overlay_obj);
        return NULL;
    }

    /* Update base with overlay keys */
    const char *key;
    json_t *val;
    json_object_foreach(overlay_obj, key, val) {
        json_object_set(base_obj, key, val);
    }

    char *result = json_dumps(base_obj, JSON_COMPACT);
    json_decref(base_obj);
    json_decref(overlay_obj);
    return result;
}
