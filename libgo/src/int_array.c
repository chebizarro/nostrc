#include "int_array.h"

// Initialize the IntArray with an initial capacity
void int_array_init(IntArray *array) {
    array->data = malloc(INT_ARRAY_INITIAL_CAPACITY * sizeof(int));
    array->size = 0;
    array->capacity = INT_ARRAY_INITIAL_CAPACITY;
}

// Append an element to the array, resizing if necessary.
// nostrc-6tuz: a zero-initialised IntArray (e.g. inside a calloc'd
// NostrFilter) has capacity 0, and doubling 0 stays 0 — realloc(NULL, 0)
// then returned a minimal block and the write overflowed the heap. Grow to
// at least INT_ARRAY_INITIAL_CAPACITY.
void int_array_add(IntArray *array, int value) {
    if (array->size >= array->capacity) {
        size_t cap = array->capacity ? array->capacity * 2 : INT_ARRAY_INITIAL_CAPACITY;
        if (cap <= array->size) cap = array->size + 1;
        int *data = realloc(array->data, cap * sizeof(int));
        if (!data) {
            fprintf(stderr, "Failed to reallocate memory for IntArray\n");
            exit(EXIT_FAILURE);
        }
        array->data = data;
        array->capacity = cap;
    }
    array->data[array->size++] = value;
}

// Get the element at the given index
int int_array_get(const IntArray *array, size_t index) {
    if (index >= array->size) {
        fprintf(stderr, "Index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    return array->data[index];
}

// Remove an element at the given index
void int_array_remove(IntArray *array, size_t index) {
    if (index >= array->size) {
        fprintf(stderr, "Index out of bounds\n");
        return;
    }
    // Shift elements to the left after the removed element
    for (size_t i = index; i < array->size - 1; i++) {
        array->data[i] = array->data[i + 1];
    }
    array->size--;
}

// Free the array memory
void int_array_free(IntArray *array) {
    free(array->data);
    array->data = NULL;
    array->size = 0;
    array->capacity = 0;
}

size_t int_array_size(IntArray *array) {
    return array->size;
}

// Function to check if an IntArray contains a given integer
int int_array_contains(const IntArray *array, int value) {
    if (!array)
        return 0; // Handle null array input

    for (size_t i = 0; i < array->size; i++) {
        if (array->data[i] == value) {
            return 1; // Value found
        }
    }
    return 0; // Value not found
}
