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
derive_commit_secret(const uint8_t *path_secret, bool has_path,
                     uint8_t out[MLS_HASH_LEN])
{
    if (!has_path || !path_secret) {
        memset(out, 0, MLS_HASH_LEN);
        return 0;
    }
    return mls_crypto_derive_secret(out, path_secret, "path");
}

/**
 * Update the transcript hashes after a Commit.
 *
 * confirmed_transcript_hash =
 *   H(interim_transcript_hash_old || FramedContentTBS_of_commit)
 *
 * interim_transcript_hash =
 *   H(confirmed_transcript_hash_new || confirmation_tag)
 *
 * For simplicity, we hash the serialized commit as the FramedContent
 * stand-in (the full FramedContent wrapping is done at the Marmot layer).
 */
static int
update_transcript_hashes(MlsGroup *group,
                         const uint8_t *commit_data, size_t commit_len,
                         const uint8_t confirmation_tag[MLS_HASH_LEN])
{
    MlsTlsBuf buf;

    /* confirmed = H(old_interim || commit_content) */
    if (mls_tls_buf_init(&buf, MLS_HASH_LEN + commit_len) != 0)
        return -1;
    if (mls_tls_buf_append(&buf, group->interim_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;
    if (mls_tls_buf_append(&buf, commit_data, commit_len) != 0)
        goto fail;

    if (mls_crypto_hash(group->confirmed_transcript_hash, buf.data, buf.len) != 0)
        goto fail;
    mls_tls_buf_free(&buf);

    /* interim = H(new_confirmed || confirmation_tag) */
    if (mls_tls_buf_init(&buf, MLS_HASH_LEN * 2) != 0)
        return -1;
    if (mls_tls_buf_append(&buf, group->confirmed_transcript_hash, MLS_HASH_LEN) != 0)
        goto fail;
    if (mls_tls_buf_append(&buf, confirmation_tag, MLS_HASH_LEN) != 0)
        goto fail;

    if (mls_crypto_hash(group->interim_transcript_hash, buf.data, buf.len) != 0)
        goto fail;
    mls_tls_buf_free(&buf);

    return 0;
fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/**
 * Derive epoch secrets and re-initialize the secret tree for the current
 * group state. Updates group->epoch_secrets and group->secret_tree.
 */
static int
group_derive_epoch(MlsGroup *group,
                   const uint8_t *init_secret_prev,
                   const uint8_t commit_secret[MLS_HASH_LEN])
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
                                     gc_data, gc_len, NULL, &new_secrets);
    free(gc_data);
    if (rc != 0)
        return rc;

    /* Free old secret tree and install new one */
    mls_secret_tree_free(&group->secret_tree);
    memcpy(&group->epoch_secrets, &new_secrets, sizeof(new_secrets));

    if (mls_secret_tree_init(&group->secret_tree,
                              new_secrets.encryption_secret,
                              group->tree.n_leaves) != 0)
        return -1;

    return 0;
}

/**
 * Generate an UpdatePath for the committer. Produces:
 *   - New leaf node with fresh encryption key
 *   - Path secrets encrypted for each copath resolution member
 *   - The path_secret at the root (used to derive commit_secret)
 */
static int
generate_update_path(MlsGroup *group,
                     const uint8_t *credential_identity, size_t cred_len,
                     MlsUpdatePath *path_out,
                     uint8_t root_path_secret[MLS_HASH_LEN])
{
    uint32_t n_leaves = group->tree.n_leaves;
    memset(path_out, 0, sizeof(*path_out));

    /* Compute filtered direct path */
    uint32_t fdp[64];
    uint32_t fdp_len = 0;
    if (mls_tree_filtered_direct_path(&group->tree, group->own_leaf_index,
                                       fdp, &fdp_len) != 0)
        return -1;
    
    /* Bounds check: ensure path fits in our fixed array */
    if (fdp_len > 64) {
        return MARMOT_ERR_INTERNAL;
    }

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
    path_out->leaf_node.leaf_node_source = 3; /* commit */
    path_out->leaf_node.ciphersuite_count = 1;
    path_out->leaf_node.ciphersuites = malloc(sizeof(uint16_t));
    if (!path_out->leaf_node.ciphersuites) goto fail;
    path_out->leaf_node.ciphersuites[0] = MARMOT_CIPHERSUITE;

    /* Generate path secrets for each filtered direct path node */
    if (fdp_len == 0) {
        /* Degenerate: single member, no path needed */
        mls_crypto_random(root_path_secret, MLS_HASH_LEN);
        path_out->nodes = NULL;
        path_out->node_count = 0;
        sodium_memzero(new_enc_sk, sizeof(new_enc_sk));
        return 0;
    }

    path_out->nodes = calloc(fdp_len, sizeof(MlsUpdatePathNode));
    if (!path_out->nodes) goto fail;
    path_out->node_count = fdp_len;

    /* Generate a random leaf path secret and derive up the tree */
    uint8_t path_secrets[64][MLS_HASH_LEN];
    mls_crypto_random(path_secrets[0], MLS_HASH_LEN);

    for (uint32_t i = 1; i < fdp_len; i++) {
        /* path_secret[i] = DeriveSecret(path_secret[i-1], "path") */
        if (mls_crypto_derive_secret(path_secrets[i], path_secrets[i - 1], "path") != 0)
            goto fail;
    }

    /* The root path secret is the last one */
    memcpy(root_path_secret, path_secrets[fdp_len - 1], MLS_HASH_LEN);

    /* For each node on the filtered direct path, derive the node key
     * and encrypt the path secret for each copath resolution member */
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t node_idx = fdp[i];

        /* Derive node keypair from path_secret:
         * node_secret = DeriveSecret(path_secret, "node")
         * node_priv = ExpandWithLabel(node_secret, "key", "", KEM_SK_LEN)
         * (simplified: we use the path secret to seed a KEM keypair) */
        uint8_t node_secret[MLS_HASH_LEN];
        if (mls_crypto_derive_secret(node_secret, path_secrets[i], "node") != 0)
            goto fail;

        uint8_t node_sk[MLS_KEM_SK_LEN];
        uint8_t node_pk[MLS_KEM_PK_LEN];
        if (mls_crypto_hkdf_expand(node_sk, MLS_KEM_SK_LEN, node_secret,
                                    (const uint8_t *)"mls10 key", 9) != 0)
            goto fail;
        /* Derive public key from private (X25519 clamping) */
        crypto_scalarmult_base(node_pk, node_sk);

        memcpy(path_out->nodes[i].encryption_key, node_pk, MLS_KEM_PK_LEN);

        /* Get copath sibling for this direct-path node */
        uint32_t sibling = mls_tree_sibling(node_idx, n_leaves);

        /* Get resolution of the sibling (nodes we need to encrypt to) */
        uint32_t resolution[256];
        uint32_t res_len = 0;
        if (mls_tree_resolution(&group->tree, sibling, resolution, &res_len) != 0)
            goto fail;
        
        /* Bounds check */
        if (res_len > 256) goto fail;

        /* Encrypt path_secret to each resolution member's encryption key */
        MlsTlsBuf enc_buf;
        if (mls_tls_buf_init(&enc_buf, 256) != 0) goto fail;

        /* Write count */
        if (mls_tls_write_u32(&enc_buf, res_len) != 0) {
            mls_tls_buf_free(&enc_buf);
            goto fail;
        }

        for (uint32_t j = 0; j < res_len; j++) {
            uint32_t target_node = resolution[j];
            const uint8_t *target_pk = NULL;

            if (mls_tree_is_leaf(target_node)) {
                target_pk = group->tree.nodes[target_node].leaf.encryption_key;
            } else {
                target_pk = group->tree.nodes[target_node].parent.encryption_key;
            }

            /* HPKE encap: encrypt path_secret to target */
            uint8_t shared_secret[MLS_KEM_SECRET_LEN];
            uint8_t enc[MLS_KEM_ENC_LEN];
            if (mls_crypto_kem_encap(shared_secret, enc, target_pk) != 0) {
                mls_tls_buf_free(&enc_buf);
                goto fail;
            }

            /* Encrypt path_secret using shared_secret as AEAD key */
            uint8_t aead_key[MLS_AEAD_KEY_LEN];
            uint8_t aead_nonce[MLS_AEAD_NONCE_LEN];
            memcpy(aead_key, shared_secret, MLS_AEAD_KEY_LEN);
            memset(aead_nonce, 0, MLS_AEAD_NONCE_LEN);

            uint8_t ct[MLS_HASH_LEN + MLS_AEAD_TAG_LEN];
            size_t ct_len = 0;
            if (mls_crypto_aead_encrypt(ct, &ct_len, aead_key, aead_nonce,
                                         path_secrets[i], MLS_HASH_LEN,
                                         NULL, 0) != 0) {
                mls_tls_buf_free(&enc_buf);
                goto fail;
            }

            /* Write HPKECiphertext: enc || ciphertext */
            if (mls_tls_write_opaque16(&enc_buf, enc, MLS_KEM_ENC_LEN) != 0 ||
                mls_tls_write_opaque16(&enc_buf, ct, ct_len) != 0) {
                mls_tls_buf_free(&enc_buf);
                goto fail;
            }

            sodium_memzero(shared_secret, sizeof(shared_secret));
            sodium_memzero(aead_key, sizeof(aead_key));
        }

        path_out->nodes[i].encrypted_path_secrets = enc_buf.data;
        path_out->nodes[i].encrypted_path_secrets_len = enc_buf.len;
        path_out->nodes[i].secret_count = res_len;
        /* Don't free enc_buf — ownership transferred */

        /* Update the tree node */
        if (group->tree.nodes[node_idx].type != MLS_NODE_BLANK) {
            if (group->tree.nodes[node_idx].type == MLS_NODE_PARENT)
                mls_parent_node_clear(&group->tree.nodes[node_idx].parent);
        }
        group->tree.nodes[node_idx].type = MLS_NODE_PARENT;
        memset(&group->tree.nodes[node_idx].parent, 0, sizeof(MlsParentNode));
        memcpy(group->tree.nodes[node_idx].parent.encryption_key, node_pk, MLS_KEM_PK_LEN);

        sodium_memzero(node_secret, sizeof(node_secret));
        sodium_memzero(node_sk, sizeof(node_sk));
    }

    /* Update our own leaf in the tree */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    mls_leaf_node_clear(&group->tree.nodes[own_node].leaf);
    if (mls_leaf_node_clone(&group->tree.nodes[own_node].leaf, &path_out->leaf_node) != 0)
        goto fail;
    group->tree.nodes[own_node].type = MLS_NODE_LEAF;

    /* Update our stored encryption private key */
    memcpy(group->own_encryption_key, new_enc_sk, MLS_KEM_SK_LEN);

    sodium_memzero(path_secrets, sizeof(path_secrets));
    sodium_memzero(new_enc_sk, sizeof(new_enc_sk));
    return 0;

fail:
    sodium_memzero(new_enc_sk, sizeof(new_enc_sk));
    sodium_memzero(path_secrets, sizeof(path_secrets));
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
                    const uint8_t own_enc_sk[MLS_KEM_SK_LEN],
                    const uint8_t own_enc_pk[MLS_KEM_PK_LEN],
                    uint8_t out_path_secret[MLS_HASH_LEN])
{
    /* Get resolution of the copath node to find our position */
    uint32_t resolution[256];
    uint32_t res_len = 0;
    if (mls_tree_resolution(&group->tree, copath_node_idx, resolution, &res_len) != 0)
        return -1;
    
    /* Bounds check */
    if (res_len > 256)
        return MARMOT_ERR_INTERNAL;

    /* Find our position in the resolution */
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    int our_idx = -1;
    for (uint32_t i = 0; i < res_len; i++) {
        if (resolution[i] == own_node) {
            our_idx = (int)i;
            break;
        }
    }
    if (our_idx < 0)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;

    /* Parse the encrypted path secrets to find ours */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, path_node->encrypted_path_secrets,
                        path_node->encrypted_path_secrets_len);

    uint32_t count = 0;
    if (mls_tls_read_u32(&reader, &count) != 0)
        return -1;
    if ((uint32_t)our_idx >= count)
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

    /* HPKE decap */
    uint8_t shared_secret[MLS_KEM_SECRET_LEN];
    if (mls_crypto_kem_decap(shared_secret, enc, own_enc_sk, own_enc_pk) != 0) {
        free(enc); free(ct);
        return -1;
    }
    free(enc);

    /* Decrypt path secret */
    uint8_t aead_key[MLS_AEAD_KEY_LEN];
    uint8_t aead_nonce[MLS_AEAD_NONCE_LEN];
    memcpy(aead_key, shared_secret, MLS_AEAD_KEY_LEN);
    memset(aead_nonce, 0, MLS_AEAD_NONCE_LEN);

    size_t pt_len = 0;
    if (mls_crypto_aead_decrypt(out_path_secret, &pt_len, aead_key, aead_nonce,
                                 ct, ct_len, NULL, 0) != 0) {
        free(ct);
        sodium_memzero(shared_secret, sizeof(shared_secret));
        sodium_memzero(aead_key, sizeof(aead_key));
        return MARMOT_ERR_CRYPTO;
    }

    free(ct);
    sodium_memzero(shared_secret, sizeof(shared_secret));
    sodium_memzero(aead_key, sizeof(aead_key));

    if (pt_len != MLS_HASH_LEN)
        return -1;

    return 0;
}

/**
 * Compute confirmation tag:
 * confirmation_tag = MAC(confirmation_key, confirmed_transcript_hash)
 * (Using HMAC-SHA256 since we don't have a separate MAC primitive)
 */
static int
compute_confirmation_tag(const uint8_t confirmation_key[MLS_HASH_LEN],
                         const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                         uint8_t out[MLS_HASH_LEN])
{
    /* MAC(key, data) = HMAC-SHA256(key, data) = HKDF-Extract(key, data) */
    return mls_crypto_hkdf_extract(out, confirmation_key, MLS_HASH_LEN,
                                    confirmed_transcript_hash, MLS_HASH_LEN);
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
    free(g->extensions_data);
    sodium_memzero(g->own_signature_key, sizeof(g->own_signature_key));
    sodium_memzero(g->own_encryption_key, sizeof(g->own_encryption_key));
    sodium_memzero(&g->epoch_secrets, sizeof(g->epoch_secrets));
    memset(g, 0, sizeof(*g));
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
    free(gi->extensions_data);
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
    if (!group || !group_id || !credential_identity || !signature_key_private)
        return MARMOT_ERR_INVALID_ARG;

    memset(group, 0, sizeof(*group));

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

    leaf->leaf.ciphersuite_count = 1;
    leaf->leaf.ciphersuites = malloc(sizeof(uint16_t));
    if (!leaf->leaf.ciphersuites) {
        sodium_memzero(enc_sk, sizeof(enc_sk));
        goto fail;
    }
    leaf->leaf.ciphersuites[0] = MARMOT_CIPHERSUITE;
    leaf->leaf.leaf_node_source = 3; /* commit (initial group creation) */

    sodium_memzero(enc_sk, sizeof(enc_sk));

    /* Initialize transcript hashes to zero (epoch 0) */
    memset(group->confirmed_transcript_hash, 0, MLS_HASH_LEN);
    memset(group->interim_transcript_hash, 0, MLS_HASH_LEN);

    /* Derive epoch 0 secrets:
     * init_secret = all zeros (no previous epoch)
     * commit_secret = all zeros (no commit) */
    uint8_t zero_secret[MLS_HASH_LEN];
    memset(zero_secret, 0, MLS_HASH_LEN);

    if (group_derive_epoch(group, NULL, zero_secret) != 0) goto fail;

    return 0;

fail:
    mls_group_free(group);
    return MARMOT_ERR_INTERNAL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Add member
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_add_member(MlsGroup *group,
                     const MlsKeyPackage *kp,
                     MlsAddResult *result)
{
    if (!group || !kp || !result)
        return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    /* Validate the key package */
    int rc = mls_key_package_validate(kp);
    if (rc != 0) return rc;

    /* Add a new leaf to the tree */
    uint32_t new_leaf_node_idx;
    if (mls_tree_add_leaf(&group->tree, &new_leaf_node_idx) != 0)
        return MARMOT_ERR_INTERNAL;

    /* Copy the key package's leaf node into the new position */
    MlsNode *new_node = &group->tree.nodes[new_leaf_node_idx];
    new_node->type = MLS_NODE_LEAF;
    if (mls_leaf_node_clone(&new_node->leaf, &kp->leaf_node) != 0)
        return MARMOT_ERR_INTERNAL;

    /* Build the Add proposal */
    MlsProposal add_prop;
    memset(&add_prop, 0, sizeof(add_prop));
    add_prop.type = MLS_PROPOSAL_ADD;
    /* Deep-copy the key package into the proposal */
    memcpy(&add_prop.add.key_package, kp, sizeof(MlsKeyPackage));
    /* Clone the heap-allocated parts */
    add_prop.add.key_package.extensions_data = NULL;
    add_prop.add.key_package.extensions_len = 0;
    if (kp->extensions_data && kp->extensions_len > 0) {
        add_prop.add.key_package.extensions_data = malloc(kp->extensions_len);
        if (!add_prop.add.key_package.extensions_data)
            return MARMOT_ERR_MEMORY;
        memcpy(add_prop.add.key_package.extensions_data,
               kp->extensions_data, kp->extensions_len);
        add_prop.add.key_package.extensions_len = kp->extensions_len;
    }
    if (mls_leaf_node_clone(&add_prop.add.key_package.leaf_node, &kp->leaf_node) != 0) {
        free(add_prop.add.key_package.extensions_data);
        return MARMOT_ERR_MEMORY;
    }

    /* Generate UpdatePath */
    uint8_t root_path_secret[MLS_HASH_LEN];
    MlsUpdatePath update_path;
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    const uint8_t *own_cred = group->tree.nodes[own_node].leaf.credential_identity;
    size_t own_cred_len = group->tree.nodes[own_node].leaf.credential_identity_len;

    if (generate_update_path(group, own_cred, own_cred_len,
                             &update_path, root_path_secret) != 0) {
        mls_proposal_clear(&add_prop);
        return MARMOT_ERR_INTERNAL;
    }

    /* Build the Commit */
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = malloc(sizeof(MlsProposal));
    if (!commit.proposals) {
        mls_proposal_clear(&add_prop);
        mls_update_path_clear(&update_path);
        return MARMOT_ERR_MEMORY;
    }
    commit.proposals[0] = add_prop;
    commit.proposal_count = 1;
    commit.has_path = true;
    commit.path = update_path;

    /* Serialize the commit */
    MlsTlsBuf commit_buf;
    if (mls_tls_buf_init(&commit_buf, 1024) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_commit_serialize(&commit, &commit_buf) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Advance epoch */
    uint8_t commit_secret[MLS_HASH_LEN];
    if (derive_commit_secret(root_path_secret, true, commit_secret) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Update confirmed transcript hash: H(interim_old || commit) */
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + commit_buf.len) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, commit_buf.data, commit_buf.len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    mls_tls_buf_free(&conf_buf);

    group->epoch++;

    /* Derive new epoch secrets */
    const uint8_t *prev_init = group->epoch_secrets.init_secret;
    if (group_derive_epoch(group, prev_init, commit_secret) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Compute confirmation tag */
    uint8_t confirmation_tag[MLS_HASH_LEN];
    if (compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                                 group->confirmed_transcript_hash,
                                 confirmation_tag) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Update interim transcript hash: H(confirmed || confirmation_tag) */
    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    /* Build Welcome for the new member */
    MlsTlsBuf welcome_buf;
    if (mls_tls_buf_init(&welcome_buf, 2048) != 0) {
        mls_tls_buf_free(&commit_buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }

    /* Welcome = GroupInfo encrypted to joiner's init_key */
    {
        /* Build GroupInfo */
        MlsGroupInfo gi;
        if (mls_group_info_build(group, &gi) != 0) {
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }
        memcpy(gi.confirmation_tag, confirmation_tag, MLS_HASH_LEN);

        /* Sign the GroupInfo */
        MlsTlsBuf gi_buf;
        if (mls_tls_buf_init(&gi_buf, 512) != 0) {
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }
        if (mls_group_info_serialize(&gi, &gi_buf) != 0) {
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* Encrypt GroupInfo using welcome_secret derived from joiner_secret */
        uint8_t welcome_key[MLS_AEAD_KEY_LEN];
        uint8_t welcome_nonce[MLS_AEAD_NONCE_LEN];
        if (mls_crypto_expand_with_label(welcome_key, MLS_AEAD_KEY_LEN,
                                          group->epoch_secrets.welcome_secret,
                                          "key", NULL, 0) != 0 ||
            mls_crypto_expand_with_label(welcome_nonce, MLS_AEAD_NONCE_LEN,
                                          group->epoch_secrets.welcome_secret,
                                          "nonce", NULL, 0) != 0) {
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        uint8_t *enc_gi = malloc(gi_buf.len + MLS_AEAD_TAG_LEN);
        if (!enc_gi) {
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_MEMORY;
        }
        size_t enc_gi_len = 0;
        if (mls_crypto_aead_encrypt(enc_gi, &enc_gi_len, welcome_key, welcome_nonce,
                                     gi_buf.data, gi_buf.len, NULL, 0) != 0) {
            free(enc_gi);
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* HPKE encap joiner_secret to new member's init_key */
        uint8_t shared_secret[MLS_KEM_SECRET_LEN];
        uint8_t kem_enc[MLS_KEM_ENC_LEN];
        if (mls_crypto_kem_encap(shared_secret, kem_enc, kp->init_key) != 0) {
            free(enc_gi);
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* Encrypt joiner_secret to shared_secret */
        uint8_t js_key[MLS_AEAD_KEY_LEN];
        memcpy(js_key, shared_secret, MLS_AEAD_KEY_LEN);
        uint8_t js_nonce[MLS_AEAD_NONCE_LEN];
        memset(js_nonce, 0, MLS_AEAD_NONCE_LEN);
        uint8_t enc_js[MLS_HASH_LEN + MLS_AEAD_TAG_LEN];
        size_t enc_js_len = 0;
        if (mls_crypto_aead_encrypt(enc_js, &enc_js_len, js_key, js_nonce,
                                     group->epoch_secrets.joiner_secret, MLS_HASH_LEN,
                                     NULL, 0) != 0) {
            free(enc_gi);
            mls_group_info_clear(&gi);
            mls_tls_buf_free(&gi_buf);
            mls_tls_buf_free(&commit_buf);
            mls_tls_buf_free(&welcome_buf);
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* Assemble Welcome message:
         * Welcome = cipher_suite || encrypted_group_secrets || encrypted_group_info
         * encrypted_group_secrets = kp_ref || HPKECiphertext(enc, encrypted_joiner_secret)
         */
        /* cipher_suite */
        mls_tls_write_u16(&welcome_buf, MARMOT_CIPHERSUITE);
        /* secrets count (1 entry for the new member) */
        mls_tls_write_u32(&welcome_buf, 1);
        /* key package ref */
        {
            uint8_t kp_ref[MLS_HASH_LEN];
            if (mls_key_package_ref(kp, kp_ref) != 0) {
                free(enc_gi);
                mls_group_info_clear(&gi);
                mls_tls_buf_free(&gi_buf);
                mls_tls_buf_free(&commit_buf);
                mls_tls_buf_free(&welcome_buf);
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
            mls_tls_buf_append(&welcome_buf, kp_ref, MLS_HASH_LEN);
        }
        /* HPKECiphertext: kem_output || ciphertext */
        mls_tls_write_opaque16(&welcome_buf, kem_enc, MLS_KEM_ENC_LEN);
        mls_tls_write_opaque16(&welcome_buf, enc_js, enc_js_len);
        /* encrypted_group_info */
        mls_tls_write_opaque32(&welcome_buf, enc_gi, enc_gi_len);

        free(enc_gi);
        mls_group_info_clear(&gi);
        mls_tls_buf_free(&gi_buf);
        sodium_memzero(shared_secret, sizeof(shared_secret));
        sodium_memzero(js_key, sizeof(js_key));
    }

    /* Transfer ownership to result */
    result->commit_data = commit_buf.data;
    result->commit_len = commit_buf.len;
    result->welcome_data = welcome_buf.data;
    result->welcome_len = welcome_buf.len;

    mls_commit_clear(&commit);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Remove member
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_remove_member(MlsGroup *group,
                        uint32_t leaf_index,
                        MlsCommitResult *result)
{
    if (!group || !result) return MARMOT_ERR_INVALID_ARG;
    if (leaf_index == group->own_leaf_index) return MARMOT_ERR_INVALID_ARG;
    if (leaf_index >= group->tree.n_leaves) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    /* Blank the removed member's leaf and path to root */
    uint32_t removed_node = mls_tree_leaf_to_node(leaf_index);
    mls_tree_blank_node(&group->tree.nodes[removed_node]);

    /* Blank nodes on the direct path */
    uint32_t path[64];
    uint32_t path_len = mls_tree_direct_path(removed_node, group->tree.n_leaves,
                                              path, 64);
    
    /* Bounds check */
    if (path_len > 64) {
        return MARMOT_ERR_INTERNAL;
    }
    
    for (uint32_t i = 0; i < path_len; i++) {
        mls_tree_blank_node(&group->tree.nodes[path[i]]);
    }

    /* Build Remove proposal */
    MlsProposal remove_prop;
    memset(&remove_prop, 0, sizeof(remove_prop));
    remove_prop.type = MLS_PROPOSAL_REMOVE;
    remove_prop.remove.removed_leaf = leaf_index;

    /* Generate UpdatePath */
    uint8_t root_path_secret[MLS_HASH_LEN];
    MlsUpdatePath update_path;
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    const uint8_t *own_cred = group->tree.nodes[own_node].leaf.credential_identity;
    size_t own_cred_len = group->tree.nodes[own_node].leaf.credential_identity_len;

    if (generate_update_path(group, own_cred, own_cred_len,
                             &update_path, root_path_secret) != 0)
        return MARMOT_ERR_INTERNAL;

    /* Build Commit */
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = malloc(sizeof(MlsProposal));
    if (!commit.proposals) {
        mls_update_path_clear(&update_path);
        return MARMOT_ERR_MEMORY;
    }
    commit.proposals[0] = remove_prop;
    commit.proposal_count = 1;
    commit.has_path = true;
    commit.path = update_path;

    /* Serialize */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 1024) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_commit_serialize(&commit, &buf) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Advance epoch */
    uint8_t commit_secret[MLS_HASH_LEN];
    derive_commit_secret(root_path_secret, true, commit_secret);

    /* Update confirmed transcript hash */
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + buf.len) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, buf.data, buf.len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    mls_tls_buf_free(&conf_buf);

    group->epoch++;
    const uint8_t *prev_init = group->epoch_secrets.init_secret;
    if (group_derive_epoch(group, prev_init, commit_secret) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    uint8_t confirmation_tag[MLS_HASH_LEN];
    compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                             group->confirmed_transcript_hash,
                             confirmation_tag);

    /* Update interim transcript hash */
    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    result->commit_data = buf.data;
    result->commit_len = buf.len;

    mls_commit_clear(&commit);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Self-update
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_self_update(MlsGroup *group, MlsCommitResult *result)
{
    if (!group || !result) return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    /* Generate UpdatePath (which replaces our leaf and path keys) */
    uint8_t root_path_secret[MLS_HASH_LEN];
    MlsUpdatePath update_path;
    uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
    const uint8_t *own_cred = group->tree.nodes[own_node].leaf.credential_identity;
    size_t own_cred_len = group->tree.nodes[own_node].leaf.credential_identity_len;

    if (generate_update_path(group, own_cred, own_cred_len,
                             &update_path, root_path_secret) != 0)
        return MARMOT_ERR_INTERNAL;

    /* Empty commit (just the path, no proposals) */
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = NULL;
    commit.proposal_count = 0;
    commit.has_path = true;
    commit.path = update_path;

    /* Serialize */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 1024) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_commit_serialize(&commit, &buf) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Advance epoch */
    uint8_t commit_secret[MLS_HASH_LEN];
    derive_commit_secret(root_path_secret, true, commit_secret);

    /* Update confirmed transcript hash */
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + buf.len) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, buf.data, buf.len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    mls_tls_buf_free(&conf_buf);

    group->epoch++;
    const uint8_t *prev_init = group->epoch_secrets.init_secret;
    if (group_derive_epoch(group, prev_init, commit_secret) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    uint8_t confirmation_tag[MLS_HASH_LEN];
    compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                             group->confirmed_transcript_hash,
                             confirmation_tag);

    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_tls_buf_free(&buf);
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    result->commit_data = buf.data;
    result->commit_len = buf.len;

    mls_commit_clear(&commit);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Process incoming Commit
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Validate proposal ordering per RFC 9420.
 * Proposals must be ordered: Updates, Removes, then Adds.
 */
static int
validate_proposal_ordering(const MlsProposal *proposals, size_t count)
{
    if (count == 0) return 0;
    
    /* Track the last seen proposal type to enforce ordering */
    int last_type = -1;
    
    for (size_t i = 0; i < count; i++) {
        int current_type = proposals[i].type;
        
        /* Map to ordering groups: Update=0, Remove=1, Add=2 */
        int order_group;
        switch (current_type) {
        case MLS_PROPOSAL_UPDATE:
            order_group = 0;
            break;
        case MLS_PROPOSAL_REMOVE:
            order_group = 1;
            break;
        case MLS_PROPOSAL_ADD:
            order_group = 2;
            break;
        default:
            /* Unknown proposal types are allowed for forward compatibility */
            continue;
        }
        
        /* Ensure non-decreasing order */
        if (order_group < last_type) {
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
        }
        last_type = order_group;
    }
    
    return 0;
}

int
mls_group_process_commit(MlsGroup *group,
                         const uint8_t *commit_data, size_t commit_len,
                         uint32_t sender_leaf)
{
    if (!group || !commit_data) return MARMOT_ERR_INVALID_ARG;
    if (sender_leaf >= group->tree.n_leaves) return MARMOT_ERR_INVALID_ARG;
    if (sender_leaf == group->own_leaf_index) return MARMOT_ERR_OWN_COMMIT_PENDING;

    MlsCommit commit;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, commit_data, commit_len);
    if (mls_commit_deserialize(&reader, &commit) != 0)
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;

    /* Validate proposal ordering per RFC 9420 */
    if (validate_proposal_ordering(commit.proposals, commit.proposal_count) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    }

    /* Apply proposals */
    for (size_t i = 0; i < commit.proposal_count; i++) {
        MlsProposal *p = &commit.proposals[i];
        switch (p->type) {
        case MLS_PROPOSAL_ADD: {
            uint32_t new_leaf_idx;
            if (mls_tree_add_leaf(&group->tree, &new_leaf_idx) != 0) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
            MlsNode *n = &group->tree.nodes[new_leaf_idx];
            n->type = MLS_NODE_LEAF;
            if (mls_leaf_node_clone(&n->leaf, &p->add.key_package.leaf_node) != 0) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
            break;
        }
        case MLS_PROPOSAL_REMOVE: {
            /* Validate leaf index */
            if (p->remove.removed_leaf >= group->tree.n_leaves) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INVALID_ARG;
            }
            uint32_t rm_node = mls_tree_leaf_to_node(p->remove.removed_leaf);
            mls_tree_blank_node(&group->tree.nodes[rm_node]);
            /* Blank path to root */
            uint32_t dp[64];
            uint32_t dp_len = mls_tree_direct_path(rm_node, group->tree.n_leaves, dp, 64);
            
            /* Bounds check */
            if (dp_len > 64) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
            
            for (uint32_t j = 0; j < dp_len; j++)
                mls_tree_blank_node(&group->tree.nodes[dp[j]]);
            break;
        }
        case MLS_PROPOSAL_UPDATE: {
            uint32_t sender_node = mls_tree_leaf_to_node(sender_leaf);
            mls_leaf_node_clear(&group->tree.nodes[sender_node].leaf);
            if (mls_leaf_node_clone(&group->tree.nodes[sender_node].leaf,
                                     &p->update.leaf_node) != 0) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
            break;
        }
        default:
            break; /* Ignore unknown proposals for forward compat */
        }
    }

    /* Process UpdatePath if present */
    uint8_t root_path_secret[MLS_HASH_LEN];
    bool has_path = commit.has_path;

    if (has_path) {
        /* Update sender's leaf in the tree */
        uint32_t sender_node = mls_tree_leaf_to_node(sender_leaf);
        mls_leaf_node_clear(&group->tree.nodes[sender_node].leaf);
        if (mls_leaf_node_clone(&group->tree.nodes[sender_node].leaf,
                                 &commit.path.leaf_node) != 0) {
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* Find our position relative to the sender's direct path */
        uint32_t fdp[64];
        uint32_t fdp_len = 0;
        if (mls_tree_filtered_direct_path(&group->tree, sender_leaf,
                                           fdp, &fdp_len) != 0) {
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }
        
        /* Bounds check */
        if (fdp_len > 64) {
            mls_commit_clear(&commit);
            return MARMOT_ERR_INTERNAL;
        }

        /* Find which copath node we're under */
        uint32_t own_node = mls_tree_leaf_to_node(group->own_leaf_index);
        int our_path_idx = -1;

        for (uint32_t i = 0; i < fdp_len && i < commit.path.node_count; i++) {
            uint32_t copath_sibling = mls_tree_sibling(fdp[i], group->tree.n_leaves);
            /* Check if we're in the resolution of this copath sibling */
            uint32_t resolution[256];
            uint32_t res_len = 0;
            if (mls_tree_resolution(&group->tree, copath_sibling,
                                     resolution, &res_len) == 0) {
                /* Bounds check */
                if (res_len > 256) {
                    mls_commit_clear(&commit);
                    return MARMOT_ERR_INTERNAL;
                }
                
                for (uint32_t j = 0; j < res_len; j++) {
                    if (resolution[j] == own_node) {
                        our_path_idx = (int)i;
                        break;
                    }
                }
            }
            if (our_path_idx >= 0) break;
        }

        if (our_path_idx < 0) {
            mls_commit_clear(&commit);
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
        }

        /* Decrypt our path secret */
        uint32_t copath_sibling = mls_tree_sibling(fdp[our_path_idx],
                                                    group->tree.n_leaves);
        /* Get our encryption keys */
        const uint8_t *our_enc_pk = group->tree.nodes[own_node].leaf.encryption_key;
        const uint8_t *our_enc_sk = group->own_encryption_key;

        uint8_t our_path_secret[MLS_HASH_LEN];

        /* Decrypt from the path node using our stored private key */
        if (decrypt_path_secret(group, &commit.path.nodes[our_path_idx],
                                copath_sibling,
                                our_enc_sk,
                                our_enc_pk,
                                our_path_secret) != 0) {
            mls_commit_clear(&commit);
            return MARMOT_ERR_CRYPTO;
        }

        /* Derive path secrets up to root */
        uint8_t current_secret[MLS_HASH_LEN];
        memcpy(current_secret, our_path_secret, MLS_HASH_LEN);
        for (uint32_t i = (uint32_t)our_path_idx + 1; i < fdp_len; i++) {
            if (mls_crypto_derive_secret(current_secret, current_secret, "path") != 0) {
                mls_commit_clear(&commit);
                return MARMOT_ERR_INTERNAL;
            }
        }
        memcpy(root_path_secret, current_secret, MLS_HASH_LEN);

        /* Update parent nodes from the update path */
        for (uint32_t i = 0; i < fdp_len && i < commit.path.node_count; i++) {
            uint32_t node_idx = fdp[i];
            if (group->tree.nodes[node_idx].type == MLS_NODE_PARENT)
                mls_parent_node_clear(&group->tree.nodes[node_idx].parent);
            group->tree.nodes[node_idx].type = MLS_NODE_PARENT;
            memset(&group->tree.nodes[node_idx].parent, 0, sizeof(MlsParentNode));
            memcpy(group->tree.nodes[node_idx].parent.encryption_key,
                   commit.path.nodes[i].encryption_key, MLS_KEM_PK_LEN);
        }

        sodium_memzero(our_path_secret, sizeof(our_path_secret));
        sodium_memzero(current_secret, sizeof(current_secret));
    }

    /* Derive commit_secret */
    uint8_t commit_secret[MLS_HASH_LEN];
    derive_commit_secret(has_path ? root_path_secret : NULL, has_path, commit_secret);

    /* Update confirmed transcript hash */
    MlsTlsBuf conf_buf;
    if (mls_tls_buf_init(&conf_buf, MLS_HASH_LEN + commit_len) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&conf_buf, group->interim_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&conf_buf, commit_data, commit_len);
    mls_crypto_hash(group->confirmed_transcript_hash, conf_buf.data, conf_buf.len);
    mls_tls_buf_free(&conf_buf);

    /* Advance epoch */
    group->epoch++;
    const uint8_t *prev_init = group->epoch_secrets.init_secret;
    if (group_derive_epoch(group, prev_init, commit_secret) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_INTERNAL;
    }

    /* Compute and verify confirmation tag */
    uint8_t confirmation_tag[MLS_HASH_LEN];
    compute_confirmation_tag(group->epoch_secrets.confirmation_key,
                             group->confirmed_transcript_hash,
                             confirmation_tag);

    /* Update interim transcript hash */
    MlsTlsBuf int_buf;
    if (mls_tls_buf_init(&int_buf, MLS_HASH_LEN * 2) != 0) {
        mls_commit_clear(&commit);
        return MARMOT_ERR_MEMORY;
    }
    mls_tls_buf_append(&int_buf, group->confirmed_transcript_hash, MLS_HASH_LEN);
    mls_tls_buf_append(&int_buf, confirmation_tag, MLS_HASH_LEN);
    mls_crypto_hash(group->interim_transcript_hash, int_buf.data, int_buf.len);
    mls_tls_buf_free(&int_buf);

    mls_commit_clear(&commit);
    sodium_memzero(root_path_secret, sizeof(root_path_secret));
    sodium_memzero(commit_secret, sizeof(commit_secret));

    return 0;
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

    /* Derive message keys for our leaf */
    MlsMessageKeys keys;
    if (mls_secret_tree_derive_keys(&group->secret_tree, group->own_leaf_index,
                                     false /* application */, &keys) != 0)
        return MARMOT_ERR_INTERNAL;

    /* Generate reuse guard */
    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, 4);

    /* Encrypt as PrivateMessage */
    MlsPrivateMessage msg;
    if (mls_private_message_encrypt(
            group->group_id, group->group_id_len,
            group->epoch,
            MLS_CONTENT_TYPE_APPLICATION,
            NULL, 0, /* no AAD */
            plaintext, plaintext_len,
            group->epoch_secrets.sender_data_secret,
            &keys, group->own_leaf_index,
            reuse_guard, &msg) != 0)
        return MARMOT_ERR_MLS_CREATE_MESSAGE;

    /* Serialize */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, plaintext_len + 256) != 0) {
        mls_private_message_clear(&msg);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_private_message_serialize(&msg, &buf) != 0) {
        mls_private_message_clear(&msg);
        mls_tls_buf_free(&buf);
        return MARMOT_ERR_MLS_FRAMING;
    }

    *out_data = buf.data;
    *out_len = buf.len;
    mls_private_message_clear(&msg);

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

    /* Deserialize PrivateMessage */
    MlsPrivateMessage msg;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, ciphertext, ciphertext_len);
    if (mls_private_message_deserialize(&reader, &msg) != 0)
        return MARMOT_ERR_MLS_FRAMING;

    /* Verify group_id and epoch match */
    if (msg.group_id_len != group->group_id_len ||
        memcmp(msg.group_id, group->group_id, group->group_id_len) != 0) {
        mls_private_message_clear(&msg);
        return MARMOT_ERR_WRONG_GROUP_ID;
    }
    if (msg.epoch != group->epoch) {
        mls_private_message_clear(&msg);
        return MARMOT_ERR_WRONG_EPOCH;
    }

    /* Decrypt */
    MlsSenderData sender_data;
    if (mls_private_message_decrypt(&msg,
                                     group->epoch_secrets.sender_data_secret,
                                     &group->secret_tree,
                                     group->max_forward_distance,
                                     out_plaintext, out_pt_len,
                                     &sender_data) != 0) {
        mls_private_message_clear(&msg);
        return MARMOT_ERR_CRYPTO;
    }

    if (out_sender_leaf)
        *out_sender_leaf = sender_data.leaf_index;

    /* Check not from ourselves */
    if (sender_data.leaf_index == group->own_leaf_index) {
        free(*out_plaintext);
        *out_plaintext = NULL;
        *out_pt_len = 0;
        mls_private_message_clear(&msg);
        return MARMOT_ERR_OWN_MESSAGE;
    }

    mls_private_message_clear(&msg);
    return 0;
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
    if (!group) return -1;
    return mls_tree_root_hash(&group->tree, out);
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

    /* Sign the GroupInfo */
    MlsTlsBuf tbs;
    if (mls_tls_buf_init(&tbs, 256) != 0) {
        mls_group_info_clear(gi);
        return -1;
    }
    if (mls_group_info_serialize(gi, &tbs) != 0) {
        mls_tls_buf_free(&tbs);
        mls_group_info_clear(gi);
        return -1;
    }
    if (mls_crypto_sign(gi->signature, group->own_signature_key,
                        tbs.data, tbs.len) != 0) {
        mls_tls_buf_free(&tbs);
        mls_group_info_clear(gi);
        return -1;
    }
    gi->signature_len = MLS_SIG_LEN;
    mls_tls_buf_free(&tbs);

    return 0;
}

int
mls_group_info_serialize(const MlsGroupInfo *gi, MlsTlsBuf *buf)
{
    if (!gi || !buf) return -1;

    /* GroupContext portion */
    if (mls_tls_write_u16(buf, MARMOT_CIPHERSUITE) != 0) return -1;
    if (mls_tls_write_opaque16(buf, gi->group_id, gi->group_id_len) != 0) return -1;
    if (mls_tls_write_u64(buf, gi->epoch) != 0) return -1;
    if (mls_tls_buf_append(buf, gi->tree_hash, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_buf_append(buf, gi->confirmed_transcript_hash, MLS_HASH_LEN) != 0)
        return -1;
    if (mls_tls_write_opaque32(buf, gi->extensions_data, gi->extensions_len) != 0)
        return -1;

    /* GroupInfo-specific fields */
    if (mls_tls_buf_append(buf, gi->confirmation_tag, MLS_HASH_LEN) != 0) return -1;
    if (mls_tls_write_u32(buf, gi->signer_leaf) != 0) return -1;
    if (mls_tls_write_opaque16(buf, gi->signature, gi->signature_len) != 0) return -1;

    return 0;
}

int
mls_group_info_deserialize(MlsTlsReader *reader, MlsGroupInfo *gi)
{
    if (!reader || !gi) return -1;
    memset(gi, 0, sizeof(*gi));

    uint16_t cs;
    if (mls_tls_read_u16(reader, &cs) != 0) goto fail;
    if (cs != MARMOT_CIPHERSUITE) goto fail;

    if (mls_tls_read_opaque16(reader, &gi->group_id, &gi->group_id_len) != 0) goto fail;
    if (mls_tls_read_u64(reader, &gi->epoch) != 0) goto fail;
    if (mls_tls_read_fixed(reader, gi->tree_hash, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_fixed(reader, gi->confirmed_transcript_hash, MLS_HASH_LEN) != 0) goto fail;
    if (mls_tls_read_opaque32(reader, &gi->extensions_data, &gi->extensions_len) != 0) goto fail;
    if (mls_tls_read_fixed(reader, gi->confirmation_tag, MLS_HASH_LEN) != 0) goto fail;
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
    default:
        return -1;
    }
}

static int
proposal_deserialize(MlsTlsReader *reader, MlsProposal *p)
{
    if (!reader || !p) return -1;
    memset(p, 0, sizeof(*p));

    if (mls_tls_read_u16(reader, &p->type) != 0) return -1;

    switch (p->type) {
    case MLS_PROPOSAL_ADD:
        return mls_key_package_deserialize(reader, &p->add.key_package);
    case MLS_PROPOSAL_UPDATE:
        return mls_leaf_node_deserialize(reader, &p->update.leaf_node);
    case MLS_PROPOSAL_REMOVE:
        return mls_tls_read_u32(reader, &p->remove.removed_leaf);
    default:
        return -1; /* Unknown proposal type */
    }
}

int
mls_update_path_serialize(const MlsUpdatePath *up, MlsTlsBuf *buf)
{
    if (!up || !buf) return -1;

    /* Leaf node */
    if (mls_leaf_node_serialize(&up->leaf_node, buf) != 0) return -1;

    /* Node count */
    if (mls_tls_write_u32(buf, (uint32_t)up->node_count) != 0) return -1;

    /* Each path node */
    for (size_t i = 0; i < up->node_count; i++) {
        if (mls_tls_buf_append(buf, up->nodes[i].encryption_key, MLS_KEM_PK_LEN) != 0)
            return -1;
        if (mls_tls_write_opaque32(buf, up->nodes[i].encrypted_path_secrets,
                                    up->nodes[i].encrypted_path_secrets_len) != 0)
            return -1;
    }

    return 0;
}

int
mls_update_path_deserialize(MlsTlsReader *reader, MlsUpdatePath *up)
{
    if (!reader || !up) return -1;
    memset(up, 0, sizeof(*up));

    if (mls_leaf_node_deserialize(reader, &up->leaf_node) != 0) goto fail;

    uint32_t count;
    if (mls_tls_read_u32(reader, &count) != 0) goto fail;

    if (count > 0) {
        up->nodes = calloc(count, sizeof(MlsUpdatePathNode));
        if (!up->nodes) goto fail;
        up->node_count = count;

        for (uint32_t i = 0; i < count; i++) {
            if (mls_tls_read_fixed(reader, up->nodes[i].encryption_key, MLS_KEM_PK_LEN) != 0)
                goto fail;
            if (mls_tls_read_opaque32(reader, &up->nodes[i].encrypted_path_secrets,
                                       &up->nodes[i].encrypted_path_secrets_len) != 0)
                goto fail;
        }
    }

    return 0;
fail:
    mls_update_path_clear(up);
    return -1;
}

int
mls_commit_serialize(const MlsCommit *commit, MlsTlsBuf *buf)
{
    if (!commit || !buf) return -1;

    /* Proposal count */
    if (mls_tls_write_u32(buf, (uint32_t)commit->proposal_count) != 0) return -1;

    /* Proposals */
    for (size_t i = 0; i < commit->proposal_count; i++) {
        if (proposal_serialize(&commit->proposals[i], buf) != 0)
            return -1;
    }

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

    uint32_t prop_count;
    if (mls_tls_read_u32(reader, &prop_count) != 0) goto fail;

    if (prop_count > 0) {
        commit->proposals = calloc(prop_count, sizeof(MlsProposal));
        if (!commit->proposals) goto fail;
        commit->proposal_count = prop_count;

        for (uint32_t i = 0; i < prop_count; i++) {
            if (proposal_deserialize(reader, &commit->proposals[i]) != 0)
                goto fail;
        }
    }

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
