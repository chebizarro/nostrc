#ifndef INT_ARRAY_H
#define INT_ARRAY_H

#include "libgo-export.h"

#include <stdio.h>
#include <stdlib.h>

#define INT_ARRAY_INITIAL_CAPACITY 4

typedef struct _IntArray {
    int *data;       // Array of integers
    size_t size;     // Number of elements in the array
    size_t capacity; // Allocated capacity
} IntArray;

// Initialize the array
LIBGO_API void int_array_init(IntArray *array);

// Append an element to the array
LIBGO_API void int_array_add(IntArray *array, int value);

// Get the element at the given index
LIBGO_API int int_array_get(const IntArray *array, size_t index);

// Remove an element at the given index
LIBGO_API void int_array_remove(IntArray *array, size_t index);

// Free the array memory
LIBGO_API void int_array_free(IntArray *array);

LIBGO_API size_t int_array_size(IntArray *array);

LIBGO_API int int_array_contains(const IntArray *array, int value);

#endif // INT_ARRAY_H
