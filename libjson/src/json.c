#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include "nostr_jansson.h"
#include "go.h"

char * jansson_event_serialize(const NostrEvent * event);
int jansson_event_deserialize(NostrEvent * event, const char* json_str);
int _deserialize_event(NostrEvent * event, json_t * json_obj);
char * jansson_envelope_serialize(Envelope * envelope);
int jansson_envelope_deserialize(Envelope * envelope, const char * json_str);

int jansson_filter_deserialize(Filter * filter, json_t * json_obj);
json_t * jansson_filter_serialize(const Filter * filter);
json_t * jansson_tag_serialize(const Tag * tag);
json_t *jansson_tags_serialize(const Tags *tags);
Tag * jansson_tag_deserialize(json_t * json_array);
Tags * jansson_tags_deserialize(json_t * json_array);
json_t *string_array_serialize(const StringArray *array);
json_t *int_array_serialize(const IntArray *array);
int tags_deserialize(Tags *tags, json_t *json_obj);
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
    if (!event) return NULL;

    // Create a new JSON object
    json_t *json_obj = json_object();
	char kind_str[12];
	snprintf(kind_str, sizeof(kind_str), "%d", event->kind);  // if 'kind' is int
	json_object_set_new(json_obj, "kind", json_string(kind_str));
    json_object_set_new(json_obj, "id", json_string(event->id));
    json_object_set_new(json_obj, "pubkey", json_string(event->pubkey));
    json_object_set_new(json_obj, "created_at", json_integer(event->created_at));
    json_object_set_new(json_obj, "content", json_string(event->content));

    // Convert JSON object to string
    char *json_str = json_dumps(json_obj, JSON_COMPACT);

    // Free the JSON object
    json_decref(json_obj);

    return json_str;
}

int jansson_event_deserialize(NostrEvent * event, const char * json_str) {
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
    // Extract fields
    json_t *json_id = json_object_get(json_obj, "id");
    json_t *json_pubkey = json_object_get(json_obj, "pubkey");
    json_t *json_created_at = json_object_get(json_obj, "created_at");
    json_t *json_kind = json_object_get(json_obj, "kind");
    json_t *json_content = json_object_get(json_obj, "content");

    event->id = strdup(json_string_value(json_id));
    event->pubkey = strdup(json_string_value(json_pubkey));
    event->created_at = json_integer_value(json_created_at);
    event->kind = atoi(json_string_value(json_kind));
    event->content = strdup(json_string_value(json_content));

    return 1;
}

char * jansson_envelope_serialize(Envelope* envelope) {
	return NULL;
}


// Deserialize a JSON string to NostrEvent
int jansson_envelope_deserialize(Envelope* envelope, const char *json_str) {
    if (!json_str) return -1;

    char *first_comma = strchr(json_str, ',');
    if (!first_comma) return -1;

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

        if (json_array_size(json_obj) == 2) {
            json_t *json_evt = json_array_get(json_obj, 1);
			env->event = create_event();
            int err = _deserialize_event(env->event, json_evt);
        } else if (json_array_size(json_obj) == 3) {
            json_t *json_id = json_array_get(json_obj, 1);
            json_t *json_evt = json_array_get(json_obj, 2);

            env->subscription_id = strdup(json_string_value(json_id));
			env->event = create_event();
            int err = _deserialize_event(env->event, json_evt);
        }
        break;
    }
    case ENVELOPE_REQ: {
        if (json_array_size(json_obj) < 3) {
            fprintf(stderr, "Failed to decode REQ envelope: missing filters\n");
            break;
        }
        ReqEnvelope *env = (ReqEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        env->subscription_id = strdup(json_string_value(json_id));

        env->filters = malloc(sizeof(Filter) * (json_array_size(json_obj) - 2));
        if (!env->filters) {
            fprintf(stderr, "Memory allocation failed for filters\n");
            break;
        }
        for (int f = 0, i = 2; i < json_array_size(json_obj); i++, f++) {
            json_t *json_filter = json_array_get(json_obj, i);
            //jansson_filter_deserialize(&env->filters[i], json_filter);
        }
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

        env->subscription_id = strdup(json_string_value(json_id));

        json_t *count_value = json_object_get(json_count, "count");
        if (json_is_integer(count_value)) {
            env->count = json_integer_value(count_value);
        }

        env->filters = malloc(sizeof(Filter) * (json_array_size(json_obj) - 3));
        if (!env->filters) {
            fprintf(stderr, "Memory allocation failed for filters\n");
            break;
        }
        for (int f = 0, i = 3; i < json_array_size(json_obj); i++, f++) {
            json_t *json_filter = json_array_get(json_obj, i);
            //env->filters[f] = jansson_filter_deserialize(json_filter);
        }
        break;
    }
    case ENVELOPE_NOTICE: {
        if (json_array_size(json_obj) < 2) {
            fprintf(stderr, "Failed to decode NOTICE envelope\n");
            break;
        }
        NoticeEnvelope *env = (NoticeEnvelope *)envelope;
        json_t *json_message = json_array_get(json_obj, 1);
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
        env->subscription_id = strdup(json_string_value(json_id));
        env->reason = strdup(json_string_value(json_message));
        break;
    }
    case ENVELOPE_OK: {
        if (json_array_size(json_obj) < 4) {
            fprintf(stderr, "Failed to decode OK envelope\n");
            break;
        }
        OKEnvelope *env = (OKEnvelope *)envelope;
        json_t *json_id = json_array_get(json_obj, 1);
        json_t *json_ok = json_array_get(json_obj, 2);
        env->event_id = strdup(json_string_value(json_id));
        env->ok = json_is_true(json_ok);
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
			_deserialize_event(env->event, json_challenge);
        } else {
            env->challenge = strdup(json_string_value(json_challenge));
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

    // Deserialize the `tags`
    json_t *tags_json = json_object_get(json_obj, "tags");
    if (tags_json && filter->tags && tags_deserialize(filter->tags, tags_json) != 0) {
        return -1;
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
        filter->search = strdup(json_string_value(search_json));  // Make a copy of the string
    }

    // Deserialize `limit_zero`
    json_t *limit_zero_json = json_object_get(json_obj, "limit_zero");
    filter->limit_zero = json_is_true(limit_zero_json);

    return 0;  // Success
}

json_t *jansson_filter_serialize(const Filter *filter) {
    if (!filter) {
        return NULL;
    }

    // Create a JSON object
    json_t *json_obj = json_object();

    // Serialize the `ids`
    json_t *ids_json = string_array_serialize(&filter->ids);
    json_object_set_new(json_obj, "ids", ids_json);

    // Serialize the `kinds`
    json_t *kinds_json = int_array_serialize(&filter->kinds);
    json_object_set_new(json_obj, "kinds", kinds_json);

    // Serialize the `authors`
    json_t *authors_json = string_array_serialize(&filter->authors);
    json_object_set_new(json_obj, "authors", authors_json);

    // Serialize the `tags`
    if (filter->tags) {
        json_t *tags_json = jansson_tags_serialize(filter->tags);
        json_object_set_new(json_obj, "tags", tags_json);
    }

    // Serialize `since` and `until` as integer timestamps
    json_object_set_new(json_obj, "since", json_integer((json_int_t)filter->since));
    json_object_set_new(json_obj, "until", json_integer((json_int_t)filter->until));

    // Serialize `limit`
    json_object_set_new(json_obj, "limit", json_integer(filter->limit));

    // Serialize `search`
    if (filter->search) {
        json_object_set_new(json_obj, "search", json_string(filter->search));
    }

    // Serialize `limit_zero`
    json_object_set_new(json_obj, "limit_zero", json_boolean(filter->limit_zero));

    return json_obj;
}


json_t *jansson_tag_serialize(const Tag *tag) {
    if (!tag) return NULL;
	return string_array_serialize(tag);
}

// Serialize a collection of Tags into a JSON array of arrays
json_t *jansson_tags_serialize(const Tags *tags) {
    if (!tags) return NULL;

    json_t *json_array = json_array();
    for (size_t i = 0; i < tags->count; i++) {
        json_t *tag_json = jansson_tag_serialize(tags->data[i]);
        json_array_append_new(json_array, tag_json);
    }
    return json_array;
}

// Deserialize a JSON array into a single Tag
int jansson_tag_deserialize(Tag *tag, json_t *json_array) {
    if (!json_is_array(json_array)) return NULL;

    return string_array_deserialize(tag, json_array);
}

// Deserialize a JSON array of arrays into a Tags collection
Tags *jansson_tags_deserialize(json_t *json_array) {
    if (!json_is_array(json_array)) return NULL;

    Tags *tags = malloc(sizeof(Tags));
    if (!tags) return NULL;

    tags->data = NULL;
    tags->count = 0;

    size_t index;
    json_t *value;
    json_array_foreach(json_array, index, value) {
        Tag *tag = jansson_tag_deserialize(value);
        if (!tag) {
            tags_free(tags);
            return NULL;
        }

        if (tags_add_tag(tags, tag) != 0) {
            tag_free(tag);
            tags_free(tags);
            return NULL;
        }
    }

    return tags;
}

json_t *string_array_serialize(const StringArray *array) {
    if (array == NULL) {
        return NULL;
    }

    json_t *json_array = json_array();
    if (!json_array) {
        return NULL;
    }

    for (size_t i = 0; i < array->size; i++) {
        json_array_append_new(json_array, json_string(array->data[i]));
    }

    return json_array;
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

// Implement the interface
NostrJsonInterface jansson_struct = {
    .init = jansson_init,
    .cleanup = jansson_cleanup,
    .serialize_event = jansson_event_serialize,
    .deserialize_event = jansson_event_deserialize,
	.serialize_envelope = jansson_envelope_serialize,
	.deserialize_envelope = jansson_envelope_deserialize,
	.serialize_filter = jansson_filter_serialize,
	.deserialize_filter = jansson_filter_deserialize
};

NostrJsonInterface *jansson_impl = &jansson_struct;
