#include "swap.h"
#include "go_tsan.h"

struct GoSwap {
    nsync_mu mutex;
    GoRefPtr current;
    void (*destructor)(void *);
};

/* make_go_refptr() assumes allocation succeeds; stores must preserve the old
 * value and caller ownership on failure instead. */
static bool make_snapshot(void *value, void (*destructor)(void *), GoRefPtr *out) {
    *out = (GoRefPtr){0};
    if (!value) return true;
    _Atomic int *count = malloc(sizeof(*count));
    if (!count) return false;
    atomic_init(count, 1);
    *out = (GoRefPtr){ .ptr = value, .destructor = destructor, .ref_count = count };
    return true;
}

GoSwap *go_swap_create(void *initial, void (*destructor)(void *)) {
    if (!destructor) return NULL;
    GoSwap *swap = malloc(sizeof(*swap));
    if (!swap) return NULL;
    if (!make_snapshot(initial, destructor, &swap->current)) {
        free(swap);
        return NULL;
    }
    nsync_mu_init(&swap->mutex);
    swap->destructor = destructor;
    return swap;
}

GoRefPtr go_swap_load(GoSwap *swap) {
    if (!swap) return (GoRefPtr){0};
    NLOCK(&swap->mutex);
    GoRefPtr snapshot = swap->current;
    /* The slot still owns a reference while we retain under the same lock
     * that protects replacement; the writer cannot retire it between these. */
    go_refptr_retain(&snapshot);
    NUNLOCK(&swap->mutex);
    return snapshot;
}

bool go_swap_store(GoSwap *swap, void *value) {
    if (!swap) return false;
    GoRefPtr next;
    if (!make_snapshot(value, swap->destructor, &next)) return false;
    NLOCK(&swap->mutex);
    GoRefPtr previous = swap->current;
    swap->current = next;
    NUNLOCK(&swap->mutex);
    go_refptr_release(&previous);
    return true;
}

void go_swap_free(GoSwap *swap) {
    if (!swap) return;
    GoRefPtr previous = swap->current;
    free(swap);
    go_refptr_release(&previous);
}
