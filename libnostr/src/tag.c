#include "tag.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *escape_string(const char *str);

Tag *create_tag(const char *key, ...) {
    va_list args;
    const char *str;
    Tag *tag = NULL;
    size_t count = 0;

    va_start(args, key);

    // First pass: count the number of strings
    str = key;
    while (str != NULL) {
        count++;
        str = va_arg(args, const char *);
    }

    tag = new_string_array(count);

    if (tag == NULL) {
        return NULL;
    }

    // Re-initialize the variable argument list for the second pass
    va_start(args, key);
    str = key;
    for (size_t i = 0; i < count; i++) {
        string_array_add(tag, str);
    }
    // Clean up the variable argument list
    va_end(args);

    return tag;
}

void free_tag(Tag *tag) {
    string_array_free(tag);
}

bool tag_starts_with(Tag *tag, Tag *prefix) {
    if (!tag || !prefix)
        return false;

    size_t prefix_len = prefix->size;
    if (prefix_len > tag->size) {
        return false;
    }

    for (size_t i = 0; i < prefix_len - 1; i++) {
        if (strcmp(prefix->data[i], tag->data[i]) != 0) {
            return false;
        }
    }

    return strncmp(tag->data[prefix_len - 1], prefix->data[prefix_len - 1], strlen(prefix->data[prefix_len - 1])) == 0;
}

char *tag_key(Tag *tag) {
    if (tag && tag->size > 0) {
        return tag->data[0];
    }
    return NULL;
}

char *tag_value(Tag *tag) {
    if (tag && tag->size > 1) {
        return tag->data[1];
    }
    return NULL;
}

char *tag_relay(Tag *tag) {
    if (tag && tag->size > 2 && (strcmp(tag->data[0], "e") == 0 || strcmp(tag->data[0], "p") == 0)) {
        // Implement NormalizeURL(tag->elements[2])
        return tag->data[2];
    }
    return NULL;
}

Tags *create_tags(size_t count, ...) {
    va_list args;

    // Allocate memory for array of Tag pointers
    Tag **new_tags = (Tag **)malloc(count * sizeof(Tag *));
    if (!new_tags)
        return NULL; // Handle memory allocation failure

    // Initialize the variable argument list
    va_start(args, count);

    // Loop through the arguments and assign them to the new_tags array
    for (size_t i = 0; i < count; i++) {
        new_tags[i] = va_arg(args, Tag *);
    }

    // Clean up the variable argument list
    va_end(args);

    // Allocate memory for Tags struct
    Tags *tags = (Tags *)malloc(sizeof(Tags));
    if (!tags) {
        free(new_tags); // Free new_tags if Tags allocation fails
        return NULL;
    }

    // Assign the newly created tag array and count
    tags->data = new_tags;
    tags->count = count;

    return tags;
}

void free_tags(Tags *tags) {
    if (tags) {
        for (size_t i = 0; i < tags->count; i++) {
            free_tag(tags->data[i]);
        }
        free(tags->data);
        free(tags);
    }
}

char *tags_get_d(Tags *tags) {
    if (!tags)
        return NULL;

    for (size_t i = 0; i < tags->count; i++) {
        if (tag_starts_with(tags->data[i], create_tag("d", ""))) {
            return tags->data[i]->data[1];
        }
    }
    return NULL;
}

Tag *tags_get_first(Tags *tags, Tag *prefix) {
    if (!tags || !prefix)
        return NULL;

    for (size_t i = 0; i < tags->count; i++) {
        if (tag_starts_with(tags->data[i], prefix)) {
            return tags->data[i];
        }
    }
    return NULL;
}

Tag *tags_get_last(Tags *tags, Tag *prefix) {
    if (!tags || !prefix)
        return NULL;

    for (size_t i = tags->count; i > 0; i--) {
        if (tag_starts_with(tags->data[i - 1], prefix)) {
            return tags->data[i - 1];
        }
    }
    return NULL;
}

// Marshal a single Tag to a JSON array
char *tag_marshal_to_json(Tag *tag) {
    size_t capacity = 128;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;

    strcpy(buffer, "["); // Start the JSON array
    size_t len = 1;

    for (size_t i = 0; i < tag->size; i++) {
        char *escaped = escape_string(tag->data[i]);
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

    strcat(buffer, "]"); // Close the JSON array
    return buffer;
}

Tags *tags_get_all(Tags *tags, Tag *prefix) {
    if (!tags || !prefix)
        return NULL;

    Tags *result = create_tags(tags->count);
    if (!result)
        return NULL;

    size_t count = 0;
    for (size_t i = 0; i < tags->count; i++) {
        if (tag_starts_with(tags->data[i], prefix)) {
            result->data[count++] = tags->data[i];
        }
    }
    result->count = count;

    return result;
}

Tags *tags_filter_out(Tags *tags, Tag *prefix) {
    if (!tags || !prefix)
        return NULL;

    Tags *filtered = create_tags(tags->count);
    if (!filtered)
        return NULL;

    size_t count = 0;
    for (size_t i = 0; i < tags->count; i++) {
        if (!tag_starts_with(tags->data[i], prefix)) {
            filtered->data[count++] = tags->data[i];
        }
    }
    filtered->count = count;

    return filtered;
}

Tags *tags_append_unique(Tags *tags, Tag *tag) {
    if (!tags || !tag)
        return NULL;

    size_t n = tag->size > 2 ? 2 : tag->size;

    for (size_t i = 0; i < tags->count; i++) {
        if (tag_starts_with(tags->data[i], tag)) {
            return tags;
        }
    }

    Tags *new_tags = create_tags(tags->count + 1);
    if (!new_tags)
        return NULL;

    memcpy(new_tags->data, tags->data, tags->count * sizeof(Tag));
    new_tags->data[tags->count] = tag;
    new_tags->count = tags->count + 1;

    free_tags(tags);
    return new_tags;
}

bool tags_contains_any(Tags *tags, const char *tag_name, char **values, size_t values_count) {
    if (!tags || !tag_name || !values)
        return false;

    for (size_t i = 0; i < tags->count; i++) {
        Tag *tag = tags->data[i];
        if (tag->size < 2)
            continue;

        if (strcmp(tag->data[0], tag_name) != 0)
            continue;

        for (size_t j = 0; j < values_count; j++) {
            if (strcmp(tag->data[1], values[j]) == 0) {
                return true;
            }
        }
    }

    return false;
}

// Marshal multiple Tags (array of Tag) to a JSON array of arrays
char *tags_marshal_to_json(Tags *tags) {
    size_t capacity = 256;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;

    strcpy(buffer, "["); // Start the outer JSON array
    size_t len = 1;

    for (size_t i = 0; i < tags->count; i++) {
        char *tag_json = tag_marshal_to_json(tags->data[i]);
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

    strcat(buffer, "]"); // Close the outer JSON array
    return buffer;
}
