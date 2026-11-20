#ifndef LIBGO_SWAP_H
#define LIBGO_SWAP_H

#include "libgo-export.h"
#include "refptr.h"
#include <stdbool.h>

typedef struct GoSwap GoSwap;

/* Create an immutable snapshot slot. destructor must be non-NULL and is used
 * for every non-NULL value. Success transfers ownership of initial; failure
 * returns NULL and leaves ownership with the caller. NULL initial is empty. */
LIBGO_API GoSwap *go_swap_create(void *initial, void (*destructor)(void *));

/* Return an independently owned reference (all-zero if empty or swap is NULL).
 * Release exactly once with go_refptr_release(), or use go_auto(GoRefPtr).
 * Snapshot contents must not be mutated. Copies require go_refptr_retain().
 * Load-and-retain uses a short nsync mutex: this API is NOT lock-free. */
LIBGO_API GoRefPtr go_swap_load(GoSwap *swap);

/* Atomically replace the snapshot; NULL clears the slot. On success, ownership
 * of value transfers to swap. On allocation failure or NULL swap, return false
 * without changing the slot or taking ownership. Each non-NULL value must have
 * independent ownership: never pass a pointer already owned by a snapshot.
 * Concurrent loads/stores (including multiple writers) are supported. */
LIBGO_API bool go_swap_store(GoSwap *swap, void *value);

/* Requires external quiescence: no concurrent or future operations on swap.
 * Already loaded snapshots remain valid. NULL is harmless. Destructors run
 * outside the slot lock, on whichever thread releases the last reference;
 * they must be thread-safe and must not access a slot being freed. */
LIBGO_API void go_swap_free(GoSwap *swap);

#endif /* LIBGO_SWAP_H */
