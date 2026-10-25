/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/tree.h>
#include <limits.h>

static const uint8_t *key_at(const porthome_ring_t *r, uint64_t epoch)
{
    for (size_t i = 0; i < r->count; ++i)
        if (r->entries[i].epoch == epoch)
            return r->entries[i].node_key;
    return NULL;
}
int vault_chunk_read(const porthome_node_chunk *chunk, const porthome_ring_t *ring,
                     const vault_blob_io *io, uint8_t **out, size_t *out_len)
{
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!chunk || !io || !io->read || !out || !out_len)
        return VAULT_ERR_ARG;
    int valid = vault_ring_valid(ring);
    if (valid)
        return valid;
    if (chunk->size_plain > SIZE_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    size_t length = (size_t)chunk->size_plain;
    const uint8_t *key = key_at(ring, chunk->chunk_key_id);
    if (!key)
        return VAULT_ERR_EPOCH;
    /* Generation cuts at 1 MiB; historical/resealed v1 chunks may be larger.
     * Match the reseal task's supported AEAD length bound. */
    if (length > INT_MAX || chunk->size_sealed > INT_MAX + (uint64_t)PORTHOME_SEAL_OVERHEAD)
        return PORTHOME_ERR_TOO_LARGE;
    uint8_t *blob = NULL, *plain = NULL, *canonical = NULL, hash[32];
    size_t len = 0, plain_len = 0, canonical_len = 0;
    int rc = io->read(io->ctx, chunk->sha256, &blob, &len) ? VAULT_ERR_IO : 0;
    if (rc)
        goto done;
    if (!blob || len != chunk->size_sealed || len < PORTHOME_SEAL_OVERHEAD) {
        rc = VAULT_ERR_IO;
        goto done;
    }
    rc = porthome_sha256(blob, len, hash);
    if (rc)
        goto done;
    if (CRYPTO_memcmp(hash, chunk->sha256, 32)) {
        rc = PORTHOME_ERR_HASH_MISMATCH;
        goto done;
    }
    if (blob[0] == PORTHOME_WIRE_VERSION_V2) {
        rc = porthome_decrypt_chunk_v2(ring, chunk->chunk_key_id, blob, len, length, &plain,
                                       &plain_len);
        if (!rc)
            rc = porthome_encrypt_chunk_v2(key, plain, plain_len, &canonical, &canonical_len, NULL);
    } else {
        rc = porthome_decrypt_chunk(key, blob, len, &plain, &plain_len);
        if (!rc && plain_len != length)
            rc = VAULT_ERR_ARG;
        if (!rc)
            rc = porthome_encrypt_chunk(key, plain, plain_len, &canonical, &canonical_len, NULL);
    }
    if (!rc && (canonical_len != len || CRYPTO_memcmp(canonical, blob, len)))
        rc = VAULT_ERR_ARG;
    if (!rc) {
        *out = plain;
        *out_len = plain_len;
        plain = NULL;
    }
done:
    if (plain)
        OPENSSL_cleanse(plain, plain_len);
    free(plain);
    free(blob);
    free(canonical);
    return rc;
}
int vault_tree_check(const vault_tree *t, const vault_blob_io *io, vault_tree_check_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !io || !io->read)
        return VAULT_ERR_ARG;
    int rc = vault_tree_validate(t);
    if (rc)
        return rc;
    for (size_t i = 0; i < t->count; ++i) {
        const porthome_dir_node *n = t->nodes[i].node;
        ++out->nodes;
        for (size_t j = 0; j < n->entries_len; ++j) {
            const porthome_node_entry *e = &n->entries[j];
            if (e->kind != PORTHOME_KIND_FILE)
                continue;
            uint64_t total = 0;
            for (size_t k = 0; k < e->chunks_len; ++k) {
                if (e->chunks[k].size_plain > SIZE_MAX)
                    return PORTHOME_ERR_TOO_LARGE;
                size_t length = (size_t)e->chunks[k].size_plain;
                porthome_ring_t ring = vault_ring_view(&t->nodes[i].ring);
                uint8_t *plain = NULL;
                size_t plain_len = 0;
                rc = vault_chunk_read(&e->chunks[k], &ring, io, &plain, &plain_len);
                if (rc)
                    return rc;
                if (plain)
                    OPENSSL_cleanse(plain, plain_len);
                free(plain);
                if (UINT64_MAX - total < length || UINT64_MAX - out->size_plain < length ||
                    UINT64_MAX - out->size_sealed < e->chunks[k].size_sealed)
                    return PORTHOME_ERR_TOO_LARGE;
                total += length;
                out->size_plain += length;
                out->size_sealed += e->chunks[k].size_sealed;
                ++out->chunks;
            }
            if (total != e->size_plain)
                return VAULT_ERR_ARG;
        }
    }
    return 0;
}
