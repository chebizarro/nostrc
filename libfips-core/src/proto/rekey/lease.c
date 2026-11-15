#include <fips/proto/rekey/lease.h>
#include <stdatomic.h>
#ifdef FIPS_TEST_HOOKS
static _Thread_local size_t leaked;
#else
static _Atomic size_t leaked;
#endif
static bool allocator_draw(void *ctx, uint32_t *out) {
    fips_session_index index;
    if (fips_index_allocator_allocate(ctx, &index) != FIPS_INDEX_OK)
        return false;
    *out = index.value;
    return true;
}
fips_rekey_index_source fips_rekey_index_source_from_allocator(fips_index_allocator *a) {
    return (fips_rekey_index_source){allocator_draw, a};
}
bool fips_rekey_allocate(fips_rekey_index_source s, fips_rekey_index_lease *out) {
    uint32_t index;
    if (!s.draw || !s.draw(s.ctx, &index))
        return false;
    *out = (fips_rekey_index_lease){index, true};
    return true;
}
bool fips_rekey_index_lease_release(fips_rekey_index_lease *l, uint32_t *index) {
    if (!l->live)
        return false;
    *index = l->index;
    *l = (fips_rekey_index_lease){0};
    return true;
}
void fips_rekey_index_lease_drop(fips_rekey_index_lease *l) {
    if (l->live) {
#ifdef FIPS_TEST_HOOKS
        ++leaked;
#else
        atomic_fetch_add_explicit(&leaked, 1, memory_order_relaxed);
#endif
    }
    *l = (fips_rekey_index_lease){0};
}
size_t fips_rekey_leaked(void) {
#ifdef FIPS_TEST_HOOKS
    return leaked;
#else
    return atomic_load_explicit(&leaked, memory_order_relaxed);
#endif
}
fips_rekey_epoch_tag fips_rekey_tag_source_draw(fips_rekey_tag_source *s) {
    uint64_t t = s->next;
    if (s->next != UINT64_MAX)
        ++s->next;
    return t;
}
#ifdef FIPS_TEST_HOOKS
fips_rekey_index_lease fips_rekey_index_lease_for_test(uint32_t i) {
    return (fips_rekey_index_lease){i, true};
}
#endif
