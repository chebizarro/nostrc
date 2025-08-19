#include "nostr-tag.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-utils.h"

NostrTag *nostr_tag_new(const char *key, ...) {
    va_list args;
    const char *str;
    NostrTag *tag = NULL;
    size_t count = 0;

    va_start(args, key);

    // First pass: count the number of strings
    str = key;
    while (str != NULL) {
        count++;
        str = va_arg(args, const char *);
    }

    // End the first traversal before reusing the va_list
    va_end(args);

    tag = new_string_array(count);

    if (tag == NULL) {
        return NULL;
    }

    // Re-initialize the variable argument list for the second pass
    va_start(args, key);
    str = key;
    for (size_t i = 0; i < count; i++) {
        string_array_add(tag, str);
        // advance to next argument for subsequent iterations
        str = va_arg(args, const char *);
    }
    // Clean up the variable argument list
    va_end(args);

    return tag;
}

void nostr_tag_free(NostrTag *tag) {
    string_array_free(tag);
}

bool nostr_tag_starts_with(NostrTag *tag, NostrTag *prefix) {
    if (!tag || !prefix)
        return false;

    size_t prefix_len = nostr_tag_size(prefix);
    if (prefix_len > nostr_tag_size(tag)) {
        return false;
    }

    for (size_t i = 0; i < prefix_len - 1; i++) {
        if (strcmp(prefix->data[i], tag->data[i]) != 0) {
            return false;
        }
    }

    return strncmp(tag->data[prefix_len - 1], prefix->data[prefix_len - 1], strlen(prefix->data[prefix_len - 1])) == 0;
}

const char *nostr_tag_get_key(const NostrTag *tag) {
    if (tag && nostr_tag_size(tag) > 0) {
        return nostr_tag_get(tag, 0);
    }
    return NULL;
}

const char *nostr_tag_get_value(const NostrTag *tag) {
    if (tag && nostr_tag_size(tag) > 1) {
        return nostr_tag_get(tag, 1);
    }
    return NULL;
}

const char *nostr_tag_get_relay(const NostrTag *tag) {
    if (tag && nostr_tag_size(tag) > 2 && (strcmp(nostr_tag_get(tag, 0), "e") == 0 || strcmp(nostr_tag_get(tag, 0), "p") == 0)) {
        return nostr_tag_get(tag, 2);
    }
    return NULL;
}

NostrTags *nostr_tags_new(size_t count, ...) {
    va_list args;
    
    // Determine initial capacity (ensure non-zero so append works)
    size_t capacity = count > 0 ? count : 4;
    // Allocate memory for array of Tag pointers with capacity
    NostrTag **new_tags = (NostrTag **)malloc(capacity * sizeof(NostrTag *));
    if (!new_tags)
        return NULL; // Handle memory allocation failure

    // Initialize the variable argument list
    va_start(args, count);

    // Loop through the arguments and assign them to the new_tags array
    for (size_t i = 0; i < count; i++) {
        new_tags[i] = va_arg(args, NostrTag *);
    }

    // Clean up the variable argument list
    va_end(args);

    // Allocate memory for Tags struct
    NostrTags *tags = (NostrTags *)malloc(sizeof(NostrTags));
    if (!tags) {
        free(new_tags); // Free new_tags if Tags allocation fails
        return NULL;
    }

    // Assign the newly created tag array and count
    tags->data = new_tags;
    tags->count = count;
    tags->capacity = capacity;

    return tags;
}

void nostr_tags_free(NostrTags *tags) {
    if (tags) {
        for (size_t i = 0; i < tags->count; i++) {
            nostr_tag_free(tags->data[i]);
        }
        free(tags->data);
        free(tags);
    }
}

const char *nostr_tags_get_d(NostrTags *tags) {
    if (!tags)
        return NULL;

    for (size_t i = 0; i < tags->count; i++) {
        NostrTag *t = tags->data[i];
        if (!t) continue;
        const char *key = nostr_tag_get_key(t);
        if (key && strcmp(key, "d") == 0) {
            /* Return the value (index 1) if present */
            return nostr_tag_get(t, 1);
        }
    }
    return NULL;
}

NostrTag *nostr_tags_get_first(NostrTags *tags, NostrTag *prefix) {
    if (!tags || !prefix)
        return NULL;

    for (size_t i = 0; i < tags->count; i++) {
        if (nostr_tag_starts_with(tags->data[i], prefix)) {
            return tags->data[i];
        }
    }
    return NULL;
}

NostrTag *nostr_tags_get_last(NostrTags *tags, NostrTag *prefix) {
    if (!tags || !prefix)
        return NULL;

    for (size_t i = tags->count; i > 0; i--) {
        if (nostr_tag_starts_with(tags->data[i - 1], prefix)) {
            return tags->data[i - 1];
        }
    }
    return NULL;
}

// Marshal a single Tag to a JSON array
char *nostr_tag_to_json(const NostrTag *tag) {
    size_t capacity = 128;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;

    strcpy(buffer, "["); // Start the JSON array
    size_t len = 1;

    for (size_t i = 0; i < tag->size; i++) {
        char *escaped = nostr_escape_string(tag->data[i]);
        size_t escaped_len = strlen(escaped);

        // Resize the buffer if necessary
        while (len + escaped_len + 4 > capacity) {
            capacity *= 2;
            buffer = realloc(buffer, capacity);
            if (!buffer) {
                free(escaped);
                return NULL;
            }
        }

        if (i > 0) {
            strcat(buffer, ","); // Add a comma between elements
            len++;
        }

        strcat(buffer, "\"");
        strcat(buffer, escaped);
        strcat(buffer, "\"");
        len += escaped_len + 2;

        free(escaped);
    }

    // Ensure space for closing ']' and terminating NUL
    if (len + 2 > capacity) {
        size_t new_capacity = capacity;
        while (len + 2 > new_capacity) new_capacity *= 2;
        char *nbuf = realloc(buffer, new_capacity);
        if (!nbuf) {
            free(buffer);
            return NULL;
        }
        buffer = nbuf;
        capacity = new_capacity;
    }
    strcat(buffer, "]"); // Close the JSON array
    return buffer;
}

NostrTags *nostr_tags_get_all(NostrTags *tags, NostrTag *prefix) {
    if (!tags || !prefix)
        return NULL;

    NostrTags *result = nostr_tags_new(nostr_tags_size(tags));
    if (!result)
        return NULL;

    size_t count = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        if (nostr_tag_starts_with(tags->data[i], prefix)) {
            result->data[count++] = tags->data[i];
        }
    }
    result->count = count;

    return result;
}

NostrTags *nostr_tags_filter_out(NostrTags *tags, NostrTag *prefix) {
    if (!tags || !prefix)
        return NULL;

    NostrTags *filtered = nostr_tags_new(nostr_tags_size(tags));
    if (!filtered)
        return NULL;

    size_t count = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        if (!nostr_tag_starts_with(tags->data[i], prefix)) {
            filtered->data[count++] = tags->data[i];
        }
    }
    filtered->count = count;

    return filtered;
}

NostrTags *nostr_tags_append_unique(NostrTags *tags, NostrTag *tag) {
    if (!tags || !tag)
        return NULL;

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        if (nostr_tag_starts_with(tags->data[i], tag)) {
            return tags;
        }
    }

    // grow in place
    size_t new_count = nostr_tags_size(tags) + 1;
    NostrTag **new_data = (NostrTag **)realloc(tags->data, new_count * sizeof(NostrTag *));
    if (!new_data) {
        return NULL;
    }
    tags->data = new_data;
    tags->data[tags->count] = tag;
    tags->count = new_count;
    tags->capacity = new_count;
    return tags;
}

bool nostr_tags_contains_any(NostrTags *tags, const char *tag_name, char **values, size_t values_count) {
    if (!tags || !tag_name || !values)
        return false;

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_size(tag) < 2)
            continue;

        if (strcmp(nostr_tag_get(tag, 0), tag_name) != 0)
            continue;

        for (size_t j = 0; j < values_count; j++) {
            if (strcmp(nostr_tag_get(tag, 1), values[j]) == 0) {
                return true;
            }
        }
    }

    return false;
}

// Marshal multiple Tags (array of Tag) to a JSON array of arrays
char *nostr_tags_to_json(NostrTags *tags) {
    size_t capacity = 256;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;

    strcpy(buffer, "["); // Start the outer JSON array
    size_t len = 1;

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        char *tag_json = nostr_tag_to_json(nostr_tags_get(tags, i));
        size_t tag_len = strlen(tag_json);

        // Resize the buffer if necessary
        while (len + tag_len + 3 > capacity) {
            capacity *= 2;
            buffer = realloc(buffer, capacity);
            if (!buffer) {
                free(tag_json);
                return NULL;
            }
        }

        if (i > 0) {
            strcat(buffer, ","); // Add a comma between tag arrays
            len++;
        }

        strcat(buffer, tag_json);
        len += tag_len;

        free(tag_json);
    }

    // Ensure space for closing ']' and terminating NUL
    if (len + 2 > capacity) {
        size_t new_capacity = capacity;
        while (len + 2 > new_capacity) new_capacity *= 2;
        char *nbuf = realloc(buffer, new_capacity);
        if (!nbuf) {
            free(buffer);
            return NULL;
        }
        buffer = nbuf;
        capacity = new_capacity;
    }
    strcat(buffer, "]"); // Close the outer JSON array
    return buffer;
}

size_t nostr_tags_size(const NostrTags *tags) {
    return tags ? tags->count : 0;
}

NostrTag *nostr_tags_get(const NostrTags *tags, size_t index) {
    return tags ? tags->data[index] : NULL;
}

void nostr_tags_set(NostrTags *tags, size_t index, NostrTag *tag) {
    if (!tags) return;
    tags->data[index] = tag;
}

void nostr_tags_append(NostrTags *tags, NostrTag *tag) {
    if (!tags) return;
    if (tags->count >= tags->capacity) {
        size_t new_capacity = tags->capacity ? tags->capacity * 2 : 4;
        NostrTag **new_data = (NostrTag **)realloc(tags->data, new_capacity * sizeof(NostrTag *));
        if (!new_data) {
            // Allocation failed; do not modify state
            return;
        }
        tags->data = new_data;
        tags->capacity = new_capacity;
    }
    tags->data[tags->count++] = tag;
}

size_t nostr_tag_size(const NostrTag *tag) {
    return tag ? string_array_size((StringArray *)tag) : 0;
}

const char *nostr_tag_get(const NostrTag *tag, size_t index) {
    return tag ? string_array_get((StringArray *)tag, index) : NULL;
}

void nostr_tag_set(NostrTag *tag, size_t index, const char *value) {
    if (!tag) return;
    string_array_set((StringArray *)tag, index, value);
}

void nostr_tag_append(NostrTag *tag, const char *value) {
    if (!tag) return;
    string_array_add((StringArray *)tag, value);
}

void nostr_tag_add(NostrTag *tag, const char *value) {
    if (!tag) return;
    string_array_add((StringArray *)tag, value);
}

void nostr_tag_reserve(NostrTag *tag, size_t capacity) {
    if (!tag) return;
    if (tag->capacity >= capacity) return;
    char **new_data = (char **)realloc(tag->data, capacity * sizeof(char *));
    if (!new_data) return; /* leave unchanged on alloc failure */
    tag->data = new_data;
    tag->capacity = capacity;
}

void nostr_tags_reserve(NostrTags *tags, size_t capacity) {
    if (!tags) return;
    if (tags->capacity >= capacity) return;
    NostrTag **new_data = (NostrTag **)realloc(tags->data, capacity * sizeof(NostrTag *));
    if (!new_data) return; /* leave unchanged on alloc failure */
    tags->data = new_data;
    tags->capacity = capacity;
}
