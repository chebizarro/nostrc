/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <openssl/rand.h>
int vault_ring_valid(const porthome_ring_t *r)
{
    if (!r || !r->count || !r->entries)
        return VAULT_ERR_ARG;
    if (r->count > VAULT_MAX_EPOCHS)
        return PORTHOME_ERR_TOO_LARGE;
    uint64_t last = 0;
    for (size_t i = 0; i < r->count; ++i) {
        if (r->entries[i].epoch <= last)
            return VAULT_ERR_ARG;
        last = r->entries[i].epoch;
        for (size_t j = 0; j < i; ++j)
            if (!CRYPTO_memcmp(r->entries[i].node_key, r->entries[j].node_key, 32))
                return VAULT_ERR_ARG;
    }
    return 0;
}
porthome_ring_t vault_ring_view(const vault_ring *r)
{
    porthome_ring_t v = {r ? r->entries : NULL, r ? r->count : 0};
    return v;
}
const porthome_ring_entry_t *vault_ring_latest(const vault_ring *r)
{
    porthome_ring_t v = vault_ring_view(r);
    return vault_ring_valid(&v) ? NULL : &r->entries[r->count - 1];
}
int vault_ring_copy(vault_ring *out, const porthome_ring_t *r)
{
    if (!out)
        return VAULT_ERR_ARG;
    int rc = vault_ring_valid(r);
    if (rc)
        return rc;
    porthome_ring_entry_t *p = malloc(r->count * sizeof(*p));
    if (!p)
        return VAULT_ERR_OOM;
    memcpy(p, r->entries, r->count * sizeof(*p));
    out->entries = p;
    out->count = r->count;
    return 0;
}
int vault_ring_init(vault_ring *out, uint64_t epoch, const uint8_t key[32])
{
    if (!out || !key || !epoch)
        return VAULT_ERR_ARG;
    porthome_ring_entry_t e = {.epoch = epoch};
    memcpy(e.node_key, key, 32);
    porthome_ring_t v = {&e, 1};
    int rc = vault_ring_copy(out, &v);
    OPENSSL_cleanse(&e, sizeof(e));
    return rc;
}
void vault_ring_dispose(vault_ring *r)
{
    if (!r)
        return;
    if (r->entries)
        OPENSSL_cleanse(r->entries, r->count * sizeof(*r->entries));
    free(r->entries);
    memset(r, 0, sizeof(*r));
}
int vault_ring_append(vault_ring *r, const uint8_t key[32], uint64_t *epoch)
{
    if (epoch)
        *epoch = 0;
    const porthome_ring_entry_t *last = vault_ring_latest(r);
    if (!last || !key)
        return VAULT_ERR_ARG;
    if (last->epoch == UINT64_MAX || r->count == VAULT_MAX_EPOCHS)
        return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < r->count; ++i)
        if (!CRYPTO_memcmp(r->entries[i].node_key, key, 32))
            return VAULT_ERR_ARG;
    uint64_t next = last->epoch + 1;
    porthome_ring_entry_t *p = malloc((r->count + 1) * sizeof(*p));
    if (!p)
        return VAULT_ERR_OOM;
    memcpy(p, r->entries, r->count * sizeof(*p));
    p[r->count].epoch = next;
    memcpy(p[r->count].node_key, key, 32);
    OPENSSL_cleanse(r->entries, r->count * sizeof(*p));
    free(r->entries);
    r->entries = p;
    ++r->count;
    if (epoch)
        *epoch = next;
    return 0;
}
int vault_ring_append_random(vault_ring *r, uint64_t *epoch)
{
    if (epoch)
        *epoch = 0;
    uint8_t key[32];
    int rc = RAND_bytes(key, 32) == 1 ? vault_ring_append(r, key, epoch) : PORTHOME_ERR_CRYPTO;
    OPENSSL_cleanse(key, 32);
    return rc;
}
int vault_ring_drop_before(vault_ring *r, uint64_t epoch)
{
    if (!vault_ring_latest(r))
        return VAULT_ERR_ARG;
    size_t i = 0;
    while (i < r->count && r->entries[i].epoch != epoch)
        ++i;
    if (i == r->count)
        return VAULT_ERR_EPOCH;
    size_t keep = r->count - i;
    memmove(r->entries, r->entries + i, keep * sizeof(*r->entries));
    OPENSSL_cleanse(r->entries + keep, i * sizeof(*r->entries));
    r->count = keep;
    return 0;
}
int vault_node_clone(const porthome_dir_node *n, porthome_dir_node **out)
{
    uint8_t *b = NULL;
    size_t z = 0;
    int rc = porthome_dir_node_encode(n, &b, &z);
    if (!rc)
        rc = porthome_dir_node_decode(b, z, out);
    if (b)
        OPENSSL_cleanse(b, z);
    free(b);
    return rc;
}
int vault_head_clone(const porthome_head *h, porthome_head **out)
{
    uint8_t *b = NULL;
    size_t z = 0;
    int rc = porthome_head_encode(h, &b, &z);
    if (!rc)
        rc = porthome_head_decode(b, z, out);
    if (b)
        OPENSSL_cleanse(b, z);
    free(b);
    return rc;
}
