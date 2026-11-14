#ifndef FIPS_UTILS_INDEX_H
#define FIPS_UTILS_INDEX_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct fips_session_index { uint32_t value; } fips_session_index;
typedef enum fips_index_status {
    FIPS_INDEX_OK, FIPS_INDEX_EXHAUSTED, FIPS_INDEX_NOT_FOUND,
    FIPS_INDEX_ALREADY_IN_USE, FIPS_INDEX_CAPACITY
} fips_index_status;
#define FIPS_INDEX_DEFAULT_MAX_ATTEMPTS 100
/* Node-global, caller-owned sorted storage. No allocation. Objects/storage and
 * entropy context must outlive use; do not modify fields after init. NULL storage
 * is allowed for zero capacity. All failed calls preserve outputs and the set.
 * CAPACITY is a C policy refusal (no entropy drawn), not random EXHAUSTED.
 */
typedef struct fips_index_allocator {
    uint32_t *indices;
    size_t count, capacity, max_attempts;
    fips_entropy entropy;
} fips_index_allocator;
FIPS_CORE_API fips_session_index fips_session_index_new(uint32_t value);
FIPS_CORE_API uint32_t fips_session_index_as_u32(fips_session_index index);
FIPS_CORE_API void fips_session_index_to_le_bytes(fips_session_index index, uint8_t out[4]);
FIPS_CORE_API fips_session_index fips_session_index_from_le_bytes(const uint8_t bytes[4]);
/* Exactly eight lowercase hex digits and a NUL; capacity includes NUL. */
FIPS_CORE_API fips_status fips_session_index_format(fips_session_index index,
    char *out, size_t capacity);
FIPS_CORE_API void fips_index_allocator_init(fips_index_allocator *a,
    uint32_t *storage, size_t capacity, size_t max_attempts, fips_entropy entropy);
FIPS_CORE_API fips_index_status fips_index_allocator_allocate(fips_index_allocator *a,
    fips_session_index *out);
FIPS_CORE_API fips_index_status fips_index_allocator_free(fips_index_allocator *a,
    fips_session_index index);
FIPS_CORE_API fips_index_status fips_index_allocator_reserve(fips_index_allocator *a,
    fips_session_index index);
FIPS_CORE_API bool fips_index_allocator_is_allocated(const fips_index_allocator *a,
    fips_session_index index);
FIPS_CORE_API size_t fips_index_allocator_count(const fips_index_allocator *a);
FIPS_CORE_API bool fips_index_allocator_is_empty(const fips_index_allocator *a);
FIPS_CORE_API void fips_index_allocator_clear(fips_index_allocator *a);
#ifdef __cplusplus
}
#endif
#endif
