#ifndef STRING_ARRAY_H
#define STRING_ARRAY_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define STRING_ARRAY_INITIAL_CAPACITY 4

typedef struct _StringArray {
    char **data;      // Array of strings
    size_t size;      // Number of elements in the array
    size_t capacity;  // Allocated capacity
} StringArray;

StringArray* new_string_array(int capacity);

// Initialize the array
void string_array_init(StringArray *array);

void string_array_init_with(StringArray * arr, ...);

// Append a string to the array
void string_array_add(StringArray *array, const char *value);

void string_array_add_many(StringArray * arr, ...);

// Get the string at the given index
const char *string_array_get(const StringArray *array, size_t index);

// Remove a string at the given index
void string_array_remove(StringArray *array, size_t index);

// Free the array memory
void string_array_free(StringArray *array);

size_t string_array_size(StringArray *array);

int string_array_contains(const StringArray * array, const char * str);

#endif // STRING_ARRAY_H
