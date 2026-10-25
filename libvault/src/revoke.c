/* SPDX-License-Identifier: MIT */
#include "internal.h"
static void remove_member(uint8_t (*keys)[32], size_t *len, const uint8_t member[32])
{
    size_t out = 0;
    for (size_t i = 0; i < *len; ++i)
        if (memcmp(keys[i], member, 32))
            memmove(keys[out++], keys[i], 32);
    *len = out;
}
void vault_revoke_result_free(vault_revoke_result *r)
{
    if (!r)
        return;
    vault_ring_dispose(&r->ring);
    if (r->node) {
        porthome_dir_node_dispose(r->node);
        free(r->node);
    }
    if (r->head) {
        porthome_head_dispose(r->head);
        free(r->head);
    }
    free(r->sealed);
    free(r->head_payload);
    OPENSSL_cleanse(r, sizeof(*r));
    free(r);
}
int vault_revoke(const uint8_t actor[32], const uint8_t member[32], const porthome_dir_node *node,
                 const vault_ring *ring, const porthome_node_entry *parent,
                 const porthome_head *head, uint64_t written_at, vault_revoke_result **out)
{
    if (out)
        *out = NULL;
    const porthome_ring_entry_t *old = vault_ring_latest(ring);
    if (!out || !actor || !member || !node || !node->vault || !old || (!parent == !head) ||
        written_at > INT64_MAX)
        return VAULT_ERR_ARG;
    if (memcmp(actor, node->vault->owner, 32) || !memcmp(member, actor, 32))
        return VAULT_ERR_AUTH;
    if (old->epoch != node->epoch)
        return VAULT_ERR_EPOCH;
    if (node->vault->grants_rev == UINT64_MAX || old->epoch == UINT64_MAX ||
        (head && head->seq == UINT64_MAX))
        return PORTHOME_ERR_TOO_LARGE;
    if (parent && (parent->kind != PORTHOME_KIND_DIR || parent->child_epoch != node->epoch ||
                   memcmp(parent->node_id, node->node_id, 32) || parent->chunks ||
                   parent->chunks_len || parent->symlink_target))
        return VAULT_ERR_ARG;
    if (head && (head->epoch != node->epoch || memcmp(head->vault_id, node->vault->vault_id, 32) ||
                 head->grants_rev != node->vault->grants_rev))
        return VAULT_ERR_ARG;
    uint8_t *prior = NULL, hash[32];
    size_t prior_len = 0;
    int rc = porthome_dir_node_seal(node, old->node_key, &prior, &prior_len, hash);
    free(prior);
    if (rc)
        return rc;
    if (CRYPTO_memcmp(hash, parent ? parent->manifest_sha256 : head->root_manifest, 32))
        return PORTHOME_ERR_HASH_MISMATCH;
    vault_revoke_result *r = calloc(1, sizeof(*r));
    if (!r)
        return VAULT_ERR_OOM;
    porthome_ring_t view = vault_ring_view(ring);
    rc = vault_ring_copy(&r->ring, &view);
    if (rc)
        goto fail;
    rc = vault_node_clone(node, &r->node);
    if (rc)
        goto fail;
    uint64_t epoch;
    rc = vault_ring_append_random(&r->ring, &epoch);
    if (rc)
        goto fail;
    porthome_vault_info *v = r->node->vault;
    remove_member(v->writers, &v->writers_len, member);
    remove_member(v->readers, &v->readers_len, member);
    ++v->grants_rev;
    r->node->epoch = epoch;
    const uint8_t *fresh = r->ring.entries[r->ring.count - 1].node_key;
    rc = porthome_dir_node_seal(r->node, fresh, &r->sealed, &r->sealed_len, r->address);
    if (rc)
        goto fail;
    if (parent) {
        r->has_parent = 1;
        r->parent = *parent;
        r->parent.has_key_override = 1;
        r->parent.child_epoch = epoch;
        memcpy(r->parent.key_override, fresh, 32);
        memcpy(r->parent.manifest_sha256, r->address, 32);
    } else {
        rc = vault_head_clone(head, &r->head);
        if (rc)
            goto fail;
        free(r->head->parents);
        r->head->parents = NULL;
        r->head->parents_len = 0;
        r->head->parents = calloc(1, sizeof(*r->head->parents));
        if (!r->head->parents) {
            rc = VAULT_ERR_OOM;
            goto fail;
        }
        r->head->parents_len = 1;
        memcpy(r->head->parents[0].writer, actor, 32);
        r->head->parents[0].seq = head->seq;
        memcpy(r->head->parents[0].root_manifest, head->root_manifest, 32);
        r->head->epoch = epoch;
        r->head->seq = head->seq + 1;
        r->head->grants_rev = v->grants_rev;
        r->head->written_at = written_at;
        memcpy(r->head->root_manifest, r->address, 32);
        r->head->has_key_override = 1;
        memcpy(r->head->key_override, fresh, 32);
        rc = porthome_head_seal(r->head, fresh, &r->head_payload);
        if (rc)
            goto fail;
    }
    *out = r;
    return 0;
fail:
    vault_revoke_result_free(r);
    return rc;
}
