#ifndef REFPTR_H
#define REFPTR_H

#include <stdatomic.h>
#include <stdlib.h>

typedef struct GoRefPtr {
    void *ptr;
    void (*destructor)(void *);
    _Atomic int *ref_count;
} GoRefPtr;

static inline GoRefPtr make_go_refptr(void *ptr, void (*destructor)(void *)) {
    GoRefPtr ref;
    ref.ptr = ptr;
    ref.destructor = destructor;
    ref.ref_count = (_Atomic int *)malloc(sizeof(_Atomic int));
    atomic_store(ref.ref_count, 1);
    return ref;
}

// Increment the reference count
static inline void go_refptr_retain(GoRefPtr *ref) {
    if (ref && ref->ref_count) {
        atomic_fetch_add(ref->ref_count, 1);
    }
}

// Decrement the reference count and clean up if necessary
static inline void go_refptr_release(GoRefPtr *ref) {
    if (ref && ref->ref_count && atomic_fetch_sub(ref->ref_count, 1) == 1) {
        ref->destructor(ref->ptr);
        free(ref->ref_count);
    }
}

// Cleanup function to be called when GoRefPtr goes out of scope
static inline void go_refptr_cleanup(GoRefPtr *ref) {
    go_refptr_release(ref);
}

typedef struct {
    void *array;
    size_t element_size;
    size_t length;
} generic_array_t;

// Cleanup function
static inline void free_generic_array_t(generic_array_t *ptr) {
    if (ptr->array) {
        free(ptr->array);
    }
}

// Macro for auto cleanup
#define auto_generic_array_t __attribute__((cleanup(free_generic_array_t))) generic_array_t

// Macro to initialize the generic array
#define init_generic_array_t(type, len) \
    (generic_array_t) { .array = malloc((len) * sizeof(type)), .element_size = sizeof(type), .length = (len) }

// Macro to access elements in the generic array
#define get_generic_array_element(arr, index, type) (((type *)(arr).array)[index])

static inline void go_string_cleanup(char **str) {
    if (*str) {
        free(*str);
    }
}

#define cleanup_refptr __attribute__((cleanup(go_refptr_cleanup)))

// Convenience macro to wrap the cleanup attribute declaration
#define go_autoptr(TypeName) cleanup_refptr GoRefPtr

#define CLEANUP(func) __attribute__((cleanup(func)))

#define go_autostr CLEANUP(go_string_cleanup) char *

#endif // REFPTR_H
