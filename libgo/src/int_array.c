#include "int_array.h"

// Initialize the IntArray with an initial capacity
void int_array_init(IntArray *array) {
    array->data = malloc(INT_ARRAY_INITIAL_CAPACITY * sizeof(int));
    array->size = 0;
    array->capacity = INT_ARRAY_INITIAL_CAPACITY;
}

// Append an element to the array, resizing if necessary
void int_array_add(IntArray *array, int value) {
    if (array->size >= array->capacity) {
        // Resize the array (double the capacity)
        array->capacity *= 2;
        array->data = realloc(array->data, array->capacity * sizeof(int));
        if (!array->data) {
            fprintf(stderr, "Failed to reallocate memory for IntArray\n");
            exit(EXIT_FAILURE);
        }
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
