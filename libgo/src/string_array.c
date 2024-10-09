#include "string_array.h"

StringArray *new_string_array(int capacity) {
    StringArray *array = (StringArray *)malloc(sizeof(StringArray));
    if (capacity == 0) {
        array->capacity = STRING_ARRAY_INITIAL_CAPACITY;
    } else {
        array->capacity = (size_t)capacity;
    }
    return array;
}

// Initialize the StringArray with an initial capacity
void string_array_init(StringArray *array) {

    array->data = malloc(array->capacity * sizeof(char *));
    array->size = 0;
}

void string_array_init_with(StringArray *arr, ...) {
    va_list args;
    va_start(args, arr);

    const char *str;
    while ((str = va_arg(args, const char *)) != NULL) {
        arr->data = realloc(arr->data, (arr->size + 1) * sizeof(char *));
        arr->data[arr->size] = strdup(str); // Duplicate the string
        arr->size++;
    }

    va_end(args);
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
    array->data[array->size++] = strdup(value); // Use strdup to allocate a copy of the string
}

// Add multiple strings to a StringArray using variadic arguments
void string_array_add_many(StringArray *arr, ...) {
    va_list args;
    va_start(args, arr);

    const char *str;
    while ((str = va_arg(args, const char *)) != NULL) {
        arr->data = realloc(arr->data, (arr->size + 1) * sizeof(char *));
        arr->data[arr->size] = strdup(str); // Duplicate the string
        arr->size++;
    }

    va_end(args);
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

size_t string_array_size(StringArray *array) {
    return array->size;
}

int string_array_contains(const StringArray *array, const char *str) {
    if (!array || !str)
        return 0; // Handle null array or string input

    for (size_t i = 0; i < array->size; i++) {
        if (strcmp(array->data[i], str) == 0) {
            return 1; // String found
        }
    }
    return 0; // String not found
}
