/* SPDX-License-Identifier: MIT */
#include <porthome/index.h>
#include <stddef.h>

static int valid_oid(const char *oid)
{
    if (!oid) return 0;
    size_t n = 0;
    while (n < 65 && oid[n]) {
        if (!((oid[n] >= '0' && oid[n] <= '9') ||
              (oid[n] >= 'a' && oid[n] <= 'f'))) return 0;
        ++n;
    }
    return n == 40 || n == 64;
}

porthome_index_status porthome_index_lookup(const porthome_index *index,
    const char *git_oid, porthome_index_entry *out)
{
    if (!index || !index->ops || !index->ops->lookup || !out || !valid_oid(git_oid))
        return PORTHOME_INDEX_ERR_ARG;
    return index->ops->lookup(index->ctx, git_oid, out);
}

porthome_index_status porthome_index_insert(const porthome_index *index,
    const porthome_index_entry *entry)
{
    if (!index || !index->ops || !index->ops->insert || !entry || !valid_oid(entry->git_oid))
        return PORTHOME_INDEX_ERR_ARG;
    return index->ops->insert(index->ctx, entry);
}

porthome_index_status porthome_index_iterate(const porthome_index *index,
    porthome_index_visit_fn visit, void *visit_ctx)
{
    if (!index || !index->ops || !index->ops->iterate || !visit)
        return PORTHOME_INDEX_ERR_ARG;
    return index->ops->iterate(index->ctx, visit, visit_ctx);
}
