/* SPDX-License-Identifier: MIT */
#include "internal.h"

static int append(porthome_head_parent *parents, size_t *count, const porthome_head *h,
                  const uint8_t *writer)
{
    if (h->parents_len > PORTHOME_HEAD_MAX_PARENTS || (h->parents_len && !h->parents))
        return VAULT_ERR_ARG;
    if (h->parents_len) {
        memcpy(parents + *count, h->parents, h->parents_len * sizeof(*parents));
        *count += h->parents_len;
    }
    if (writer) {
        porthome_head_parent *p = &parents[(*count)++];
        p->seq = h->seq;
        memcpy(p->writer, writer, 32);
        memcpy(p->root_manifest, h->root_manifest, 32);
    }
    return 0;
}
static int compare(const void *a, const void *b)
{
    const porthome_head_parent *x = a, *y = b;
    int c = memcmp(x->writer, y->writer, 32);
    if (c)
        return c;
    if (x->seq != y->seq)
        return x->seq > y->seq ? -1 : 1;
    return memcmp(x->root_manifest, y->root_manifest, 32);
}
int vault_head_cover(porthome_head *h, const vault_record *const *records, size_t count)
{
    if (!h || (count && !records) || count > VAULT_MAX_HEADS)
        return VAULT_ERR_ARG;
    size_t cap = (count + 1) * (PORTHOME_HEAD_MAX_PARENTS + 1);
    porthome_head_parent *parents = malloc(cap * sizeof(*parents));
    if (!parents)
        return VAULT_ERR_OOM;
    size_t n = 0;
    int rc = append(parents, &n, h, NULL);
    for (size_t i = 0; !rc && i < count; ++i) {
        if (!records[i] || memcmp(h->vault_id, records[i]->head->vault_id, 32))
            rc = VAULT_ERR_ARG;
        else
            rc = append(parents, &n, records[i]->head, records[i]->writer);
    }
    if (rc)
        goto done;
    qsort(parents, n, sizeof(*parents), compare);
    /* Check ALL equal-sequence tuples before reducing coverage. Otherwise a
     * higher sequence seen first would conceal a lower-sequence root fork. */
    for (size_t i = 1; i < n; ++i)
        if (!memcmp(parents[i - 1].writer, parents[i].writer, 32) &&
            parents[i - 1].seq == parents[i].seq &&
            memcmp(parents[i - 1].root_manifest, parents[i].root_manifest, 32)) {
            rc = VAULT_ERR_CONFLICT;
            goto done;
        }
    size_t writers = 0;
    for (size_t i = 0; i < n; ++i)
        if (!writers || memcmp(parents[writers - 1].writer, parents[i].writer, 32))
            parents[writers++] = parents[i];
    if (writers > PORTHOME_HEAD_MAX_PARENTS) {
        rc = PORTHOME_ERR_TOO_LARGE;
        goto done;
    }
    free(h->parents);
    h->parents = parents;
    h->parents_len = writers;
    return 0;
done:
    free(parents);
    return rc;
}
int vault_head_include(porthome_head *h, const porthome_head *previous, const uint8_t writer[32])
{
    if (!h || !previous || !writer)
        return VAULT_ERR_ARG;
    vault_record record = {.head = (porthome_head *)previous};
    memcpy(record.writer, writer, 32);
    const vault_record *records[] = {&record};
    return vault_head_cover(h, records, 1);
}
