#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "string_array.h"

static char *sa_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Make room for at least @needed elements. nostrc-6tuz: never derive the
 * new capacity by doubling 0 (a zero-initialised StringArray), and keep
 * `capacity` truthful for every growth path (the variadic helpers used to
 * grow `data` without updating it, so a later add could "double" a stale
 * capacity below `size` and write past the block). */
static void sa_reserve(StringArray *array, size_t needed) {
    if (needed <= array->capacity && array->data)
        return;
    size_t cap = array->capacity ? array->capacity : STRING_ARRAY_INITIAL_CAPACITY;
    while (cap < needed)
        cap *= 2;
    char **data = realloc(array->data, cap * sizeof(char *));
    if (!data) {
        fprintf(stderr, "Failed to reallocate memory for StringArray\n");
        exit(EXIT_FAILURE);
    }
    array->data = data;
    array->capacity = cap;
}

StringArray *new_string_array(int capacity) {
    StringArray *array = (StringArray *)malloc(sizeof(StringArray));
    if (capacity == 0) {
        array->capacity = STRING_ARRAY_INITIAL_CAPACITY;
    } else {
        array->capacity = (size_t)capacity;
    }
    array->data = malloc(array->capacity * sizeof(char *));
    array->size = 0;    
    return array;
}

// Initialize the StringArray with an initial capacity
void string_array_init(StringArray *array) {
    array->capacity = STRING_ARRAY_INITIAL_CAPACITY;
    array->data = malloc(array->capacity * sizeof(char *));
    array->size = 0;
}

void string_array_init_with(StringArray *arr, ...) {
    va_list args;
    va_start(args, arr);

    const char *str;
    while ((str = va_arg(args, const char *)) != NULL) {
        sa_reserve(arr, arr->size + 1);
        arr->data[arr->size] = sa_strdup(str); // Duplicate the string
        arr->size++;
    }

    va_end(args);
}

// Append a string to the array, resizing if necessary
void string_array_add(StringArray *array, const char *value) {
    sa_reserve(array, array->size + 1);
    array->data[array->size++] = sa_strdup(value); // Use local strdup to allocate a copy of the string
}

// Add multiple strings to a StringArray using variadic arguments
void string_array_add_many(StringArray *arr, ...) {
    va_list args;
    va_start(args, arr);

    const char *str;
    while ((str = va_arg(args, const char *)) != NULL) {
        sa_reserve(arr, arr->size + 1);
        arr->data[arr->size] = sa_strdup(str); // Duplicate the string
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

// Set the string at the given index
void string_array_set(StringArray *array, size_t index, const char *value) {
    if (index >= array->size) {
        fprintf(stderr, "Index out of bounds\n");
        return;
    }
    // Free the string at the given index
    free(array->data[index]);
    array->data[index] = sa_strdup(value); // Use local strdup to allocate a copy of the string
}
