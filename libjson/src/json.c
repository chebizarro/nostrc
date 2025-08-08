#include "go.h"
#include "nostr_jansson.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *jansson_event_serialize(const NostrEvent *event);
int jansson_event_deserialize(NostrEvent *event, const char *json_str);
int _deserialize_event(NostrEvent *event, json_t *json_obj);
char *jansson_envelope_serialize(const Envelope *envelope);
int jansson_envelope_deserialize(Envelope *envelope, const char *json_str);

int jansson_filter_deserialize(Filter *filter, json_t *json_obj);
json_t *jansson_filter_serialize(const Filter *filter);
json_t *jansson_tag_serialize(const Tag *tag);
json_t *jansson_tags_serialize(const Tags *tags);
int jansson_tag_deserialize(Tag *tag, json_t *json);
Tags *jansson_tags_deserialize(json_t *json_array);
json_t *string_array_serialize(const StringArray *array);
json_t *int_array_serialize(const IntArray *array);
int string_array_deserialize(StringArray *array, json_t *json_array);
int int_array_deserialize(IntArray *array, json_t *json_array);

// Initializes the JSON interface (if needed)
void jansson_init(void) {
    // Initialize if necessary, Jansson doesn't require special init
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
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
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
        Tags *t = jansson_tags_deserialize(json_tags);
        if (t) {
            // free existing default empty tags if present
            if (event->tags) free_tags(event->tags);
            event->tags = t;
        }
    }

    return 0;
}

char *jansson_envelope_serialize(const Envelope *envelope) {
    if (!envelope) return NULL;
    json_t *arr = json_array();
    if (!arr) return NULL;
    switch (envelope->type) {
    case ENVELOPE_EVENT: {
        EventEnvelope *env = (EventEnvelope *)envelope;
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
    case ENVELOPE_NOTICE: {
        NoticeEnvelope *env = (NoticeEnvelope *)envelope;
        json_array_append_new(arr, json_string("NOTICE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case ENVELOPE_EOSE: {
        EOSEEnvelope *env = (EOSEEnvelope *)envelope;
        json_array_append_new(arr, json_string("EOSE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case ENVELOPE_CLOSE: {
        CloseEnvelope *env = (CloseEnvelope *)envelope;
        json_array_append_new(arr, json_string("CLOSE"));
        json_array_append_new(arr, json_string(env->message ? env->message : ""));
        break;
    }
    case ENVELOPE_CLOSED: {
        ClosedEnvelope *env = (ClosedEnvelope *)envelope;
        json_array_append_new(arr, json_string("CLOSED"));
        json_array_append_new(arr, json_string(env->subscription_id ? env->subscription_id : ""));
        json_array_append_new(arr, json_string(env->reason ? env->reason : ""));
        break;
    }
    case ENVELOPE_OK: {
        OKEnvelope *env = (OKEnvelope *)envelope;
        json_array_append_new(arr, json_string("OK"));
        json_array_append_new(arr, json_string(env->event_id ? env->event_id : ""));
        json_array_append_new(arr, env->ok ? json_true() : json_false());
        if (env->reason) json_array_append_new(arr, json_string(env->reason));
        break;
    }
    case ENVELOPE_AUTH: {
        AuthEnvelope *env = (AuthEnvelope *)envelope;
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
int jansson_envelope_deserialize(Envelope *envelope, const char *json_str) {
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
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return -1;
    }
    if (!json_is_array(json_obj)) {
        fprintf(stderr, "Error: root is not an array\n");
        json_decref(json_obj);
        return -1;
    }

    // Process according to envelope type
    switch (envelope->type) {
    case ENVELOPE_EVENT: {
        EventEnvelope *env = (EventEnvelope *)envelope;
        size_t n = json_array_size(json_obj);
        if (n < 2) { json_decref(json_obj); return -1; }
        json_t *json_evt = json_array_get(json_obj, n-1);
        if (!json_is_object(json_evt)) { json_decref(json_obj); return -1; }
        if (n == 3) {
            json_t *json_id = json_array_get(json_obj, 1);
            if (!json_is_string(json_id)) { json_decref(json_obj); return -1; }
            env->subscription_id = strdup(json_string_value(json_id));
        }
        env->event = create_event();
        if (!env->event) { json_decref(json_obj); return -1; }
        (void)_deserialize_event(env->event, json_evt);
        break;
    }
    case ENVELOPE_REQ: {
        if (json_array_size(json_obj) < 3) {
            fprintf(stderr, "Failed to decode REQ envelope: missing filters\n");
            break;
        }
        ReqEnvelope *env = (ReqEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        if (!json_is_string(json_id)) { break; }
        env->subscription_id = strdup(json_string_value(json_id));
        Filters *fs = create_filters();
        if (!fs) { break; }
        for (size_t i = 2; i < json_array_size(json_obj); i++) {
            json_t *json_filter = json_array_get(json_obj, i);
            Filter f = {0};
            if (jansson_filter_deserialize(&f, json_filter) != 0) { continue; }
            filters_add(fs, &f);
        }
        env->filters = fs;
        break;
    }
    case ENVELOPE_COUNT: {
        if (json_array_size(json_obj) < 4) {
            fprintf(stderr, "Failed to decode COUNT envelope: missing filters\n");
            break;
        }
        CountEnvelope *env = (CountEnvelope *)envelope;
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
        Filters *fs = create_filters();
        if (!fs) { break; }
        for (size_t i = 3; i < json_array_size(json_obj); i++) {
            json_t *json_filter = json_array_get(json_obj, i);
            Filter f = {0};
            if (jansson_filter_deserialize(&f, json_filter) != 0) { continue; }
            filters_add(fs, &f);
        }
        env->filters = fs;
        break;
    }
    case ENVELOPE_NOTICE: {
        if (json_array_size(json_obj) < 2) {
            fprintf(stderr, "Failed to decode NOTICE envelope\n");
            break;
        }
        NoticeEnvelope *env = (NoticeEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case ENVELOPE_EOSE: {
        if (json_array_size(json_obj) < 2) {
            fprintf(stderr, "Failed to decode EOSE envelope\n");
            break;
        }
        EOSEEnvelope *env = (EOSEEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case ENVELOPE_CLOSE: {
        if (json_array_size(json_obj) < 2) {
            fprintf(stderr, "Failed to decode CLOSE envelope\n");
            break;
        }
        CloseEnvelope *env = (CloseEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
        if (!json_is_string(json_message)) { break; }
        env->message = strdup(json_string_value(json_message));
        break;
    }
    case ENVELOPE_CLOSED: {
        if (json_array_size(json_obj) < 3) {
            fprintf(stderr, "Failed to decode CLOSED envelope\n");
            break;
        }
        ClosedEnvelope *env = (ClosedEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        json_t *json_message = json_array_get(json_obj, 2);
        if (!json_is_string(json_id) || !json_is_string(json_message)) { break; }
        env->subscription_id = strdup(json_string_value(json_id));
        env->reason = strdup(json_string_value(json_message));
        break;
    }
    case ENVELOPE_OK: {
        if (json_array_size(json_obj) < 3) {
            fprintf(stderr, "Failed to decode OK envelope\n");
            break;
        }
        OKEnvelope *env = (OKEnvelope *)envelope;
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
    case ENVELOPE_AUTH: {
        if (json_array_size(json_obj) < 2) {
            fprintf(stderr, "Failed to decode AUTH envelope\n");
            break;
        }
        AuthEnvelope *env = (AuthEnvelope *)envelope;
        json_t *json_challenge = json_array_get(json_obj, 1);
        if (json_is_object(json_challenge)) {
            env->event = create_event();
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

int jansson_filter_deserialize(Filter *filter, json_t *json_obj) {
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
        filter->tags = create_tags(0);
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
        Tags *t = jansson_tags_deserialize(tags_json);
        if (t) {
            if (filter->tags) free_tags(filter->tags);
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
                Tag *tag = new_string_array(0);
                if (!tag) continue;
                string_array_add(tag, tag_name);
                string_array_add(tag, v);
                Tags *nt = tags_append_unique(filter->tags, tag);
                if (nt) {
                    filter->tags = nt;
                } else {
                    free_tag(tag);
                }
            }
        }
    }

    // Deserialize `since` and `until` as integer timestamps
    json_t *since_json = json_object_get(json_obj, "since");
    if (json_is_integer(since_json)) {
        filter->since = (Timestamp)json_integer_value(since_json);
    }

    json_t *until_json = json_object_get(json_obj, "until");
    if (json_is_integer(until_json)) {
        filter->until = (Timestamp)json_integer_value(until_json);
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

json_t *jansson_filter_serialize(const Filter *filter) {
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
            Tag *t = filter->tags->data[i];
            if (!t || t->size < 2) continue;
            const char *name = t->data[0];
            const char *value = t->data[1];
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

json_t *jansson_tag_serialize(const Tag *tag) {
    if (!tag)
        return NULL;
    return string_array_serialize(tag);
}

// Serialize a collection of Tags into a JSON array of arrays
json_t *jansson_tags_serialize(const Tags *tags) {
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
int jansson_tag_deserialize(Tag *tag, json_t *json_array) {
    if (!json_is_array(json_array))
        return -1;

    return string_array_deserialize(tag, json_array);
}

// Deserialize a JSON array of arrays into a Tags collection
Tags *jansson_tags_deserialize(json_t *json_array) {
    if (!json_is_array(json_array))
        return NULL;

    Tags *tags = malloc(sizeof(Tags));
    if (!tags)
        return NULL;

    tags->data = NULL;
    tags->count = 0;

    size_t index;
    json_t *value;
    json_array_foreach(json_array, index, value) {
        Tag *tag = new_string_array(0);
        if (!tag) {
            free_tags(tags);
            return NULL;
        }
        if (jansson_tag_deserialize(tag, value) != 0) {
            free_tag(tag);
            free_tags(tags);
            return NULL;
        }
        Tags *new_tags = tags_append_unique(tags, tag);
        if (!new_tags) {
            // on failure, free constructed tag and existing tags
            free_tag(tag);
            free_tags(tags);
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
static char *jansson_filter_serialize_str(const Filter *filter) {
    json_t *obj = jansson_filter_serialize(filter);
    if (!obj) return NULL;
    char *s = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    return s;
}

static int jansson_filter_deserialize_str(Filter *filter, const char *json_str) {
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
