/*
 * libmarmot - MLS Group State Machine (RFC 9420 §11, §12)
 *
 * Full group lifecycle: create, add/remove members, self-update,
 * process commits, encrypt/decrypt application messages.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_group.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Internal helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Derive the commit secret from a path secret.
 * commit_secret = DeriveSecret(path_secret, "path")
 * For no-path commits, commit_secret is all zeros.
 */
static int
hpke_encrypt_with_label(uint8_t enc[MLS_KEM_ENC_LEN],
                        uint8_t *ct, size_t *ct_len,
                        const uint8_t pk[MLS_KEM_PK_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len,
                        const uint8_t *pt, size_t pt_len);
static int
hpke_decrypt_with_label(uint8_t *pt, size_t *pt_len,
                        const uint8_t enc[MLS_KEM_ENC_LEN],
                        const uint8_t sk[MLS_KEM_SK_LEN],
                        const uint8_t pk[MLS_KEM_PK_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len,
                        const uint8_t *ct, size_t ct_len);

static int
derive_commit_secret(const uint8_t *path_secret, bool has_path,
                     uint8_t out[MLS_HASH_LEN])
{
    if (!has_path || !path_secret) {
        memset(out, 0, MLS_HASH_LEN);
        return 0;
    }
    return mls_crypto_derive_secret(out, path_secret, "path");
}

static int
replace_parent_hash(uint8_t **field, size_t *field_len,
                    const uint8_t hash[MLS_HASH_LEN])
{
    if (!field || !field_len || !hash) return -1;
    uint8_t *copy = malloc(MLS_HASH_LEN);
    if (!copy) return -1;
    memcpy(copy, hash, MLS_HASH_LEN);
    free(*field);
    *field = copy;
    *field_len = MLS_HASH_LEN;
    return 0;
}

static void
remember_resumption_psk(MlsGroup *group, uint64_t epoch,
                        const uint8_t psk[MLS_HASH_LEN]);
static int
lookup_own_path_key(const MlsGroup *group, uint32_t node,
                    const uint8_t **out_sk, const uint8_t **out_pk);
static int
remember_own_path_key(MlsGroup *group, uint32_t node,
                      const uint8_t sk[MLS_KEM_SK_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN]);
static void
prune_own_path_keys(MlsGroup *group);
static int
apply_group_context_extensions(MlsGroup *group,
                               const uint8_t *extensions,
                               size_t extensions_len);
static int
group_context_extensions_validate(const MlsGroup *group,
                                  const uint8_t *extensions, size_t extensions_len,
                                  uint32_t replaced_leaf);

static int
child_below_path_node(uint32_t sender_leaf, uint32_t n_leaves,
                      uint32_t path_node, uint32_t *out_child)
{
    if (!out_child || sender_leaf >= n_leaves) return -1;
    uint32_t child = mls_tree_leaf_to_node(sender_leaf);
    uint32_t root = mls_tree_root(n_leaves);
    while (child != root) {
        uint32_t parent = mls_tree_parent(child, n_leaves);
        if (parent == path_node) {
            *out_child = child;
            return 0;
        }
        child = parent;
    }
    return -1;
}

/**
 * RFC 9420 §12.1.1: a leaf added at `leaf_node_idx` becomes an unmerged leaf
 * of every non-blank parent on its direct path.  Shared by the Add producer
 * and the Commit processor so both build the same tree.
 */
static int
tree_add_unmerged_leaf(MlsRatchetTree *tree, uint32_t leaf_node_idx)
{
    uint32_t dp[64];
    uint32_t dp_len = 0;
    if (mls_tree_direct_path(leaf_node_idx, tree->n_leaves, dp, 64, &dp_len) != 0)
        return -1;
    uint32_t leaf = mls_tree_node_to_leaf(leaf_node_idx);
    for (uint32_t j = 0; j < dp_len; j++) {
        MlsNode *parent = &tree->nodes[dp[j]];
        if (parent->type != MLS_NODE_PARENT) continue;
        uint32_t *leaves = realloc(parent->parent.unmerged_leaves,
                                   (parent->parent.unmerged_leaf_count + 1) *
                                       sizeof(uint32_t));
        if (!leaves) return -1;
        parent->parent.unmerged_leaves = leaves;
        parent->parent.unmerged_leaves[parent->parent.unmerged_leaf_count++] = leaf;
    }
    return 0;
}

/**
 * Resolution of `node_idx` without the leaves in `excluded` (RFC 9420
 * §12.4.2: UpdatePath secrets are not encrypted to leaves added by the same
 * Commit, and receivers index the HPKECiphertexts over this reduced list).
 * Committer, receiver and the ciphertext-count check all use this one
 * definition so their indices cannot disagree.
 */
static int
resolution_excluding(const MlsRatchetTree *tree, uint32_t node_idx,
                     const uint32_t *excluded, size_t excluded_count,
                     uint32_t *out, uint32_t max_len, uint32_t *out_len)
{
    if (excluded_count > 0 && !excluded) return -1;
    if (mls_tree_resolution(tree, node_idx, out, max_len, out_len) != 0)
        return -1;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < *out_len; i++) {
        bool drop = false;
        if (mls_tree_is_leaf(out[i])) {
            uint32_t leaf = mls_tree_node_to_leaf(out[i]);
            for (size_t j = 0; j < excluded_count && !drop; j++)
                drop = excluded[j] == leaf;
        }
        if (!drop) out[kept++] = out[i];
    }
    *out_len = kept;
    return 0;
}

/**
 * Derive epoch secrets and re-initialize the secret tree for the current
 * group state. Updates group->epoch_secrets and group->secret_tree.
 */
static int
group_derive_epoch(MlsGroup *group,
                   const uint8_t *init_secret_prev,
                   const uint8_t commit_secret[MLS_HASH_LEN],
                   const uint8_t psk_secret[MLS_HASH_LEN])
{
    /* Build GroupContext */
    uint8_t tree_hash[MLS_HASH_LEN];
    if (mls_group_tree_hash(group, tree_hash) != 0)
        return -1;

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    if (mls_group_context_serialize(
            group->group_id, group->group_id_len,
            group->epoch, tree_hash,
            group->confirmed_transcript_hash,
            group->extensions_data, group->extensions_len,
            &gc_data, &gc_len) != 0)
        return -1;

    /* Derive epoch secrets */
    MlsEpochSecrets new_secrets;
    int rc = mls_key_schedule_derive(init_secret_prev, commit_secret,
                                     gc_data, gc_len, psk_secret, &new_secrets);
    free(gc_data);
    if (rc != 0)
        return rc;

    /* Free old secret tree and install new one */
    mls_secret_tree_free(&group->secret_tree);
    memcpy(&group->epoch_secrets, &new_secrets, sizeof(new_secrets));

    /* The secret tree has the canonical (truncated) tree's structure, the
     * view tree_hash is computed over (RFC 9420 sections 7.7, 9; OpenMLS):
     * sized by the live width, a member whose right edge was removed derived
     * other leaf secrets than OpenMLS and read none of its messages
     * (nostrc-qp24.5.1.3, found by the MDK v0.11.0 SelfRemove vector). */
    rc = mls_secret_tree_init(&group->secret_tree, new_secrets.encryption_secret,
                              mls_tree_canonical_leaves(&group->tree));
    /* The tree holds every leaf secret now: the root is deleted (RFC 9420
     * §9.2), so no consumed message key can be derived again from it. */
    sodium_memzero(group->epoch_secrets.encryption_secret, MLS_HASH_LEN);
    sodium_memzero(&new_secrets, sizeof(new_secrets));
    return rc == 0 ? 0 : -1;
}

/**
 * Generate an UpdatePath for the committer. Produces:
 *   - New leaf node with fresh encryption key
 *   - Path secrets encrypted for each copath resolution member, except the
 *     `added_leaves` this Commit adds (RFC 9420 §12.4.2; they learn the
 *     epoch from the Welcome)
 *   - The path_secret at the root (used to derive commit_secret)
 *   - Optionally, the path_secret of each of `secret_nodes` (which must lie
 *     on the filtered direct path) into out_node_secrets[i]: the lowest
 *     common ancestor a Welcome hands to each joiner (RFC 9420 §12.4.3.1,
 *     nostrc-il4i)
 */
static int
generate_update_path(MlsGroup *group,
                     const uint32_t *added_leaves, size_t added_leaf_count,
                     const uint32_t *secret_nodes, size_t secret_count,
                     uint8_t (*out_node_secrets)[MLS_HASH_LEN],
                     MlsUpdatePath *path_out,
                     uint8_t root_path_secret[MLS_HASH_LEN])
{
    if (secret_count > 0 && (!secret_nodes || !out_node_secrets)) return -1;
    /* The committer's credential is carried over from its current leaf. */
    const MlsLeafNode *cur_leaf =
        &group->tree.nodes[mls_tree_leaf_to_node(group->own_leaf_index)].leaf;
    const uint8_t *credential_identity = cur_leaf->credential_identity;
    size_t cred_len = cur_leaf->credential_identity_len;
    uint32_t n_leaves = group->tree.n_leaves;
    uint8_t *path_context = NULL;
    size_t path_context_len = 0;
    memset(path_out, 0, sizeof(*path_out));

    /* Compute the filtered direct path (RFC 9420 §4.1.2).  The UpdatePath
     * carries exactly one node per filtered node; it is empty when every
     * copath subtree is blank (the committer is the only member left). */
    uint32_t fdp[64];
    uint32_t fdp_len = 0;
    if (mls_tree_filtered_direct_path(&group->tree, group->own_leaf_index,
                                       fdp, 64, &fdp_len) != 0)
        return -1;

    /* Generate new leaf encryption key */
    uint8_t new_enc_sk[MLS_KEM_SK_LEN];
    uint8_t new_enc_pk[MLS_KEM_PK_LEN];
    if (mls_crypto_kem_keygen(new_enc_sk, new_enc_pk) != 0)
        return -1;

    /* Build new leaf node */
    MlsLeafNode *old_leaf = &group->tree.nodes[mls_tree_leaf_to_node(group->own_leaf_index)].leaf;
    memset(&path_out->leaf_node, 0, sizeof(MlsLeafNode));
    memcpy(path_out->leaf_node.encryption_key, new_enc_pk, MLS_KEM_PK_LEN);
    /* Preserve signing key from existing leaf */
    memcpy(path_out->leaf_node.signature_key, old_leaf->signature_key, MLS_SIG_PK_LEN);
    path_out->leaf_node.credential_type = MLS_CREDENTIAL_BASIC;
    if (cred_len > 0 && credential_identity) {
        path_out->leaf_node.credential_identity = malloc(cred_len);
        if (!path_out->leaf_node.credential_identity) goto fail;
        memcpy(path_out->leaf_node.credential_identity, credential_identity, cred_len);
        path_out->leaf_node.credential_identity_len = cred_len;
    } else if (old_leaf->credential_identity_len > 0) {
        path_out->leaf_node.credential_identity = malloc(old_leaf->credential_identity_len);
        if (!path_out->leaf_node.credential_identity) goto fail;
        memcpy(path_out->leaf_node.credential_identity,
               old_leaf->credential_identity, old_leaf->credential_identity_len);
        path_out->leaf_node.credential_identity_len = old_leaf->credential_identity_len;
    }
    path_out->leaf_node.leaf_node_source = MLS_LEAF_NODE_SOURCE_COMMIT;
    /* Capabilities (RFC 9420 §7.2; nostrc-prqu.10): the group's profile's
     * (an adopted leaf must keep advertising app_data_update, nostrc-
     * qp24.5.1). */
    if ((group->profile == MARMOT_GROUP_PROFILE_ADOPTED
             ? mls_leaf_node_set_adopted_capabilities(&path_out->leaf_node)
             : mls_leaf_node_set_marmot_capabilities(&path_out->leaf_node)) != 0)
        goto fail;
    /* The LeafNode extensions carry over, and with the signature key and the
     * credential they bind, so does the account proof: it must not be
     * dropped from a member's leaf (nostrc-7vyi; receivers check). */
    if (old_leaf->extensions_len > 0) {
        path_out->leaf_node.extensions_data = malloc(old_leaf->extensions_len);
        if (!path_out->leaf_node.extensions_data) goto fail;
        memcpy(path_out->leaf_node.extensions_data, old_leaf->extensions_data,
               old_leaf->extensions_len);
        path_out->leaf_node.extensions_len = old_leaf->extensions_len;
    }

    /* Generate path secrets for each filtered direct path node */
    uint8_t path_secrets[64][MLS_HASH_LEN];
    memset(path_secrets, 0, sizeof(path_secrets));
    if (fdp_len == 0) {
        /* Degenerate: single member, no path nodes, but still rotate and
         * sign/install the sender leaf below. */
        mls_crypto_random(root_path_secret, MLS_HASH_LEN);
        path_out->nodes = NULL;
        path_out->node_count = 0;
    } else {
        path_out->nodes = calloc(fdp_len, sizeof(MlsUpdatePathNode));
        if (!path_out->nodes) goto fail;
        path_out->node_count = fdp_len;

        /* Generate a random leaf path secret and derive up the tree */
        mls_crypto_random(path_secrets[0], MLS_HASH_LEN);

        for (uint32_t i = 1; i < fdp_len; i++) {
            /* path_secret[i] = DeriveSecret(path_secret[i-1], "path") */
            if (mls_crypto_derive_secret(path_secrets[i], path_secrets[i - 1], "path") != 0)
                goto fail;
        }

        /* The root path secret is the last one */
        memcpy(root_path_secret, path_secrets[fdp_len - 1], MLS_HASH_LEN);
    }
    for (size_t j = 0; j < secret_count; j++) {
        uint32_t k = 0;
        while (k < fdp_len && fdp[k] != secret_nodes[j]) k++;
        if (k == fdp_len) goto fail;
        memcpy(out_node_secrets[j], path_secrets[k], MLS_HASH_LEN);
    }

    /* RFC 9420 §7.4/§7.5: blank the whole direct path first, exactly as
     * receivers do in mls_treekem_apply_update_path(); only the filtered
     * nodes get new keys.  A parent over an empty copath (which trees
     * persisted by libmarmot <= 0.3.6 can hold) otherwise stays non-blank at
     * the committer alone, and joiners reject the resulting tree. */
    {
        uint32_t dp[64];
        uint32_t dp_len = 0;
        if (mls_tree_direct_path(mls_tree_leaf_to_node(group->own_leaf_index),
                                 n_leaves, dp, 64, &dp_len) != 0)
            goto fail;
        for (uint32_t i = 0; i < dp_len; i++)
            mls_tree_blank_node(&group->tree.nodes[dp[i]]);
    }

    /* For each node on the filtered direct path, derive the node key and
     * install it in the tree.  Encryption of the path secrets happens below,
     * once the provisional tree (and thus the provisional GroupContext the
     * HPKE payloads are bound to) is complete. */
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t node_idx = fdp[i];

        /* Derive node keypair per RFC 9420 §7.4 / RFC 9180 §7.1.3. */
        uint8_t node_sk[MLS_KEM_SK_LEN];
        uint8_t node_pk[MLS_KEM_PK_LEN];
        if (mls_tree_derive_node_keypair(path_secrets[i], node_sk, node_pk) != 0)
            goto fail;

        memcpy(path_out->nodes[i].encryption_key, node_pk, MLS_KEM_PK_LEN);

        /* The committer keeps every private key it installs on its path
         * (RFC 9420 §7.4): a later Commit may encrypt to this node rather
         * than to our leaf (nostrc-va60). */
        if (remember_own_path_key(group, node_idx, node_sk, node_pk) != 0) {
            sodium_memzero(node_sk, sizeof(node_sk));
            goto fail;
        }

        /* Update the tree node */
        if (group->tree.nodes[node_idx].type != MLS_NODE_BLANK) {
            if (group->tree.nodes[node_idx].type == MLS_NODE_PARENT)
                mls_parent_node_clear(&group->tree.nodes[node_idx].parent);
        }
        group->tree.nodes[node_idx].type = MLS_NODE_PARENT;
        memset(&group->tree.nodes[node_idx].parent, 0, sizeof(MlsParentNode));
        memcpy(group->tree.nodes[node_idx].parent.encryption_key, node_pk, MLS_KEM_PK_LEN);

        sodium_memzero(node_sk, sizeof(node_sk));
    }

    /* Populate parent_hash along the FILTERED direct path (RFC 9420 §7.9),
     * top-down because each value covers the parent_hash of the node above.
     * The topmost filtered node -- the root, or a lower node when the root's
     * copath is blank -- carries an empty parent_hash (installed zeroed
     * above).  Every other node's parent_hash is ParentHash of the next node
     * up the filtered path, taken over that node's child subtree which does
     * not contain the committer; skipped (filtered) levels are not links. */
    for (uint32_t pos = fdp_len; pos-- > 1;) {
        uint32_t parent = fdp[pos];
        uint32_t node_idx = fdp[pos - 1];
        if (group->tree.nodes[node_idx].type != MLS_NODE_PARENT) goto fail;
        uint8_t ph[MLS_HASH_LEN];
        if (mls_tree_parent_hash(&group->tree, parent, node_idx, ph) != 0)
            goto fail;
        if (replace_parent_hash(&group->tree.nodes[node_idx].parent.parent_hash,
                                &group->tree.nodes[node_idx].parent.parent_hash_len,
                                ph) != 0)
            goto fail;
    }

    /* The new leaf links to the bottom filtered node; with an empty filtered
     * path it has no parent link and its parent_hash stays empty. */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    if (fdp_len > 0) {
        uint8_t ph[MLS_HASH_LEN];
        if (mls_tree_parent_hash(&group->tree, fdp[0], own_node, ph) != 0)
            goto fail;
        if (replace_parent_hash(&path_out->leaf_node.parent_hash,
                                &path_out->leaf_node.parent_hash_len,
                                ph) != 0)
            goto fail;
    }
    /* LeafNodeTBS for the commit source binds group_id and our leaf index
     * (RFC 9420 §7.2). */
    if (mls_leaf_node_sign(&path_out->leaf_node, group->own_signature_key,
                           group->group_id, group->group_id_len,
                           group->own_leaf_index) != 0)
        goto fail;

    mls_leaf_node_clear(&group->tree.nodes[own_node].leaf);
    if (mls_leaf_node_clone(&group->tree.nodes[own_node].leaf, &path_out->leaf_node) != 0)
        goto fail;
    group->tree.nodes[own_node].type = MLS_NODE_LEAF;

    /* Update our stored encryption private key */
    memcpy(group->own_encryption_key, new_enc_sk, MLS_KEM_SK_LEN);
    /* Keys of path nodes this UpdatePath replaced are now stale. */
    prune_own_path_keys(group);

    /* UpdatePathNode HPKE binds the provisional GroupContext (RFC 9420 §7.6):
     * next epoch, the tree hash after applying this UpdatePath, and the
     * current confirmed transcript hash.  This mirrors the context receivers
     * rebuild in process_commit_impl before decrypt_path_secret(), so it uses
     * the same canonical tree hash as the GroupContext: after a Remove blanks
     * the right edge, the live tree keeps its width but the hashed tree does
     * not (RFC 9420 §12.1.3 truncation). */
    if (fdp_len > 0) {
        uint8_t provisional_tree_hash[MLS_HASH_LEN];
        if (mls_group_tree_hash(group, provisional_tree_hash) != 0)
            goto fail;
        if (mls_group_context_serialize(group->group_id, group->group_id_len,
                                        group->epoch + 1, provisional_tree_hash,
                                        group->confirmed_transcript_hash,
                                        group->extensions_data, group->extensions_len,
                                        &path_context, &path_context_len) != 0)
            goto fail;
    }

    /* Encrypt each path secret to the resolution of its copath node. */
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t node_idx = fdp[i];

        /* Get the copath node for this direct-path node.  For path[i], this
         * is the sibling of the child immediately below it: the committer leaf
         * for i=0, otherwise the previous direct-path node. */
        uint32_t child_below = UINT32_MAX;
        if (child_below_path_node(group->own_leaf_index, n_leaves,
                                  node_idx, &child_below) != 0)
            goto fail;
        uint32_t sibling = mls_tree_sibling(child_below, n_leaves);

        /* Resolution of the sibling minus the leaves this Commit adds: the
         * nodes we encrypt to, in the order receivers index them. */
        uint32_t resolution[256];
        uint32_t res_len = 0;
        if (resolution_excluding(&group->tree, sibling, added_leaves,
                                 added_leaf_count, resolution, 256, &res_len) != 0)
            goto fail;

        /* Encrypt path_secret to each resolution member's encryption key */
        MlsTlsBuf enc_buf;
        if (mls_tls_buf_init(&enc_buf, 256) != 0) goto fail;

        for (uint32_t j = 0; j < res_len; j++) {
            uint32_t target_node = resolution[j];
            const uint8_t *target_pk = NULL;

            if (mls_tree_is_leaf(target_node)) {
                target_pk = group->tree.nodes[target_node].leaf.encryption_key;
            } else {
                target_pk = group->tree.nodes[target_node].parent.encryption_key;
            }

            uint8_t enc[MLS_KEM_ENC_LEN];
            uint8_t ct[MLS_HASH_LEN + MLS_AEAD_TAG_LEN];
            size_t ct_len = 0;
            if (hpke_encrypt_with_label(enc, ct, &ct_len, target_pk,
                                        "UpdatePathNode", path_context, path_context_len,
                                        path_secrets[i], MLS_HASH_LEN) != 0) {
                mls_tls_buf_free(&enc_buf);
                goto fail;
            }

            /* Write HPKECiphertext: enc || ciphertext */
            if (mls_tls_write_opaque16(&enc_buf, enc, MLS_KEM_ENC_LEN) != 0 ||
                mls_tls_write_opaque16(&enc_buf, ct, ct_len) != 0) {
                mls_tls_buf_free(&enc_buf);
                goto fail;
            }

        }

        path_out->nodes[i].encrypted_path_secrets = enc_buf.data;
        path_out->nodes[i].encrypted_path_secrets_len = enc_buf.len;
        path_out->nodes[i].secret_count = res_len;
        /* Don't free enc_buf — ownership transferred */
    }

    free(path_context);
    sodium_memzero(path_secrets, sizeof(path_secrets));
    sodium_memzero(new_enc_sk, sizeof(new_enc_sk));
    return 0;

fail:
    free(path_context);
    sodium_memzero(new_enc_sk, sizeof(new_enc_sk));
    sodium_memzero(path_secrets, sizeof(path_secrets));
    if (out_node_secrets && secret_count > 0)
        sodium_memzero(out_node_secrets, secret_count * MLS_HASH_LEN);
    mls_update_path_clear(path_out);
    return -1;
}

/**
 * Decrypt a path secret from an UpdatePathNode targeted at us.
 *
 * Finds our position in the resolution, decaps the corresponding
 * HPKECiphertext, and decrypts the path secret.
 */
static int
decrypt_path_secret(const MlsGroup *group,
                    const MlsUpdatePathNode *path_node,
                    uint32_t copath_node_idx,
                    const uint32_t *excluded_leaves,
                    size_t excluded_leaf_count,
                    const uint8_t *group_context, size_t group_context_len,
                    const uint8_t own_enc_sk[MLS_KEM_SK_LEN],
                    const uint8_t own_enc_pk[MLS_KEM_PK_LEN],
                    uint8_t out_path_secret[MLS_HASH_LEN])
{
    /* Our position in the copath resolution minus the added leaves */
    uint32_t resolution[256];
    uint32_t res_len = 0;
    if (resolution_excluding(&group->tree, copath_node_idx, excluded_leaves,
                             excluded_leaf_count, resolution, 256, &res_len) != 0)
        return -1;

    /* Find a resolution entry for which we have the private key. */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    int our_idx = -1;
    const uint8_t *recipient_sk = NULL;
    const uint8_t *recipient_pk = NULL;
    for (uint32_t i = 0; i < res_len; i++) {
        if (resolution[i] == own_node) {
            our_idx = (int)i;
            recipient_sk = own_enc_sk;
            recipient_pk = own_enc_pk;
            break;
        }
        if (lookup_own_path_key(group, resolution[i],
                                &recipient_sk, &recipient_pk) == 0) {
            our_idx = (int)i;
            break;
        }
    }
    if (our_idx < 0 || !recipient_sk || !recipient_pk)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;

    /* Parse the encrypted path secrets to find ours */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, path_node->encrypted_path_secrets,
                        path_node->encrypted_path_secrets_len);

    if ((uint32_t)our_idx >= path_node->secret_count)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;

    /* Skip to our entry */
    for (int i = 0; i < our_idx; i++) {
        uint8_t *skip_enc = NULL, *skip_ct = NULL;
        size_t skip_enc_len = 0, skip_ct_len = 0;
        if (mls_tls_read_opaque16(&reader, &skip_enc, &skip_enc_len) != 0) return -1;
        free(skip_enc);
        if (mls_tls_read_opaque16(&reader, &skip_ct, &skip_ct_len) != 0) return -1;
        free(skip_ct);
    }

    /* Read our HPKECiphertext */
    uint8_t *enc = NULL, *ct = NULL;
    size_t enc_len = 0, ct_len = 0;
    if (mls_tls_read_opaque16(&reader, &enc, &enc_len) != 0) return -1;
    if (mls_tls_read_opaque16(&reader, &ct, &ct_len) != 0) {
        free(enc);
        return -1;
    }

    if (enc_len != MLS_KEM_ENC_LEN) {
        free(enc); free(ct);
        return -1;
    }

    size_t pt_len = 0;
    if (hpke_decrypt_with_label(out_path_secret, &pt_len, enc,
                                recipient_sk, recipient_pk,
                                "UpdatePathNode", group_context, group_context_len,
                                ct, ct_len) != 0) {
        free(enc);
        free(ct);
        return MARMOT_ERR_CRYPTO;
    }

    free(enc);
    free(ct);

    if (pt_len != MLS_HASH_LEN)
        return -1;

    return 0;
}

/**
 * Compute confirmation tag:
 * confirmation_tag = MAC(confirmation_key, confirmed_transcript_hash)
 * (Using HMAC-SHA256 since we don't have a separate MAC primitive)
 */
int
mls_treekem_commit_secret_from_path_secret(const uint8_t root_path_secret[MLS_HASH_LEN],
                                           uint8_t out[MLS_HASH_LEN])
{
    if (!root_path_secret || !out) return -1;
    return derive_commit_secret(root_path_secret, true, out);
}

int
mls_treekem_update_path_decrypt_secret(const MlsRatchetTree *tree,
                                       const MlsUpdatePathNode *path_node,
                                       uint32_t copath_node_idx,
                                       uint32_t resolution_node_idx,
                                       const uint8_t *group_context,
                                       size_t group_context_len,
                                       const uint8_t node_enc_sk[MLS_KEM_SK_LEN],
                                       uint8_t out_path_secret[MLS_HASH_LEN])
{
    if (!tree || !path_node || !node_enc_sk || !out_path_secret ||
        (group_context_len > 0 && !group_context))
        return -1;

    uint32_t resolution[256];
    uint32_t res_len = 0;
    if (mls_tree_resolution(tree, copath_node_idx, resolution, 256, &res_len) != 0)
        return -1;

    int target_idx = -1;
    for (uint32_t i = 0; i < res_len; i++) {
        if (resolution[i] == resolution_node_idx) {
            target_idx = (int)i;
            break;
        }
    }
    if (target_idx < 0 || (uint32_t)target_idx >= path_node->secret_count)
        return -1;

    const uint8_t *node_enc_pk = mls_tree_node_encryption_key(tree, resolution_node_idx);
    if (!node_enc_pk) return -1;

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, path_node->encrypted_path_secrets,
                        path_node->encrypted_path_secrets_len);

    for (int i = 0; i < target_idx; i++) {
        uint8_t *skip_enc = NULL, *skip_ct = NULL;
        size_t skip_enc_len = 0, skip_ct_len = 0;
        if (mls_tls_read_opaque16(&reader, &skip_enc, &skip_enc_len) != 0)
            return -1;
        free(skip_enc);
        if (mls_tls_read_opaque16(&reader, &skip_ct, &skip_ct_len) != 0)
            return -1;
        free(skip_ct);
    }

    uint8_t *enc = NULL, *ct = NULL;
    size_t enc_len = 0, ct_len = 0;
    if (mls_tls_read_opaque16(&reader, &enc, &enc_len) != 0)
        return -1;
    if (mls_tls_read_opaque16(&reader, &ct, &ct_len) != 0) {
        free(enc);
        return -1;
    }
    if (enc_len != MLS_KEM_ENC_LEN) {
        free(enc);
        free(ct);
        return -1;
    }

    size_t pt_len = 0;
    int rc = hpke_decrypt_with_label(out_path_secret, &pt_len, enc,
                                     node_enc_sk, node_enc_pk,
                                     "UpdatePathNode", group_context, group_context_len,
                                     ct, ct_len);
    free(enc);
    free(ct);
    if (rc != 0 || pt_len != MLS_HASH_LEN)
        return -1;
    return 0;
}

int
mls_treekem_apply_update_path(MlsRatchetTree *tree,
                              uint32_t sender_leaf,
                              const MlsUpdatePath *path)
{
    if (!tree || !path || sender_leaf >= tree->n_leaves)
        return -1;
    uint32_t sender_node = mls_tree_leaf_to_node(sender_leaf);
    if (sender_node >= tree->n_nodes || tree->nodes[sender_node].type == MLS_NODE_BLANK)
        return -1;

    /* One UpdatePathNode per filtered direct path node (RFC 9420 §7.6). */
    uint32_t fdp[128];
    uint32_t fdp_len = 0;
    if (mls_tree_filtered_direct_path(tree, sender_leaf, fdp, 128, &fdp_len) != 0)
        return -1;
    if (path->node_count != fdp_len)
        return -1;

    uint32_t direct_path[128];
    uint32_t direct_path_len = 0;
    if (mls_tree_direct_path(sender_node, tree->n_leaves,
                             direct_path, 128, &direct_path_len) != 0)
        return -1;
    for (uint32_t i = 0; i < direct_path_len; i++)
        mls_tree_blank_node(&tree->nodes[direct_path[i]]);

    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t node_idx = fdp[i];
        mls_tree_blank_node(&tree->nodes[node_idx]);
        tree->nodes[node_idx].type = MLS_NODE_PARENT;
        memset(&tree->nodes[node_idx].parent, 0, sizeof(MlsParentNode));
        memcpy(tree->nodes[node_idx].parent.encryption_key,
               path->nodes[i].encryption_key, MLS_KEM_PK_LEN);
    }

    mls_tree_blank_node(&tree->nodes[sender_node]);
    tree->nodes[sender_node].type = MLS_NODE_LEAF;
    if (mls_leaf_node_clone(&tree->nodes[sender_node].leaf, &path->leaf_node) != 0)
        return -1;

    /* Rebuild the parent_hash chain exactly as the committer must have
     * (RFC 9420 §7.9, see generate_update_path): the topmost filtered node
     * keeps an empty parent_hash, each lower filtered node links to the next
     * filtered node up. */
    for (uint32_t pos = fdp_len; pos-- > 1;) {
        uint32_t parent = fdp[pos];
        uint32_t node_idx = fdp[pos - 1];
        if (tree->nodes[node_idx].type != MLS_NODE_PARENT ||
            tree->nodes[parent].type != MLS_NODE_PARENT)
            return -1;
        uint8_t ph[MLS_HASH_LEN];
        if (mls_tree_parent_hash(tree, parent, node_idx, ph) != 0 ||
            replace_parent_hash(&tree->nodes[node_idx].parent.parent_hash,
                                &tree->nodes[node_idx].parent.parent_hash_len,
                                ph) != 0)
            return -1;
    }

    /* RFC 9420 §7.9.2: the committer's new leaf must carry the parent hash
     * of the bottom filtered node -- empty when the filtered path is empty. */
    const MlsLeafNode *leaf = &tree->nodes[sender_node].leaf;
    if (leaf->leaf_node_source != 3)
        return -1;
    if (fdp_len == 0) {
        if (leaf->parent_hash_len != 0)
            return -1;
    } else {
        uint8_t expected[MLS_HASH_LEN];
        if (leaf->parent_hash_len != MLS_HASH_LEN || !leaf->parent_hash ||
            mls_tree_parent_hash(tree, fdp[0], sender_node, expected) != 0 ||
            sodium_memcmp(expected, leaf->parent_hash, MLS_HASH_LEN) != 0)
            return -1;
    }

    if (mls_tree_verify_parent_hashes(tree) != 0)
        return -1;
    return 0;
}

static int
compute_confirmation_tag(const uint8_t confirmation_key[MLS_HASH_LEN],
                         const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                         uint8_t out[MLS_HASH_LEN])
{
    return mls_compute_confirmation_tag(confirmation_key, confirmed_transcript_hash, out);
}

/* AuthenticatedContent up to the signature: wire_format || FramedContent ||
 * signature, with the wire format the content arrived in (a Commit may come
 * as a PrivateMessage, RFC 9420 section 8.2). */
static int
public_message_confirmed_transcript_input(const MlsPublicMessage *pm, uint16_t wire_format,
                                          uint8_t **out, size_t *out_len)
{
    if (!pm || !out || !out_len) return -1;
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 512) != 0) return -1;
    if (mls_tls_write_u16(&buf, wire_format) != 0 ||
        mls_framed_content_serialize(&pm->content, &buf) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }
    const uint8_t *sig = pm->auth.signature_data ? pm->auth.signature_data : pm->auth.signature;
    size_t sig_len = pm->auth.signature_data ? pm->auth.signature_len :
                     (pm->auth.signature_len ? pm->auth.signature_len : MLS_SIG_LEN);
    if (mls_tls_write_opaque16(&buf, sig, sig_len) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static int
build_encrypt_context(const char *label,
                      const uint8_t *context, size_t context_len,
                      uint8_t **out, size_t *out_len)
{
    if (!label || !out || !out_len || (context_len > 0 && !context)) return -1;
    const char prefix[] = "MLS 1.0 ";
    size_t label_len = strlen(prefix) + strlen(label);
    char *full_label = malloc(label_len);
    if (!full_label) return -1;
    memcpy(full_label, prefix, strlen(prefix));
    memcpy(full_label + strlen(prefix), label, strlen(label));

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, label_len + context_len + 16) != 0) {
        free(full_label);
        return -1;
    }
    if (mls_tls_write_opaque16(&buf, (const uint8_t *)full_label, label_len) != 0 ||
        mls_tls_write_opaque32(&buf, context, context_len) != 0) {
        free(full_label);
        mls_tls_buf_free(&buf);
        return -1;
    }
    free(full_label);
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static int
hpke_encrypt_with_label(uint8_t enc[MLS_KEM_ENC_LEN],
                        uint8_t *ct, size_t *ct_len,
                        const uint8_t pk[MLS_KEM_PK_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len,
                        const uint8_t *pt, size_t pt_len)
{
    uint8_t *info = NULL;
    size_t info_len = 0;
    if (build_encrypt_context(label, context, context_len, &info, &info_len) != 0)
        return -1;
    int rc = mls_crypto_hpke_seal_base(enc, ct, ct_len, pk, info, info_len,
                                       NULL, 0, pt, pt_len);
    free(info);
    return rc;
}

static int
hpke_decrypt_with_label(uint8_t *pt, size_t *pt_len,
                        const uint8_t enc[MLS_KEM_ENC_LEN],
                        const uint8_t sk[MLS_KEM_SK_LEN],
                        const uint8_t pk[MLS_KEM_PK_LEN],
                        const char *label,
                        const uint8_t *context, size_t context_len,
                        const uint8_t *ct, size_t ct_len)
{
    uint8_t *info = NULL;
    size_t info_len = 0;
    if (build_encrypt_context(label, context, context_len, &info, &info_len) != 0)
        return -1;
    int rc = mls_crypto_hpke_open_base(pt, pt_len, enc, sk, pk, info, info_len,
                                       NULL, 0, ct, ct_len);
    free(info);
    return rc;
}

/* GroupSecrets (RFC 9420 §12.4.3.1): joiner_secret<V>,
 * optional<PathSecret> path_secret (present when `path_secret` is non-NULL),
 * psks<V> empty.  The buffer holds secrets: callers wipe it before freeing. */
static int
serialize_group_secrets(const uint8_t joiner_secret[MLS_HASH_LEN],
                        const uint8_t *path_secret,
                        uint8_t **out, size_t *out_len)
{
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 2 * MLS_HASH_LEN + 16) != 0) return -1;
    if (mls_tls_write_opaque16(&buf, joiner_secret, MLS_HASH_LEN) != 0 ||
        mls_tls_write_u8(&buf, path_secret ? 1 : 0) != 0 ||
        (path_secret &&
         mls_tls_write_opaque16(&buf, path_secret, MLS_HASH_LEN) != 0) ||
        mls_tls_write_opaque32(&buf, NULL, 0) != 0) {
        if (buf.data) sodium_memzero(buf.data, buf.len);
        mls_tls_buf_free(&buf);
        return -1;
    }
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

#define MLS_EXTENSION_RATCHET_TREE 0x0002

static int
build_group_info_extensions_with_tree(const MlsRatchetTree *tree,
                                      uint8_t **out, size_t *out_len)
{
    /* RFC 9420 ratchet_tree extension: optional<Node> vector encoding
     * (mls_ratchet_tree_serialize), matching the parser used by
     * mls_group_join_from_welcome / mls_ratchet_tree_deserialize. */
    uint8_t *tree_data = NULL;
    size_t tree_len = 0;
    if (mls_ratchet_tree_serialize(tree, &tree_data, &tree_len) != 0) return -1;
    MlsTlsBuf ext;
    if (mls_tls_buf_init(&ext, tree_len + 16) != 0) {
        free(tree_data);
        return -1;
    }
    if (mls_tls_write_u16(&ext, MLS_EXTENSION_RATCHET_TREE) != 0 ||
        mls_tls_write_opaque32(&ext, tree_data, tree_len) != 0) {
        free(tree_data);
        mls_tls_buf_free(&ext);
        return -1;
    }
    free(tree_data);
    *out = ext.data;
    *out_len = ext.len;
    return 0;
}

static int
mls_group_info_tbs_serialize_local(const MlsGroupInfo *gi, MlsTlsBuf *buf)
{
    if (!gi || !buf) return -1;
    if (mls_tls_write_u16(buf, 1) != 0) return -1;
    if (mls_tls_write_u16(buf, MARMOT_CIPHERSUITE) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->group_id, gi->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, gi->epoch) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->tree_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmed_transcript_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_opaque32(buf, gi->extensions_data, gi->extensions_len) != 0) return -1;
    if (mls_tls_write_opaque32(buf, gi->group_info_extensions_data,
                                gi->group_info_extensions_len) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmation_tag, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_u32(buf, gi->signer_leaf) != 0) return -1;
    return 0;
}

static int
mls_group_info_sign_local(MlsGroupInfo *gi,
                          const uint8_t signature_key[MLS_SIG_SK_LEN])
{
    MlsTlsBuf tbs;
    if (mls_tls_buf_init(&tbs, 512) != 0) return -1;
    if (mls_group_info_tbs_serialize_local(gi, &tbs) != 0) {
        mls_tls_buf_free(&tbs);
        return -1;
    }
    int rc = mls_crypto_sign_with_label(gi->signature, signature_key,
                                        "GroupInfoTBS", tbs.data, tbs.len);
    mls_tls_buf_free(&tbs);
    if (rc == 0) gi->signature_len = MLS_SIG_LEN;
    return rc;
}

/**
 * Phase 1 of commit message assembly: build and sign the FramedContent for a
 * commit, and return the RFC 9420 §8.1 ConfirmedTranscriptHashInput bytes
 * (wire_format || FramedContent || signature).  The caller mixes those into
 * the confirmed transcript hash, derives the new epoch, and computes the
 * confirmation tag before calling finish_commit_public_message().
 */
static int
begin_commit_public_message(const MlsGroup *group,
                            uint64_t message_epoch,
                            const uint8_t *group_context, size_t group_context_len,
                            const uint8_t *commit_body, size_t commit_body_len,
                            MlsMLSMessage *msg,
                            uint8_t **transcript_input, size_t *transcript_input_len)
{
    if (!group || !commit_body || !msg || !transcript_input || !transcript_input_len)
        return -1;
    memset(msg, 0, sizeof(*msg));
    msg->wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg->cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg->public_message;
    pm->content.group_id = malloc(group->group_id_len);
    pm->content.content = malloc(commit_body_len);
    if (!pm->content.group_id || !pm->content.content) goto fail;
    memcpy(pm->content.group_id, group->group_id, group->group_id_len);
    pm->content.group_id_len = group->group_id_len;
    pm->content.epoch = message_epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = group->own_leaf_index;
    pm->content.content_type = MLS_CONTENT_TYPE_COMMIT;
    memcpy(pm->content.content, commit_body, commit_body_len);
    pm->content.content_len = commit_body_len;

    if (mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                group_context, group_context_len,
                                group->own_signature_key, &pm->auth) != 0)
        goto fail;
    if (public_message_confirmed_transcript_input(pm, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                                  transcript_input,
                                                  transcript_input_len) != 0)
        goto fail;
    return 0;
fail:
    mls_message_clear(msg);
    return -1;
}

/**
 * Phase 2: attach the confirmation tag, compute the membership tag, and
 * serialize the wire message.  Clears msg regardless of outcome.
 */
static int
finish_commit_public_message(MlsMLSMessage *msg,
                             const uint8_t membership_key[MLS_HASH_LEN],
                             const uint8_t *group_context, size_t group_context_len,
                             const uint8_t confirmation_tag[MLS_HASH_LEN],
                             uint8_t **out, size_t *out_len)
{
    if (!msg || !out || !out_len) return -1;
    MlsPublicMessage *pm = &msg->public_message;
    memcpy(pm->auth.confirmation_tag, confirmation_tag, MLS_HASH_LEN);
    pm->auth.confirmation_tag_len = MLS_HASH_LEN;
    pm->auth.has_confirmation_tag = true;
    if (mls_public_message_compute_membership_tag(pm, membership_key,
                                                   group_context,
                                                   group_context_len) != 0)
        goto fail;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, pm->content.content_len + 256) != 0) goto fail;
    if (mls_message_serialize(msg, &buf) != 0) {
        mls_tls_buf_free(&buf);
        goto fail;
    }
    *out = buf.data;
    *out_len = buf.len;
    mls_message_clear(msg);
    return 0;
fail:
    mls_message_clear(msg);
    return -1;
}

/**
 * Stage a Commit on a deep copy of the group (via its persisted form, which
 * carries every field an epoch transition reads).  Commit producers and
 * process_commit_impl mutate only the stage and install it with
 * group_install_staged() once the whole Commit succeeded, so any failure --
 * including one inside generate_update_path after the proposals and the new
 * path are applied -- leaves the live group untouched.
 */
static int
group_stage_clone(const MlsGroup *group, MlsGroup *staged)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    memset(staged, 0, sizeof(*staged));
    int rc = (mls_group_serialize(group, &blob, &blob_len) == 0 &&
              mls_group_deserialize(blob, blob_len, staged) == 0) ? 0 : -1;
    if (blob) {
        sodium_memzero(blob, blob_len);
        free(blob);
    }
    return rc;
}

/* Replace the live group with a completed stage (zeroizing the old state). */
static void
group_install_staged(MlsGroup *live, MlsGroup *staged)
{
    MlsGroup old = *live;
    *live = *staged;
    memset(staged, 0, sizeof(*staged));
    mls_group_free(&old);
}

/* group_install_staged() for a stage a local Commit producer built: the
 * epoch it enters keeps the group's profile and, for an adopted group, its
 * invariants (nostrc-qp24.5.1).  On failure the stage is freed, the live
 * group is unchanged.
 *
 * EVERY Commit producer installs through this function, never through
 * group_install_staged() directly (only the Commit processor does, after its
 * own mls_group_profile_check_entered()): a producer that skips it can make
 * an adopted group enter an epoch that breaks its invariants.  New producers
 * (e.g. W24 slice A's mls_group_replace_members(), slice B's
 * mls_group_commit_by_ref()) must use it -- test_install_checked pins it
 * (W24 review L4). */
static int
group_install_checked(MlsGroup *live, MlsGroup *staged)
{
    int rc = staged->profile != live->profile ? MARMOT_ERR_VALIDATION
                                              : mls_group_profile_check_entered(staged);
    if (rc != 0) {
        mls_group_free(staged);
        return rc;
    }
    group_install_staged(live, staged);
    return 0;
}

static int
profile_check(const MlsGroup *g, bool entered)
{
    if (!g) return MARMOT_ERR_INVALID_ARG;
    if (g->profile != MARMOT_GROUP_PROFILE_LEGACY && g->profile != MARMOT_GROUP_PROFILE_ADOPTED)
        return MARMOT_ERR_VALIDATION;
    /* No group changes profile: the GroupContext still claims the one the
     * group was admitted under. */
    if (mls_group_context_profile_of(g->extensions_data, g->extensions_len) != g->profile)
        return MARMOT_ERR_VALIDATION;
    if (g->profile != MARMOT_GROUP_PROFILE_ADOPTED) return 0;
    MlsAdoptedGroupContext gc;
    int rc = mls_adopted_group_context_parse(g->extensions_data, g->extensions_len, &gc);
    if (rc == 0) rc = mls_adopted_tree_check(&g->tree, &gc, entered);
    return rc;
}

int
mls_group_profile_check(const MlsGroup *g)
{
    return profile_check(g, false);
}

int
mls_group_profile_check_entered(const MlsGroup *g)
{
    return profile_check(g, true);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_group_free(MlsGroup *g)
{
    if (!g) return;
    free(g->group_id);
    mls_tree_free(&g->tree);
    mls_secret_tree_free(&g->secret_tree);
    /* The GroupContext may hold group secrets (0x8002 image key and upload
     * key; slice I review L4). */
    if (g->extensions_data) sodium_memzero(g->extensions_data, g->extensions_len);
    free(g->extensions_data);
    sodium_memzero(g->own_signature_key, sizeof(g->own_signature_key));
    sodium_memzero(g->own_encryption_key, sizeof(g->own_encryption_key));
    sodium_memzero(g->own_path_keys, sizeof(g->own_path_keys));
    sodium_memzero(&g->epoch_secrets, sizeof(g->epoch_secrets));
    memset(g, 0, sizeof(*g));
}

void
mls_group_strip_to_reader(MlsGroup *g)
{
    if (!g) return;
    uint8_t sender_data[MLS_HASH_LEN];
    memcpy(sender_data, g->epoch_secrets.sender_data_secret, MLS_HASH_LEN);
    sodium_memzero(&g->epoch_secrets, sizeof(g->epoch_secrets));
    memcpy(g->epoch_secrets.sender_data_secret, sender_data, MLS_HASH_LEN);
    sodium_memzero(sender_data, sizeof(sender_data));
    sodium_memzero(g->own_signature_key, sizeof(g->own_signature_key));
    sodium_memzero(g->own_encryption_key, sizeof(g->own_encryption_key));
    sodium_memzero(g->own_path_keys, sizeof(g->own_path_keys));
    sodium_memzero(g->resumption_psk_cache, sizeof(g->resumption_psk_cache));
}

void
mls_proposal_clear(MlsProposal *p)
{
    if (!p) return;
    switch (p->type) {
    case MLS_PROPOSAL_ADD:
        mls_key_package_clear(&p->add.key_package);
        break;
    case MLS_PROPOSAL_UPDATE:
        mls_leaf_node_clear(&p->update.leaf_node);
        break;
    case MLS_PROPOSAL_PSK:
        free(p->psk.psk_id);
        free(p->psk.resumption_group_id);
        free(p->psk.psk_nonce);
        break;
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
        free(p->group_context_extensions.extensions);
        break;
    case MLS_PROPOSAL_APP_DATA_UPDATE:
        mls_app_data_update_clear(&p->app_data_update);
        break;
    default:
        break;
    }
    memset(p, 0, sizeof(*p));
}

void
mls_update_path_clear(MlsUpdatePath *up)
{
    if (!up) return;
    mls_leaf_node_clear(&up->leaf_node);
    if (up->nodes) {
        for (size_t i = 0; i < up->node_count; i++) {
            free(up->nodes[i].encrypted_path_secrets);
        }
        free(up->nodes);
    }
    memset(up, 0, sizeof(*up));
}

void
mls_commit_clear(MlsCommit *c)
{
    if (!c) return;
    if (c->proposals) {
        for (size_t i = 0; i < c->proposal_count; i++)
            mls_proposal_clear(&c->proposals[i]);
        free(c->proposals);
    }
    if (c->has_path)
        mls_update_path_clear(&c->path);
    memset(c, 0, sizeof(*c));
}

void
mls_add_result_clear(MlsAddResult *r)
{
    if (!r) return;
    free(r->commit_data);
    free(r->welcome_data);
    memset(r, 0, sizeof(*r));
}

void
mls_commit_result_clear(MlsCommitResult *r)
{
    if (!r) return;
    free(r->commit_data);
    memset(r, 0, sizeof(*r));
}

void
mls_group_info_clear(MlsGroupInfo *gi)
{
    if (!gi) return;
    free(gi->group_id);
    if (gi->extensions_data) sodium_memzero(gi->extensions_data, gi->extensions_len);
    free(gi->extensions_data);
    free(gi->group_info_extensions_data);
    memset(gi, 0, sizeof(*gi));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Group creation
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_create(MlsGroup *group,
                 const uint8_t *group_id, size_t group_id_len,
                 const uint8_t *credential_identity, size_t credential_identity_len,
                 const uint8_t signature_key_private[MLS_SIG_SK_LEN],
                 const uint8_t *extensions_data, size_t extensions_len)
{
    return mls_group_create_with_leaf_extensions(group, group_id, group_id_len,
                                                 credential_identity,
                                                 credential_identity_len,
                                                 signature_key_private,
                                                 extensions_data, extensions_len,
                                                 NULL, 0);
}

int
mls_group_create_with_leaf_extensions(MlsGroup *group,
                                      const uint8_t *group_id, size_t group_id_len,
                                      const uint8_t *credential_identity,
                                      size_t credential_identity_len,
                                      const uint8_t signature_key_private[MLS_SIG_SK_LEN],
                                      const uint8_t *extensions_data, size_t extensions_len,
                                      const uint8_t *leaf_extensions,
                                      size_t leaf_extensions_len)
{
    if (!group || !group_id || !credential_identity || !signature_key_private ||
        (leaf_extensions_len > 0 && !leaf_extensions))
        return MARMOT_ERR_INVALID_ARG;
    /* The GroupContext decides the group's profile for good (nostrc-
     * qp24.5.1): an adopted one must be complete and canonical before
     * anything is built on it; a legacy one keeps its old opaque handling. */
    MarmotGroupProfile profile = mls_group_context_profile_of(extensions_data, extensions_len);
    if (profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        MlsAdoptedGroupContext gc;
        int gc_rc = mls_adopted_group_context_parse(extensions_data, extensions_len, &gc);
        if (gc_rc != 0) return gc_rc;
    }

    memset(group, 0, sizeof(*group));
    group->profile = profile;

    /* Copy group ID */
    group->group_id = malloc(group_id_len);
    if (!group->group_id) return MARMOT_ERR_MEMORY;
    memcpy(group->group_id, group_id, group_id_len);
    group->group_id_len = group_id_len;

    /* Epoch 0 */
    group->epoch = 0;
    group->own_leaf_index = 0;
    group->max_forward_distance = 1000;

    /* Store signing key */
    memcpy(group->own_signature_key, signature_key_private, MLS_SIG_SK_LEN);

    /* Extensions */
    if (extensions_data && extensions_len > 0) {
        group->extensions_data = malloc(extensions_len);
        if (!group->extensions_data) goto fail;
        memcpy(group->extensions_data, extensions_data, extensions_len);
        group->extensions_len = extensions_len;
    }

    /* Create ratchet tree with 1 leaf */
    if (mls_tree_new(&group->tree, 1) != 0) goto fail;

    /* Populate leaf 0 with our identity */
    uint8_t enc_sk[MLS_KEM_SK_LEN], enc_pk[MLS_KEM_PK_LEN];
    if (mls_crypto_kem_keygen(enc_sk, enc_pk) != 0) goto fail;

    /* Store our encryption private key */
    memcpy(group->own_encryption_key, enc_sk, MLS_KEM_SK_LEN);

    MlsNode *leaf = &group->tree.nodes[0];
    leaf->type = MLS_NODE_LEAF;
    memset(&leaf->leaf, 0, sizeof(MlsLeafNode));
    memcpy(leaf->leaf.encryption_key, enc_pk, MLS_KEM_PK_LEN);

    /* Extract public signing key from the 64-byte libsodium format */
    memcpy(leaf->leaf.signature_key, signature_key_private + 32, MLS_SIG_PK_LEN);

    leaf->leaf.credential_type = MLS_CREDENTIAL_BASIC;
    leaf->leaf.credential_identity = malloc(credential_identity_len);
    if (!leaf->leaf.credential_identity) {
        sodium_memzero(enc_sk, sizeof(enc_sk));
        goto fail;
    }
    memcpy(leaf->leaf.credential_identity, credential_identity, credential_identity_len);
    leaf->leaf.credential_identity_len = credential_identity_len;

    /* Capabilities (RFC 9420 §7.2; nostrc-prqu.10) */
    if (mls_leaf_node_set_marmot_capabilities(&leaf->leaf) != 0) {
        sodium_memzero(enc_sk, sizeof(enc_sk));
        goto fail;
    }
    /* The creator's LeafNode extensions (its account proof, nostrc-7vyi),
     * covered by the leaf signature below. */
    if (leaf_extensions_len > 0) {
        leaf->leaf.extensions_data = malloc(leaf_extensions_len);
        if (!leaf->leaf.extensions_data) {
            sodium_memzero(enc_sk, sizeof(enc_sk));
            goto fail;
        }
        memcpy(leaf->leaf.extensions_data, leaf_extensions, leaf_extensions_len);
        leaf->leaf.extensions_len = leaf_extensions_len;
    }
    leaf->leaf.leaf_node_source = MLS_LEAF_NODE_SOURCE_COMMIT; /* initial group creation */
    if (profile == MARMOT_GROUP_PROFILE_ADOPTED &&
        mls_leaf_node_set_adopted_capabilities(&leaf->leaf) != 0) {
        sodium_memzero(enc_sk, sizeof(enc_sk));
        goto fail;
    }
    if (mls_leaf_node_sign(&leaf->leaf, signature_key_private,
                           group->group_id, group->group_id_len, 0) != 0) {
        sodium_memzero(enc_sk, sizeof(enc_sk));
        goto fail;
    }

    sodium_memzero(enc_sk, sizeof(enc_sk));

    /* An adopted group starts valid or not at all: the creator's leaf must
     * carry the account proof and advertise every required component. */
    if (profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        int check_rc = mls_group_profile_check(group);
        if (check_rc != 0) {
            mls_group_free(group);
            return check_rc;
        }
    }

    /* Initialize transcript hashes to zero (epoch 0) */
    memset(group->confirmed_transcript_hash, 0, MLS_HASH_LEN);
    memset(group->interim_transcript_hash, 0, MLS_HASH_LEN);

    /* Derive epoch 0 secrets:
     * init_secret = all zeros (no previous epoch)
     * commit_secret = all zeros (no commit) */
    uint8_t zero_secret[MLS_HASH_LEN];
    memset(zero_secret, 0, MLS_HASH_LEN);

    if (group_derive_epoch(group, NULL, zero_secret, NULL) != 0) goto fail;

    return 0;

fail:
    mls_group_free(group);
    return MARMOT_ERR_INTERNAL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Add member
 * ══════════════════════════════════════════════════════════════════════════ */

static int leaf_keys_unique(const MlsRatchetTree *tree, uint32_t leaf_index);

/**
 * Shared by the Add and Remove producers: `proposals` (ownership taken, their
 * effect already applied to group->tree) plus an UpdatePath become a signed
 * Commit PublicMessage, and the group advances to the next epoch.  The path
 * secret of each of `secret_nodes` is returned in `node_secrets` (Welcome,
 * RFC 9420 §12.4.3.1) and the new epoch's confirmation tag in
 * `confirmation_tag` (GroupInfo).  Runs on a staged copy: on failure the
 * caller discards the group.
 */
static int
path_commit_with_proposals(MlsGroup *group,
                           MlsProposal *proposals, size_t proposal_count,
                           const uint32_t *added_leaves, size_t added_count,
                           const uint32_t *secret_nodes,
                           uint8_t (*node_secrets)[MLS_HASH_LEN],
                           const uint8_t *pre_gc, size_t pre_gc_len,
                           uint64_t pre_epoch,
                           const uint8_t pre_membership_key[MLS_HASH_LEN],
                           uint8_t confirmation_tag[MLS_HASH_LEN],
                           uint8_t **out_commit, size_t *out_commit_len)
{
    int rc = MARMOT_ERR_INTERNAL;
    uint8_t root_path_secret[MLS_HASH_LEN];
    uint8_t commit_secret[MLS_HASH_LEN];
    memset(root_path_secret, 0, sizeof(root_path_secret));
    memset(commit_secret, 0, sizeof(commit_secret));
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = proposals;
    commit.proposal_count = proposal_count;
    MlsTlsBuf body = {0};
    MlsMLSMessage wire_msg;
    memset(&wire_msg, 0, sizeof(wire_msg));
    bool have_msg = false;
    uint8_t *ct_input = NULL;
    size_t ct_input_len = 0;

    if (generate_update_path(group, added_leaves, added_count,
                             secret_nodes, secret_nodes ? added_count : 0,
                             node_secrets, &commit.path, root_path_secret) != 0)
        goto done;
    commit.has_path = true;

    if (mls_tls_buf_init(&body, 1024) != 0 ||
        mls_commit_serialize(&commit, &body) != 0)
        goto done;
    /* The confirmed transcript hash input is wire_format || FramedContent ||
     * signature (RFC 9420 §8.1): sign the framing before advancing. */
    if (begin_commit_public_message(group, pre_epoch, pre_gc, pre_gc_len,
                                    body.data, body.len,
                                    &wire_msg, &ct_input, &ct_input_len) != 0)
        goto done;
    have_msg = true;
    if (derive_commit_secret(root_path_secret, true, commit_secret) != 0)
        goto done;

    MlsTlsBuf conf = {0};
    if (mls_tls_buf_init(&conf, MLS_HASH_LEN + ct_input_len) != 0) {
        rc = MARMOT_ERR_MEMORY;
        goto done;
    }
    mls_tls_buf_append(&conf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf, ct_input, ct_input_len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf.data, conf.len);
    mls_tls_buf_free(&conf);

    remember_resumption_psk(group, group->epoch, group->epoch_secrets.resumption_psk);
    group->epoch++;
    if (group_derive_epoch(group, group->epoch_secrets.init_secret,
                           commit_secret, NULL) != 0 ||
        compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                                 group->confirmed_transcript_hash,
                                 confirmation_tag) != 0)
        goto done;

    MlsTlsBuf interim = {0};
    if (mls_tls_buf_init(&interim, MLS_HASH_LEN * 2) != 0) {
        rc = MARMOT_ERR_MEMORY;
        goto done;
    }
    mls_tls_buf_append(&interim, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_write_opaque32(&interim, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, interim.data, interim.len);
    mls_tls_buf_free(&interim);

    /* finish_commit_public_message() clears wire_msg either way. */
    have_msg = false;
    if (finish_commit_public_message(&wire_msg, pre_membership_key,
                                     pre_gc, pre_gc_len, confirmation_tag,
                                     out_commit, out_commit_len) != 0)
        goto done;
    rc = 0;
done:
    if (have_msg) mls_message_clear(&wire_msg);
    free(ct_input);
    mls_tls_buf_free(&body);
    mls_commit_clear(&commit);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));
    return rc;
}

/* Deep copy of a KeyPackage into an Add proposal. */
static int
add_proposal_from_key_package(MlsProposal *p, const MlsKeyPackage *kp)
{
    memset(p, 0, sizeof(*p));
    p->type = MLS_PROPOSAL_ADD;
    memcpy(&p->add.key_package, kp, sizeof(*kp));
    p->add.key_package.extensions_data = NULL;
    p->add.key_package.extensions_len = 0;
    memset(&p->add.key_package.leaf_node, 0, sizeof(p->add.key_package.leaf_node));
    if (kp->extensions_data && kp->extensions_len > 0) {
        p->add.key_package.extensions_data = malloc(kp->extensions_len);
        if (!p->add.key_package.extensions_data) return MARMOT_ERR_MEMORY;
        memcpy(p->add.key_package.extensions_data, kp->extensions_data,
               kp->extensions_len);
        p->add.key_package.extensions_len = kp->extensions_len;
    }
    if (mls_leaf_node_clone(&p->add.key_package.leaf_node, &kp->leaf_node) != 0) {
        free(p->add.key_package.extensions_data);
        p->add.key_package.extensions_data = NULL;
        return MARMOT_ERR_MEMORY;
    }
    return 0;
}

/* One EncryptedGroupSecrets entry for `kp`: GroupSecrets (joiner_secret and
 * the joiner's LCA path secret) HPKE-sealed to its init_key, bound to the
 * encrypted GroupInfo (RFC 9420 §12.4.3.1). */
static int
write_encrypted_group_secrets(MlsTlsBuf *vec, const MlsKeyPackage *kp,
                              const uint8_t joiner_secret[MLS_HASH_LEN],
                              const uint8_t path_secret[MLS_HASH_LEN],
                              const uint8_t *enc_gi, size_t enc_gi_len)
{
    uint8_t *gs = NULL;
    size_t gs_len = 0;
    if (serialize_group_secrets(joiner_secret, path_secret, &gs, &gs_len) != 0)
        return -1;
    int rc = -1;
    uint8_t kem_enc[MLS_KEM_ENC_LEN];
    uint8_t kp_ref[MLS_HASH_LEN];
    uint8_t *ct = malloc(gs_len + MLS_AEAD_TAG_LEN);
    size_t ct_len = 0;
    if (ct &&
        hpke_encrypt_with_label(kem_enc, ct, &ct_len, kp->init_key, "Welcome",
                                enc_gi, enc_gi_len, gs, gs_len) == 0 &&
        mls_key_package_ref(kp, kp_ref) == 0 &&
        mls_tls_write_opaque16(vec, kp_ref, MLS_HASH_LEN) == 0 &&
        mls_tls_write_opaque16(vec, kem_enc, MLS_KEM_ENC_LEN) == 0 &&
        mls_tls_write_opaque16(vec, ct, ct_len) == 0)
        rc = 0;
    free(ct);
    sodium_memzero(gs, gs_len);
    free(gs);
    return rc;
}

/* Welcome for the joiners (RFC 9420 §12.4.3.1): the signed GroupInfo of the
 * new epoch, encrypted with the welcome secret, and one
 * EncryptedGroupSecrets per KeyPackage. */
static int
build_welcome(const MlsGroup *group, const uint8_t confirmation_tag[MLS_HASH_LEN],
              const MlsKeyPackage *const *kps, size_t kp_count,
              uint8_t (*path_secrets)[MLS_HASH_LEN],
              uint8_t **out, size_t *out_len)
{
    int rc = MARMOT_ERR_INTERNAL;
    MlsGroupInfo gi;
    bool have_gi = false;
    MlsTlsBuf gi_buf = {0}, secrets = {0}, welcome = {0};
    uint8_t *enc_gi = NULL;
    size_t enc_gi_len = 0;
    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];

    if (mls_group_info_build(group, &gi) != 0) goto done;
    have_gi = true;
    memcpy(gi.confirmation_tag, confirmation_tag, MLS_HASH_LEN);
    if (mls_group_info_sign_local(&gi, group->own_signature_key) != 0 ||
        mls_tls_buf_init(&gi_buf, 512) != 0 ||
        mls_group_info_serialize(&gi, &gi_buf) != 0 ||
        mls_crypto_expand_with_label(key, MLS_AEAD_KEY_LEN,
                                     group->epoch_secrets.welcome_secret,
                                     "key", NULL, 0) != 0 ||
        mls_crypto_expand_with_label(nonce, MLS_AEAD_NONCE_LEN,
                                     group->epoch_secrets.welcome_secret,
                                     "nonce", NULL, 0) != 0)
        goto done;
    enc_gi = malloc(gi_buf.len + MLS_AEAD_TAG_LEN);
    if (!enc_gi) {
        rc = MARMOT_ERR_MEMORY;
        goto done;
    }
    if (mls_crypto_aead_encrypt(enc_gi, &enc_gi_len, key, nonce,
                                gi_buf.data, gi_buf.len, NULL, 0) != 0 ||
        mls_tls_buf_init(&secrets, 128 * kp_count) != 0)
        goto done;
    for (size_t i = 0; i < kp_count; i++)
        if (write_encrypted_group_secrets(&secrets, kps[i],
                                          group->epoch_secrets.joiner_secret,
                                          path_secrets[i], enc_gi, enc_gi_len) != 0)
            goto done;
    /* MLSMessage { version, wire_format welcome, Welcome } */
    if (mls_tls_buf_init(&welcome, 256 + secrets.len + enc_gi_len) != 0 ||
        mls_tls_write_u16(&welcome, 1) != 0 ||
        mls_tls_write_u16(&welcome, MLS_WIRE_FORMAT_WELCOME) != 0 ||
        mls_tls_write_u16(&welcome, MARMOT_CIPHERSUITE) != 0 ||
        mls_tls_write_opaque32(&welcome, secrets.data, secrets.len) != 0 ||
        mls_tls_write_opaque32(&welcome, enc_gi, enc_gi_len) != 0)
        goto done;
    *out = welcome.data;
    *out_len = welcome.len;
    welcome.data = NULL;
    rc = 0;
done:
    sodium_memzero(key, sizeof(key));
    sodium_memzero(nonce, sizeof(nonce));
    if (have_gi) mls_group_info_clear(&gi);
    free(enc_gi);
    mls_tls_buf_free(&gi_buf);
    mls_tls_buf_free(&secrets);
    mls_tls_buf_free(&welcome);
    return rc;
}

static bool leaf_occupied(const MlsGroup *group, uint32_t leaf);

/* The limit receivers apply to Adds per Commit (process_commit_impl). */
#define MLS_MAX_ADDS_PER_COMMIT 64

static int key_package_supports_group(const MlsKeyPackage *kp, const uint8_t *exts,
                                      size_t exts_len);

#ifdef MARMOT_TEST_HOOKS
bool mls_test_allow_unsupported_adds = false;
#define ALLOW_UNSUPPORTED_ADDS mls_test_allow_unsupported_adds
#else
#define ALLOW_UNSUPPORTED_ADDS false   /* compiled out (re-review R3) */
#endif

/* Removes, Adds, then a GroupContextExtensions proposal or (adopted groups,
 * nostrc-qp24.5.1.3) inline AppDataUpdates, with an UpdatePath; a Welcome
 * when there are Adds.  kp_count may be 0 when something else is
 * committed. */
static int
add_members_staged(MlsGroup *group,
                   const uint32_t *removes, size_t remove_count,
                   const MlsKeyPackage *const *kps, size_t kp_count,
                   const uint8_t *gce_extensions, size_t gce_len,
                   const MlsAppDataUpdate *adus, size_t adu_count,
                   MlsAddResult *result)
{
    int rc = MARMOT_ERR_INTERNAL;
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint64_t pre_epoch = group->epoch;
    uint8_t pre_membership_key[MLS_HASH_LEN];
    uint8_t confirmation_tag[MLS_HASH_LEN];
    size_t slots = kp_count ? kp_count : 1;
    uint8_t (*lca_secrets)[MLS_HASH_LEN] = calloc(slots, MLS_HASH_LEN);
    uint32_t *added = calloc(slots, sizeof(uint32_t));
    uint32_t *lca_nodes = calloc(slots, sizeof(uint32_t));
    MlsProposal *proposals = calloc(remove_count + kp_count + 1 + adu_count, sizeof(MlsProposal));
    size_t proposals_made = 0;
    uint8_t *commit = NULL, *welcome = NULL;
    size_t commit_len = 0, welcome_len = 0;
    memcpy(pre_membership_key, group->epoch_secrets.membership_key, MLS_HASH_LEN);

    if (!lca_secrets || !added || !lca_nodes || !proposals) {
        rc = MARMOT_ERR_MEMORY;
        goto done;
    }
    if (mls_group_context_build(group, &pre_gc, &pre_gc_len) != 0) goto done;

    /* Removes first, as receivers apply them (RFC 9420 §12.4.2): each leaf
     * and its direct path blanked, so an Add may take its slot. */
    for (size_t i = 0; i < remove_count; i++) {
        uint32_t node = mls_tree_leaf_to_node(removes[i]);
        uint32_t path[64];
        uint32_t path_len = 0;
        if (removes[i] >= group->tree.n_leaves || removes[i] == group->own_leaf_index ||
            group->tree.nodes[node].type != MLS_NODE_LEAF ||
            mls_tree_direct_path(node, group->tree.n_leaves, path, 64, &path_len) != 0) {
            rc = MARMOT_ERR_INVALID_ARG;
            goto done;
        }
        mls_tree_blank_node(&group->tree.nodes[node]);
        for (uint32_t j = 0; j < path_len; j++)
            mls_tree_blank_node(&group->tree.nodes[path[j]]);
        proposals[proposals_made].type = MLS_PROPOSAL_REMOVE;
        proposals[proposals_made].remove.removed_leaf = removes[i];
        proposals_made++;
    }

    /* Apply the Adds as receivers do (RFC 9420 §12.1.1, in proposal order):
     * each new leaf takes the leftmost blank slot and is unmerged at every
     * non-blank parent on its direct path; the UpdatePath below re-keys (and
     * so clears) the ones on our own path. */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    for (size_t i = 0; i < kp_count; i++) {
        rc = mls_key_package_validate(kps[i]);
        if (rc != 0) goto done;
        /* nostrc-zbmb: the joiner supports what the epoch it joins requires
         * -- this Commit's GroupContextExtensions when it has them. */
        rc = ALLOW_UNSUPPORTED_ADDS
                 ? 0
                 : key_package_supports_group(kps[i],
                                              gce_extensions ? gce_extensions
                                                             : group->extensions_data,
                                              gce_extensions ? gce_len : group->extensions_len);
        if (rc != 0) goto done;
        uint32_t node;
        if (mls_tree_add_leaf(&group->tree, &node) != 0) {
            rc = MARMOT_ERR_INTERNAL;
            goto done;
        }
        group->tree.nodes[node].type = MLS_NODE_LEAF;
        if (mls_leaf_node_clone(&group->tree.nodes[node].leaf, &kps[i]->leaf_node) != 0 ||
            tree_add_unmerged_leaf(&group->tree, node) != 0) {
            rc = MARMOT_ERR_MEMORY;
            goto done;
        }
        added[i] = mls_tree_node_to_leaf(node);
        rc = add_proposal_from_key_package(&proposals[remove_count + i], kps[i]);
        if (rc != 0) goto done;
        proposals_made++;
    }
    /* Refuse what receivers reject (§7.3 step 8): every new leaf's keys are
     * unique in the resulting tree -- no member added twice. */
    for (size_t i = 0; i < kp_count; i++) {
        if (leaf_keys_unique(&group->tree, added[i]) != 0) {
            rc = MARMOT_ERR_INVALID_ARG;
            goto done;
        }
        /* The Welcome gives each joiner the path secret of its lowest common
         * ancestor with us (RFC 9420 §12.4.3.1, nostrc-il4i); the joiner is a
         * non-blank leaf below it, so the node is on our filtered path. */
        lca_nodes[i] = mls_tree_common_ancestor(own_node,
                                                mls_tree_leaf_to_node(added[i]),
                                                group->tree.n_leaves);
        if (lca_nodes[i] == UINT32_MAX) {
            rc = MARMOT_ERR_INTERNAL;
            goto done;
        }
    }

    /* A GroupContextExtensions proposal.  RFC 9420 §12.3 applies it before
     * the Adds and evaluates them against the new extensions; libmarmot
     * applies the proposals in its own order, then checks the new extensions
     * against every leaf.  The result is the same: the tree and GroupContext
     * do not depend on the order, and every member, the joiners included,
     * must support the new extensions either way (§12.1.7). */
    if (gce_extensions) {
        rc = group_context_extensions_validate(group, gce_extensions, gce_len, UINT32_MAX);
        if (rc != 0) goto done;
        MlsProposal *gce = &proposals[remove_count + kp_count];
        gce->type = MLS_PROPOSAL_GROUP_CONTEXT_EXT;
        gce->update_leaf_index = UINT32_MAX;
        gce->group_context_extensions.extensions = malloc(gce_len ? gce_len : 1);
        if (!gce->group_context_extensions.extensions) {
            rc = MARMOT_ERR_MEMORY;
            goto done;
        }
        if (gce_len) memcpy(gce->group_context_extensions.extensions, gce_extensions, gce_len);
        gce->group_context_extensions.extensions_len = gce_len;
        proposals_made++;
        if (apply_group_context_extensions(group, gce_extensions, gce_len) != 0) {
            rc = MARMOT_ERR_UNSUPPORTED;
            goto done;
        }
    }

    /* Inline AppDataUpdates (adopted groups only), applied to the
     * dictionary as receivers apply them, before the UpdatePath binds the
     * new GroupContext (nostrc-qp24.5.1.3). */
    if (adu_count > 0) {
        if (group->profile != MARMOT_GROUP_PROFILE_ADOPTED || gce_extensions) {
            rc = MARMOT_ERR_UNSUPPORTED;
            goto done;
        }
        if (adu_count > MLS_APP_DATA_UPDATE_MAX) {
            rc = MARMOT_ERR_INVALID_ARG;
            goto done;
        }
        for (size_t i = 0; i < adu_count; i++) {
            MlsProposal *p = &proposals[proposals_made];
            p->type = MLS_PROPOSAL_APP_DATA_UPDATE;
            p->update_leaf_index = UINT32_MAX;
            p->sender_leaf = UINT32_MAX;
            p->app_data_update.component_id = adus[i].component_id;
            p->app_data_update.operation = adus[i].operation;
            if (adus[i].update_len > 0) {
                p->app_data_update.update = malloc(adus[i].update_len);
                if (!p->app_data_update.update) {
                    rc = MARMOT_ERR_MEMORY;
                    goto done;
                }
                memcpy(p->app_data_update.update, adus[i].update, adus[i].update_len);
                p->app_data_update.update_len = adus[i].update_len;
            }
            proposals_made++;
        }
        uint8_t *next_ext = NULL;
        size_t next_len = 0;
        const MlsAppDataUpdate **ops = malloc(adu_count * sizeof(*ops));
        if (!ops) {
            rc = MARMOT_ERR_MEMORY;
            goto done;
        }
        for (size_t i = 0; i < adu_count; i++) ops[i] = &adus[i];
        rc = mls_app_data_update_apply(group->extensions_data, group->extensions_len, ops,
                                       adu_count, &next_ext, &next_len);
        free(ops);
        if (rc != 0) {
            if (rc == MARMOT_ERR_MLS_PROCESS_MESSAGE) rc = MARMOT_ERR_INVALID_ARG;
            goto done;
        }
        free(group->extensions_data);
        group->extensions_data = next_ext;
        group->extensions_len = next_len;
    }

    rc = path_commit_with_proposals(group, proposals, proposals_made, added, kp_count,
                                    lca_nodes, lca_secrets, pre_gc, pre_gc_len,
                                    pre_epoch, pre_membership_key, confirmation_tag,
                                    &commit, &commit_len);
    proposals = NULL;   /* owned by the helper */
    proposals_made = 0;
    if (rc != 0) goto done;
    if (kp_count > 0) {
        rc = build_welcome(group, confirmation_tag, kps, kp_count, lca_secrets,
                           &welcome, &welcome_len);
        if (rc != 0) goto done;
    }

    result->commit_data = commit;
    result->commit_len = commit_len;
    result->welcome_data = welcome;
    result->welcome_len = welcome_len;
    commit = welcome = NULL;
    rc = 0;
done:
    if (proposals) {
        for (size_t i = 0; i < proposals_made; i++) mls_proposal_clear(&proposals[i]);
        free(proposals);
    }
    free(commit);
    free(welcome);
    free(pre_gc);
    free(added);
    free(lca_nodes);
    if (lca_secrets) {
        sodium_memzero(lca_secrets, slots * MLS_HASH_LEN);
        free(lca_secrets);
    }
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Remove member
 * ══════════════════════════════════════════════════════════════════════════ */

static int
remove_members_staged(MlsGroup *group,
                      const uint32_t *leaves, size_t leaf_count,
                      MlsCommitResult *result)
{
    int rc = MARMOT_ERR_INTERNAL;
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint64_t pre_epoch = group->epoch;
    uint8_t pre_membership_key[MLS_HASH_LEN];
    uint8_t confirmation_tag[MLS_HASH_LEN];
    memcpy(pre_membership_key, group->epoch_secrets.membership_key, MLS_HASH_LEN);
    MlsProposal *proposals = calloc(leaf_count, sizeof(MlsProposal));
    if (!proposals) return MARMOT_ERR_MEMORY;
    if (mls_group_context_build(group, &pre_gc, &pre_gc_len) != 0) {
        free(proposals);
        return MARMOT_ERR_INTERNAL;
    }

    /* Blank each removed leaf and its direct path, as receivers do. */
    for (size_t i = 0; i < leaf_count; i++) {
        uint32_t node = mls_tree_leaf_to_node(leaves[i]);
        mls_tree_blank_node(&group->tree.nodes[node]);
        uint32_t path[64];
        uint32_t path_len = 0;
        if (mls_tree_direct_path(node, group->tree.n_leaves, path, 64, &path_len) != 0) {
            free(proposals);
            free(pre_gc);
            return MARMOT_ERR_INTERNAL;
        }
        for (uint32_t j = 0; j < path_len; j++)
            mls_tree_blank_node(&group->tree.nodes[path[j]]);
        proposals[i].type = MLS_PROPOSAL_REMOVE;
        proposals[i].remove.removed_leaf = leaves[i];
    }

    uint8_t *commit = NULL;
    size_t commit_len = 0;
    rc = path_commit_with_proposals(group, proposals, leaf_count, NULL, 0, NULL, NULL,
                                    pre_gc, pre_gc_len, pre_epoch, pre_membership_key,
                                    confirmation_tag, &commit, &commit_len);
    free(pre_gc);
    if (rc != 0) return rc;
    result->commit_data = commit;
    result->commit_len = commit_len;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Self-update
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * A Commit carrying `proposals` (borrowed; currently at most
 * GroupContextExtensions) and an UpdatePath: the self-update Commit when
 * proposal_count is 0.  The proposals are applied before the path is
 * generated, as receivers apply them (RFC 9420 §12.4.2), so the UpdatePath's
 * provisional GroupContext and the new epoch use the resulting extensions.
 */
static int
path_commit_staged(MlsGroup *group,
                   const MlsProposal *proposals, size_t proposal_count,
                   const uint8_t *leaf_ext, size_t leaf_ext_len, bool replace_leaf_ext,
                   MlsCommitResult *result)
{
    /* Capture pre-commit context/keys for PublicMessage authentication. */
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint64_t pre_epoch = group->epoch;
    uint8_t pre_membership_key[MLS_HASH_LEN];
    memcpy(pre_membership_key, group->epoch_secrets.membership_key, MLS_HASH_LEN);
    if (mls_group_context_build(group, &pre_gc, &pre_gc_len) != 0)
        return MARMOT_ERR_INTERNAL;

    for (size_t i = 0; i < proposal_count; i++) {
        if (proposals[i].type != MLS_PROPOSAL_GROUP_CONTEXT_EXT ||
            apply_group_context_extensions(group,
                proposals[i].group_context_extensions.extensions,
                proposals[i].group_context_extensions.extensions_len) != 0) {
            free(pre_gc);
            return MARMOT_ERR_INTERNAL;
        }
    }

    /* New LeafNode extensions for our UpdatePath leaf (a self-update adding
     * the account proof, nostrc-rgb5).  Only the new leaf carries them: the
     * pre-Commit GroupContext was captured above, and the UpdatePath replaces
     * this leaf before anything else hashes it. */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    if (replace_leaf_ext) {
        MlsLeafNode *own = &group->tree.nodes[own_node].leaf;
        uint8_t *copy = leaf_ext_len ? malloc(leaf_ext_len) : NULL;
        if (leaf_ext_len && !copy) {
            free(pre_gc);
            return MARMOT_ERR_MEMORY;
        }
        if (copy) memcpy(copy, leaf_ext, leaf_ext_len);
        free(own->extensions_data);
        own->extensions_data = copy;
        own->extensions_len = leaf_ext_len;
    }

    /* Generate UpdatePath (which replaces our leaf and path keys) */
    uint8_t root_path_secret[MLS_HASH_LEN];
    MlsUpdatePath update_path;
    const uint8_t *own_cred = group->tree.nodes[own_node].leaf.credential_identity;
    size_t own_cred_len = group->tree.nodes[own_node].leaf.credential_identity_len;

    (void)own_cred;
    (void)own_cred_len;
    if (generate_update_path(group, NULL, 0, NULL, 0, NULL,
                             &update_path, root_path_secret) != 0) {
        free(pre_gc);
        return MARMOT_ERR_INTERNAL;
    }

    /* The Commit borrows `proposals`: release only its path. */
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = (MlsProposal *)proposals;
    commit.proposal_count = proposal_count;
    commit.has_path = true;
    commit.path = update_path;

    /* Serialize */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 1024) != 0) {
        free(pre_gc);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_commit_serialize(&commit, &buf) != 0) {
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_INTERNAL;
    }

    /* Build and sign the PublicMessage framing now: the confirmed transcript
     * hash input is wire_format || FramedContent || signature (RFC 9420 §8.1),
     * so the signed framing must exist before the transcript is advanced. */
    MlsMLSMessage wire_msg;
    uint8_t *ct_input = NULL;
    size_t ct_input_len = 0;
    if (begin_commit_public_message(group, pre_epoch, pre_gc, pre_gc_len,
                                    buf.data, buf.len,
                                    &wire_msg, &ct_input, &ct_input_len) != 0) {
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_INTERNAL;
    }

    /* Advance epoch */
    uint8_t commit_secret[MLS_HASH_LEN];
    derive_commit_secret(root_path_secret, true, commit_secret);

    /* Update confirmed transcript hash */
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + ct_input_len) != 0) {
        free(ct_input);
        mls_message_clear(&wire_msg);
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, ct_input, ct_input_len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    mls_tls_buf_free(&conf_buf);
    free(ct_input);

    uint64_t previous_epoch = group->epoch;
    remember_resumption_psk(group, previous_epoch,
                            group->epoch_secrets.resumption_psk);
    group->epoch++;
    const uint8_t *prev_init = group->epoch_secrets.init_secret;
    if (group_derive_epoch(group, prev_init, commit_secret, NULL) != 0) {
        mls_message_clear(&wire_msg);
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_INTERNAL;
    }

    uint8_t confirmation_tag[MLS_HASH_LEN];
    compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                             group->confirmed_transcript_hash,
                             confirmation_tag);

    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_message_clear(&wire_msg);
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_write_opaque32(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    /* Finish the PublicMessage: confirmation tag + membership tag using the
     * pre-commit group context and membership key (RFC 9420 §6.2). */
    uint8_t *wire_commit = NULL;
    size_t wire_commit_len = 0;
    if (finish_commit_public_message(&wire_msg, pre_membership_key,
                                     pre_gc, pre_gc_len, confirmation_tag,
                                     &wire_commit, &wire_commit_len) != 0) {
        free(pre_gc);
        mls_tls_buf_free(&buf);
        mls_update_path_clear(&commit.path);
        sodium_memzero(root_path_secret, sizeof(root_path_secret));
        sodium_memzero(commit_secret, sizeof(commit_secret));
        return MARMOT_ERR_INTERNAL;
    }
    free(pre_gc);
    mls_tls_buf_free(&buf);

    result->commit_data = wire_commit;
    result->commit_len = wire_commit_len;

    mls_update_path_clear(&commit.path);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Commit producers: staged, installed only on success
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_add_members(MlsGroup *group,
                      const MlsKeyPackage *const *kps, size_t kp_count,
                      MlsAddResult *result)
{
    return mls_group_add_members_with_extensions(group, kps, kp_count, NULL, 0, result);
}

int
mls_group_add_members_with_extensions(MlsGroup *group,
                                      const MlsKeyPackage *const *kps, size_t kp_count,
                                      const uint8_t *extensions, size_t extensions_len,
                                      MlsAddResult *result)
{
    if (!group || !kps || kp_count == 0 || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (kp_count > MLS_MAX_ADDS_PER_COMMIT) return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 0; i < kp_count; i++)
        if (!kps[i]) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = add_members_staged(&staged, NULL, 0, kps, kp_count, extensions, extensions_len,
                                NULL, 0, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_add_result_clear(result);
    return rc;
}

int
mls_group_replace_members(MlsGroup *group,
                          const uint32_t *removes, size_t remove_count,
                          const MlsKeyPackage *const *kps, size_t kp_count,
                          MlsAddResult *result)
{
    if (!group || (remove_count && !removes) || !kps || kp_count == 0 || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (kp_count > MLS_MAX_ADDS_PER_COMMIT || remove_count > 64) return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 0; i < kp_count; i++)
        if (!kps[i]) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = add_members_staged(&staged, removes, remove_count, kps, kp_count, NULL, 0, NULL, 0,
                                result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_add_result_clear(result);
    return rc;
}

int
mls_group_commit_adopted(MlsGroup *group,
                         const uint32_t *removes, size_t remove_count,
                         const MlsKeyPackage *const *kps, size_t kp_count,
                         const MlsAppDataUpdate *adus, size_t adu_count,
                         MlsAddResult *result)
{
    if (!group || !result || (remove_count && !removes) || (kp_count && !kps) ||
        (adu_count && !adus) || remove_count + kp_count + adu_count == 0)
        return MARMOT_ERR_INVALID_ARG;
    if (group->profile != MARMOT_GROUP_PROFILE_ADOPTED) return MARMOT_ERR_UNSUPPORTED;
    if (kp_count > MLS_MAX_ADDS_PER_COMMIT || remove_count > 64 ||
        adu_count > MLS_APP_DATA_UPDATE_MAX)
        return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 0; i < kp_count; i++)
        if (!kps[i]) return MARMOT_ERR_INVALID_ARG;
    /* Each Remove names a current member other than us, once (RFC 9420
     * §12.2). */
    for (size_t i = 0; i < remove_count; i++) {
        if (removes[i] == group->own_leaf_index || !leaf_occupied(group, removes[i]))
            return MARMOT_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++)
            if (removes[j] == removes[i]) return MARMOT_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = add_members_staged(&staged, removes, remove_count, kps, kp_count, NULL, 0, adus,
                                adu_count, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_add_result_clear(result);
    return rc;
}

int
mls_group_add_member(MlsGroup *group,
                     const MlsKeyPackage *kp,
                     MlsAddResult *result)
{
    if (!kp) return MARMOT_ERR_INVALID_ARG;
    const MlsKeyPackage *kps[1] = { kp };
    return mls_group_add_members(group, kps, 1, result);
}

int
mls_group_remove_members(MlsGroup *group,
                         const uint32_t *leaves, size_t leaf_count,
                         MlsCommitResult *result)
{
    if (!group || !leaves || leaf_count == 0 || !result) return MARMOT_ERR_INVALID_ARG;
    /* Each target is a current member other than us, named once (RFC 9420
     * §12.2: no two Removes of one leaf). */
    for (size_t i = 0; i < leaf_count; i++) {
        if (leaves[i] == group->own_leaf_index || leaves[i] >= group->tree.n_leaves ||
            group->tree.nodes[mls_tree_leaf_to_node(leaves[i])].type != MLS_NODE_LEAF)
            return MARMOT_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++)
            if (leaves[j] == leaves[i]) return MARMOT_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = remove_members_staged(&staged, leaves, leaf_count, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_commit_result_clear(result);
    return rc;
}

int
mls_group_remove_member(MlsGroup *group,
                        uint32_t leaf_index,
                        MlsCommitResult *result)
{
    return mls_group_remove_members(group, &leaf_index, 1, result);
}

int
mls_group_self_update(MlsGroup *group, MlsCommitResult *result)
{
    if (!group || !result) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = path_commit_staged(&staged, NULL, 0, NULL, 0, false, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_commit_result_clear(result);
    return rc;
}

int
mls_group_self_update_with_leaf_extensions(MlsGroup *group,
                                           const uint8_t *leaf_ext, size_t leaf_ext_len,
                                           MlsCommitResult *result)
{
    if (!group || !result || (leaf_ext_len && !leaf_ext)) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    int rc = path_commit_staged(&staged, NULL, 0, leaf_ext, leaf_ext_len, true, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_commit_result_clear(result);
    return rc;
}

int
mls_group_commit_extensions(MlsGroup *group,
                            const uint8_t *extensions, size_t extensions_len,
                            MlsCommitResult *result)
{
    if (!group || !result || (extensions_len > 0 && !extensions))
        return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    /* Refuse what receivers would reject (RFC 9420 §12.1.7): every current
     * member, the committer included, must support the new extensions. */
    int rc = group_context_extensions_validate(group, extensions, extensions_len,
                                               UINT32_MAX);
    if (rc != 0) return rc;

    MlsProposal gce;
    memset(&gce, 0, sizeof(gce));
    gce.type = MLS_PROPOSAL_GROUP_CONTEXT_EXT;
    gce.group_context_extensions.extensions = (uint8_t *)extensions;
    gce.group_context_extensions.extensions_len = extensions_len;
    gce.update_leaf_index = UINT32_MAX;

    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) return MARMOT_ERR_INTERNAL;
    rc = path_commit_staged(&staged, &gce, 1, NULL, 0, false, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_commit_result_clear(result);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Process incoming Commit
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * RFC 9420 §12.4: a Commit's path field MUST be populated if it covers no
 * proposals, or any proposal whose type is registered "Path Required" (§17.4:
 * Update, Remove, ExternalInit, GroupContextExtensions).  Add, PreSharedKey
 * and ReInit may be committed without a path, as may AppDataUpdate
 * (draft-ietf-mls-extensions registers it Path Required: N; libmarmot still
 * never applies it).  Any other type fails closed as path-required.
 *
 * Without this rule a pathless Remove advances with an all-zero
 * commit_secret from the parent-epoch init_secret, which the removed member
 * holds: removal would not exclude them from the next epoch.
 */
static bool
commit_path_required(const MlsProposal *proposals, size_t count)
{
    if (count == 0) return true;
    for (size_t i = 0; i < count; i++) {
        switch (proposals[i].type) {
        case MLS_PROPOSAL_ADD:
        case MLS_PROPOSAL_PSK:
        case MLS_PROPOSAL_REINIT:
        case MLS_PROPOSAL_APP_DATA_UPDATE:
            break;
        default:
            return true;
        }
    }
    return false;
}

/** Validate that all proposals have types this processor understands, that
 *  at most one is GroupContextExtensions (RFC 9420 §12.2), and that no
 *  GroupContextExtensions proposal follows an AppDataUpdate (OpenMLS
 *  AppDataUpdateValidationError::IncorrectOrder; other proposal types may
 *  follow one). */
static int
validate_proposal_ordering(const MlsProposal *proposals, size_t count)
{
    size_t gce_count = 0;
    bool seen_adu = false;
    for (size_t i = 0; i < count; i++) {
        switch (proposals[i].type) {
        case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
            if (++gce_count > 1 || seen_adu) return MARMOT_ERR_MLS_PROCESS_MESSAGE;
            break;
        case MLS_PROPOSAL_APP_DATA_UPDATE:
            seen_adu = true;
            break;
        case MLS_PROPOSAL_ADD:
        case MLS_PROPOSAL_UPDATE:
        case MLS_PROPOSAL_REMOVE:
        case MLS_PROPOSAL_PSK:
        case MLS_PROPOSAL_SELF_REMOVE:
            break;
        default:
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
        }
    }

    return 0;
}

static int
proposal_application_order(uint16_t type)
{
    switch (type) {
    case MLS_PROPOSAL_UPDATE: return 0;
    case MLS_PROPOSAL_REMOVE: return 1;
    case MLS_PROPOSAL_SELF_REMOVE: return 1;
    case MLS_PROPOSAL_ADD: return 2;
    case MLS_PROPOSAL_PSK: return 3;
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT: return 3;
    default: return 4;
    }
}

/* Stable, in O(n): a counting pass over the five application-order
 * classes, then each proposal moved once (slice H re-review R2: the
 * insertion sort it replaces moved 512-byte proposals O(n^2) times, before
 * any authorization, for a member's Commit of tens of thousands of
 * AppDataUpdates followed by Adds).  The proposals are moved, not copied:
 * ownership of their buffers goes with them.  MARMOT_ERR_MEMORY, the array
 * unchanged, if the scratch array cannot be had. */
static int
sort_proposals_for_application(MlsProposal *proposals, size_t count)
{
    if (count < 2) return 0;
    size_t start[6] = {0};
    for (size_t i = 0; i < count; i++)
        start[proposal_application_order(proposals[i].type) + 1]++;
    for (size_t k = 1; k < 6; k++) start[k] += start[k - 1];
    MlsProposal *sorted = malloc(count * sizeof(*sorted));
    if (!sorted) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < count; i++)
        sorted[start[proposal_application_order(proposals[i].type)]++] = proposals[i];
    memcpy(proposals, sorted, count * sizeof(*sorted));
    free(sorted);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Referenced-proposal store (RFC 9420 §12.4)
 *
 * A commit may reference proposals by ProposalRef instead of inlining them.
 * The referenced proposals are the standalone Proposal MLSMessages previously
 * received in the same epoch.  We parse each, compute its
 * ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent),
 * and resolve the commit's references against that set.
 * ──────────────────────────────────────────────────────────────────────── */

static int proposal_deserialize(MlsTlsReader *reader, MlsProposal *p);
static int proposal_type_apply_supported(const MlsGroup *group, const MlsProposal *p);

typedef struct {
    uint8_t     ref[MLS_HASH_LEN];
    size_t      ref_len;
    MlsProposal prop;
    bool        consumed;
} MlsStoredProposal;

typedef struct {
    MlsStoredProposal *items;
    size_t             count;
} MlsProposalStore;

static int
proposal_msg_ref(const MlsPublicMessage *pm, uint8_t out[MLS_HASH_LEN])
{
    /* AuthenticatedContent = wire_format || FramedContent || signature,
     * exactly the bytes produced for the confirmed-transcript input. */
    uint8_t *ac = NULL;
    size_t ac_len = 0;
    if (public_message_confirmed_transcript_input(pm, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                                  &ac, &ac_len) != 0)
        return -1;
    int rc = mls_crypto_ref_hash(out, "MLS 1.0 Proposal Reference", ac, ac_len);
    free(ac);
    return rc;
}

static void
proposal_store_free(MlsProposalStore *store)
{
    if (!store || !store->items) return;
    for (size_t i = 0; i < store->count; i++) {
        if (!store->items[i].consumed)
            mls_proposal_clear(&store->items[i].prop);
    }
    free(store->items);
    store->items = NULL;
    store->count = 0;
}

static int
proposal_store_build(MlsProposalStore *store, const MlsGroup *group,
                     const uint8_t *const *msgs, const size_t *lens, size_t n)
{
    memset(store, 0, sizeof(*store));
    if (n == 0) return 0;
    if (!group || !msgs || !lens) return -1;
    uint8_t *group_context = NULL;
    size_t group_context_len = 0;
    if (mls_group_context_build(group, &group_context, &group_context_len) != 0)
        return -1;
    store->items = calloc(n, sizeof(*store->items));
    if (!store->items) { free(group_context); return -1; }

    for (size_t i = 0; i < n; i++) {
        MlsMLSMessage msg;
        memset(&msg, 0, sizeof(msg));
        MlsTlsReader r;
        mls_tls_reader_init(&r, msgs[i], lens[i]);
        if (mls_message_deserialize(&r, &msg) != 0 ||
            !mls_tls_reader_done(&r) ||
            msg.wire_format != MLS_WIRE_FORMAT_PUBLIC_MESSAGE ||
            msg.public_message.content.content_type != MLS_CONTENT_TYPE_PROPOSAL) {
            mls_message_clear(&msg);
            proposal_store_free(store);
            free(group_context);
            return -1;
        }
        MlsPublicMessage *pm = &msg.public_message;
        uint32_t sender_leaf = pm->content.sender.leaf_index;
        if (pm->content.sender.sender_type != MLS_SENDER_TYPE_MEMBER ||
            sender_leaf >= group->tree.n_leaves ||
            pm->content.group_id_len != group->group_id_len ||
            memcmp(pm->content.group_id, group->group_id, group->group_id_len) != 0 ||
            pm->content.epoch != group->epoch ||
            group->tree.nodes[mls_tree_leaf_to_node(sender_leaf)].type != MLS_NODE_LEAF ||
            mls_framed_content_verify(&pm->content, &pm->auth,
                MLS_WIRE_FORMAT_PUBLIC_MESSAGE, group_context, group_context_len,
                group->tree.nodes[mls_tree_leaf_to_node(sender_leaf)].leaf.signature_key) != 0 ||
            mls_public_message_verify_membership_tag(pm,
                group->epoch_secrets.membership_key,
                group_context, group_context_len) != 0) {
            mls_message_clear(&msg);
            proposal_store_free(store);
            free(group_context);
            return -1;
        }
        MlsStoredProposal *slot = &store->items[store->count];
        memset(slot, 0, sizeof(*slot));

        MlsTlsReader pr;
        mls_tls_reader_init(&pr, pm->content.content, pm->content.content_len);
        if (proposal_deserialize(&pr, &slot->prop) != 0 ||
            !mls_tls_reader_done(&pr) ||
            proposal_msg_ref(pm, slot->ref) != 0) {
            mls_proposal_clear(&slot->prop);
            mls_message_clear(&msg);
            proposal_store_free(store);
            free(group_context);
            return -1;
        }
        /* An Update replaces the LeafNode of the member that sent the
         * proposal (RFC 9420 §12.1.2); capture that leaf from the framing. */
        if (slot->prop.type == MLS_PROPOSAL_UPDATE &&
            pm->content.sender.sender_type == MLS_SENDER_TYPE_MEMBER)
            slot->prop.update_leaf_index = pm->content.sender.leaf_index;
        slot->prop.sender_leaf = sender_leaf;
        slot->ref_len = MLS_HASH_LEN;
        store->count++;
        mls_message_clear(&msg);
    }
    free(group_context);
    return 0;
}

/* Resolve `p` (a referenced proposal) by moving the matching stored proposal
 * into it.  Returns 0 on success, -1 when no stored proposal matches, -2
 * when the matching one was already taken by an earlier reference of the
 * same Commit (a malformed Commit, not a missing proposal). */
static int
proposal_store_resolve(MlsProposalStore *store, MlsProposal *p)
{
    if (!store) return -1;
    bool taken = false;
    for (size_t i = 0; i < store->count; i++) {
        MlsStoredProposal *s = &store->items[i];
        if (s->consumed) {
            taken |= s->ref_len == p->ref_len && memcmp(s->ref, p->ref, p->ref_len) == 0;
            continue;
        }
        if (s->ref_len == p->ref_len &&
            memcmp(s->ref, p->ref, p->ref_len) == 0) {
            MlsProposal moved = s->prop;
            s->consumed = true;
            memset(&s->prop, 0, sizeof(s->prop));
            *p = moved;   /* clears is_ref/ref; installs resolved proposal */
            return 0;
        }
    }
    return taken ? -2 : -1;
}

/* RFC 9420 section 12.2: a proposal type outside the default set (Add ..
 * GroupContextExtensions) needs every member's support. */
static bool
leaf_supports_proposal(const MlsLeafNode *leaf, uint16_t type)
{
    if (type >= MLS_PROPOSAL_ADD && type <= MLS_PROPOSAL_GROUP_CONTEXT_EXT) return true;
    for (size_t i = 0; i < leaf->proposal_count; i++)
        if (leaf->proposals[i] == type) return true;
    return false;
}

/* Every member leaf but those of `gone` (leaves a Commit removes) supports
 * `type`: OpenMLS checks the leaves that remain (valn: public_group
 * validation.rs, nostrc-2um6 review L2). */
static bool
members_support_proposal_except(const MlsGroup *group, uint16_t type, const uint32_t *gone,
                                size_t n_gone)
{
    if (!group) return false;
    for (uint32_t i = 0; i < group->tree.n_leaves; i++) {
        const MlsNode *n = &group->tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type != MLS_NODE_LEAF || leaf_supports_proposal(&n->leaf, type)) continue;
        bool leaving = false;
        for (size_t k = 0; k < n_gone && !leaving; k++) leaving = gone[k] == i;
        if (!leaving) return false;
    }
    return true;
}

bool
mls_group_members_support_proposal(const MlsGroup *group, uint16_t type)
{
    return members_support_proposal_except(group, type, NULL, 0);
}

/* RequiredCapabilities (0x0003) { ExtensionType extension_types<V>;
 * ProposalType proposal_types<V>; CredentialType credential_types<V>; }. */
bool
mls_group_requires_proposal(const MlsGroup *group, uint16_t type)
{
    if (!group || !group->extensions_data) return false;
    MlsTlsReader r;
    mls_tls_reader_init(&r, group->extensions_data, group->extensions_len);
    while (!mls_tls_reader_done(&r)) {
        uint16_t ext_type = 0;
        size_t len = 0;
        if (mls_tls_read_u16(&r, &ext_type) != 0 || mls_tls_read_vli(&r, &len) != 0 ||
            len > mls_tls_reader_remaining(&r))
            return false;
        const uint8_t *data = r.data + r.pos;
        r.pos += len;
        if (ext_type != 0x0003) continue;
        MlsTlsReader rc;
        mls_tls_reader_init(&rc, data, len);
        size_t exts = 0, props = 0;
        if (mls_tls_read_vli(&rc, &exts) != 0 || exts > mls_tls_reader_remaining(&rc))
            return false;
        rc.pos += exts;
        if (mls_tls_read_vli(&rc, &props) != 0 || props > mls_tls_reader_remaining(&rc) ||
            props % 2 != 0)
            return false;
        for (size_t k = 0; k < props; k += 2) {
            uint16_t t = (uint16_t)((rc.data[rc.pos + k] << 8) | rc.data[rc.pos + k + 1]);
            if (t == type) return true;
        }
        return false;
    }
    return false;
}

static bool
leaf_occupied(const MlsGroup *group, uint32_t leaf)
{
    return leaf < group->tree.n_leaves &&
           group->tree.nodes[mls_tree_leaf_to_node(leaf)].type == MLS_NODE_LEAF;
}

/* An opened proposal's AuthenticatedContent `ac` (wire_format ||
 * FramedContent || FramedContentAuthData), checked for `group`'s epoch: of
 * this group and epoch, from an occupied member leaf, a Proposal whose
 * signature verifies under that wire format, with a body that parses to the
 * end and that a Commit can apply.  On success *prop (caller clears) carries
 * its sender, `ref` its ProposalRef. */
static int
proposal_ac_open(const MlsGroup *group, const uint8_t *ac, size_t ac_len, MlsProposal *prop,
                 uint8_t ref[MLS_HASH_LEN], uint32_t *out_sender, uint16_t *out_wf)
{
    memset(prop, 0, sizeof(*prop));
    MlsTlsReader r;
    mls_tls_reader_init(&r, ac, ac_len);
    uint16_t wf = 0;
    MlsFramedContent fc;
    MlsFramedContentAuthData auth;
    if (mls_authenticated_content_deserialize(&r, &wf, &fc, &auth) != 0)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    int rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    uint32_t sender = fc.sender.leaf_index;
    if (!mls_tls_reader_done(&r) ||
        (wf != MLS_WIRE_FORMAT_PUBLIC_MESSAGE && wf != MLS_WIRE_FORMAT_PRIVATE_MESSAGE) ||
        fc.content_type != MLS_CONTENT_TYPE_PROPOSAL)
        goto out;
    if (fc.group_id_len != group->group_id_len ||
        memcmp(fc.group_id, group->group_id, fc.group_id_len) != 0) {
        rc = MARMOT_ERR_WRONG_GROUP_ID;
        goto out;
    }
    if (fc.epoch != group->epoch) {
        rc = MARMOT_ERR_WRONG_EPOCH;
        goto out;
    }
    if (fc.sender.sender_type != MLS_SENDER_TYPE_MEMBER || !leaf_occupied(group, sender))
        goto out;
    if (mls_group_context_build(group, &gc, &gc_len) != 0) {
        rc = MARMOT_ERR_INTERNAL;
        goto out;
    }
    if (mls_framed_content_verify(&fc, &auth, wf, gc, gc_len,
            group->tree.nodes[mls_tree_leaf_to_node(sender)].leaf.signature_key) != 0)
        goto out;
    MlsTlsReader pr;
    mls_tls_reader_init(&pr, fc.content, fc.content_len);
    if (proposal_deserialize(&pr, prop) != 0 || !mls_tls_reader_done(&pr)) {
        mls_proposal_clear(prop);
        goto out;
    }
    switch (prop->type) {
    case MLS_PROPOSAL_ADD:
    case MLS_PROPOSAL_UPDATE:
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
    case MLS_PROPOSAL_APP_DATA_UPDATE:   /* adopted groups (nostrc-qp24.5.1.3) */
        rc = proposal_type_apply_supported(group, prop) ? 0 : MARMOT_ERR_UNSUPPORTED;
        break;
    case MLS_PROPOSAL_REMOVE:
        rc = leaf_occupied(group, prop->remove.removed_leaf) ? 0
                                                            : MARMOT_ERR_MLS_PROCESS_MESSAGE;
        break;
    case MLS_PROPOSAL_SELF_REMOVE:
        /* Always a PublicMessage (draft-ietf-mls-extensions; MIP-03). */
        rc = wf == MLS_WIRE_FORMAT_PUBLIC_MESSAGE ? 0 : MARMOT_ERR_MLS_PROCESS_MESSAGE;
        break;
    default:
        rc = MARMOT_ERR_UNSUPPORTED;   /* PSK, ReInit, ExternalInit */
        break;
    }
    if (rc == 0 && mls_crypto_ref_hash(ref, "MLS 1.0 Proposal Reference", ac, ac_len) != 0)
        rc = MARMOT_ERR_INTERNAL;
    if (rc != 0) {
        mls_proposal_clear(prop);
        goto out;
    }
    prop->sender_leaf = sender;
    if (prop->type == MLS_PROPOSAL_UPDATE) prop->update_leaf_index = sender;
    *out_sender = sender;
    *out_wf = wf;
out:
    free(gc);
    mls_framed_content_clear(&fc);
    mls_framed_content_auth_data_clear(&auth);
    return rc;
}

/* The store of a Commit's references from opened proposals' records (each
 * checked again for this epoch by proposal_ac_open()).  A record that no
 * longer opens -- of another epoch, its sender gone -- is left out: a
 * reference to it does not resolve. */
static int
proposal_store_build_ac(MlsProposalStore *store, const MlsGroup *group,
                        const uint8_t *const *acs, const size_t *lens, size_t n)
{
    memset(store, 0, sizeof(*store));
    if (n == 0) return 0;
    if (!acs || !lens) return -1;
    store->items = calloc(n, sizeof(*store->items));
    if (!store->items) return -1;
    for (size_t i = 0; i < n; i++) {
        MlsStoredProposal *slot = &store->items[store->count];
        memset(slot, 0, sizeof(*slot));
        uint32_t sender = 0;
        uint16_t wf = 0;
        int rc = acs[i] ? proposal_ac_open(group, acs[i], lens[i], &slot->prop, slot->ref,
                                           &sender, &wf)
                        : MARMOT_ERR_INVALID_ARG;
        if (rc == MARMOT_ERR_INTERNAL || rc == MARMOT_ERR_MEMORY) {
            proposal_store_free(store);
            return -1;
        }
        if (rc != 0) continue;
        slot->ref_len = MLS_HASH_LEN;
        store->count++;
    }
    return 0;
}

/* Whether `group` applies a proposal of `p`'s type.  An adopted group
 * (nostrc-qp24.5.1.3) applies Add, Remove, SelfRemove and AppDataUpdate:
 * its GroupContext changes only through AppDataUpdate (a
 * GroupContextExtensions proposal may not touch app_data_dictionary,
 * draft-ietf-mls-extensions 4.7, and libmarmot implements no other
 * GroupContext change there), only the committer's UpdatePath renews a
 * leaf (an Update proposal is an admin-only MDK flow libmarmot does not
 * judge), and Marmot defines no PSK.  Everything else fails closed
 * (MARMOT_ERR_UNSUPPORTED).  A legacy group never applies AppDataUpdate. */
static int
proposal_type_apply_supported(const MlsGroup *group, const MlsProposal *p)
{
    if (!p || p->unsupported) return 0;
    bool adopted = group && group->profile == MARMOT_GROUP_PROFILE_ADOPTED;
    switch (p->type) {
    case MLS_PROPOSAL_ADD:
    case MLS_PROPOSAL_REMOVE:
    case MLS_PROPOSAL_SELF_REMOVE:
        return 1;
    case MLS_PROPOSAL_UPDATE:
    case MLS_PROPOSAL_PSK:
        return !adopted;
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
        return !adopted && mls_group_extensions_supported(
                               p->group_context_extensions.extensions,
                               p->group_context_extensions.extensions_len) == 0;
    case MLS_PROPOSAL_APP_DATA_UPDATE:
        return adopted;
    default:
        return 0;
    }
}

static const MlsPskInput *
find_external_psk(const MlsPskInput *external_psks, size_t external_psk_count,
                  const uint8_t *psk_id, size_t psk_id_len)
{
    if (!psk_id) return NULL;
    for (size_t i = 0; i < external_psk_count; i++) {
        if (external_psks[i].psk_id &&
            external_psks[i].psk_id_len == psk_id_len &&
            memcmp(external_psks[i].psk_id, psk_id, psk_id_len) == 0)
            return &external_psks[i];
    }
    return NULL;
}

static const uint8_t *
lookup_resumption_psk(const MlsGroup *group, uint64_t epoch);

static int
commit_psk_secret_compute(const MlsGroup *group,
                          const MlsProposal *proposals, size_t proposal_count,
                          const MlsPskInput *external_psks,
                          size_t external_psk_count,
                          uint8_t out[MLS_HASH_LEN])
{
    if (!group || !out) return MARMOT_ERR_INVALID_ARG;

    size_t psk_count = 0;
    for (size_t i = 0; i < proposal_count; i++) {
        if (proposals[i].type == MLS_PROPOSAL_PSK)
            psk_count++;
    }

    if (psk_count == 0) {
        if (mls_psk_secret_compute(NULL, 0, out) != 0)
            return MARMOT_ERR_INTERNAL;
        return 0;
    }

    MlsPskInput *inputs = calloc(psk_count, sizeof(*inputs));
    if (!inputs) return MARMOT_ERR_MEMORY;

    size_t idx = 0;
    for (size_t i = 0; i < proposal_count; i++) {
        const MlsProposal *p = &proposals[i];
        if (p->type != MLS_PROPOSAL_PSK)
            continue;

        inputs[idx].psk_nonce = p->psk.psk_nonce;
        inputs[idx].psk_nonce_len = p->psk.psk_nonce_len;
        if (!inputs[idx].psk_nonce || inputs[idx].psk_nonce_len == 0) {
            free(inputs);
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
        }

        if (p->psk.psk_type == 1) {
            const MlsPskInput *ext =
                find_external_psk(external_psks, external_psk_count,
                                  p->psk.psk_id, p->psk.psk_id_len);
            if (!ext || !ext->psk || ext->psk_len == 0) {
                free(inputs);
                return MARMOT_ERR_UNSUPPORTED;
            }
            inputs[idx].psk_id = p->psk.psk_id;
            inputs[idx].psk_id_len = p->psk.psk_id_len;
            inputs[idx].psk_type = 1;
            inputs[idx].psk = ext->psk;
            inputs[idx].psk_len = ext->psk_len;
        } else if (p->psk.psk_type == 2) {
            if (p->psk.resumption_group_id_len != group->group_id_len ||
                memcmp(p->psk.resumption_group_id, group->group_id,
                       group->group_id_len) != 0) {
                free(inputs);
                return MARMOT_ERR_UNSUPPORTED;
            }
            const uint8_t *resumption_psk =
                lookup_resumption_psk(group, p->psk.resumption_epoch);
            if (!resumption_psk) {
                free(inputs);
                return MARMOT_ERR_UNSUPPORTED;
            }
            inputs[idx].psk_type = 2;
            inputs[idx].resumption_usage = p->psk.resumption_usage;
            inputs[idx].resumption_group_id = p->psk.resumption_group_id;
            inputs[idx].resumption_group_id_len = p->psk.resumption_group_id_len;
            inputs[idx].resumption_epoch = p->psk.resumption_epoch;
            inputs[idx].psk = resumption_psk;
            inputs[idx].psk_len = MLS_HASH_LEN;
        } else {
            free(inputs);
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
        }
        idx++;
    }

    int rc = mls_psk_secret_compute(inputs, psk_count, out);
    free(inputs);
    return rc == 0 ? 0 : MARMOT_ERR_INTERNAL;
}

static int
apply_group_context_extensions(MlsGroup *group,
                               const uint8_t *extensions,
                               size_t extensions_len)
{
    if (!group || mls_group_extensions_supported(extensions, extensions_len) != 0)
        return -1;
    uint8_t *copy = NULL;
    if (extensions_len > 0) {
        if (!extensions) return -1;
        copy = malloc(extensions_len);
        if (!copy) return -1;
        memcpy(copy, extensions, extensions_len);
    }
    if (group->extensions_data) sodium_memzero(group->extensions_data, group->extensions_len);
    free(group->extensions_data);
    group->extensions_data = copy;
    group->extensions_len = extensions_len;
    return 0;
}

static void
remember_resumption_psk(MlsGroup *group, uint64_t epoch,
                        const uint8_t psk[MLS_HASH_LEN])
{
    if (!group || !psk) return;
    size_t slot = 0;
    for (size_t i = 0; i < MLS_RESUMPTION_PSK_CACHE_SIZE; i++) {
        if (group->resumption_psk_cache[i].valid &&
            group->resumption_psk_cache[i].epoch == epoch) {
            slot = i;
            goto store;
        }
        if (!group->resumption_psk_cache[i].valid) {
            slot = i;
            goto store;
        }
        if (group->resumption_psk_cache[i].epoch <
            group->resumption_psk_cache[slot].epoch)
            slot = i;
    }

store:
    group->resumption_psk_cache[slot].valid = true;
    group->resumption_psk_cache[slot].epoch = epoch;
    memcpy(group->resumption_psk_cache[slot].psk, psk, MLS_HASH_LEN);
}

static const uint8_t *
lookup_resumption_psk(const MlsGroup *group, uint64_t epoch)
{
    if (!group) return NULL;
    if (group->epoch == epoch)
        return group->epoch_secrets.resumption_psk;
    for (size_t i = 0; i < MLS_RESUMPTION_PSK_CACHE_SIZE; i++) {
        if (group->resumption_psk_cache[i].valid &&
            group->resumption_psk_cache[i].epoch == epoch)
            return group->resumption_psk_cache[i].psk;
    }
    return NULL;
}

static int
lookup_own_path_key(const MlsGroup *group, uint32_t node,
                    const uint8_t **out_sk, const uint8_t **out_pk)
{
    if (!group || !out_sk || !out_pk || node >= group->tree.n_nodes)
        return -1;
    const uint8_t *tree_pk = mls_tree_node_encryption_key(&group->tree, node);
    if (!tree_pk) return -1;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        const MlsOwnPathKeyCacheEntry *entry = &group->own_path_keys[i];
        if (!entry->valid || entry->node != node) continue;
        if (memcmp(entry->pk, tree_pk, MLS_KEM_PK_LEN) != 0) continue;
        *out_sk = entry->sk;
        *out_pk = entry->pk;
        return 0;
    }
    return -1;
}

static int
remember_own_path_key(MlsGroup *group, uint32_t node,
                      const uint8_t sk[MLS_KEM_SK_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN])
{
    if (!group || !sk || !pk) return -1;
    size_t slot = MLS_OWN_PATH_KEY_CACHE_SIZE;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        MlsOwnPathKeyCacheEntry *entry = &group->own_path_keys[i];
        if (entry->valid && entry->node == node) {
            slot = i;
            break;
        }
        if (slot == MLS_OWN_PATH_KEY_CACHE_SIZE && !entry->valid)
            slot = i;
    }
    if (slot == MLS_OWN_PATH_KEY_CACHE_SIZE)
        return -1;
    group->own_path_keys[slot].valid = true;
    group->own_path_keys[slot].node = node;
    memcpy(group->own_path_keys[slot].sk, sk, MLS_KEM_SK_LEN);
    memcpy(group->own_path_keys[slot].pk, pk, MLS_KEM_PK_LEN);
    return 0;
}

/* A member only ever learns private keys of nodes on its own direct path
 * (RFC 9420 §4.2), one per level, so a pruned cache holds at most the tree
 * depth: 31 levels for the uint32 node index space. */
_Static_assert(MLS_OWN_PATH_KEY_CACHE_SIZE >= 32,
               "own path-key cache must hold one key per tree level");

/**
 * Drop cached path keys that no longer match the tree: the node left our
 * direct path's non-blank parents (blanked by a Remove/Update, truncated) or
 * a later UpdatePath replaced its key.  Called once the tree is final for the
 * epoch being entered, by committers and receivers, so the cache holds exactly
 * the keys of our current path and cannot fill up with stale entries.
 */
static void
prune_own_path_keys(MlsGroup *group)
{
    if (!group) return;
    uint32_t dp[64];
    uint32_t dp_len = 0;
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    if (own_node >= group->tree.n_nodes ||
        mls_tree_direct_path(own_node, group->tree.n_leaves, dp, 64, &dp_len) != 0)
        dp_len = 0;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        MlsOwnPathKeyCacheEntry *entry = &group->own_path_keys[i];
        if (!entry->valid) continue;
        bool keep = false;
        for (uint32_t j = 0; j < dp_len && !keep; j++) {
            if (dp[j] != entry->node) continue;
            const MlsNode *n = &group->tree.nodes[entry->node];
            keep = n->type == MLS_NODE_PARENT &&
                   memcmp(n->parent.encryption_key, entry->pk, MLS_KEM_PK_LEN) == 0;
        }
        if (!keep)
            sodium_memzero(entry, sizeof(*entry));
    }
}

int
mls_group_welcome_install_path_secret(MlsGroup *group, uint32_t committer_leaf,
                                      const uint8_t path_secret[MLS_HASH_LEN])
{
    if (!group || !path_secret) return MARMOT_ERR_INVALID_ARG;
    const uint32_t n = group->tree.n_leaves;
    if (committer_leaf >= n || committer_leaf == group->own_leaf_index ||
        group->own_leaf_index >= n)
        return MARMOT_ERR_WELCOME_INVALID;
    uint32_t committer_node = mls_tree_leaf_to_node(committer_leaf);
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    if (group->tree.nodes[committer_node].type != MLS_NODE_LEAF)
        return MARMOT_ERR_WELCOME_INVALID;

    /* The path secret belongs to the lowest common ancestor, which must be on
     * the committer's filtered direct path; the chain continues through the
     * filtered nodes above it, exactly as the committer derived it. */
    uint32_t lca = mls_tree_common_ancestor(own_node, committer_node, n);
    uint32_t fdp[64];
    uint32_t fdp_len = 0;
    if (lca == UINT32_MAX ||
        mls_tree_filtered_direct_path(&group->tree, committer_leaf,
                                      fdp, 64, &fdp_len) != 0)
        return MARMOT_ERR_WELCOME_INVALID;
    uint32_t k = 0;
    while (k < fdp_len && fdp[k] != lca) k++;
    if (k == fdp_len) return MARMOT_ERR_WELCOME_INVALID;

    /* Derive and verify every key before installing any: a mismatch leaves
     * the cache untouched. */
    uint8_t secret[MLS_HASH_LEN];
    uint8_t sks[64][MLS_KEM_SK_LEN];
    uint8_t pks[64][MLS_KEM_PK_LEN];
    int rc = MARMOT_ERR_WELCOME_INVALID;
    memcpy(secret, path_secret, MLS_HASH_LEN);
    for (uint32_t i = k; i < fdp_len; i++) {
        const MlsNode *node = &group->tree.nodes[fdp[i]];
        if (node->type != MLS_NODE_PARENT ||
            mls_tree_derive_node_keypair(secret, sks[i], pks[i]) != 0 ||
            sodium_memcmp(pks[i], node->parent.encryption_key, MLS_KEM_PK_LEN) != 0)
            goto done;
        if (i + 1 < fdp_len) {
            uint8_t next[MLS_HASH_LEN];
            int next_rc = mls_tree_derive_next_path_secret(secret, next);
            memcpy(secret, next, MLS_HASH_LEN);
            sodium_memzero(next, sizeof(next));
            if (next_rc != 0) goto done;
        }
    }
    for (uint32_t i = k; i < fdp_len; i++) {
        if (remember_own_path_key(group, fdp[i], sks[i], pks[i]) != 0) {
            rc = MARMOT_ERR_INTERNAL;
            goto done;
        }
    }
    rc = 0;
done:
    if (rc != 0) {
        /* Drop anything installed before the failure. */
        for (uint32_t i = k; i < fdp_len; i++) {
            for (size_t j = 0; j < MLS_OWN_PATH_KEY_CACHE_SIZE; j++) {
                MlsOwnPathKeyCacheEntry *e = &group->own_path_keys[j];
                if (e->valid && e->node == fdp[i])
                    sodium_memzero(e, sizeof(*e));
            }
        }
    }
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(sks, sizeof(sks));
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * LeafNode validation (RFC 9420 §7.3)
 *
 * Every LeafNode a Commit installs -- each committed Update proposal and the
 * UpdatePath leaf -- is validated before it is applied.  All checks run on
 * the staged clone in process_commit_impl, so a rejection never touches the
 * live group.  Public keys are compared with memcmp: they are not secret.
 * ──────────────────────────────────────────────────────────────────────── */

#define MLS_PROTOCOL_VERSION_MLS10          0x0001
#define MLS_EXTENSION_REQUIRED_CAPABILITIES 0x0003

static bool
u16_list_contains(const uint16_t *list, size_t count, uint16_t value)
{
    if (count > 0 && !list) return false;
    for (size_t i = 0; i < count; i++) {
        if (list[i] == value) return true;
    }
    return false;
}

/* RFC 9420 §7.2: the default extension types (application_id through
 * external_senders) and proposal types (add through
 * group_context_extensions) are implicitly supported and are not listed in
 * capabilities. */
static bool
extension_type_is_default(uint16_t type)
{
    return type >= 0x0001 && type <= 0x0005;
}

static bool
proposal_type_is_default(uint16_t type)
{
    return type >= 0x0001 && type <= 0x0007;
}

typedef int (*extension_visit_fn)(uint16_t type, const uint8_t *data,
                                  size_t len, const MlsLeafNode *leaf);

/* Visit each Extension { uint16 extension_type; opaque extension_data<V>; }
 * of a serialized list.  A malformed list or a repeated type fails: support
 * cannot be established for extensions that cannot be enumerated. */
static int
extensions_foreach(const uint8_t *list, size_t list_len,
                   extension_visit_fn visit, const MlsLeafNode *leaf)
{
    if (list_len > 0 && !list) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, list, list_len);
    while (!mls_tls_reader_done(&r)) {
        size_t entry_start = r.pos;
        uint16_t type;
        size_t len;
        if (mls_tls_read_u16(&r, &type) != 0 ||
            mls_tls_read_vli(&r, &len) != 0 ||
            len > mls_tls_reader_remaining(&r))
            return -1;
        const uint8_t *data = r.data + r.pos;
        r.pos += len;

        MlsTlsReader prev;
        mls_tls_reader_init(&prev, list, entry_start);
        while (!mls_tls_reader_done(&prev)) {
            uint16_t prev_type;
            size_t prev_len;
            if (mls_tls_read_u16(&prev, &prev_type) != 0 ||
                mls_tls_read_vli(&prev, &prev_len) != 0 ||
                prev_type == type)
                return -1;
            prev.pos += prev_len;
        }
        if (visit(type, data, len, leaf) != 0) return -1;
    }
    return 0;
}

/* §7.3 step 6: each LeafNode extension is listed in its own capabilities. */
static int
leaf_extension_supported(uint16_t type, const uint8_t *data, size_t len,
                         const MlsLeafNode *leaf)
{
    (void)data;
    (void)len;
    return (extension_type_is_default(type) ||
            u16_list_contains(leaf->cap_extensions, leaf->cap_extension_count, type))
               ? 0 : -1;
}

/* RequiredCapabilities { ExtensionType extension_types<V>;
 *   ProposalType proposal_types<V>; CredentialType credential_types<V>; } */
static int
required_capabilities_supported(const MlsLeafNode *leaf,
                                const uint8_t *data, size_t len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    for (int list = 0; list < 3; list++) {
        size_t bytes;
        if (mls_tls_read_vli(&r, &bytes) != 0 || bytes % 2 != 0 ||
            bytes > mls_tls_reader_remaining(&r))
            return -1;
        for (size_t i = 0; i < bytes / 2; i++) {
            uint16_t type;
            if (mls_tls_read_u16(&r, &type) != 0) return -1;
            bool supported;
            if (list == 0)
                supported = extension_type_is_default(type) ||
                            u16_list_contains(leaf->cap_extensions,
                                              leaf->cap_extension_count, type);
            else if (list == 1)
                supported = proposal_type_is_default(type) ||
                            u16_list_contains(leaf->proposals,
                                              leaf->proposal_count, type);
            else
                supported = u16_list_contains(leaf->cap_credentials,
                                              leaf->cap_credential_count, type);
            if (!supported) return -1;
        }
    }
    return mls_tls_reader_done(&r) ? 0 : -1;
}

/* §7.3 step 3: the leaf supports every GroupContext extension and whatever
 * a required_capabilities extension demands. */
static int
group_extension_supported(uint16_t type, const uint8_t *data, size_t len,
                          const MlsLeafNode *leaf)
{
    if (!extension_type_is_default(type) &&
        !u16_list_contains(leaf->cap_extensions, leaf->cap_extension_count, type))
        return -1;
    if (type == MLS_EXTENSION_REQUIRED_CAPABILITIES)
        return required_capabilities_supported(leaf, data, len);
    return 0;
}

/* nostrc-zbmb (RFC 9420 §12.1.1, §7.2): a KeyPackage's leaf supports every
 * extension of `exts` (a GroupContext's list) and whatever its
 * required_capabilities demand. */
static int
key_package_supports_group(const MlsKeyPackage *kp, const uint8_t *exts, size_t exts_len)
{
    return extensions_foreach(exts, exts_len, group_extension_supported, &kp->leaf_node) == 0
               ? 0
               : MARMOT_ERR_KEY_PACKAGE_CAPABILITIES;
}

static int
extension_well_formed(uint16_t type, const uint8_t *data, size_t len,
                      const MlsLeafNode *leaf)
{
    (void)type;
    (void)data;
    (void)len;
    (void)leaf;
    return 0;
}

/**
 * A GroupContextExtensions list the group can adopt (RFC 9420 §12.1.7,
 * §11.1): well formed with no repeated type (MARMOT_ERR_INVALID_ARG), no type
 * libmarmot does not apply (MARMOT_ERR_UNSUPPORTED), and supported -- each
 * extension and any required_capabilities -- by every member leaf other than
 * `replaced_leaf` (UINT32_MAX: none), else MARMOT_ERR_UNSUPPORTED.
 */
static int
group_context_extensions_validate(const MlsGroup *group,
                                  const uint8_t *extensions, size_t extensions_len,
                                  uint32_t replaced_leaf)
{
    if (!group || (extensions_len > 0 && !extensions)) return MARMOT_ERR_INVALID_ARG;
    if (extensions_foreach(extensions, extensions_len, extension_well_formed, NULL) != 0)
        return MARMOT_ERR_INVALID_ARG;
    if (mls_group_extensions_supported(extensions, extensions_len) != 0)
        return MARMOT_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < group->tree.n_leaves; i++) {
        if (i == replaced_leaf) continue;
        const MlsNode *n = &group->tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type != MLS_NODE_LEAF) continue;
        if (extensions_foreach(extensions, extensions_len,
                               group_extension_supported, &n->leaf) != 0)
            return MARMOT_ERR_UNSUPPORTED;
    }
    return 0;
}

/**
 * RFC 9420 §7.3 checks that depend on the LeafNode, the group and the member
 * leaf it replaces (`replaced`, the current LeafNode at `leaf_index`).  Key
 * uniqueness needs the tree the leaf lands in and is checked separately.
 * `group_extensions` are the GroupContext extensions of the epoch the leaf
 * joins (after any GroupContextExtensions proposal in the same Commit).
 */
static int
leaf_node_validate(const MlsGroup *group, const MlsLeafNode *leaf,
                   uint8_t expected_source, uint32_t leaf_index,
                   const MlsLeafNode *replaced,
                   const uint8_t *group_extensions, size_t group_extensions_len)
{
    if (!group || !leaf || !replaced) return -1;

    /* Step 7: leaf_node_source.  Only key_package leaves carry a lifetime,
     * so no lifetime rule applies to update/commit leaves.  An update leaf
     * has no parent_hash; a commit leaf's must be empty or hash-sized (its
     * value is checked against the tree when the UpdatePath is merged). */
    if (leaf->leaf_node_source != expected_source) return -1;
    if (expected_source == MLS_LEAF_NODE_SOURCE_UPDATE &&
        (leaf->parent_hash || leaf->parent_hash_len != 0))
        return -1;
    if (expected_source == MLS_LEAF_NODE_SOURCE_COMMIT &&
        leaf->parent_hash_len != 0 && leaf->parent_hash_len != MLS_HASH_LEN)
        return -1;

    /* Step 1 (§5.3.1, §5.3.3): a basic credential, and a valid successor of
     * the credential it replaces -- a member cannot take on another identity
     * through an Update or a Commit. */
    if (leaf->credential_type != MLS_CREDENTIAL_BASIC ||
        !leaf->credential_identity || leaf->credential_identity_len == 0 ||
        leaf->credential_type != replaced->credential_type ||
        leaf->credential_identity_len != replaced->credential_identity_len ||
        !replaced->credential_identity ||
        memcmp(leaf->credential_identity, replaced->credential_identity,
               leaf->credential_identity_len) != 0)
        return -1;

    /* Step 3 (and §7.2): the group's protocol version, ciphersuite and the
     * leaf's own credential type are advertised; GroupContext extensions and
     * required capabilities are supported. */
    if (!u16_list_contains(leaf->versions, leaf->version_count,
                           MLS_PROTOCOL_VERSION_MLS10) ||
        !u16_list_contains(leaf->ciphersuites, leaf->ciphersuite_count,
                           MARMOT_CIPHERSUITE) ||
        !u16_list_contains(leaf->cap_credentials, leaf->cap_credential_count,
                           leaf->credential_type))
        return -1;
    if (extensions_foreach(group_extensions, group_extensions_len,
                           group_extension_supported, leaf) != 0)
        return -1;

    /* Step 4: every other member supports this credential type, and this
     * leaf supports every credential type the other members use. */
    for (uint32_t i = 0; i < group->tree.n_leaves; i++) {
        if (i == leaf_index) continue;
        const MlsNode *n = &group->tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type != MLS_NODE_LEAF) continue;
        if (!u16_list_contains(n->leaf.cap_credentials, n->leaf.cap_credential_count,
                               leaf->credential_type) ||
            !u16_list_contains(leaf->cap_credentials, leaf->cap_credential_count,
                               n->leaf.credential_type))
            return -1;
    }

    /* Step 6: the leaf's own extensions are listed in its capabilities. */
    if (extensions_foreach(leaf->extensions_data, leaf->extensions_len,
                           leaf_extension_supported, leaf) != 0)
        return -1;

    /* Step 2: signature over LeafNodeTBS, which for the update and commit
     * sources binds this group_id and leaf_index (§7.2). */
    if (mls_leaf_node_verify_signature(leaf, group->group_id, group->group_id_len,
                                       leaf_index) != 0)
        return -1;
    return 0;
}

/**
 * Validate a committed Update proposal against the pre-Commit tree
 * (RFC 9420 §12.1.2): its target is the proposer's leaf, known only from the
 * framing of a by-reference Update.  An inline Update, or one the committer
 * sent, is the committer updating itself, which a Commit must do through its
 * UpdatePath instead (§12.2).
 */
static int
update_proposal_validate(const MlsGroup *group, const MlsProposal *p,
                         uint32_t committer_leaf,
                         const uint8_t *group_extensions, size_t group_extensions_len)
{
    if (p->type != MLS_PROPOSAL_UPDATE) return 0;
    uint32_t leaf = p->update_leaf_index;
    if (leaf == UINT32_MAX || leaf == committer_leaf || leaf >= group->tree.n_leaves)
        return -1;
    const MlsNode *current = &group->tree.nodes[mls_tree_leaf_to_node(leaf)];
    if (current->type != MLS_NODE_LEAF) return -1;
    if (leaf_node_validate(group, &p->update.leaf_node, MLS_LEAF_NODE_SOURCE_UPDATE,
                           leaf, &current->leaf,
                           group_extensions, group_extensions_len) != 0)
        return -1;
    /* §7.3 step 7: the Update must replace the encryption key. */
    if (memcmp(p->update.leaf_node.encryption_key, current->leaf.encryption_key,
               MLS_KEM_PK_LEN) == 0)
        return -1;
    return 0;
}

/**
 * §7.3 step 8 for a leaf just installed at `leaf_index`: its signature and
 * encryption keys are unique among the members, and its encryption key does
 * not reuse any parent node's public key.
 */
static int
leaf_keys_unique(const MlsRatchetTree *tree, uint32_t leaf_index)
{
    uint32_t node_idx = mls_tree_leaf_to_node(leaf_index);
    if (leaf_index >= tree->n_leaves || tree->nodes[node_idx].type != MLS_NODE_LEAF)
        return -1;
    const MlsLeafNode *leaf = &tree->nodes[node_idx].leaf;
    for (uint32_t i = 0; i < tree->n_nodes; i++) {
        if (i == node_idx) continue;
        const MlsNode *n = &tree->nodes[i];
        if (n->type == MLS_NODE_LEAF) {
            if (memcmp(n->leaf.encryption_key, leaf->encryption_key, MLS_KEM_PK_LEN) == 0 ||
                memcmp(n->leaf.signature_key, leaf->signature_key, MLS_SIG_PK_LEN) == 0)
                return -1;
        } else if (n->type == MLS_NODE_PARENT) {
            if (memcmp(n->parent.encryption_key, leaf->encryption_key, MLS_KEM_PK_LEN) == 0)
                return -1;
        }
    }
    return 0;
}

/**
 * RFC 9420 §12.4.2, against the tree the proposals produced (before the
 * UpdatePath is merged): none of the UpdatePath's public keys may appear in
 * any node of that tree -- including the committer's current leaf, so the
 * leaf encryption key is always replaced -- and none may repeat within the
 * path.  The new leaf's signature key must not belong to another member.
 */
static int
update_path_keys_fresh(const MlsRatchetTree *tree, uint32_t sender_leaf,
                       const MlsUpdatePath *path)
{
    for (size_t k = 0; k <= path->node_count; k++) {
        const uint8_t *key = (k == 0) ? path->leaf_node.encryption_key
                                      : path->nodes[k - 1].encryption_key;
        for (size_t j = 0; j < k; j++) {
            const uint8_t *prior = (j == 0) ? path->leaf_node.encryption_key
                                            : path->nodes[j - 1].encryption_key;
            if (memcmp(prior, key, MLS_KEM_PK_LEN) == 0) return -1;
        }
        for (uint32_t i = 0; i < tree->n_nodes; i++) {
            const uint8_t *existing = mls_tree_node_encryption_key(tree, i);
            if (existing && memcmp(existing, key, MLS_KEM_PK_LEN) == 0) return -1;
        }
    }
    for (uint32_t i = 0; i < tree->n_leaves; i++) {
        if (i == sender_leaf) continue;
        const MlsNode *n = &tree->nodes[mls_tree_leaf_to_node(i)];
        if (n->type == MLS_NODE_LEAF &&
            memcmp(n->leaf.signature_key, path->leaf_node.signature_key,
                   MLS_SIG_PK_LEN) == 0)
            return -1;
    }
    return 0;
}

/* A handshake PrivateMessage's sender (RFC 9420 section 6.3.2): the member
 * leaf its sender data names, decrypted with `group`'s sender_data_secret. */
static int
private_message_sender(const MlsGroup *group, const MlsPrivateMessage *pm,
                       MlsSenderData *out)
{
    if (pm->group_id_len != group->group_id_len ||
        memcmp(pm->group_id, group->group_id, group->group_id_len) != 0 ||
        pm->epoch != group->epoch ||
        (pm->content_type != MLS_CONTENT_TYPE_PROPOSAL &&
         pm->content_type != MLS_CONTENT_TYPE_COMMIT))
        return -1;
    size_t sample_len = pm->ciphertext_len < MLS_HASH_LEN ? pm->ciphertext_len : MLS_HASH_LEN;
    const MlsSenderDataAAD aad = { pm->group_id, pm->group_id_len, pm->epoch,
                                   pm->content_type };
    if (mls_sender_data_decrypt(group->epoch_secrets.sender_data_secret, &aad, pm->ciphertext,
                                sample_len, pm->encrypted_sender_data,
                                pm->encrypted_sender_data_len, out) != 0)
        return -1;
    if (out->leaf_index >= group->tree.n_leaves ||
        group->tree.nodes[mls_tree_leaf_to_node(out->leaf_index)].type != MLS_NODE_LEAF)
        return -1;
    return 0;
}

/* A Commit sent as a PrivateMessage (MDK's default: OpenMLS
 * MIXED_CIPHERTEXT): decrypted with its sender's handshake ratchet into the
 * FramedContent and auth data a PublicMessage would carry. The ratchet is
 * left exactly as it was: the Commit ends the epoch, and one refused later
 * must not have consumed a key. The caller verifies the signature (wire
 * format mls_private_message); there is no membership tag. */
static int
private_handshake_open(const MlsGroup *group, const MlsPrivateMessage *pm, uint8_t content_type,
                       uint32_t *out_sender, MlsPublicMessage *out)
{
    memset(out, 0, sizeof(*out));
    MlsSenderData sd;
    if (pm->content_type != content_type || private_message_sender(group, pm, &sd) != 0)
        return -1;
    /* The tree is the caller's (const here): borrowed and put back as it was. */
    MlsSecretTree *st = (MlsSecretTree *)&group->secret_tree;
    MlsSenderSnapshot before;
    if (mls_secret_tree_sender_save(st, sd.leaf_index, &before) != 0) return -1;
    uint8_t *content = NULL;
    size_t content_len = 0;
    MlsSenderData used;
    int rc = mls_private_message_decrypt_with_sender_data(pm, &sd, st, group->max_forward_distance,
                                                          &content, &content_len, &used);
    mls_secret_tree_sender_restore(st, &before);
    if (rc != 0) return -1;
    rc = mls_handshake_content_decode(pm, sd.leaf_index, content, content_len, &out->content,
                                      &out->auth);
    sodium_memzero(content, content_len);
    free(content);
    if (rc != 0) return -1;
    *out_sender = sd.leaf_index;
    return 0;
}

/* A Proposal sent as a PrivateMessage opens the same way (nostrc-2um6): the
 * ratchet is put back too, so one refused later consumed nothing. */
static int
private_commit_open(const MlsGroup *group, const MlsPrivateMessage *pm, uint32_t *out_sender,
                    MlsPublicMessage *out)
{
    return private_handshake_open(group, pm, MLS_CONTENT_TYPE_COMMIT, out_sender, out);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Standalone proposals (since 0.12.0, nostrc-2um6)
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_opened_proposal_clear(MlsOpenedProposal *p)
{
    if (!p) return;
    free(p->ac);
    memset(p, 0, sizeof(*p));
}

int
mls_group_open_proposal(const MlsGroup *group, const uint8_t *msg, size_t msg_len,
                        MlsOpenedProposal *out)
{
    if (!group || !msg || !out) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    MlsMLSMessage wire;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, msg, msg_len);
    if (mls_message_deserialize(&reader, &wire) != 0) return MARMOT_ERR_MLS_FRAMING;
    int rc = MARMOT_ERR_MLS_FRAMING;
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    MlsPublicMessage opened;
    memset(&opened, 0, sizeof(opened));
    const MlsPublicMessage *pm = NULL;
    uint16_t wf = wire.wire_format;
    const uint8_t *g = NULL;
    size_t g_len = 0;
    uint64_t epoch = 0;
    if (!mls_tls_reader_done(&reader)) goto out;
    if (wf == MLS_WIRE_FORMAT_PUBLIC_MESSAGE) {
        if (wire.public_message.content.content_type != MLS_CONTENT_TYPE_PROPOSAL) goto out;
        g = wire.public_message.content.group_id;
        g_len = wire.public_message.content.group_id_len;
        epoch = wire.public_message.content.epoch;
    } else if (wf == MLS_WIRE_FORMAT_PRIVATE_MESSAGE) {
        if (wire.private_message.content_type != MLS_CONTENT_TYPE_PROPOSAL) goto out;
        g = wire.private_message.group_id;
        g_len = wire.private_message.group_id_len;
        epoch = wire.private_message.epoch;
    } else {
        goto out;
    }
    rc = MARMOT_ERR_WRONG_GROUP_ID;
    if (g_len != group->group_id_len || memcmp(g, group->group_id, g_len) != 0) goto out;
    rc = MARMOT_ERR_WRONG_EPOCH;
    if (epoch != group->epoch) goto out;
    rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
    if (wf == MLS_WIRE_FORMAT_PUBLIC_MESSAGE) {
        pm = &wire.public_message;
        if (pm->content.sender.sender_type != MLS_SENDER_TYPE_MEMBER) goto out;
        if (mls_group_context_build(group, &gc, &gc_len) != 0) {
            rc = MARMOT_ERR_INTERNAL;
            goto out;
        }
        /* The membership tag is the PublicMessage's own; the signature is
         * checked with the AuthenticatedContent below. */
        if (mls_public_message_verify_membership_tag(pm, group->epoch_secrets.membership_key,
                                                     gc, gc_len) != 0)
            goto out;
    } else {
        /* Our own (a Remove of ourselves): its key is spent in our ratchet. */
        MlsSenderData sd;
        if (private_message_sender(group, &wire.private_message, &sd) == 0 &&
            sd.leaf_index == group->own_leaf_index) {
            sodium_memzero(&sd, sizeof(sd));
            rc = MARMOT_ERR_OWN_MESSAGE;
            goto out;
        }
        sodium_memzero(&sd, sizeof(sd));
        uint32_t from = UINT32_MAX;
        if (private_handshake_open(group, &wire.private_message, MLS_CONTENT_TYPE_PROPOSAL,
                                   &from, &opened) != 0)
            goto out;
        pm = &opened;
    }
    MlsTlsBuf ac;
    if (mls_tls_buf_init(&ac, pm->content.content_len + 128) != 0) {
        rc = MARMOT_ERR_MEMORY;
        goto out;
    }
    if (mls_authenticated_content_serialize(wf, &pm->content, &pm->auth, &ac) != 0) {
        mls_tls_buf_free(&ac);
        goto out;
    }
    MlsProposal prop;
    rc = proposal_ac_open(group, ac.data, ac.len, &prop, out->ref, &out->sender_leaf, &out->wire_format);
    if (rc != 0) {
        mls_tls_buf_free(&ac);
        goto out;
    }
    out->type = prop.type;
    out->target_leaf = prop.type == MLS_PROPOSAL_REMOVE        ? prop.remove.removed_leaf
                       : prop.type == MLS_PROPOSAL_SELF_REMOVE ? out->sender_leaf
                                                                : UINT32_MAX;
    out->component_id = prop.type == MLS_PROPOSAL_APP_DATA_UPDATE
                            ? prop.app_data_update.component_id : 0;
    mls_proposal_clear(&prop);
    out->ac = ac.data;
    out->ac_len = ac.len;
out:
    free(gc);
    mls_public_message_clear(&opened);
    mls_message_clear(&wire);
    if (rc != 0) memset(out, 0, sizeof(*out));
    return rc;
}

int
mls_group_handshake_sender(const MlsGroup *group, const uint8_t *msg, size_t msg_len,
                           uint32_t *out_leaf)
{
    if (!group || !msg || !out_leaf) return MARMOT_ERR_INVALID_ARG;
    MlsMLSMessage wire;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, msg, msg_len);
    if (mls_message_deserialize(&reader, &wire) != 0) return MARMOT_ERR_MLS_FRAMING;
    int rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
    if (!mls_tls_reader_done(&reader)) {
        rc = MARMOT_ERR_MLS_FRAMING;
    } else if (wire.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE) {
        if (wire.public_message.content.sender.sender_type == MLS_SENDER_TYPE_MEMBER) {
            *out_leaf = wire.public_message.content.sender.leaf_index;
            rc = 0;
        }
    } else if (wire.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE) {
        MlsSenderData sd;
        if (private_message_sender(group, &wire.private_message, &sd) == 0) {
            *out_leaf = sd.leaf_index;
            rc = 0;
        }
        sodium_memzero(&sd, sizeof(sd));
    }
    mls_message_clear(&wire);
    return rc;
}

/* The authenticated part of processing a Commit (RFC 9420 sections 6.1-6.3):
 * a Commit of this group and epoch from member `sender_leaf`, as a
 * PublicMessage (signature over the pre-Commit GroupContext and membership
 * tag) or a PrivateMessage (decrypted, then its signature), and its Commit
 * body. On success the caller owns *wire -- a PublicMessage holding the
 * Commit's FramedContent and auth data either way -- (the Commit body points
 * into it), *commit and *pre_gc; *wire_format is the one it arrived in. */
static int
commit_authenticate(const MlsGroup *group, const uint8_t *commit_data, size_t commit_len,
                    uint32_t sender_leaf, MlsMLSMessage *wire, MlsCommit *commit,
                    uint8_t **pre_gc, size_t *pre_gc_len, uint16_t *wire_format)
{
    memset(wire, 0, sizeof(*wire));
    *pre_gc = NULL;
    *pre_gc_len = 0;
    if (mls_group_context_build(group, pre_gc, pre_gc_len) != 0)
        return MARMOT_ERR_INTERNAL;

    MlsTlsReader wire_reader;
    mls_tls_reader_init(&wire_reader, commit_data, commit_len);
    if (mls_message_deserialize(&wire_reader, wire) != 0 ||
        !mls_tls_reader_done(&wire_reader))
        goto fail;
    *wire_format = wire->wire_format;
    if (wire->wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE) {
        MlsPublicMessage opened;
        uint32_t from = UINT32_MAX;
        int open_rc = private_commit_open(group, &wire->private_message, &from, &opened);
        mls_message_clear(wire);
        if (open_rc != 0) goto fail;
        wire->wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;   /* owns the opened content */
        wire->public_message = opened;
        if (from != sender_leaf) goto fail;
    } else if (wire->wire_format != MLS_WIRE_FORMAT_PUBLIC_MESSAGE) {
        goto fail;
    }
    MlsPublicMessage *pm = &wire->public_message;
    if (pm->content.sender.sender_type != MLS_SENDER_TYPE_MEMBER ||
        pm->content.sender.leaf_index != sender_leaf ||
        pm->content.content_type != MLS_CONTENT_TYPE_COMMIT ||
        pm->content.group_id_len != group->group_id_len ||
        memcmp(pm->content.group_id, group->group_id, group->group_id_len) != 0 ||
        pm->content.epoch != group->epoch ||
        !pm->auth.has_confirmation_tag ||
        pm->auth.confirmation_tag_len != MLS_HASH_LEN)
        goto fail;
    uint32_t sender_node_for_sig = mls_tree_leaf_to_node(sender_leaf);
    if (group->tree.nodes[sender_node_for_sig].type != MLS_NODE_LEAF ||
        mls_framed_content_verify(&pm->content, &pm->auth, *wire_format,
                                  *pre_gc, *pre_gc_len,
                                  group->tree.nodes[sender_node_for_sig].leaf.signature_key) != 0)
        goto fail;
    if (*wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE &&
        mls_public_message_verify_membership_tag(pm,
                                                 group->epoch_secrets.membership_key,
                                                 *pre_gc, *pre_gc_len) != 0)
        goto fail;

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, pm->content.content, pm->content.content_len);
    if (mls_commit_deserialize(&reader, commit) != 0 || !mls_tls_reader_done(&reader))
        goto fail;
    return 0;

fail:
    free(*pre_gc);
    *pre_gc = NULL;
    mls_message_clear(wire);
    return MARMOT_ERR_MLS_PROCESS_MESSAGE;
}

/* The leaf a departure proposal removes: a Remove's target, a SelfRemove's
 * sender (UINT32_MAX when inline: its sender would be the committer);
 * UINT32_MAX for every other type. */
static uint32_t
departure_leaf(const MlsProposal *p)
{
    if (p->type == MLS_PROPOSAL_REMOVE) return p->remove.removed_leaf;
    if (p->type == MLS_PROPOSAL_SELF_REMOVE) return p->sender_leaf;
    return UINT32_MAX;
}

/* nostrc-2um6: a Commit's departures, judged on the pre-Commit `group`.
 * Every Remove and SelfRemove names an occupied leaf other than the
 * committer's (RFC 9420 section 12.2; draft-ietf-mls-extensions: the
 * committer never commits its own SelfRemove), each leaf at most once; a
 * SelfRemove is by reference only and needs every member's support.
 * Unresolved references (type 0) are skipped.  Fills `sum`. */
static int
commit_departures_check(const MlsGroup *group, const MlsProposal *props, size_t n,
                        uint32_t committer, MlsCommitSummary *sum)
{
    memset(sum, 0, sizeof(*sum));
    sum->proposal_count = n;
    bool self_remove = false;
    for (size_t i = 0; i < n; i++) {
        const MlsProposal *p = &props[i];
        if (p->type != MLS_PROPOSAL_REMOVE && p->type != MLS_PROPOSAL_SELF_REMOVE) continue;
        uint32_t leaf = departure_leaf(p);
        if (leaf == UINT32_MAX || !leaf_occupied(group, leaf) || leaf == committer) return -1;
        for (size_t j = 0; j < i; j++)
            if (departure_leaf(&props[j]) == leaf) return -1;   /* removed twice */
        if (p->type == MLS_PROPOSAL_SELF_REMOVE) {
            self_remove = true;
            if (sum->self_remove_count == MLS_COMMIT_SUMMARY_MAX) return -1;
            sum->self_removed[sum->self_remove_count++] = leaf;
        } else if (p->sender_leaf == leaf) {
            /* A member's own Remove, by reference: MDK 0.8's leave where the
             * group does not require SelfRemove. */
            if (sum->left_count == MLS_COMMIT_SUMMARY_MAX) return -1;
            sum->left[sum->left_count++] = leaf;
        }
    }
    if (self_remove) {
        /* The leaves that remain must support it (review L2): this Commit's
         * Removes and SelfRemoves do not count. */
        uint32_t gone[2 * MLS_COMMIT_SUMMARY_MAX];
        size_t n_gone = 0;
        for (size_t i = 0; i < n && n_gone < 2 * MLS_COMMIT_SUMMARY_MAX; i++) {
            uint32_t leaf = departure_leaf(&props[i]);
            if (leaf != UINT32_MAX) gone[n_gone++] = leaf;
        }
        if (!members_support_proposal_except(group, MLS_PROPOSAL_SELF_REMOVE, gone, n_gone))
            return -1;
    }
    return 0;
}

/* nostrc-qp24.5.1.3: the shape of a Commit whose references are resolved
 * (an unresolved one counts as other), for the Marmot layer's adopted
 * authorization (group-messaging.md "Commit authorization"; MDK
 * is_allowed_non_admin_commit).  A by-reference proposal is one whose
 * sender_leaf is set: the store sets it, inline proposals have none. */
static int
commit_shape_fill(const MlsProposal *props, size_t n, bool has_path, MlsCommitSummary *sum)
{
    sum->shape_known = true;
    sum->has_path = has_path;
    for (size_t i = 0; i < n; i++) {
        const MlsProposal *p = &props[i];
        switch (p->type) {
        case MLS_PROPOSAL_ADD: sum->add_count++; break;
        case MLS_PROPOSAL_REMOVE: sum->remove_count++; break;
        case MLS_PROPOSAL_UPDATE: sum->update_count++; break;
        case MLS_PROPOSAL_GROUP_CONTEXT_EXT: sum->gce_count++; break;
        case MLS_PROPOSAL_APP_DATA_UPDATE: {
            uint16_t id = p->app_data_update.component_id;
            bool by_ref = p->is_ref || p->sender_leaf != UINT32_MAX;
            sum->adu_count++;
            if (id == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1) sum->adu_lifecycle_count++;
            if (!by_ref && (id == MLS_COMPONENT_APP_COMPONENTS ||
                            id == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1))
                sum->adu_inline_enablement_count++;
            break;
        }
        case MLS_PROPOSAL_SELF_REMOVE: break;   /* counted by the departures */
        default: sum->other_count++; break;
        }
        if (p->is_ref || p->sender_leaf == UINT32_MAX || p->type == MLS_PROPOSAL_SELF_REMOVE)
            continue;
        if (sum->ref_count == MLS_COMMIT_SUMMARY_MAX) return -1;
        sum->ref_sender[sum->ref_count] = p->sender_leaf;
        sum->ref_type[sum->ref_count] = p->type;
        sum->ref_component[sum->ref_count] =
            p->type == MLS_PROPOSAL_APP_DATA_UPDATE ? p->app_data_update.component_id : 0;
        sum->ref_count++;
    }
    return 0;
}

/* mls_group_commit_removes_self() with references resolved against `store`
 * (nullable): an unresolved one proves nothing and is skipped. */
static int
commit_removes_self_impl(const MlsGroup *group, const uint8_t *commit_data, size_t commit_len,
                         uint32_t sender_leaf, MlsProposalStore *store, bool *out_removed,
                         MlsCommitSummary *summary)
{
    if (!group || !commit_data || !out_removed) return MARMOT_ERR_INVALID_ARG;
    *out_removed = false;
    if (sender_leaf >= group->tree.n_leaves ||
        group->own_leaf_index >= group->tree.n_leaves)
        return MARMOT_ERR_INVALID_ARG;
    if (sender_leaf == group->own_leaf_index) return MARMOT_ERR_OWN_COMMIT_PENDING;
    MlsMLSMessage wire;
    MlsCommit commit;
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint16_t wire_format = 0;
    int rc = commit_authenticate(group, commit_data, commit_len, sender_leaf, &wire, &commit,
                                 &pre_gc, &pre_gc_len, &wire_format);
    if (rc != 0) return rc;
    free(pre_gc);
    if (!commit.has_path) rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;   /* a removal needs an UpdatePath */
    size_t gce = 0;
    for (size_t i = 0; rc == 0 && i < commit.proposal_count; i++) {
        MlsProposal *p = &commit.proposals[i];
        if (p->is_ref && store) (void)proposal_store_resolve(store, p);
        if (p->is_ref) continue;
        switch (p->type) {
        case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
            if (++gce > 1) rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            break;
        case MLS_PROPOSAL_ADD:
        case MLS_PROPOSAL_UPDATE:
        case MLS_PROPOSAL_REMOVE:
        case MLS_PROPOSAL_PSK:
        case MLS_PROPOSAL_SELF_REMOVE:
            break;
        case MLS_PROPOSAL_APP_DATA_UPDATE:
            /* An admin's removal of another admin's last leaf carries the
             * admin-policy update with it (admin-policy-v1.md; MDK
             * do_send_remove_members): adopted groups only. */
            if (group->profile != MARMOT_GROUP_PROFILE_ADOPTED)
                rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            break;
        default:
            rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            break;
        }
    }
    MlsCommitSummary sum;
    if (rc == 0 &&
        (commit_departures_check(group, commit.proposals, commit.proposal_count, sender_leaf,
                                 &sum) != 0 ||
         commit_shape_fill(commit.proposals, commit.proposal_count, commit.has_path, &sum) != 0))
        rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;   /* not a valid removal (section 12.1.3) */
    for (size_t i = 0; rc == 0 && i < commit.proposal_count; i++)
        if (departure_leaf(&commit.proposals[i]) == group->own_leaf_index)
            *out_removed = true;
    if (rc == 0 && summary) *summary = sum;
    if (rc != 0) *out_removed = false;
    mls_commit_clear(&commit);
    mls_message_clear(&wire);
    return rc;
}

int
mls_group_commit_authentic(const MlsGroup *group, const uint8_t *commit_data,
                           size_t commit_len, uint32_t sender_leaf)
{
    if (!group || !commit_data || sender_leaf >= group->tree.n_leaves)
        return MARMOT_ERR_INVALID_ARG;
    MlsMLSMessage wire;
    MlsCommit commit;
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint16_t wire_format = 0;
    int rc = commit_authenticate(group, commit_data, commit_len, sender_leaf, &wire, &commit,
                                 &pre_gc, &pre_gc_len, &wire_format);
    if (rc != 0) return rc;
    free(pre_gc);
    mls_commit_clear(&commit);
    mls_message_clear(&wire);
    return 0;
}

int
mls_group_commit_removes_self(const MlsGroup *group,
                              const uint8_t *commit_data, size_t commit_len,
                              uint32_t sender_leaf, bool *out_removed)
{
    return commit_removes_self_impl(group, commit_data, commit_len, sender_leaf, NULL,
                                    out_removed, NULL);
}

int
mls_group_commit_removes_self_by_ref(const MlsGroup *group,
                                     const uint8_t *commit_data, size_t commit_len,
                                     uint32_t sender_leaf,
                                     const uint8_t *const *acs, const size_t *ac_lens,
                                     size_t ac_count, bool *out_removed,
                                     MlsCommitSummary *summary)
{
    if (!group || !out_removed) return MARMOT_ERR_INVALID_ARG;
    *out_removed = false;
    MlsProposalStore store;
    if (proposal_store_build_ac(&store, group, acs, ac_lens, ac_count) != 0)
        return MARMOT_ERR_MEMORY;
    int rc = commit_removes_self_impl(group, commit_data, commit_len, sender_leaf, &store,
                                      out_removed, summary);
    proposal_store_free(&store);
    return rc;
}

/* nostrc-2um6: the departures `props` (by reference, already validated by
 * commit_departures_check()) applied as receivers apply them, then the
 * UpdatePath.  `props` is owned by the Commit from here. */
static int
commit_by_ref_staged(MlsGroup *group, MlsProposal *props, size_t n, MlsCommitResult *result)
{
    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    uint64_t pre_epoch = group->epoch;
    uint8_t pre_membership_key[MLS_HASH_LEN];
    uint8_t confirmation_tag[MLS_HASH_LEN];
    memcpy(pre_membership_key, group->epoch_secrets.membership_key, MLS_HASH_LEN);
    if (mls_group_context_build(group, &pre_gc, &pre_gc_len) != 0) {
        for (size_t i = 0; i < n; i++) mls_proposal_clear(&props[i]);
        free(props);
        return MARMOT_ERR_INTERNAL;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t node = mls_tree_leaf_to_node(departure_leaf(&props[i]));
        mls_tree_blank_node(&group->tree.nodes[node]);
        uint32_t path[64];
        uint32_t path_len = 0;
        if (mls_tree_direct_path(node, group->tree.n_leaves, path, 64, &path_len) != 0) {
            for (size_t k = 0; k < n; k++) mls_proposal_clear(&props[k]);
            free(props);
            free(pre_gc);
            return MARMOT_ERR_INTERNAL;
        }
        for (uint32_t j = 0; j < path_len; j++) mls_tree_blank_node(&group->tree.nodes[path[j]]);
    }
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    int rc = path_commit_with_proposals(group, props, n, NULL, 0, NULL, NULL, pre_gc, pre_gc_len,
                                        pre_epoch, pre_membership_key, confirmation_tag,
                                        &commit, &commit_len);
    free(pre_gc);
    if (rc != 0) return rc;
    result->commit_data = commit;
    result->commit_len = commit_len;
    return 0;
}

int
mls_group_commit_by_ref(MlsGroup *group, const uint8_t *const *acs, const size_t *ac_lens,
                        size_t ac_count, MlsCommitResult *result)
{
    if (!group || !acs || !ac_lens || ac_count == 0 || !result) return MARMOT_ERR_INVALID_ARG;
    if (ac_count > MLS_COMMIT_SUMMARY_MAX) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    MlsProposalStore store;
    if (proposal_store_build_ac(&store, group, acs, ac_lens, ac_count) != 0)
        return MARMOT_ERR_MEMORY;
    int rc = store.count == ac_count ? 0 : MARMOT_ERR_INVALID_ARG;   /* each must open */
    MlsProposal *props = rc == 0 ? calloc(ac_count, sizeof(*props)) : NULL;
    if (rc == 0 && !props) rc = MARMOT_ERR_MEMORY;
    for (size_t i = 0; rc == 0 && i < ac_count; i++) {
        const MlsStoredProposal *sp = &store.items[i];
        if ((sp->prop.type != MLS_PROPOSAL_REMOVE && sp->prop.type != MLS_PROPOSAL_SELF_REMOVE) ||
            sp->prop.sender_leaf == group->own_leaf_index) {
            rc = MARMOT_ERR_INVALID_ARG;   /* only others' departures */
            break;
        }
        props[i].type = sp->prop.type;
        props[i].sender_leaf = sp->prop.sender_leaf;
        props[i].update_leaf_index = UINT32_MAX;
        if (sp->prop.type == MLS_PROPOSAL_REMOVE)
            props[i].remove.removed_leaf = sp->prop.remove.removed_leaf;
        props[i].is_ref = true;
        props[i].ref_len = sp->ref_len;
        memcpy(props[i].ref, sp->ref, sp->ref_len);
    }
    proposal_store_free(&store);
    MlsCommitSummary sum;
    if (rc == 0 && commit_departures_check(group, props, ac_count, group->own_leaf_index,
                                           &sum) != 0)
        rc = MARMOT_ERR_INVALID_ARG;   /* removed twice, gone, ours, or unsupported */
    if (rc != 0) {
        free(props);   /* references own nothing */
        return rc;
    }
    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) {
        free(props);
        return MARMOT_ERR_INTERNAL;
    }
    rc = commit_by_ref_staged(&staged, props, ac_count, result);
    if (rc == 0) rc = group_install_checked(group, &staged);
    else mls_group_free(&staged);
    if (rc != 0) mls_commit_result_clear(result);
    return rc;
}

/* nostrc-2um6 review M1: MDK 0.8's leave where the group does not require
 * SelfRemove -- a Remove of our own leaf, sent as a PrivateMessage under
 * our handshake ratchet (MDK groups may reject a PublicMessage handshake).
 * The ratchet step is taken in `group` (the caller stores it). */
int
mls_group_remove_self_proposal(MlsGroup *group, uint8_t **out_msg, size_t *out_len,
                               MlsOpenedProposal *out_own)
{
    if (!group || !out_msg || !out_len || !out_own) return MARMOT_ERR_INVALID_ARG;
    *out_msg = NULL;
    *out_len = 0;
    memset(out_own, 0, sizeof(*out_own));
    uint32_t own = group->own_leaf_index;
    if (!leaf_occupied(group, own)) return MARMOT_ERR_INVALID_ARG;
    uint8_t body[6] = { 0x00, 0x03, (uint8_t)(own >> 24), (uint8_t)(own >> 16),
                        (uint8_t)(own >> 8), (uint8_t)own };
    MlsFramedContent fc;
    memset(&fc, 0, sizeof(fc));
    fc.group_id = group->group_id;          /* borrowed */
    fc.group_id_len = group->group_id_len;
    fc.epoch = group->epoch;
    fc.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    fc.sender.leaf_index = own;
    fc.content_type = MLS_CONTENT_TYPE_PROPOSAL;
    fc.content = body;
    fc.content_len = sizeof body;
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    MlsFramedContentAuthData auth;
    memset(&auth, 0, sizeof(auth));
    MlsTlsBuf pt = {0}, ac = {0}, out = {0};
    MlsMLSMessage w;
    memset(&w, 0, sizeof(w));
    MlsMessageKeys keys;
    memset(&keys, 0, sizeof(keys));
    MlsProposal prop;
    memset(&prop, 0, sizeof(prop));
    int rc = MARMOT_ERR_INTERNAL;
    if (mls_group_context_build(group, &gc, &gc_len) != 0 ||
        mls_framed_content_sign(&fc, MLS_WIRE_FORMAT_PRIVATE_MESSAGE, gc, gc_len,
                                group->own_signature_key, &auth) != 0)
        goto out;
    /* PrivateMessageContent: the Proposal, its signature<V>, no padding. */
    if (mls_tls_buf_init(&pt, 128) != 0 || mls_tls_buf_append(&pt, body, sizeof body) != 0 ||
        mls_tls_write_opaque16(&pt, auth.signature, MLS_SIG_LEN) != 0)
        goto out;
    if (mls_secret_tree_derive_keys(&group->secret_tree, own, true, &keys) != 0) goto out;
    uint8_t guard[4];
    mls_crypto_random(guard, sizeof guard);
    w.wire_format = MLS_WIRE_FORMAT_PRIVATE_MESSAGE;
    w.cipher_suite = MARMOT_CIPHERSUITE;
    if (mls_private_message_encrypt(group->group_id, group->group_id_len, group->epoch,
                                    MLS_CONTENT_TYPE_PROPOSAL, NULL, 0, pt.data, pt.len,
                                    group->epoch_secrets.sender_data_secret, &keys, own, guard,
                                    &w.private_message) != 0)
        goto out;
    if (mls_tls_buf_init(&out, 256) != 0 || mls_message_serialize(&w, &out) != 0) goto out;
    /* Kept as receivers keep it (its key is spent here, so it is not
     * decrypted again): AuthenticatedContent under mls_private_message. */
    if (mls_tls_buf_init(&ac, 256) != 0 ||
        mls_authenticated_content_serialize(MLS_WIRE_FORMAT_PRIVATE_MESSAGE, &fc, &auth,
                                            &ac) != 0)
        goto out;
    rc = proposal_ac_open(group, ac.data, ac.len, &prop, out_own->ref, &out_own->sender_leaf,
                          &out_own->wire_format);
    if (rc != 0) goto out;
    out_own->type = MLS_PROPOSAL_REMOVE;
    out_own->target_leaf = own;
    out_own->ac = ac.data;
    out_own->ac_len = ac.len;
    ac.data = NULL;
    *out_msg = out.data;
    *out_len = out.len;
    out.data = NULL;
    rc = 0;
out:
    mls_proposal_clear(&prop);
    sodium_memzero(&keys, sizeof(keys));
    mls_message_clear(&w);
    mls_tls_buf_free(&pt);
    mls_tls_buf_free(&ac);
    mls_tls_buf_free(&out);
    free(auth.signature_data);
    free(gc);
    return rc;
}

int
mls_group_self_remove_proposal(const MlsGroup *group, uint8_t **out_msg, size_t *out_len,
                               MlsOpenedProposal *out_own)
{
    if (!group || !out_msg || !out_len || !out_own) return MARMOT_ERR_INVALID_ARG;
    *out_msg = NULL;
    *out_len = 0;
    memset(out_own, 0, sizeof(*out_own));
    if (!leaf_occupied(group, group->own_leaf_index)) return MARMOT_ERR_INVALID_ARG;
    if (!mls_group_members_support_proposal(group, MLS_PROPOSAL_SELF_REMOVE))
        return MARMOT_ERR_UNSUPPORTED;
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    if (mls_group_context_build(group, &gc, &gc_len) != 0) return MARMOT_ERR_INTERNAL;
    int rc = MARMOT_ERR_INTERNAL;
    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg.public_message;
    static const uint8_t body[2] = { 0x00, 0x0A };   /* Proposal { self_remove; SelfRemove {} } */
    pm->content.group_id = malloc(group->group_id_len);
    pm->content.content = malloc(sizeof body);
    MlsTlsBuf buf = {0};
    if (!pm->content.group_id || !pm->content.content) {
        rc = MARMOT_ERR_MEMORY;
        goto out;
    }
    memcpy(pm->content.group_id, group->group_id, group->group_id_len);
    pm->content.group_id_len = group->group_id_len;
    pm->content.epoch = group->epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = group->own_leaf_index;
    pm->content.content_type = MLS_CONTENT_TYPE_PROPOSAL;
    memcpy(pm->content.content, body, sizeof body);
    pm->content.content_len = sizeof body;
    if (mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE, gc, gc_len,
                                group->own_signature_key, &pm->auth) != 0 ||
        mls_public_message_compute_membership_tag(pm, group->epoch_secrets.membership_key,
                                                  gc, gc_len) != 0 ||
        mls_tls_buf_init(&buf, 256) != 0 || mls_message_serialize(&msg, &buf) != 0)
        goto out;
    /* Opened as any receiver opens it: what our Commit store keeps. */
    rc = mls_group_open_proposal(group, buf.data, buf.len, out_own);
    if (rc != 0) goto out;
    *out_msg = buf.data;
    *out_len = buf.len;
    buf.data = NULL;
out:
    mls_tls_buf_free(&buf);
    mls_message_clear(&msg);
    free(gc);
    return rc;
}

static int
process_commit_impl(MlsGroup *group,
                    const uint8_t *commit_data, size_t commit_len,
                    uint32_t sender_leaf,
                    MlsProposalStore *store,
                    const MlsPskInput *external_psks,
                    size_t external_psk_count,
                    MlsCommitSummary *summary,
                    MlsGroup *public_out)
{
    if (!group || !commit_data) return MARMOT_ERR_INVALID_ARG;
    if (sender_leaf >= group->tree.n_leaves) return MARMOT_ERR_INVALID_ARG;
    if (sender_leaf == group->own_leaf_index) return MARMOT_ERR_OWN_COMMIT_PENDING;

    uint8_t *pre_gc = NULL;
    size_t pre_gc_len = 0;
    MlsMLSMessage wire_msg;
    MlsCommit commit;
    uint16_t wire_format = 0;
    int auth_rc = commit_authenticate(group, commit_data, commit_len, sender_leaf,
                                      &wire_msg, &commit, &pre_gc, &pre_gc_len, &wire_format);
    if (auth_rc != 0) return auth_rc;
    MlsPublicMessage *pm = &wire_msg.public_message;

    MlsGroup staged;
    if (group_stage_clone(group, &staged) != 0) {
        free(pre_gc);
        mls_commit_clear(&commit);
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_INTERNAL;
    }
    MlsGroup *live_group = group;
    group = &staged;
    int staged_rc = MARMOT_ERR_INTERNAL;

    /* Resolve referenced proposals (ProposalOrRef type 2) against the store,
     * and reject commits carrying proposal types we still do not apply. */
    for (size_t i = 0; i < commit.proposal_count; i++) {
        MlsProposal *p = &commit.proposals[i];
        if (p->is_ref) {
            int resolved = proposal_store_resolve(store, p);
            if (resolved != 0) {
                mls_commit_clear(&commit);
                /* Without a proposal store we cannot resolve references;
                 * with one, the referent has not arrived (yet): the
                 * authenticated Commit is offered again once it does
                 * (nostrc-2um6 review H1). */
                staged_rc = !store          ? MARMOT_ERR_UNSUPPORTED
                            : resolved == -2    ? MARMOT_ERR_MLS_PROCESS_MESSAGE
                                                : MARMOT_ERR_PROPOSAL_UNKNOWN;
                goto staged_fail;
            }
        }
        if (!proposal_type_apply_supported(group, p)) {
            mls_commit_clear(&commit);
            staged_rc = MARMOT_ERR_UNSUPPORTED;
            goto staged_fail;
        }
    }

    /* Once references are resolved every proposal type is known: enforce the
     * UpdatePath requirement before anything is applied (RFC 9420 §12.4). */
    if (!commit.has_path &&
        commit_path_required(commit.proposals, commit.proposal_count)) {
        mls_commit_clear(&commit);
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }

    /* Validate proposal ordering per RFC 9420 */
    if (validate_proposal_ordering(commit.proposals, commit.proposal_count) != 0) {
        mls_commit_clear(&commit);
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }

    /* Removes and SelfRemoves, on the pre-Commit tree (nostrc-2um6).  Among
     * them RFC 9420 §12.2: a Commit MUST NOT remove its committer, which
     * Marmot authorization relies on (the committer's leaf is the one leaf a
     * Commit renews in place; W24 review A2, test_commit_removing_committer_
     * refused). */
    {
        MlsCommitSummary sum;
        if (commit_departures_check(group, commit.proposals, commit.proposal_count,
                                    sender_leaf, &sum) != 0 ||
            commit_shape_fill(commit.proposals, commit.proposal_count, commit.has_path,
                              &sum) != 0) {
            mls_commit_clear(&commit);
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
        if (summary) *summary = sum;
    }

    /* Validate every Update's LeafNode (RFC 9420 §12.1.2, §7.3) against
     * the pre-Commit tree, before any proposal is applied.  Capabilities are
     * checked against the GroupContext extensions of the epoch being entered:
     * the last GroupContextExtensions proposal, as applied below. */
    {
        const uint8_t *next_ext = group->extensions_data;
        size_t next_ext_len = group->extensions_len;
        for (size_t i = 0; i < commit.proposal_count; i++) {
            if (commit.proposals[i].type != MLS_PROPOSAL_GROUP_CONTEXT_EXT) continue;
            next_ext = commit.proposals[i].group_context_extensions.extensions;
            next_ext_len = commit.proposals[i].group_context_extensions.extensions_len;
        }
        for (size_t i = 0; i < commit.proposal_count; i++) {
            if (update_proposal_validate(group, &commit.proposals[i], sender_leaf,
                                         next_ext, next_ext_len) != 0) {
                mls_commit_clear(&commit);
                staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
                goto staged_fail;
            }
            /* nostrc-zbmb: so is every joiner's leaf (RFC 9420 §12.1.1). */
            if (commit.proposals[i].type == MLS_PROPOSAL_ADD &&
                key_package_supports_group(&commit.proposals[i].add.key_package, next_ext,
                                           next_ext_len) != 0) {
                mls_commit_clear(&commit);
                staged_rc = MARMOT_ERR_KEY_PACKAGE_CAPABILITIES;
                goto staged_fail;
            }
        }
    }

    uint8_t psk_secret[MLS_HASH_LEN];
    int psk_rc = commit_psk_secret_compute(group, commit.proposals,
                                           commit.proposal_count,
                                           external_psks, external_psk_count,
                                           psk_secret);
    if (psk_rc != 0) {
        mls_commit_clear(&commit);
        staged_rc = psk_rc;
        goto staged_fail;
    }

    if (sort_proposals_for_application(commit.proposals, commit.proposal_count) != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_MEMORY;
        goto staged_fail;
    }

    uint32_t added_leaves[64];
    size_t added_leaf_count = 0;
    uint32_t updated_leaves[64];
    size_t updated_leaf_count = 0;
    bool has_gce = false;
    /* AppDataUpdates, in Commit order, applied after the others
     * (nostrc-qp24.5.1.3; OpenMLS apply_proposals_with_app_data_updates). */
    size_t adu_count = 0;

    /* Apply proposals */
    for (size_t i = 0; i < commit.proposal_count; i++) {
        MlsProposal *p = &commit.proposals[i];
        switch (p->type) {
        case MLS_PROPOSAL_ADD: {
            if (mls_key_package_validate(&p->add.key_package) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
                goto staged_fail;
            }
            uint32_t new_leaf_idx;
            if (mls_tree_add_leaf(&group->tree, &new_leaf_idx) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            MlsNode *n = &group->tree.nodes[new_leaf_idx];
            n->type = MLS_NODE_LEAF;
            if (mls_leaf_node_clone(&n->leaf, &p->add.key_package.leaf_node) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            if (added_leaf_count < sizeof(added_leaves) / sizeof(added_leaves[0]))
                added_leaves[added_leaf_count++] = mls_tree_node_to_leaf(new_leaf_idx);
            else {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            if (tree_add_unmerged_leaf(&group->tree, new_leaf_idx) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            break;
        }
        case MLS_PROPOSAL_REMOVE: {
            /* Validate leaf index */
            if (p->remove.removed_leaf >= group->tree.n_leaves) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INVALID_ARG;
                goto staged_fail;
            }
            uint32_t rm_node = mls_tree_leaf_to_node(p->remove.removed_leaf);
            mls_tree_blank_node(&group->tree.nodes[rm_node]);
            /* Blank path to root */
            uint32_t dp[64];
            uint32_t dp_len = 0;
            if (mls_tree_direct_path(rm_node, group->tree.n_leaves, dp, 64, &dp_len) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            
            for (uint32_t j = 0; j < dp_len; j++)
                mls_tree_blank_node(&group->tree.nodes[dp[j]]);
            break;
        }
        case MLS_PROPOSAL_UPDATE: {
            /* Update targets the proposer's leaf (from the standalone proposal
             * framing); update_proposal_validate() already rejected inline and
             * committer-sent Updates and validated the LeafNode. */
            uint32_t upd_leaf = p->update_leaf_index;
            if (upd_leaf >= group->tree.n_leaves ||
                updated_leaf_count >= sizeof(updated_leaves) / sizeof(updated_leaves[0])) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INVALID_ARG;
                goto staged_fail;
            }
            updated_leaves[updated_leaf_count++] = upd_leaf;
            uint32_t upd_node = mls_tree_leaf_to_node(upd_leaf);
            mls_leaf_node_clear(&group->tree.nodes[upd_node].leaf);
            if (mls_leaf_node_clone(&group->tree.nodes[upd_node].leaf,
                                     &p->update.leaf_node) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            /* Applying an Update blanks the updated leaf's direct path to the
             * root (RFC 9420 §12.1.2); the stale path secrets are invalidated. */
            uint32_t upd_dp[64];
            uint32_t upd_dp_len = 0;
            if (mls_tree_direct_path(upd_node, group->tree.n_leaves,
                                     upd_dp, 64, &upd_dp_len) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            for (uint32_t j = 0; j < upd_dp_len; j++)
                mls_tree_blank_node(&group->tree.nodes[upd_dp[j]]);
            break;
        }
        case MLS_PROPOSAL_SELF_REMOVE: {
            /* The sender leaves: its leaf and direct path are blanked as for
             * a Remove (commit_departures_check() validated it). */
            uint32_t sr_node = mls_tree_leaf_to_node(p->sender_leaf);
            mls_tree_blank_node(&group->tree.nodes[sr_node]);
            uint32_t sr_dp[64];
            uint32_t sr_dp_len = 0;
            if (mls_tree_direct_path(sr_node, group->tree.n_leaves, sr_dp, 64, &sr_dp_len) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            for (uint32_t j = 0; j < sr_dp_len; j++)
                mls_tree_blank_node(&group->tree.nodes[sr_dp[j]]);
            break;
        }
        case MLS_PROPOSAL_PSK:
            /* PSKs are applied by injecting the combined psk_secret into the
             * epoch key schedule below.  They do not directly mutate the tree. */
            break;
        case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
            has_gce = true;
            if (apply_group_context_extensions(group,
                    p->group_context_extensions.extensions,
                    p->group_context_extensions.extensions_len) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_UNSUPPORTED;
                goto staged_fail;
            }
            break;
        case MLS_PROPOSAL_APP_DATA_UPDATE:
            adu_count++;   /* applied below, in Commit order */
            break;
        default:
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
    }

    /* The app_data_dictionary the AppDataUpdates leave (an adopted group's
     * only GroupContext change): its bytes are judged with the resulting
     * epoch below (mls_group_profile_check_entered()) and by the Marmot
     * layer (component values, who sent and committed them). */
    if (adu_count > 0) {
        uint8_t *next_ext = NULL;
        size_t next_len = 0;
        /* Sized by the Commit, not by MLS_APP_DATA_UPDATE_MAX (review L3). */
        const MlsAppDataUpdate **adus = malloc(adu_count * sizeof(*adus));
        int arc = MARMOT_ERR_MEMORY;
        if (adus) {
            size_t k = 0;
            for (size_t i = 0; i < commit.proposal_count; i++)
                if (commit.proposals[i].type == MLS_PROPOSAL_APP_DATA_UPDATE)
                    adus[k++] = &commit.proposals[i].app_data_update;
            arc = mls_app_data_update_apply(group->extensions_data, group->extensions_len,
                                            adus, adu_count, &next_ext, &next_len);
            free(adus);
        }
        if (arc != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = arc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY
                                                  : MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
        free(group->extensions_data);
        group->extensions_data = next_ext;
        group->extensions_len = next_len;
    }

    /* §7.3 step 8: every leaf this Commit installed has signature and
     * encryption keys unique in the resulting tree.  A leaf that a later
     * proposal blanked again (Update plus Remove of one leaf) fails too. */
    for (size_t i = 0; i < updated_leaf_count + added_leaf_count; i++) {
        uint32_t leaf = (i < updated_leaf_count) ? updated_leaves[i]
                                                 : added_leaves[i - updated_leaf_count];
        if (leaf_keys_unique(&group->tree, leaf) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
    }

    /* RFC 9420 §12.1.7: the new GroupContext extensions must be supported by
     * every member of the epoch being entered -- including leaves this Commit
     * adds or updates.  The committer's leaf is replaced by the (path-
     * required) UpdatePath leaf, which leaf_node_validate() checks against
     * the new extensions below. */
    if (has_gce) {
        if (group_context_extensions_validate(group, group->extensions_data,
                                              group->extensions_len,
                                              sender_leaf) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
    }

    if (public_out && !commit.has_path) goto public_result;

    /* Process UpdatePath if present */
    uint8_t root_path_secret[MLS_HASH_LEN];
    bool has_path = commit.has_path;
    if (has_path) {
        /* RFC 9420 §12.4.2: validate the UpdatePath LeafNode (§7.3, source
         * commit, bound to the committer's leaf) and the freshness of every
         * UpdatePath public key against the tree the proposals produced,
         * before anything of the path is merged. */
        uint32_t sender_node = mls_tree_leaf_to_node(sender_leaf);
        if (group->tree.nodes[sender_node].type != MLS_NODE_LEAF ||
            leaf_node_validate(group, &commit.path.leaf_node,
                               MLS_LEAF_NODE_SOURCE_COMMIT, sender_leaf,
                               &group->tree.nodes[sender_node].leaf,
                               group->extensions_data, group->extensions_len) != 0 ||
            update_path_keys_fresh(&group->tree, sender_leaf, &commit.path) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }

        /* Update sender's leaf in the tree */
        mls_leaf_node_clear(&group->tree.nodes[sender_node].leaf);
        if (mls_leaf_node_clone(&group->tree.nodes[sender_node].leaf,
                                 &commit.path.leaf_node) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_INTERNAL;
            goto staged_fail;
        }

        /* Find our position relative to the sender's direct path */
        uint32_t fdp[64];
        uint32_t fdp_len = 0;
        if (mls_tree_filtered_direct_path(&group->tree, sender_leaf,
                                           fdp, 64, &fdp_len) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_INTERNAL;
            goto staged_fail;
        }

        /* One UpdatePathNode per filtered direct path node, each with exactly
         * one HPKECiphertext per node of its copath resolution minus the
         * leaves this Commit adds (RFC 9420 §7.6, §12.4.2).  Checked for every
         * node, not only ours, so all members accept or reject a Commit alike:
         * a path that also encrypts to a new member would otherwise fork the
         * group between members whose index it shifts and those it does not. */
        if (commit.path.node_count != fdp_len) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
        for (uint32_t i = 0; i < fdp_len; i++) {
            uint32_t below = UINT32_MAX;
            uint32_t res[256];
            uint32_t res_len = 0;
            if (child_below_path_node(sender_leaf, group->tree.n_leaves,
                                      fdp[i], &below) != 0 ||
                resolution_excluding(&group->tree,
                                     mls_tree_sibling(below, group->tree.n_leaves),
                                     added_leaves, added_leaf_count,
                                     res, 256, &res_len) != 0 ||
                commit.path.nodes[i].secret_count != res_len) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
                goto staged_fail;
            }
        }

        /* The public result (mls_group_commit_public_result_by_ref()): what
         * a member the Commit removes can judge -- the tree its proposals
         * and UpdatePath leaf leave, and the GroupContext extensions -- with
         * no path secret to decrypt and no key schedule (slice H review
         * L1). */
        if (public_out) goto public_result;

        /* Find which copath node we're under */
        uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
        int our_path_idx = -1;

        for (uint32_t i = 0; i < fdp_len && i < commit.path.node_count; i++) {
            uint32_t child_below = UINT32_MAX;
            if (child_below_path_node(sender_leaf, group->tree.n_leaves,
                                      fdp[i], &child_below) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(psk_secret, sizeof(psk_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            uint32_t copath_sibling = mls_tree_sibling(child_below, group->tree.n_leaves);
            /* Check if we're in the resolution of this copath sibling */
            uint32_t resolution[256];
            uint32_t res_len = 0;
            if (mls_tree_resolution(&group->tree, copath_sibling,
                                     resolution, 256, &res_len) == 0) {
                for (uint32_t j = 0; j < res_len; j++) {
                    if (resolution[j] == own_node) {
                        our_path_idx = (int)i;
                        break;
                    }
                    const uint8_t *cached_sk = NULL;
                    const uint8_t *cached_pk = NULL;
                    if (lookup_own_path_key(group, resolution[j],
                                            &cached_sk, &cached_pk) == 0) {
                        our_path_idx = (int)i;
                        break;
                    }
                }
            }
            if (our_path_idx >= 0) break;
        }

        if (our_path_idx < 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }

        /* Decrypt our path secret */
        uint32_t child_below = UINT32_MAX;
        if (child_below_path_node(sender_leaf, group->tree.n_leaves,
                                  fdp[our_path_idx], &child_below) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_INTERNAL;
            goto staged_fail;
        }
        uint32_t copath_sibling = mls_tree_sibling(child_below,
                                                    group->tree.n_leaves);
        /* Get our encryption keys */
        const uint8_t *our_enc_pk = group->tree.nodes[own_node].leaf.encryption_key;
        const uint8_t *our_enc_sk = group->own_encryption_key;

        uint8_t our_path_secret[MLS_HASH_LEN];

        /* Decrypt from the path node using our stored private key.  The
         * UpdatePathNode HPKE info is bound to the provisional GroupContext
         * for the target epoch: post-UpdatePath tree hash and otherwise the
         * commit's resulting epoch context inputs. */
        uint8_t *path_context = NULL;
        size_t path_context_len = 0;
        uint8_t *tree_snapshot = NULL;
        size_t tree_snapshot_len = 0;
        MlsRatchetTree context_tree;
        memset(&context_tree, 0, sizeof(context_tree));
        uint8_t provisional_tree_hash[MLS_HASH_LEN];
        if (mls_ratchet_tree_serialize(&group->tree, &tree_snapshot,
                                       &tree_snapshot_len) != 0 ||
            mls_ratchet_tree_deserialize(tree_snapshot, tree_snapshot_len,
                                         &context_tree) != 0 ||
            mls_treekem_apply_update_path(&context_tree, sender_leaf,
                                          &commit.path) != 0 ||
            mls_tree_root_hash(&context_tree, provisional_tree_hash) != 0 ||
            mls_group_context_serialize(group->group_id, group->group_id_len,
                                        group->epoch + 1, provisional_tree_hash,
                                        group->confirmed_transcript_hash,
                                        group->extensions_data, group->extensions_len,
                                        &path_context, &path_context_len) != 0) {
            free(tree_snapshot);
            mls_tree_free(&context_tree);
            mls_commit_clear(&commit);
            staged_rc = MARMOT_ERR_INTERNAL;
            goto staged_fail;
        }
        free(tree_snapshot);
        mls_tree_free(&context_tree);

        int decrypt_rc = decrypt_path_secret(group, &commit.path.nodes[our_path_idx],
                                             copath_sibling,
                                             added_leaves, added_leaf_count,
                                             path_context, path_context_len,
                                             our_enc_sk,
                                             our_enc_pk,
                                             our_path_secret);
        if (decrypt_rc != 0) {
            free(path_context);
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            staged_rc = MARMOT_ERR_CRYPTO;
            goto staged_fail;
        }
        free(path_context);

        /* Derive path secrets up to the top filtered node, caching the
         * private keys for the path nodes whose secrets we learn.  Future
         * commits may encrypt to one of these parent nodes rather than to our
         * leaf directly. */
        uint8_t current_secret[MLS_HASH_LEN];
        memcpy(current_secret, our_path_secret, MLS_HASH_LEN);
        for (uint32_t i = (uint32_t)our_path_idx; i < fdp_len; i++) {
            uint8_t node_sk[MLS_KEM_SK_LEN];
            uint8_t node_pk[MLS_KEM_PK_LEN];
            if (mls_tree_derive_node_keypair(current_secret, node_sk, node_pk) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(node_sk, sizeof(node_sk));
                sodium_memzero(node_pk, sizeof(node_pk));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            if (memcmp(node_pk, commit.path.nodes[i].encryption_key,
                       MLS_KEM_PK_LEN) == 0 &&
                remember_own_path_key(group, fdp[i], node_sk, node_pk) != 0) {
                mls_commit_clear(&commit);
                sodium_memzero(node_sk, sizeof(node_sk));
                sodium_memzero(node_pk, sizeof(node_pk));
                sodium_memzero(psk_secret, sizeof(psk_secret));
                sodium_memzero(our_path_secret, sizeof(our_path_secret));
                sodium_memzero(current_secret, sizeof(current_secret));
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
            sodium_memzero(node_sk, sizeof(node_sk));
            sodium_memzero(node_pk, sizeof(node_pk));
            if (i + 1 < fdp_len &&
                mls_crypto_derive_secret(current_secret, current_secret, "path") != 0) {
                mls_commit_clear(&commit);
                staged_rc = MARMOT_ERR_INTERNAL;
                goto staged_fail;
            }
        }
        memcpy(root_path_secret, current_secret, MLS_HASH_LEN);

        uint8_t final_tree_hash[MLS_HASH_LEN];
        int final_apply_rc = mls_treekem_apply_update_path(&group->tree,
                                                           sender_leaf,
                                                           &commit.path);
        int final_hash_rc = (final_apply_rc == 0)
                                ? mls_group_tree_hash(group, final_tree_hash)
                                : -1;
        if (final_apply_rc != 0 || final_hash_rc != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            sodium_memzero(our_path_secret, sizeof(our_path_secret));
            sodium_memzero(current_secret, sizeof(current_secret));
            staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            goto staged_fail;
        }
        if (sodium_memcmp(final_tree_hash, provisional_tree_hash,
                          MLS_HASH_LEN) != 0) {
            mls_commit_clear(&commit);
            sodium_memzero(psk_secret, sizeof(psk_secret));
            sodium_memzero(our_path_secret, sizeof(our_path_secret));
            sodium_memzero(current_secret, sizeof(current_secret));
            staged_rc = MARMOT_ERR_INTERNAL;
            goto staged_fail;
        }

        sodium_memzero(our_path_secret, sizeof(our_path_secret));
        sodium_memzero(current_secret, sizeof(current_secret));
    }

    /* Derive commit_secret */
    uint8_t commit_secret[MLS_HASH_LEN];
    derive_commit_secret(has_path ? root_path_secret : NULL, has_path, commit_secret);

    /* Update confirmed transcript hash over AuthenticatedContentTBM (through
     * FramedContentAuthData.signature, excluding the commit confirmation tag). */
    uint8_t *confirmed_input = NULL;
    size_t confirmed_input_len = 0;
    int transcript_rc =
        public_message_confirmed_transcript_input(pm, wire_format,
                                                  &confirmed_input,
                                                  &confirmed_input_len);
    if (transcript_rc != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_MEMORY;
        goto staged_fail;
    }
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + confirmed_input_len) != 0) {
        free(confirmed_input);
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_MEMORY;
        goto staged_fail;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, confirmed_input, confirmed_input_len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    free(confirmed_input);
    mls_tls_buf_free(&conf_buf);

    /* Advance epoch */
    uint64_t previous_epoch = group->epoch;
    remember_resumption_psk(group, previous_epoch,
                            group->epoch_secrets.resumption_psk);
    uint8_t prev_init_copy[MLS_HASH_LEN];
    memcpy(prev_init_copy, group->epoch_secrets.init_secret, MLS_HASH_LEN);
    group->epoch++;
    const uint8_t *prev_init = prev_init_copy;
    if (group_derive_epoch(group, prev_init, commit_secret, psk_secret) != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_INTERNAL;
        goto staged_fail;
    }

    /* Compute and verify confirmation tag */
    uint8_t confirmation_tag[MLS_HASH_LEN];
    compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                             group->confirmed_transcript_hash,
                             confirmation_tag);
    if (sodium_memcmp(confirmation_tag, pm->auth.confirmation_tag, MLS_HASH_LEN) != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        sodium_memzero(root_path_secret, sizeof(root_path_secret));
        sodium_memzero(commit_secret, sizeof(commit_secret));
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }

    /* Update interim transcript hash */
    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_MEMORY;
        goto staged_fail;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_write_opaque32(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    mls_commit_clear(&commit);
    sodium_memzero(psk_secret, sizeof(psk_secret));
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    /* Removes, Updates and the UpdatePath may have blanked or re-keyed nodes
     * on our path: forget their old private keys. */
    prune_own_path_keys(group);

    /* The epoch entered keeps the group's profile and, for an adopted group,
     * every resulting-state invariant (nostrc-qp24.5.1; group-setup.md). */
    if (staged.profile != live_group->profile || mls_group_profile_check_entered(&staged) != 0) {
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }

    group_install_staged(live_group, &staged);
    free(pre_gc);
    mls_message_clear(&wire_msg);

    return 0;

public_result:
    /* The public result (mls_group_commit_public_result_by_ref()): never
     * installed.  The UpdatePath's public part as every other member applies
     * it, its parent-hash chain checked (RFC 9420 section 7.9.2; slice H
     * re-review R5). */
    if (commit.has_path &&
        mls_treekem_apply_update_path(&staged.tree, sender_leaf, &commit.path) != 0) {
        mls_commit_clear(&commit);
        sodium_memzero(psk_secret, sizeof(psk_secret));
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }
    mls_commit_clear(&commit);
    sodium_memzero(psk_secret, sizeof(psk_secret));
    staged.epoch = live_group->epoch + 1;
    if (staged.profile != live_group->profile || mls_group_profile_check_entered(&staged) != 0) {
        staged_rc = MARMOT_ERR_MLS_PROCESS_MESSAGE;
        goto staged_fail;
    }
    *public_out = staged;
    free(pre_gc);
    mls_message_clear(&wire_msg);
    return 0;

staged_fail:
    /* A rejected Commit never touched the live group; release the staged
     * clone (zeroizing its key material) and the parsed message. */
    mls_group_free(&staged);
    free(pre_gc);
    mls_message_clear(&wire_msg);
    return staged_rc;
}

int
mls_group_process_commit(MlsGroup *group,
                         const uint8_t *commit_data, size_t commit_len,
                         uint32_t sender_leaf)
{
    return process_commit_impl(group, commit_data, commit_len, sender_leaf,
                               NULL, NULL, 0, NULL, NULL);
}

int
mls_group_process_commit_by_ref(MlsGroup *group,
                                const uint8_t *commit_data, size_t commit_len,
                                uint32_t sender_leaf,
                                const uint8_t *const *acs, const size_t *ac_lens,
                                size_t ac_count, MlsCommitSummary *summary)
{
    if (!group || !commit_data) return MARMOT_ERR_INVALID_ARG;
    MlsProposalStore store;
    if (proposal_store_build_ac(&store, group, acs, ac_lens, ac_count) != 0)
        return MARMOT_ERR_MEMORY;
    int rc = process_commit_impl(group, commit_data, commit_len, sender_leaf,
                                 &store, NULL, 0, summary, NULL);
    proposal_store_free(&store);
    return rc;
}

int
mls_group_commit_public_result_by_ref(const MlsGroup *group,
                                      const uint8_t *commit_data, size_t commit_len,
                                      uint32_t sender_leaf,
                                      const uint8_t *const *acs, const size_t *ac_lens,
                                      size_t ac_count, MlsGroup *out,
                                      MlsCommitSummary *summary)
{
    if (!group || !commit_data || !out) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    MlsProposalStore store;
    if (proposal_store_build_ac(&store, group, acs, ac_lens, ac_count) != 0)
        return MARMOT_ERR_MEMORY;
    /* The group itself is only cloned (group_stage_clone()) and never
     * installed into in this mode. */
    int rc = process_commit_impl((MlsGroup *)group, commit_data, commit_len, sender_leaf,
                                 &store, NULL, 0, summary, out);
    proposal_store_free(&store);
    return rc;
}

int
mls_group_process_commit_ex(MlsGroup *group,
                            const uint8_t *commit_data, size_t commit_len,
                            uint32_t sender_leaf,
                            const uint8_t *const *proposal_msgs,
                            const size_t *proposal_lens,
                            size_t proposal_count)
{
    MlsProposalStore store;
    if (proposal_store_build(&store, group, proposal_msgs, proposal_lens,
                             proposal_count) != 0)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    int rc = process_commit_impl(group, commit_data, commit_len, sender_leaf,
                                 &store, NULL, 0, NULL, NULL);
    proposal_store_free(&store);
    return rc;
}

int
mls_group_process_commit_ex_with_psks(MlsGroup *group,
                                      const uint8_t *commit_data,
                                      size_t commit_len,
                                      uint32_t sender_leaf,
                                      const uint8_t *const *proposal_msgs,
                                      const size_t *proposal_lens,
                                      size_t proposal_count,
                                      const MlsPskInput *external_psks,
                                      size_t external_psk_count)
{
    MlsProposalStore store;
    if (proposal_store_build(&store, group, proposal_msgs, proposal_lens,
                             proposal_count) != 0)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    int rc = process_commit_impl(group, commit_data, commit_len, sender_leaf,
                                 &store, external_psks, external_psk_count, NULL, NULL);
    proposal_store_free(&store);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Application messages
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_encrypt(MlsGroup *group,
                  const uint8_t *plaintext, size_t plaintext_len,
                  uint8_t **out_data, size_t *out_len)
{
    if (!group || !plaintext || !out_data || !out_len)
        return MARMOT_ERR_INVALID_ARG;

    /* PrivateMessageContent (RFC 9420 §6.3.1): the application data signed
     * with our leaf's signature key (since 0.9.0, nostrc-we6g). */
    uint8_t *gc = NULL, *content = NULL;
    size_t gc_len = 0, content_len = 0;
    int content_rc = mls_group_context_build(group, &gc, &gc_len) == 0
        ? mls_application_content_encode(group->group_id, group->group_id_len,
                                         group->epoch, group->own_leaf_index,
                                         NULL, 0, plaintext, plaintext_len,
                                         gc, gc_len, group->own_signature_key,
                                         &content, &content_len)
        : -1;
    free(gc);
    if (content_rc != 0)
        return MARMOT_ERR_MLS_CREATE_MESSAGE;

    /* Derive message keys for our leaf */
    MlsMessageKeys keys;
    if (mls_secret_tree_derive_keys(&group->secret_tree, group->own_leaf_index,
                                     false /* application */, &keys) != 0) {
        sodium_memzero(content, content_len);
        free(content);
        return MARMOT_ERR_INTERNAL;
    }

    /* Generate reuse guard */
    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, 4);

    /* Encrypt as PrivateMessage; the key is used once, then deleted. */
    MlsPrivateMessage msg;
    int enc_rc = mls_private_message_encrypt(
            group->group_id, group->group_id_len,
            group->epoch,
            MLS_CONTENT_TYPE_APPLICATION,
            NULL, 0, /* no AAD */
            content, content_len,
            group->epoch_secrets.sender_data_secret,
            &keys, group->own_leaf_index,
            reuse_guard, &msg);
    sodium_memzero(&keys, sizeof(keys));
    sodium_memzero(content, content_len);
    free(content);
    if (enc_rc != 0)
        return MARMOT_ERR_MLS_CREATE_MESSAGE;

    /* Serialize */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, plaintext_len + 256) != 0) {
        mls_private_message_clear(&msg);
        return MARMOT_ERR_MEMORY;
    }
    MlsMLSMessage wire_msg;
    memset(&wire_msg, 0, sizeof(wire_msg));
    wire_msg.wire_format = MLS_WIRE_FORMAT_PRIVATE_MESSAGE;
    wire_msg.cipher_suite = MARMOT_CIPHERSUITE;
    wire_msg.private_message = msg; /* transfer ownership for serialization/clear */

    if (mls_message_serialize(&wire_msg, &buf) != 0) {
        mls_message_clear(&wire_msg);
        mls_tls_buf_free(&buf);
        return MARMOT_ERR_MLS_FRAMING;
    }

    *out_data = buf.data;
    *out_len = buf.len;
    mls_message_clear(&wire_msg);

    return 0;
}

int
mls_group_decrypt(MlsGroup *group,
                  const uint8_t *ciphertext, size_t ciphertext_len,
                  uint8_t **out_plaintext, size_t *out_pt_len,
                  uint32_t *out_sender_leaf)
{
    if (!group || !ciphertext || !out_plaintext || !out_pt_len)
        return MARMOT_ERR_INVALID_ARG;

    /* Deserialize MLSMessage envelope containing a PrivateMessage */
    MlsMLSMessage wire_msg;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, ciphertext, ciphertext_len);
    if (mls_message_deserialize(&reader, &wire_msg) != 0)
        return MARMOT_ERR_MLS_FRAMING;
    if (!mls_tls_reader_done(&reader) ||
        wire_msg.wire_format != MLS_WIRE_FORMAT_PRIVATE_MESSAGE) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_MLS_FRAMING;
    }

    MlsPrivateMessage *msg = &wire_msg.private_message;

    /* Verify group_id and epoch match */
    if (msg->group_id_len != group->group_id_len ||
        memcmp(msg->group_id, group->group_id, group->group_id_len) != 0) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_WRONG_GROUP_ID;
    }
    if (msg->epoch != group->epoch) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_WRONG_EPOCH;
    }

    /* Step 1: Decrypt sender data to identify the sender */
    size_t sample_len = msg->ciphertext_len < MLS_HASH_LEN
                         ? msg->ciphertext_len : MLS_HASH_LEN;
    MlsSenderData sender_data;
    const MlsSenderDataAAD sd_aad = { msg->group_id, msg->group_id_len, msg->epoch,
                                      msg->content_type };
    if (mls_sender_data_decrypt(group->epoch_secrets.sender_data_secret, &sd_aad,
                                 msg->ciphertext, sample_len,
                                 msg->encrypted_sender_data,
                                 msg->encrypted_sender_data_len,
                                 &sender_data) != 0) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_CRYPTO;
    }

    /* Validate metadata before consuming ratchet state. */
    if (sender_data.leaf_index >= group->tree.n_leaves) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_FROM_NON_MEMBER;
    }
    if (msg->content_type != MLS_CONTENT_TYPE_APPLICATION &&
        msg->content_type != MLS_CONTENT_TYPE_PROPOSAL &&
        msg->content_type != MLS_CONTENT_TYPE_COMMIT) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_MESSAGE;
    }

    if (out_sender_leaf)
        *out_sender_leaf = sender_data.leaf_index;

    /* Check not from ourselves — before consuming ratchet state */
    if (sender_data.leaf_index == group->own_leaf_index) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_OWN_MESSAGE;
    }

    /* Marmot sends handshake messages as PublicMessages: a PrivateMessage
     * here is an application message, nothing else is delivered. */
    if (msg->content_type != MLS_CONTENT_TYPE_APPLICATION) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_UNSUPPORTED;
    }
    /* The sender is a member: its leaf holds the key that must have signed. */
    const MlsNode *sender_node =
        &group->tree.nodes[mls_tree_leaf_to_node(sender_data.leaf_index)];
    if (sender_node->type != MLS_NODE_LEAF) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_FROM_NON_MEMBER;
    }

    /* Only an authentic message consumes a key: keep the sender's ratchet as
     * it is to put it back if the signature fails (nostrc-we6g). */
    MlsSenderSnapshot before;
    if (mls_secret_tree_sender_save(&group->secret_tree, sender_data.leaf_index,
                                    &before) != 0) {
        mls_message_clear(&wire_msg);
        return MARMOT_ERR_INTERNAL;
    }

    /* Step 2: Full decrypt (content keys + AEAD) using sender data already parsed above. */
    uint8_t *content = NULL;
    size_t content_len = 0;
    int rc = mls_private_message_decrypt_with_sender_data(msg,
                                                           &sender_data,
                                                           &group->secret_tree,
                                                           group->max_forward_distance,
                                                           &content,
                                                           &content_len,
                                                           &sender_data);
    if (rc != 0) {
        mls_secret_tree_sender_discard(&before);   /* restored there already */
        mls_message_clear(&wire_msg);
        return rc;
    }

    /* Step 3: PrivateMessageContent (RFC 9420 §6.3.1): verify the sender
     * leaf's signature over the FramedContent before delivering anything. */
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    rc = mls_group_context_build(group, &gc, &gc_len) == 0 &&
         mls_application_content_decode(msg->group_id, msg->group_id_len, msg->epoch,
                                        sender_data.leaf_index,
                                        msg->authenticated_data,
                                        msg->authenticated_data_len,
                                        content, content_len, gc, gc_len,
                                        sender_node->leaf.signature_key,
                                        out_plaintext, out_pt_len) == 0
             ? 0 : MARMOT_ERR_CRYPTO;
    free(gc);
    sodium_memzero(content, content_len);
    free(content);
    if (rc != 0)
        mls_secret_tree_sender_restore(&group->secret_tree, &before);
    else
        mls_secret_tree_sender_discard(&before);

    mls_message_clear(&wire_msg);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GroupContext helpers
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_context_build(const MlsGroup *group,
                        uint8_t **out_data, size_t *out_len)
{
    if (!group) return -1;

    uint8_t tree_hash[MLS_HASH_LEN];
    if (mls_group_tree_hash(group, tree_hash) != 0)
        return -1;

    return mls_group_context_serialize(
        group->group_id, group->group_id_len,
        group->epoch, tree_hash,
        group->confirmed_transcript_hash,
        group->extensions_data, group->extensions_len,
        out_data, out_len);
}

int
mls_group_tree_hash(const MlsGroup *group, uint8_t out[MLS_HASH_LEN])
{
    if (!group || !out) return -1;

    /* The ratchet-tree extension and MDK vectors use the canonical tree
     * representation, which omits a blank right edge.  Preserve the live
     * n_leaves/leaf-index space, but compute GroupContext tree_hash over the
     * same canonicalized view used for UpdatePath HPKE context construction. */
    uint8_t *serialized = NULL;
    size_t serialized_len = 0;
    MlsRatchetTree canonical;
    memset(&canonical, 0, sizeof(canonical));
    if (mls_ratchet_tree_serialize(&group->tree, &serialized,
                                   &serialized_len) != 0)
        return -1;
    int rc = -1;
    if (mls_ratchet_tree_deserialize(serialized, serialized_len,
                                     &canonical) == 0)
        rc = mls_tree_root_hash(&canonical, out);
    mls_tree_free(&canonical);
    free(serialized);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GroupInfo
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_info_build(const MlsGroup *group, MlsGroupInfo *gi)
{
    if (!group || !gi) return -1;
    memset(gi, 0, sizeof(*gi));

    gi->group_id = malloc(group->group_id_len);
    if (!gi->group_id) return -1;
    memcpy(gi->group_id, group->group_id, group->group_id_len);
    gi->group_id_len = group->group_id_len;

    gi->epoch = group->epoch;
    gi->signer_leaf = group->own_leaf_index;

    /* Tree hash */
    if (mls_group_tree_hash(group, gi->tree_hash) != 0) {
        mls_group_info_clear(gi);
        return -1;
    }

    memcpy(gi->confirmed_transcript_hash, group->confirmed_transcript_hash,
           MLS_HASH_LEN);

    /* Extensions */
    if (group->extensions_data && group->extensions_len > 0) {
        gi->extensions_data = malloc(group->extensions_len);
        if (!gi->extensions_data) {
            mls_group_info_clear(gi);
            return -1;
        }
        memcpy(gi->extensions_data, group->extensions_data, group->extensions_len);
        gi->extensions_len = group->extensions_len;
    }

    if (build_group_info_extensions_with_tree(&group->tree,
                                               &gi->group_info_extensions_data,
                                               &gi->group_info_extensions_len) != 0) {
        mls_group_info_clear(gi);
        return -1;
    }

    /* Sign the GroupInfo over GroupInfoTBS (RFC 9420 §12.4.3.1).
     * Callers that set a confirmation_tag after build() (e.g. Welcome
     * assembly) re-sign via mls_group_info_sign_local() once the tag is set. */
    if (mls_group_info_sign_local(gi, group->own_signature_key) != 0) {
        mls_group_info_clear(gi);
        return -1;
    }

    return 0;
}

int
mls_group_info_serialize(const MlsGroupInfo *gi, MlsTlsBuf *buf)
{
    if (!gi || !buf) return -1;

    /* GroupContext portion */
    if (mls_tls_write_u16(buf, 1) != 0) return -1;
    if (mls_tls_write_u16(buf, MARMOT_CIPHERSUITE) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->group_id, gi->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, gi->epoch) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->tree_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmed_transcript_hash, MLS_HASH_LEN) != 0)
        return -1;
    if (mls_tls_write_opaque32(buf, gi->extensions_data, gi->extensions_len) != 0)
        return -1;

    /* GroupInfo-specific fields */
    if (mls_tls_write_opaque32(buf, gi->group_info_extensions_data,
                                gi->group_info_extensions_len) != 0) return -1;
    if (mls_tls_write_opaque8(buf, gi->confirmation_tag, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_u32(buf, gi->signer_leaf) != 0) return -1;
    if (mls_tls_write_opaque16(buf, gi->signature, gi->signature_len) != 0) return -1;

    return 0;
}

int
mls_group_info_deserialize(MlsTlsReader *reader, MlsGroupInfo *gi)
{
    if (!reader || !gi) return -1;
    memset(gi, 0, sizeof(*gi));

    uint16_t version;
    uint16_t cs;
    if (mls_tls_read_u16(reader, &version) != 0) goto fail;
    if (version != 1) goto fail;
    if (mls_tls_read_u16(reader, &cs) != 0) goto fail;
    if (cs != MARMOT_CIPHERSUITE) goto fail;

    if (mls_tls_read_opaque8(reader, &gi->group_id, &gi->group_id_len) != 0) goto fail;
    if (mls_tls_read_u64(reader, &gi->epoch) != 0) goto fail;
    uint8_t *tree_hash = NULL;
    size_t tree_hash_len = 0;
    if (mls_tls_read_opaque8(reader, &tree_hash, &tree_hash_len) != 0) goto fail;
    if (tree_hash_len != MLS_HASH_LEN) { free(tree_hash); goto fail; }
    memcpy(gi->tree_hash, tree_hash, MLS_HASH_LEN);
    free(tree_hash);
    uint8_t *cth = NULL;
    size_t cth_len = 0;
    if (mls_tls_read_opaque8(reader, &cth, &cth_len) != 0) goto fail;
    if (cth_len != MLS_HASH_LEN) { free(cth); goto fail; }
    memcpy(gi->confirmed_transcript_hash, cth, MLS_HASH_LEN);
    free(cth);
    if (mls_tls_read_opaque32(reader, &gi->extensions_data, &gi->extensions_len) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &gi->group_info_extensions_data,
                               &gi->group_info_extensions_len) != 0) goto fail;
    uint8_t *confirmation_tag = NULL;
    size_t confirmation_tag_len = 0;
    if (mls_tls_read_opaque8(reader, &confirmation_tag, &confirmation_tag_len) != 0) goto fail;
    if (confirmation_tag_len != MLS_HASH_LEN) { free(confirmation_tag); goto fail; }
    memcpy(gi->confirmation_tag, confirmation_tag, MLS_HASH_LEN);
    free(confirmation_tag);
    if (mls_tls_read_u32(reader, &gi->signer_leaf) != 0) goto fail;

    {
        uint8_t *sig = NULL;
        size_t sig_len = 0;
        if (mls_tls_read_opaque16(reader, &sig, &sig_len) != 0) goto fail;
        if (sig_len > MLS_SIG_LEN) { free(sig); goto fail; }
        if (sig_len > 0) memcpy(gi->signature, sig, sig_len);
        gi->signature_len = sig_len;
        free(sig);
    }

    return 0;
fail:
    mls_group_info_clear(gi);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Commit serialization
 * ══════════════════════════════════════════════════════════════════════════ */

static int
proposal_serialize(const MlsProposal *p, MlsTlsBuf *buf)
{
    if (!p || !buf) return -1;
    if (mls_tls_write_u16(buf, p->type) != 0) return -1;

    switch (p->type) {
    case MLS_PROPOSAL_ADD:
        return mls_key_package_serialize(&p->add.key_package, buf);
    case MLS_PROPOSAL_UPDATE:
        return mls_leaf_node_serialize(&p->update.leaf_node, buf);
    case MLS_PROPOSAL_REMOVE:
        return mls_tls_write_u32(buf, p->remove.removed_leaf);
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT:
        /* GroupContextExtensions { Extension extensions<V>; } */
        return mls_tls_write_opaque32(buf, p->group_context_extensions.extensions,
                                      p->group_context_extensions.extensions_len);
    case MLS_PROPOSAL_APP_DATA_UPDATE:
        return mls_app_data_update_serialize(&p->app_data_update, buf);
    case MLS_PROPOSAL_SELF_REMOVE:
        return 0;   /* struct {} SelfRemove */
    default:
        return -1;
    }
}

static int
proposal_deserialize(MlsTlsReader *reader, MlsProposal *p)
{
    if (!reader || !p) return -1;
    memset(p, 0, sizeof(*p));
    p->update_leaf_index = UINT32_MAX; /* default: committer's own leaf */
    p->sender_leaf = UINT32_MAX;       /* inline: sent by the committer */

    if (mls_tls_read_u16(reader, &p->type) != 0) return -1;

    switch (p->type) {
    case MLS_PROPOSAL_ADD:
        if (mls_tls_reader_remaining(reader) >= 4 &&
            reader->data[reader->pos] == 0x00 &&
            reader->data[reader->pos + 1] == 0x01 &&
            reader->data[reader->pos + 2] == 0x00 &&
            reader->data[reader->pos + 3] == MLS_WIRE_FORMAT_KEY_PACKAGE) {
            reader->pos += 4;
        }
        return mls_key_package_deserialize(reader, &p->add.key_package);
    case MLS_PROPOSAL_UPDATE:
        return mls_leaf_node_deserialize(reader, &p->update.leaf_node);
    case MLS_PROPOSAL_REMOVE:
        return mls_tls_read_u32(reader, &p->remove.removed_leaf);
    case MLS_PROPOSAL_PSK: {
        uint8_t psktype;
        if (mls_tls_read_u8(reader, &psktype) != 0) return -1;
        p->psk.psk_type = psktype;
        if (psktype == 1) {
            if (mls_tls_read_opaque32(reader, &p->psk.psk_id,
                                      &p->psk.psk_id_len) != 0)
                return -1;
        } else if (psktype == 2) {
            if (mls_tls_read_u8(reader, &p->psk.resumption_usage) != 0)
                return -1;
            if (mls_tls_read_opaque32(reader, &p->psk.resumption_group_id,
                                      &p->psk.resumption_group_id_len) != 0)
                return -1;
            if (mls_tls_read_u64(reader, &p->psk.resumption_epoch) != 0)
                return -1;
        } else {
            return -1;
        }
        if (mls_tls_read_opaque32(reader, &p->psk.psk_nonce,
                                  &p->psk.psk_nonce_len) != 0)
            return -1;
        return 0;
    }
    case MLS_PROPOSAL_REINIT: {
        uint8_t *gid = NULL; size_t gidl = 0; uint16_t version, cs;
        uint8_t *ext = NULL; size_t extl = 0;
        p->unsupported = true;
        if (mls_tls_read_opaque32(reader, &gid, &gidl) != 0) return -1;
        free(gid);
        if (mls_tls_read_u16(reader, &version) != 0) return -1;
        if (mls_tls_read_u16(reader, &cs) != 0) return -1;
        if (mls_tls_read_opaque32(reader, &ext, &extl) != 0) return -1;
        free(ext);
        return 0;
    }
    case MLS_PROPOSAL_EXTERNAL_INIT: {
        uint8_t *kem = NULL; size_t keml = 0;
        p->unsupported = true;
        if (mls_tls_read_opaque32(reader, &kem, &keml) != 0) return -1;
        free(kem);
        return 0;
    }
    case MLS_PROPOSAL_GROUP_CONTEXT_EXT: {
        if (mls_tls_read_opaque32(reader,
                                  &p->group_context_extensions.extensions,
                                  &p->group_context_extensions.extensions_len) != 0)
            return -1;
        return 0;
    }
    case MLS_PROPOSAL_APP_DATA_UPDATE:
        /* Draft-10 wire shape.  Applied only in an adopted group
         * (proposal_type_apply_supported(); nostrc-qp24.5.1.3), whose
         * resulting state and authorization are checked after. */
        return mls_app_data_update_deserialize(reader, &p->app_data_update);
    case MLS_PROPOSAL_SELF_REMOVE:
        return 0;   /* empty body (draft-ietf-mls-extensions) */
    default:
        return -1; /* Unknown proposal type */
    }
}

static int
count_hpke_ciphertexts(const uint8_t *data, size_t len, uint32_t *out_count)
{
    if (!out_count) return -1;
    *out_count = 0;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);

    while (!mls_tls_reader_done(&reader)) {
        uint8_t *enc = NULL, *ct = NULL;
        size_t enc_len = 0, ct_len = 0;
        if (mls_tls_read_opaque16(&reader, &enc, &enc_len) != 0) return -1;
        if (mls_tls_read_opaque16(&reader, &ct, &ct_len) != 0) {
            free(enc);
            return -1;
        }
        free(enc);
        free(ct);
        (*out_count)++;
    }
    return 0;
}

int
mls_update_path_serialize(const MlsUpdatePath *up, MlsTlsBuf *buf)
{
    if (!up || !buf) return -1;

    /* Leaf node */
    if (mls_leaf_node_serialize(&up->leaf_node, buf) != 0) return -1;

    /* nodes: UpdatePathNode<V> */
    MlsTlsBuf nodes_buf;
    if (mls_tls_buf_init(&nodes_buf, 256) != 0) return -1;

    for (size_t i = 0; i < up->node_count; i++) {
        if (mls_tls_write_opaque16(&nodes_buf, up->nodes[i].encryption_key,
                                    MLS_KEM_PK_LEN) != 0)
            goto fail;
        if (mls_tls_write_opaque32(&nodes_buf, up->nodes[i].encrypted_path_secrets,
                                    up->nodes[i].encrypted_path_secrets_len) != 0)
            goto fail;
    }

    if (mls_tls_write_opaque32(buf, nodes_buf.data, nodes_buf.len) != 0) goto fail;
    mls_tls_buf_free(&nodes_buf);
    return 0;

fail:
    mls_tls_buf_free(&nodes_buf);
    return -1;
}

int
mls_update_path_deserialize(MlsTlsReader *reader, MlsUpdatePath *up)
{
    if (!reader || !up) return -1;
    memset(up, 0, sizeof(*up));

    if (mls_leaf_node_deserialize(reader, &up->leaf_node) != 0) goto fail;

    uint8_t *nodes_data = NULL;
    size_t nodes_len = 0;
    if (mls_tls_read_opaque32(reader, &nodes_data, &nodes_len) != 0) goto fail;

    if (nodes_len > 0) {
        MlsTlsReader nodes_reader;
        mls_tls_reader_init(&nodes_reader, nodes_data, nodes_len);

        while (!mls_tls_reader_done(&nodes_reader)) {
            MlsUpdatePathNode *new_nodes = realloc(
                up->nodes, (up->node_count + 1) * sizeof(MlsUpdatePathNode));
            if (!new_nodes) { free(nodes_data); goto fail; }
            up->nodes = new_nodes;

            MlsUpdatePathNode *node = &up->nodes[up->node_count];
            memset(node, 0, sizeof(*node));
            up->node_count++;
            uint8_t *enc_key = NULL;
            size_t enc_key_len = 0;
            if (mls_tls_read_opaque16(&nodes_reader, &enc_key, &enc_key_len) != 0) {
                free(nodes_data); goto fail;
            }
            if (enc_key_len != MLS_KEM_PK_LEN) {
                free(enc_key); free(nodes_data); goto fail;
            }
            memcpy(node->encryption_key, enc_key, MLS_KEM_PK_LEN);
            free(enc_key);
            if (mls_tls_read_opaque32(&nodes_reader, &node->encrypted_path_secrets,
                                       &node->encrypted_path_secrets_len) != 0) {
                free(nodes_data); goto fail;
            }
            if (count_hpke_ciphertexts(node->encrypted_path_secrets,
                                       node->encrypted_path_secrets_len,
                                       &node->secret_count) != 0) {
                free(nodes_data); goto fail;
            }
        }
    }
    free(nodes_data);

    return 0;
fail:
    mls_update_path_clear(up);
    return -1;
}

int
mls_commit_serialize(const MlsCommit *commit, MlsTlsBuf *buf)
{
    if (!commit || !buf) return -1;
    /* Every libmarmot-produced Commit is encoded here: never emit one that a
     * conformant receiver must reject for a missing UpdatePath (§12.4). */
    if (!commit->has_path &&
        commit_path_required(commit->proposals, commit->proposal_count))
        return -1;

    /* proposals: Proposal<V> */
    MlsTlsBuf proposals_buf;
    if (mls_tls_buf_init(&proposals_buf, 256) != 0) return -1;

    for (size_t i = 0; i < commit->proposal_count; i++) {
        const MlsProposal *p = &commit->proposals[i];
        /* ProposalOrRef type 2: a ProposalRef<V> (since 0.12.0). */
        if (p->is_ref) {
            if (p->ref_len == 0 || p->ref_len > MLS_HASH_LEN ||
                mls_tls_write_u8(&proposals_buf, 2) != 0 ||
                mls_tls_write_opaque32(&proposals_buf, p->ref, p->ref_len) != 0) {
                mls_tls_buf_free(&proposals_buf);
                return -1;
            }
            continue;
        }
        /* ProposalOrRef (RFC 9420 §12.4): type 1 = inline proposal. The
         * deserializer reads this discriminator, so it must be written here. */
        if (mls_tls_write_u8(&proposals_buf, 1) != 0) {
            mls_tls_buf_free(&proposals_buf);
            return -1;
        }
        if (proposal_serialize(&commit->proposals[i], &proposals_buf) != 0) {
            mls_tls_buf_free(&proposals_buf);
            return -1;
        }
    }
    if (mls_tls_write_opaque32(buf, proposals_buf.data, proposals_buf.len) != 0) {
        mls_tls_buf_free(&proposals_buf);
        return -1;
    }
    mls_tls_buf_free(&proposals_buf);

    /* has_path flag */
    if (mls_tls_write_u8(buf, commit->has_path ? 1 : 0) != 0) return -1;

    /* UpdatePath (if present) */
    if (commit->has_path) {
        if (mls_update_path_serialize(&commit->path, buf) != 0)
            return -1;
    }

    return 0;
}

int
mls_commit_deserialize(MlsTlsReader *reader, MlsCommit *commit)
{
    if (!reader || !commit) return -1;
    memset(commit, 0, sizeof(*commit));

    uint8_t *proposals_data = NULL;
    size_t proposals_len = 0;
    if (mls_tls_read_opaque32(reader, &proposals_data, &proposals_len) != 0) goto fail;

    if (proposals_len > 0) {
        MlsTlsReader proposals_reader;
        mls_tls_reader_init(&proposals_reader, proposals_data, proposals_len);
        size_t capacity = 0;

        while (!mls_tls_reader_done(&proposals_reader)) {
            /* A longer Commit is refused before anything else is done with
             * it (slice H re-review R2). */
            if (commit->proposal_count == MLS_COMMIT_MAX_PROPOSALS) {
                free(proposals_data); goto fail;
            }
            if (commit->proposal_count == capacity) {
                size_t grown = capacity ? 2 * capacity : 8;
                if (grown > MLS_COMMIT_MAX_PROPOSALS) grown = MLS_COMMIT_MAX_PROPOSALS;
                MlsProposal *new_proposals = realloc(commit->proposals,
                                                     grown * sizeof(MlsProposal));
                if (!new_proposals) { free(proposals_data); goto fail; }
                commit->proposals = new_proposals;
                capacity = grown;
            }

            MlsProposal *proposal = &commit->proposals[commit->proposal_count];
            memset(proposal, 0, sizeof(*proposal));
            commit->proposal_count++;
            uint8_t proposal_or_ref = 1;
            if (mls_tls_read_u8(&proposals_reader, &proposal_or_ref) != 0) {
                free(proposals_data); goto fail;
            }
            if (proposal_or_ref == 2) {
                /* Referenced proposal (ProposalOrRef type 2): record the
                 * ProposalRef so the caller can resolve it against a store of
                 * previously-received proposals (mls_group_process_commit_ex). */
                uint8_t *ref = NULL;
                size_t ref_len = 0;
                if (mls_tls_read_opaque32(&proposals_reader, &ref, &ref_len) != 0) {
                    free(proposals_data); goto fail;
                }
                if (ref_len == 0 || ref_len > MLS_HASH_LEN) {
                    free(ref); free(proposals_data); goto fail;
                }
                proposal->is_ref = true;
                proposal->ref_len = ref_len;
                memcpy(proposal->ref, ref, ref_len);
                free(ref);
                continue;
            }
            if (proposal_or_ref != 1) {
                free(proposals_data); goto fail;
            }
            if (proposal_deserialize(&proposals_reader, proposal) != 0) {
                free(proposals_data); goto fail;
            }
        }
    }
    free(proposals_data);

    uint8_t has_path;
    if (mls_tls_read_u8(reader, &has_path) != 0) goto fail;
    commit->has_path = (has_path != 0);

    if (commit->has_path) {
        if (mls_update_path_deserialize(reader, &commit->path) != 0)
            goto fail;
    }

    return 0;
fail:
    mls_commit_clear(commit);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Group state serialization / deserialization
 *
 * Binary format for persisting MlsGroup to storage. Uses the TLS
 * serialization primitives for consistency. NOT a wire protocol format —
 * this is internal-only for state persistence.
 *
 * Format (version 3, nostrc-ai04):
 *   magic "MLSG" (4 bytes)
 *   version u32 (3)
 *   group_id opaque32
 *   epoch u64
 *   n_leaves u32
 *   For each node (0..node_width-1):
 *     node_type u8 (0=blank, 1=leaf, 2=parent)
 *     [LeafNode or ParentNode via TLS serialization if non-blank]
 *   own_leaf_index u32
 *   own_signature_key (64 bytes)
 *   own_encryption_key (32 bytes)
 *   epoch secrets, 8 * 32 bytes: sender_data, exporter, external,
 *     confirmation_key, membership_key, resumption_psk, epoch_authenticator,
 *     init (next epoch)
 *   confirmed_transcript_hash (32 bytes)
 *   interim_transcript_hash (32 bytes)
 *   extensions opaque32
 *   max_forward_distance u32
 *   resumption_psk_cache_count u32
 *   repeated: epoch u64 || resumption_psk[32]
 *   own_path_key_count u32
 *   repeated: node u32 || sk[32] || pk[32]
 *   secret tree (mls_secret_tree_serialize: every sender's handshake and
 *     application ratchet and its skipped-key cache, or its unused leaf secret)
 *
 * Only unconsumed secrets of this epoch are stored (RFC 9420 §9.2).  The
 * encryption_secret (the secret tree's root) and the joiner_secret it is
 * derived from (with the stored GroupContext) are consumed with the epoch's
 * first message key, and the welcome_secret once the Welcome is built, so
 * version 3 omits them: this state alone cannot re-derive a used message
 * key of its epoch.
 *
 * NOT covered by this alone (review B1): after a Commit, libmarmot also keeps
 * the previous epoch's state as the retained parent ("mls_group_parent", to
 * judge a competing Commit and read late messages).  While it is kept in
 * full -- its init secret and the private keys that open the Commit's
 * UpdatePath -- it and the Commit, which relays carry, derive the current
 * epoch again from scratch: every message of the current epoch (and the
 * parent epoch's unconsumed keys) is exposed to whoever obtains the whole
 * store.  Since 0.10.0 (nostrc-yuj2) that lasts only until every member
 * that could publish a winning competing Commit was seen at the new epoch
 * (at once when there is none; at the latest the next Commit): the parent
 * is then reduced by mls_group_strip_to_reader() to its sender-data secret
 * and secret tree, which read the parent epoch's late messages but cannot
 * process a Commit (commits.c, "Retained parent record").
 *
 * Versions 1 and 2 stored those three secrets and no ratchet (every load
 * restarted each sender at generation 0: key reuse, nostrc-ai04).  They are
 * still read: the tree is re-derived, the secrets deleted, and the own
 * sender moved MLS_SECRET_TREE_LEGACY_OWN_STRIDE generations forward.  The
 * next save writes version 3.
 * ══════════════════════════════════════════════════════════════════════════ */

#define MLS_GROUP_SERIAL_MAGIC  0x4D4C5347  /* "MLSG" */
#define MLS_GROUP_SERIAL_VER    4
/* The last version that stored no secret tree. */
#define MLS_GROUP_SERIAL_VER_LEGACY_MAX 2
/* Version 4 (nostrc-qp24.5.1) is version 3 followed by one byte, the
 * MarmotGroupProfile.  A legacy-profile group is still written as version 3,
 * byte for byte as before, so its stored form does not change; only an
 * adopted-profile group is written as version 4.  Versions 1 to 3 load as
 * legacy. */
#define MLS_GROUP_SERIAL_VER_NO_PROFILE 3
/* The profile byte of format 4: a storage format constant, mapped
 * explicitly -- never the public MarmotGroupProfile value, whose numbering
 * may change without changing what is on disk (W24 review L4).  0x01 is the
 * only value written (legacy states are format 3). */
#define MLS_GROUP_SERIAL_PROFILE_ADOPTED 0x01

int
mls_group_serialize(const MlsGroup *group, uint8_t **out_data, size_t *out_len)
{
    if (!group || !out_data || !out_len) return -1;
    const bool adopted = group->profile == MARMOT_GROUP_PROFILE_ADOPTED;
    if (!adopted && group->profile != MARMOT_GROUP_PROFILE_LEGACY) return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 4096) != 0) return -1;

    /* Header */
    if (mls_tls_write_u32(&buf, MLS_GROUP_SERIAL_MAGIC) != 0) goto fail;
    if (mls_tls_write_u32(&buf, adopted ? MLS_GROUP_SERIAL_VER
                                        : MLS_GROUP_SERIAL_VER_NO_PROFILE) != 0)
        goto fail;

    /* Group ID */
    if (mls_tls_write_opaque32(&buf, group->group_id, group->group_id_len) != 0)
        goto fail;

    /* Epoch */
    if (mls_tls_write_u64(&buf, group->epoch) != 0) goto fail;

    /* Ratchet tree */
    if (mls_tls_write_u32(&buf, group->tree.n_leaves) != 0) goto fail;

    uint32_t n_nodes = mls_tree_node_width(group->tree.n_leaves);
    for (uint32_t i = 0; i < n_nodes; i++) {
        const MlsNode *node = &group->tree.nodes[i];
        if (node->type == MLS_NODE_BLANK) {
            if (mls_tls_write_u8(&buf, 0) != 0) goto fail;
        } else if (node->type == MLS_NODE_LEAF) {
            if (mls_tls_write_u8(&buf, 1) != 0) goto fail;
            if (mls_leaf_node_serialize(&node->leaf, &buf) != 0) goto fail;
        } else { /* MLS_NODE_PARENT */
            if (mls_tls_write_u8(&buf, 2) != 0) goto fail;
            if (mls_parent_node_serialize(&node->parent, &buf) != 0) goto fail;
        }
    }

    /* Own state */
    if (mls_tls_write_u32(&buf, group->own_leaf_index) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->own_signature_key, MLS_SIG_SK_LEN) != 0)
        goto fail;
    if (mls_tls_buf_append(&buf, group->own_encryption_key, MLS_KEM_SK_LEN) != 0)
        goto fail;

    /* Epoch secrets still in use (not encryption, welcome or joiner) */
    if (mls_tls_buf_append(&buf, group->epoch_secrets.sender_data_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.exporter_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.external_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.confirmation_key, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.membership_key, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.resumption_psk, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.epoch_authenticator, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_buf_append(&buf, group->epoch_secrets.init_secret, MLS_HASH_LEN) != 0) goto fail;

    /* Transcript hashes */
    if (mls_tls_buf_append(&buf, group->confirmed_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;
    if (mls_tls_buf_append(&buf, group->interim_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;

    /* Extensions */
    if (mls_tls_write_opaque32(&buf, group->extensions_data, group->extensions_len) != 0)
        goto fail;

    /* Config */
    if (mls_tls_write_u32(&buf, group->max_forward_distance) != 0) goto fail;

    uint32_t cache_count = 0;
    for (size_t i = 0; i < MLS_RESUMPTION_PSK_CACHE_SIZE; i++) {
        if (group->resumption_psk_cache[i].valid)
            cache_count++;
    }
    if (mls_tls_write_u32(&buf, cache_count) != 0) goto fail;
    for (size_t i = 0; i < MLS_RESUMPTION_PSK_CACHE_SIZE; i++) {
        if (!group->resumption_psk_cache[i].valid)
            continue;
        if (mls_tls_write_u64(&buf, group->resumption_psk_cache[i].epoch) != 0)
            goto fail;
        if (mls_tls_buf_append(&buf, group->resumption_psk_cache[i].psk,
                               MLS_HASH_LEN) != 0)
            goto fail;
    }

    uint32_t path_key_count = 0;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        if (group->own_path_keys[i].valid)
            path_key_count++;
    }
    if (mls_tls_write_u32(&buf, path_key_count) != 0) goto fail;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        if (!group->own_path_keys[i].valid)
            continue;
        if (mls_tls_write_u32(&buf, group->own_path_keys[i].node) != 0)
            goto fail;
        if (mls_tls_buf_append(&buf, group->own_path_keys[i].sk,
                               MLS_KEM_SK_LEN) != 0)
            goto fail;
        if (mls_tls_buf_append(&buf, group->own_path_keys[i].pk,
                               MLS_KEM_PK_LEN) != 0)
            goto fail;
    }

    /* Secret tree: where every sender ratchet is (nostrc-ai04). */
    /* Its width is fixed when the epoch starts (the canonical tree's then,
     * mls_tree_canonical_leaves()): never wider than the live tree. */
    if (group->secret_tree.n_leaves == 0 ||
        group->secret_tree.n_leaves > group->tree.n_leaves ||
        mls_secret_tree_serialize(&group->secret_tree, &buf) != 0)
        goto fail;

    /* Version 4: the profile (only adopted groups are written as 4). */
    if (adopted && mls_tls_write_u8(&buf, MLS_GROUP_SERIAL_PROFILE_ADOPTED) != 0) goto fail;

    *out_data = buf.data;
    *out_len = buf.len;
    buf.data = NULL;
    return 0;

fail:
    if (buf.data) sodium_memzero(buf.data, buf.len);   /* secrets */
    mls_tls_buf_free(&buf);
    return -1;
}

int
mls_group_deserialize(const uint8_t *data, size_t len, MlsGroup *group)
{
    if (!data || !len || !group) return -1;

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);
    memset(group, 0, sizeof(*group));

    /* Header */
    uint32_t magic, version;
    if (mls_tls_read_u32(&reader, &magic) != 0 || magic != MLS_GROUP_SERIAL_MAGIC)
        goto fail;
    if (mls_tls_read_u32(&reader, &version) != 0 ||
        version == 0 || version > MLS_GROUP_SERIAL_VER)
        goto fail;
    bool legacy = version <= MLS_GROUP_SERIAL_VER_LEGACY_MAX;

    /* Group ID */
    if (mls_tls_read_opaque32(&reader, &group->group_id, &group->group_id_len) != 0)
        goto fail;

    /* Epoch */
    if (mls_tls_read_u64(&reader, &group->epoch) != 0) goto fail;

    /* Ratchet tree */
    uint32_t n_leaves;
    if (mls_tls_read_u32(&reader, &n_leaves) != 0) goto fail;
    if (n_leaves == 0 || n_leaves > 100000) goto fail; /* sanity */

    if (mls_tree_new(&group->tree, n_leaves) != 0) goto fail;

    uint32_t n_nodes = mls_tree_node_width(n_leaves);
    for (uint32_t i = 0; i < n_nodes; i++) {
        uint8_t node_type;
        if (mls_tls_read_u8(&reader, &node_type) != 0) goto fail;

        if (node_type == 0) {
            group->tree.nodes[i].type = MLS_NODE_BLANK;
        } else if (node_type == 1) {
            group->tree.nodes[i].type = MLS_NODE_LEAF;
            memset(&group->tree.nodes[i].leaf, 0, sizeof(MlsLeafNode));
            if (mls_leaf_node_deserialize(&reader, &group->tree.nodes[i].leaf) != 0)
                goto fail;
        } else if (node_type == 2) {
            group->tree.nodes[i].type = MLS_NODE_PARENT;
            memset(&group->tree.nodes[i].parent, 0, sizeof(MlsParentNode));
            if (mls_parent_node_deserialize(&reader, &group->tree.nodes[i].parent) != 0)
                goto fail;
        } else {
            goto fail; /* unknown node type */
        }
    }

    /* Own state */
    if (mls_tls_read_u32(&reader, &group->own_leaf_index) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->own_signature_key, MLS_SIG_SK_LEN) != 0)
        goto fail;
    if (mls_tls_read_fixed(&reader, group->own_encryption_key, MLS_KEM_SK_LEN) != 0)
        goto fail;

    /* Epoch secrets (legacy formats also stored encryption, welcome, joiner) */
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.sender_data_secret, MLS_HASH_LEN) != 0) goto fail;
    if (legacy &&
        mls_tls_read_fixed(&reader, group->epoch_secrets.encryption_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.exporter_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.external_secret, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.confirmation_key, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.membership_key, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.resumption_psk, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.epoch_authenticator, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(&reader, group->epoch_secrets.init_secret, MLS_HASH_LEN) != 0) goto fail;
    if (legacy) {
        /* Consumed long ago: read past, never kept (nor written again). */
        uint8_t consumed[2 * MLS_HASH_LEN];
        int rc = mls_tls_read_fixed(&reader, consumed, sizeof(consumed));
        sodium_memzero(consumed, sizeof(consumed));
        if (rc != 0) goto fail;
    }

    /* Transcript hashes */
    if (mls_tls_read_fixed(&reader, group->confirmed_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;
    if (mls_tls_read_fixed(&reader, group->interim_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;

    /* Extensions (checked against the profile once it is known, below) */
    if (mls_tls_read_opaque32(&reader, &group->extensions_data, &group->extensions_len) != 0)
        goto fail;

    /* Config */
    if (mls_tls_read_u32(&reader, &group->max_forward_distance) != 0) goto fail;

    /* Version 3 always has the caches and the secret tree after them. */
    if (!legacy || mls_tls_reader_remaining(&reader) > 0) {
        uint32_t cache_count = 0;
        if (mls_tls_read_u32(&reader, &cache_count) != 0) goto fail;
        if (cache_count > MLS_RESUMPTION_PSK_CACHE_SIZE) goto fail;
        for (uint32_t i = 0; i < cache_count; i++) {
            uint64_t epoch = 0;
            uint8_t psk[MLS_HASH_LEN];
            if (mls_tls_read_u64(&reader, &epoch) != 0) goto fail;
            if (mls_tls_read_fixed(&reader, psk, MLS_HASH_LEN) != 0) goto fail;
            group->resumption_psk_cache[i].valid = true;
            group->resumption_psk_cache[i].epoch = epoch;
            memcpy(group->resumption_psk_cache[i].psk, psk, MLS_HASH_LEN);
            sodium_memzero(psk, sizeof(psk));
        }
        if (!legacy || mls_tls_reader_remaining(&reader) > 0) {
            uint32_t path_key_count = 0;
            if (mls_tls_read_u32(&reader, &path_key_count) != 0) goto fail;
            if (path_key_count > MLS_OWN_PATH_KEY_CACHE_SIZE) goto fail;
            for (uint32_t i = 0; i < path_key_count; i++) {
                uint32_t node = 0;
                if (mls_tls_read_u32(&reader, &node) != 0) goto fail;
                group->own_path_keys[i].valid = true;
                group->own_path_keys[i].node = node;
                if (mls_tls_read_fixed(&reader, group->own_path_keys[i].sk,
                                       MLS_KEM_SK_LEN) != 0) goto fail;
                if (mls_tls_read_fixed(&reader, group->own_path_keys[i].pk,
                                       MLS_KEM_PK_LEN) != 0) goto fail;
            }
        }
    }

    if (!legacy) {
        /* The width its epoch started with: the canonical tree's (since
         * 0.12.0, mls_tree_canonical_leaves()), or the live width a state
         * written before kept; never wider than the live tree. */
        uint32_t width = 0;
        if (mls_tls_reader_remaining(&reader) >= 4) {
            const uint8_t *p = reader.data + reader.pos;
            width = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3];
        }
        if (width == 0 || width > group->tree.n_leaves ||
            mls_secret_tree_deserialize(&reader, width, &group->secret_tree) != 0)
            goto fail;
    } else {
        /* Migration (nostrc-ai04): no ratchet was stored, so re-derive the
         * tree -- receivers start every other sender at generation 0 again,
         * a window bounded by max_forward_distance -- and move our own
         * sender past every generation this state may have sent at. */
        int rc = mls_secret_tree_init(&group->secret_tree,
                                      group->epoch_secrets.encryption_secret,
                                      mls_tree_canonical_leaves(&group->tree));
        sodium_memzero(group->epoch_secrets.encryption_secret, MLS_HASH_LEN);
        if (rc != 0) goto fail;
        /* (An own leaf outside the tree -- e.g. we were removed -- cannot
         * send in this epoch at all: nothing to move.) */
        if (group->own_leaf_index < group->tree.n_leaves &&
            mls_secret_tree_skip(&group->secret_tree, group->own_leaf_index,
                                 MLS_SECRET_TREE_LEGACY_OWN_STRIDE) != 0)
            goto fail;
    }

    /* The profile (version 4), and a state that still satisfies it: a
     * stored or cloned adopted state is re-validated, never trusted; an
     * adopted GroupContext under a legacy (or no) profile is refused, as
     * before 0.12.0 (nostrc-qp24.5.1). */
    group->profile = MARMOT_GROUP_PROFILE_LEGACY;
    if (version > MLS_GROUP_SERIAL_VER_NO_PROFILE) {
        uint8_t profile = 0;
        if (mls_tls_read_u8(&reader, &profile) != 0 ||
            profile != MLS_GROUP_SERIAL_PROFILE_ADOPTED)
            goto fail;
        group->profile = MARMOT_GROUP_PROFILE_ADOPTED;
    }
    if (!mls_tls_reader_done(&reader) || mls_group_profile_check(group) != 0) goto fail;

    return 0;

fail:
    mls_group_free(group);
    return -1;
}
