#ifndef STRING_ARRAY_H
#define STRING_ARRAY_H

#include "libgo-export.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRING_ARRAY_INITIAL_CAPACITY 4

typedef struct _StringArray {
    char **data;     // Array of strings
    size_t size;     // Number of elements in the array
    size_t capacity; // Allocated capacity
} StringArray;

LIBGO_API StringArray *new_string_array(int capacity);

// Initialize the array
LIBGO_API void string_array_init(StringArray *array);

LIBGO_API void string_array_init_with(StringArray *arr, ...);

// Append a string to the array
LIBGO_API void string_array_add(StringArray *array, const char *value);

LIBGO_API void string_array_add_many(StringArray *arr, ...);

// Get the string at the given index
LIBGO_API const char *string_array_get(const StringArray *array, size_t index);

// Remove a string at the given index
LIBGO_API void string_array_remove(StringArray *array, size_t index);

// Set the string at the given index
LIBGO_API void string_array_set(StringArray *array, size_t index, const char *value);

// Free the array memory
LIBGO_API void string_array_free(StringArray *array);

LIBGO_API size_t string_array_size(StringArray *array);

LIBGO_API int string_array_contains(const StringArray *array, const char *str);

#endif // STRING_ARRAY_H
