/*
 * refptr.h — Reference-counted pointer and generic array utilities.
 *
 * This header provides GoRefPtr (atomic ref-counted wrapper), generic_array_t
 * (type-erased dynamic array), and includes the comprehensive go_auto.h
 * auto-cleanup macros.
 *
 * The auto-cleanup macros (go_autoptr, go_autofree, go_autostr, go_auto,
 * go_steal_pointer, go_clear_pointer) are now defined in go_auto.h.
 */
#ifndef REFPTR_H
#define REFPTR_H

#include <stdatomic.h>
#include <stdlib.h>

/* ── GoRefPtr: atomic reference-counted wrapper ─────────────────────── */

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

/* Increment the reference count */
static inline void go_refptr_retain(GoRefPtr *ref) {
    if (ref && ref->ref_count) {
        atomic_fetch_add(ref->ref_count, 1);
    }
}

/* Decrement the reference count and clean up if necessary */
static inline void go_refptr_release(GoRefPtr *ref) {
    if (ref && ref->ref_count && atomic_fetch_sub(ref->ref_count, 1) == 1) {
        ref->destructor(ref->ptr);
        free(ref->ref_count);
        /* Prevent use-after-free if a cleanup attribute or caller releases again */
        ref->ref_count = NULL;
        ref->ptr = NULL;
        ref->destructor = NULL;
    }
}

/* Cleanup function to be called when GoRefPtr goes out of scope */
static inline void go_refptr_cleanup(GoRefPtr *ref) {
    go_refptr_release(ref);
}

/* ── generic_array_t: type-erased dynamic array ─────────────────────── */

typedef struct {
    void *array;
    size_t element_size;
    size_t length;
} generic_array_t;

static inline void free_generic_array_t(generic_array_t *ptr) {
    if (ptr->array) {
        free(ptr->array);
        ptr->array = NULL;
    }
}

#define auto_generic_array_t __attribute__((cleanup(free_generic_array_t))) generic_array_t

#define init_generic_array_t(type, len) \
    (generic_array_t) { .array = malloc((len) * sizeof(type)), .element_size = sizeof(type), .length = (len) }

#define get_generic_array_element(arr, index, type) (((type *)(arr).array)[index])

/* ── Include the comprehensive auto-cleanup system ──────────────────── */
#include "go_auto.h"

/* ── Register GoRefPtr for go_auto() ────────────────────────────────── */
/* Must come after both GoRefPtr (above) and go_auto.h macro defs. */
GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GoRefPtr, go_refptr_cleanup)

#endif /* REFPTR_H */
