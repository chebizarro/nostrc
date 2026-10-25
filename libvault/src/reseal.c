/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <nostr/nip44/padding.h>
#include <limits.h>
typedef struct {
    vault_reseal_item item;
    porthome_node_chunk old;
    uint64_t target, cost;
    uint8_t key[32];
} job;
struct vault_reseal_task {
    job *jobs;
    size_t count, done;
    uint64_t reserved;
    int eager, failed, pending;
    uint8_t address[32];
};
static const porthome_ring_entry_t *find_key(const vault_ring *r, uint64_t epoch)
{
    for (size_t i = 0; i < r->count; ++i)
        if (r->entries[i].epoch == epoch)
            return &r->entries[i];
    return NULL;
}
static int job_compare(const void *a, const void *b)
{
    const job *x = *(const job *const *)a, *y = *(const job *const *)b;
    uintptr_t p = (uintptr_t)x->item.node, q = (uintptr_t)y->item.node;
    if (p != q)
        return p < q ? -1 : 1;
    if (x->item.entry != y->item.entry)
        return x->item.entry < y->item.entry ? -1 : 1;
    return x->item.chunk == y->item.chunk ? 0 : (x->item.chunk < y->item.chunk ? -1 : 1);
}
void vault_reseal_task_free(vault_reseal_task *t)
{
    if (!t)
        return;
    if (t->jobs)
        OPENSSL_cleanse(t->jobs, t->count * sizeof(*t->jobs));
    free(t->jobs);
    OPENSSL_cleanse(t, sizeof(*t));
    free(t);
}
int vault_reseal_task_new(const vault_reseal_item *items, size_t count, int eager,
                          vault_reseal_task **out)
{
    if (out)
        *out = NULL;
    if (!out || (count && !items) || (eager != 0 && eager != 1))
        return VAULT_ERR_ARG;
    if (count > SIZE_MAX / sizeof(job))
        return PORTHOME_ERR_TOO_LARGE;
    vault_reseal_task *t = calloc(1, sizeof(*t));
    if (!t)
        return VAULT_ERR_OOM;
    t->eager = eager;
    if (count && !(t->jobs = calloc(count, sizeof(*t->jobs)))) {
        free(t);
        return VAULT_ERR_OOM;
    }
    int rc = VAULT_ERR_ARG;
    for (size_t i = 0; i < count; ++i) {
        const vault_reseal_item *v = &items[i];
        const porthome_ring_entry_t *latest = vault_ring_latest(v->ring);
        if (!latest || !v->node || v->node->epoch != latest->epoch ||
            v->entry >= v->node->entries_len || !v->node->entries)
            goto fail;
        porthome_node_entry *e = &v->node->entries[v->entry];
        if (e->kind != PORTHOME_KIND_FILE || !e->chunks || v->chunk >= e->chunks_len)
            goto fail;
        porthome_node_chunk old = e->chunks[v->chunk];
        if (old.chunk_key_id > latest->epoch || !find_key(v->ring, old.chunk_key_id)) {
            rc = VAULT_ERR_EPOCH;
            goto fail;
        }
        if (v->size_plain > INT_MAX || old.size_sealed > INT_MAX + (uint64_t)29 ||
            old.size_sealed < 29) {
            rc = PORTHOME_ERR_TOO_LARGE;
            goto fail;
        }
        if (old.chunk_key_id == latest->epoch)
            continue;
        size_t padded = nip44_calc_padded_len(v->size_plain);
        if (padded > INT_MAX) {
            rc = PORTHOME_ERR_TOO_LARGE;
            goto fail;
        }
        job *j = &t->jobs[t->count++];
        j->item = *v;
        j->old = old;
        j->target = latest->epoch;
        memcpy(j->key, latest->node_key, 32);
        j->cost = old.size_sealed + padded + 29;
    }
    if (t->count > 1) {
        job **sorted = malloc(t->count * sizeof(*sorted));
        if (!sorted) {
            rc = VAULT_ERR_OOM;
            goto fail;
        }
        for (size_t i = 0; i < t->count; ++i)
            sorted[i] = &t->jobs[i];
        qsort(sorted, t->count, sizeof(*sorted), job_compare);
        int duplicate = 0;
        for (size_t i = 1; i < t->count; ++i)
            if (!job_compare(&sorted[i - 1], &sorted[i])) {
                duplicate = 1;
                break;
            }
        free(sorted);
        if (duplicate)
            goto fail;
    }
    *out = t;
    return 0;
fail:
    vault_reseal_task_free(t);
    return rc;
}
static int run_job(vault_reseal_task *t, job *j, const vault_blob_io *io)
{
    const porthome_ring_entry_t *latest = vault_ring_latest(j->item.ring);
    porthome_node_chunk *dest = &j->item.node->entries[j->item.entry].chunks[j->item.chunk];
    if (!latest || latest->epoch != j->target || CRYPTO_memcmp(latest->node_key, j->key, 32) ||
        memcmp(dest, &j->old, sizeof(*dest)))
        return VAULT_ERR_ARG;
    uint8_t *blob = NULL, *plain = NULL, *check = NULL, *fresh = NULL, hash[32];
    size_t len = 0, plain_len = 0, check_len = 0, fresh_len = 0;
    int rc = io->read(io->ctx, j->old.sha256, &blob, &len) ? VAULT_ERR_IO : 0;
    if (rc)
        goto done;
    if (!blob || len != j->old.size_sealed) {
        rc = VAULT_ERR_IO;
        goto done;
    }
    rc = porthome_sha256(blob, len, hash);
    if (rc)
        goto done;
    if (CRYPTO_memcmp(hash, j->old.sha256, 32)) {
        rc = PORTHOME_ERR_HASH_MISMATCH;
        goto done;
    }
    porthome_ring_t view = vault_ring_view(j->item.ring);
    const porthome_ring_entry_t *old = find_key(j->item.ring, j->old.chunk_key_id);
    if (!old) {
        rc = VAULT_ERR_EPOCH;
        goto done;
    }
    if (blob[0] == PORTHOME_WIRE_VERSION_V2) {
        rc = porthome_decrypt_chunk_v2(&view, j->old.chunk_key_id, blob, len, j->item.size_plain,
                                       &plain, &plain_len);
        if (!rc)
            rc = porthome_encrypt_chunk_v2(old->node_key, plain, plain_len, &check, &check_len,
                                           hash);
    } else {
        rc = porthome_decrypt_chunk(old->node_key, blob, len, &plain, &plain_len);
        if (!rc && plain_len != j->item.size_plain)
            rc = VAULT_ERR_ARG;
        if (!rc)
            rc = porthome_encrypt_chunk(old->node_key, plain, plain_len, &check, &check_len, hash);
    }
    if (rc)
        goto done;
    /* Canonical re-seal binds the supplied length even for trailing-zero data. */
    if (check_len != len || CRYPTO_memcmp(hash, j->old.sha256, 32)) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    rc = porthome_encrypt_chunk_v2(j->key, plain, plain_len, &fresh, &fresh_len, hash);
    if (rc)
        goto done;
    if (fresh_len + j->old.size_sealed != j->cost) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    memcpy(t->address, hash, 32);
    t->pending = 1;
    if (io->put(io->ctx, fresh, fresh_len, hash)) {
        rc = VAULT_ERR_IO;
        goto done;
    }
    memcpy(dest->sha256, hash, 32);
    dest->size_sealed = fresh_len;
    dest->chunk_key_id = j->target;
    t->pending = 0;
done:
    if (plain)
        OPENSSL_cleanse(plain, plain_len);
    free(plain);
    free(blob);
    free(check);
    free(fresh);
    return rc;
}
int vault_reseal_task_run(vault_reseal_task *t, uint64_t budget, const vault_blob_io *io)
{
    if (!t || !io || !io->read || !io->put)
        return VAULT_ERR_ARG;
    if (t->failed)
        return t->failed;
    if (t->eager) {
        uint64_t need = 0;
        for (size_t i = t->done; i < t->count; ++i) {
            if (UINT64_MAX - need < t->jobs[i].cost)
                return PORTHOME_ERR_TOO_LARGE;
            need += t->jobs[i].cost;
        }
        if (need > budget)
            return VAULT_ERR_BUDGET;
    }
    while (t->done < t->count) {
        job *j = &t->jobs[t->done];
        if (j->cost > budget)
            return VAULT_ERR_BUDGET;
        if (UINT64_MAX - t->reserved < j->cost)
            return PORTHOME_ERR_TOO_LARGE;
        budget -= j->cost;
        t->reserved += j->cost;
        int rc = run_job(t, j, io);
        if (rc) {
            t->failed = rc;
            return rc;
        }
        ++t->done;
    }
    return 0;
}
size_t vault_reseal_task_completed(const vault_reseal_task *t) { return t ? t->done : 0; }
uint64_t vault_reseal_task_reserved(const vault_reseal_task *t) { return t ? t->reserved : 0; }
const uint8_t *vault_reseal_task_pending_address(const vault_reseal_task *t)
{
    return t && t->pending ? t->address : NULL;
}
