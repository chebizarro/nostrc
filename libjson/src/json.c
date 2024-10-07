#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr_jansson.h"
#include <jansson.h>
#include "go.h"

json_t *tags_serialize(const Tags *tags);
json_t *string_array_serialize(const StringArray *array);
json_t *int_array_serialize(const IntArray *array);
int tags_deserialize(Tags *tags, json_t *json_obj);
int string_array_deserialize(StringArray *array, json_t *json_array);
int int_array_deserialize(IntArray *array, json_t *json_array);

int filter_deserialize(Filter *filter, json_t *json_obj) {
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

json_t *filter_serialize(const Filter *filter) {
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
        json_t *tags_json = tags_serialize(filter->tags);
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

// Initializes the JSON interface (if needed)
void nostr_json_init(void) {
    // Initialize if necessary, Jansson doesn't require special init
}

// Cleans up the JSON interface (if needed)
void nostr_json_cleanup(void) {
    // Clean up if needed, Jansson doesn't require special cleanup
}

json_t *tag_serialize(const Tag *tag) {
    if (!tag) return NULL;

    json_t *json_array = json_array();
    for (size_t i = 0; i < tag->count; i++) {
        json_array_append_new(json_array, json_string(tag->elements[i]));
    }
    return json_array;
}

// Serialize a collection of Tags into a JSON array of arrays
json_t *tags_serialize(const Tags *tags) {
    if (!tags) return NULL;

    json_t *json_array = json_array();
    for (size_t i = 0; i < tags->count; i++) {
        json_t *tag_json = tag_serialize(tags->data[i]);
        json_array_append_new(json_array, tag_json);
    }
    return json_array;
}

// Deserialize a JSON array into a single Tag
Tag *tag_deserialize(json_t *json_array) {
    if (!json_is_array(json_array)) return NULL;

    Tag *tag = malloc(sizeof(Tag));
    if (!tag) return NULL;

    tag->elements = NULL;
    tag->count = 0;

    size_t index;
    json_t *value;
    json_array_foreach(json_array, index, value) {
        if (!json_is_string(value)) {
            tag_free(tag);
            return NULL;
        }

        if (tag_add_element(tag, json_string_value(value)) != 0) {
            tag_free(tag);
            return NULL;
        }
    }

    return tag;
}

// Deserialize a JSON array of arrays into a Tags collection
Tags *tags_deserialize(json_t *json_array) {
    if (!json_is_array(json_array)) return NULL;

    Tags *tags = malloc(sizeof(Tags));
    if (!tags) return NULL;

    tags->data = NULL;
    tags->count = 0;

    size_t index;
    json_t *value;
    json_array_foreach(json_array, index, value) {
        Tag *tag = tag_deserialize(value);
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

NostrEvent *nostr_json_deserialize_event(json_t *json_obj) {

    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event) {
        return NULL;
    }

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

    return event;
}

void envelope_deserialize(Envelope *envelope, const char* json_str) {
    if (!json_str) return;

    // Parse the JSON string
    json_error_t error;
    json_t *json_obj = json_loads(json_str, 0, &error);
    if (!json_obj) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return;
    }
	if(!json_is_array(json_obj)) {
		fprintf(stderr, "error: root is not an array\n");
		json_decref(json_obj);
		return;
	}

    switch (envelope->type) {
        case ENVELOPE_EVENT:
			EventEnvelope *env = (EventEnvelope *)envelope;
			switch (json_array_size(json_obj)) {
			case 2:
				json_t *json_evt = json_array_get(json_obj, 1);
				env->event = nostr_json_deserialize_event(json_evt);
				break;
			case 3:
				json_t *json_id = json_array_get(json_obj, 1);
				json_t *json_evt = json_array_get(json_obj, 2);					
				env->subscription_id = strdup(json_string_value(json_id));
				env->event = nostr_json_deserialize_event(json_evt);
				break;
			default:
				break;
			}
			json_decref(root);
            break;
        case ENVELOPE_REQ:
			if (json_array_size(json_obj) < 3) {
				fprintf(stderr, "failed to decode REQ envelope: missing filters");
				break;
			}
			ReqEnvelope *env = (ReqEnvelope *)envelope;
			json_t *json_id = json_array_get(json_obj, 1);
			env->subscription_id = strdup(json_string_value(json_id));
			env->filters = malloc(sizeof(Filter)*(json_array_size(json_obj)-2));
			int f = 0;
			for (int i = 2; i < json_array_size(json_obj); i++) {
				json_t *json_filter = json_array_get(json_obj, i);
				env->filters[f] = nostr_json_deserialize_filter(json_filter); 
				f++;
			}
            break;
        case ENVELOPE_COUNT:
			if (json_array_size(json_obj) < 4) {
				fprintf(stderr, "failed to decode COUNT envelope: missing filters");
				break;
			}
			ReqEnvelope *env = (ReqEnvelope *)envelope;
			json_t *json_id = json_array_get(json_obj, 1);
			env->subscription_id = strdup(json_string_value(json_id));
			json_t *count = json_array_get(json_obj, 2);
			if (!json_is_object(count)) {
				fprintf(stderr, "failed to decode COUNT envelope: count element is not a dictionary");				
				break;
			}
			env->count = json_object_get(count, "count");
			env->filters = malloc(sizeof(Filter)*(json_array_size(json_obj)-3));
			int f = 0;
			for (int i = 3; i < json_array_size(json_obj); i++) {
				json_t *json_filter = json_array_get(json_obj, i);
				env->filters[f] = nostr_json_deserialize_filter(json_filter); 
				f++;
			}
            break;
        case ENVELOPE_NOTICE:
			if (json_array_size(json_obj) < 2) {
				fprintf(stderr, "failed to decode NOTICE envelope");
				break;
			}
			NoticeEnvelope *env = (NoticeEnvelope *)envelope;
			json_t *json_message = json_array_get(json_obj, 1);
			env->message = strdup(json_string_value(json_message));
            break;
        case ENVELOPE_EOSE:
			if (json_array_size(json_obj) < 2) {
				fprintf(stderr, "failed to decode ESOSE envelope");
				break;
			}
			EOSEEnvelope *env = (EOSEEnvelope *)envelope;
			json_t *json_message = json_array_get(json_obj, 1);
			env->message = strdup(json_string_value(json_message));
            break;
        case ENVELOPE_CLOSE:
			if (json_array_size(json_obj) < 2) {
				fprintf(stderr, "failed to decode CLOSE envelope");
				break;
			}
			CloseEnvelope *env = (CloseEnvelope *)envelope;
			json_t *json_message = json_array_get(json_obj, 1);
			env->message = strdup(json_string_value(json_message));
            break;
        case ENVELOPE_CLOSED:
			if (json_array_size(json_obj) < 3) {
				fprintf(stderr, "failed to decode CLOSED envelope");
				break;
			}
			ClosedEnvelope *env = (ClosedEnvelope *)envelope;
			json_t *json_id = json_array_get(json_obj, 1);
			json_t *json_message = json_array_get(json_obj, 2);
			env->subscription_id = strdup(json_string_value(json_id));
			env->message = strdup(json_string_value(json_message));
            break;
        case ENVELOPE_OK:
			if (json_array_size(json_obj) < 4) {
				fprintf(stderr, "failed to decode OK envelope");
				break;
			}
			OKEnvelope *env = (OKEnvelope *)envelope;
			json_t *json_id = json_array_get(json_obj, 1);
			json_t *json_message = json_array_get(json_obj, 2);
			env->subscription_id = strdup(json_string_value(json_id));
			env->message = strdup(json_string_value(json_message));
            break;
        case ENVELOPE_AUTH:
			if (json_array_size(json_obj) < 2) {
				fprintf(stderr, "failed to decode AUTH envelope");
				break;
			}
			AuthEnvelope *env = (AuthEnvelope *)envelope;
			json_t *json_challenge = json_array_get(json_obj, 1);
			if (json_is_object(json_challenge)) {
				env->event = nostr_json_deserialize_event(json_challenge);
			} else {
				env->challenge = strdup(json_string_value(json_challenge));
			}
            break;
        default:
            break;
    }
	json_decref(json_obj);
	return;
}


// Serialize NostrEvent to a JSON string
char *nostr_json_serialize(const NostrEvent *event) {
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

// Deserialize a JSON string to NostrEvent
NostrEvent *nostr_json_deserialize(const char *json_str) {
    if (!json_str) return NULL;

    // Parse the JSON string
    json_error_t error;
    json_t *json_obj = json_loads(json_str, 0, &error);
    if (!json_obj) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return NULL;
    }

    // Allocate memory for NostrEvent
    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event) {
        json_decref(json_obj);
        return NULL;
    }

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

    // Free the JSON object
    json_decref(json_obj);

    return event;
}

// Implement the interface
NostrJsonInterface jansson_struct = {
    .init = nostr_json_init,
    .cleanup = nostr_json_cleanup,
    .serialize = nostr_json_serialize,
    .deserialize = nostr_json_deserialize
};

NostrJsonInterface *jansson_impl = &jansson_struct;
