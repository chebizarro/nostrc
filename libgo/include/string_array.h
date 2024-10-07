#ifndef STRING_ARRAY_H
#define STRING_ARRAY_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define STRING_ARRAY_INITIAL_CAPACITY 4

typedef struct {
    char **data;      // Array of strings
    size_t size;      // Number of elements in the array
    size_t capacity;  // Allocated capacity
} StringArray;

// Initialize the array
void string_array_init(StringArray *array);

// Append a string to the array
void string_array_add(StringArray *array, const char *value);

// Get the string at the given index
const char *string_array_get(const StringArray *array, size_t index);

// Remove a string at the given index
void string_array_remove(StringArray *array, size_t index);

// Free the array memory
void string_array_free(StringArray *array);

#endif // STRING_ARRAY_H
