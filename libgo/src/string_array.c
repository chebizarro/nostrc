#include "string_array.h"

// Initialize the StringArray with an initial capacity
void string_array_init(StringArray *array) {
    array->data = malloc(STRING_ARRAY_INITIAL_CAPACITY * sizeof(char *));
    array->size = 0;
    array->capacity = STRING_ARRAY_INITIAL_CAPACITY;
}

// Append a string to the array, resizing if necessary
void string_array_add(StringArray *array, const char *value) {
    if (array->size >= array->capacity) {
        // Resize the array (double the capacity)
        array->capacity *= 2;
        array->data = realloc(array->data, array->capacity * sizeof(char *));
        if (!array->data) {
            fprintf(stderr, "Failed to reallocate memory for StringArray\n");
            exit(EXIT_FAILURE);
        }
    }
    array->data[array->size++] = strdup(value);  // Use strdup to allocate a copy of the string
}

// Get the string at the given index
const char *string_array_get(const StringArray *array, size_t index) {
    if (index >= array->size) {
        fprintf(stderr, "Index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    return array->data[index];
}

// Remove a string at the given index
void string_array_remove(StringArray *array, size_t index) {
    if (index >= array->size) {
        fprintf(stderr, "Index out of bounds\n");
        return;
    }
    // Free the string at the given index
    free(array->data[index]);

    // Shift elements to the left after the removed element
    for (size_t i = index; i < array->size - 1; i++) {
        array->data[i] = array->data[i + 1];
    }
    array->size--;
}

// Free the array memory
void string_array_free(StringArray *array) {
    // Free each individual string
    for (size_t i = 0; i < array->size; i++) {
        free(array->data[i]);
    }
    // Free the array data
    free(array->data);
    array->data = NULL;
    array->size = 0;
    array->capacity = 0;
}
