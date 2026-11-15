#ifndef FIPS_REKEY_LEASE_H
#define FIPS_REKEY_LEASE_H
#include <fips/utils/index.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Linear by contract. Never memcpy an owning machine, lease or output list.
 * Only allocate mints a production lease; release consumes it for the host. */
typedef struct fips_rekey_index_lease {
    uint32_t index;
    bool live;
} fips_rekey_index_lease;
typedef struct fips_rekey_index_source {
    bool (*draw)(void *ctx, uint32_t *index);
    void *ctx;
} fips_rekey_index_source;
typedef uint64_t fips_rekey_epoch_tag;
typedef struct fips_rekey_tag_source {
    uint64_t next;
} fips_rekey_tag_source;
FIPS_CORE_API fips_rekey_index_source
fips_rekey_index_source_from_allocator(fips_index_allocator *a);
FIPS_CORE_API bool fips_rekey_allocate(fips_rekey_index_source source, fips_rekey_index_lease *out);
FIPS_CORE_API bool fips_rekey_index_lease_release(fips_rekey_index_lease *lease, uint32_t *index);
/* Explicit C drop: records an unreturned lease, never frees a wire-provided index. */
FIPS_CORE_API void fips_rekey_index_lease_drop(fips_rekey_index_lease *lease);
FIPS_CORE_API size_t fips_rekey_leaked(void);
FIPS_CORE_API fips_rekey_epoch_tag fips_rekey_tag_source_draw(fips_rekey_tag_source *source);
#ifdef FIPS_TEST_HOOKS
FIPS_CORE_API fips_rekey_index_lease fips_rekey_index_lease_for_test(uint32_t index);
#endif
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_LEASE_H */
