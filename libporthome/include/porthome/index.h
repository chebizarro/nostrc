/* Storage-independent OID to sealed-object index. SPDX-License-Identifier: MIT */
#ifndef PORTHOME_INDEX_H
#define PORTHOME_INDEX_H

#include <porthome/export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plain, owned values. No pointers, storage handles or libgit2 types.
 * Only these fields form the ref_state_node index row (design section 3.5). */
typedef struct {
    char git_oid[65];             /* 40 or 64 lowercase hex bytes, then NUL */
    uint8_t sealed_sha256[32];    /* SHA-256 of the sealed object bytes */
    uint32_t chunk_key_id;
} porthome_index_entry;

typedef enum {
    PORTHOME_INDEX_OK = 0,
    PORTHOME_INDEX_ERR_ARG = -1,
    PORTHOME_INDEX_NOT_FOUND = -2,
    PORTHOME_INDEX_ERR_STORAGE = -3,
    PORTHOME_INDEX_ERR_OOM = -4
} porthome_index_status;

/* Entry is borrowed for the duration of the visit. A nonzero result stops
 * iteration and is returned unchanged. Do not mutate the index in a visitor. */
typedef porthome_index_status (*porthome_index_visit_fn)(
    void *ctx, const porthome_index_entry *entry);

typedef struct {
    porthome_index_status (*lookup)(void *ctx, const char *git_oid,
                                    porthome_index_entry *out);
    /* Upsert by git_oid. Copy entry before returning; do not retain it. */
    porthome_index_status (*insert)(void *ctx, const porthome_index_entry *entry);
    porthome_index_status (*iterate)(void *ctx, porthome_index_visit_fn visit,
                                     void *visit_ctx);
} porthome_index_ops;

/* Borrowed backend and vtable; caller owns their lifetime and synchronization.
 * Backends own persistence/transactions. libporthome never opens a database. */
typedef struct {
    void *ctx;
    const porthome_index_ops *ops;
} porthome_index;

PORTHOME_API porthome_index_status porthome_index_lookup(
    const porthome_index *index, const char *git_oid, porthome_index_entry *out);
PORTHOME_API porthome_index_status porthome_index_insert(
    const porthome_index *index, const porthome_index_entry *entry);
PORTHOME_API porthome_index_status porthome_index_iterate(
    const porthome_index *index, porthome_index_visit_fn visit, void *visit_ctx);

#ifdef __cplusplus
}
#endif
#endif /* PORTHOME_INDEX_H */
