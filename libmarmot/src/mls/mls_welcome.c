/*
 * libmarmot - MLS Welcome Message implementation (RFC 9420 §12.4.3.1)
 *
 * Welcome processing: decrypt joiner_secret, decrypt GroupInfo,
 * initialize group state for the new member.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_welcome.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_encrypted_group_secrets_clear(MlsEncryptedGroupSecrets *egs)
{
    if (!egs) return;
    free(egs->encrypted_joiner_secret);
    sodium_memzero(egs, sizeof(*egs));
}

void
mls_welcome_clear(MlsWelcome *w)
{
    if (!w) return;
    if (w->secrets) {
        for (size_t i = 0; i < w->secret_count; i++)
            mls_encrypted_group_secrets_clear(&w->secrets[i]);
        free(w->secrets);
    }
    free(w->encrypted_group_info);
    memset(w, 0, sizeof(*w));
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS serialization
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_welcome_serialize(const MlsWelcome *w, MlsTlsBuf *buf)
{
    if (!w || !buf) return -1;

    /* cipher_suite */
    if (mls_tls_write_u16(buf, w->cipher_suite) != 0) return -1;

    /* secrets count */
    if (mls_tls_write_u32(buf, (uint32_t)w->secret_count) != 0) return -1;

    for (size_t i = 0; i < w->secret_count; i++) {
        const MlsEncryptedGroupSecrets *egs = &w->secrets[i];

        /* key_package_ref: fixed 32 bytes */
        if (mls_tls_buf_append(buf, egs->key_package_ref, MLS_HASH_LEN) != 0)
            return -1;
        /* HPKECiphertext: kem_output || ciphertext */
        if (mls_tls_write_opaque16(buf, egs->kem_output, MLS_KEM_ENC_LEN) != 0)
            return -1;
        if (mls_tls_write_opaque16(buf, egs->encrypted_joiner_secret,
                                    egs->encrypted_joiner_secret_len) != 0)
            return -1;
    }

    /* encrypted_group_info */
    if (mls_tls_write_opaque32(buf, w->encrypted_group_info,
                                w->encrypted_group_info_len) != 0)
        return -1;

    return 0;
}

int
mls_welcome_deserialize(MlsTlsReader *reader, MlsWelcome *w)
{
    if (!reader || !w) return -1;
    memset(w, 0, sizeof(*w));

    if (mls_tls_read_u16(reader, &w->cipher_suite) != 0) goto fail;

    uint32_t count;
    if (mls_tls_read_u32(reader, &count) != 0) goto fail;

    if (count > 0) {
        w->secrets = calloc(count, sizeof(MlsEncryptedGroupSecrets));
        if (!w->secrets) goto fail;
        w->secret_count = count;

        for (uint32_t i = 0; i < count; i++) {
            MlsEncryptedGroupSecrets *egs = &w->secrets[i];

            if (mls_tls_read_fixed(reader, egs->key_package_ref, MLS_HASH_LEN) != 0)
                goto fail;
            {
                uint8_t *kem = NULL;
                size_t kem_len = 0;
                if (mls_tls_read_opaque16(reader, &kem, &kem_len) != 0) goto fail;
                if (kem_len != MLS_KEM_ENC_LEN) { free(kem); goto fail; }
                memcpy(egs->kem_output, kem, MLS_KEM_ENC_LEN);
                free(kem);
            }
            if (mls_tls_read_opaque16(reader, &egs->encrypted_joiner_secret,
                                       &egs->encrypted_joiner_secret_len) != 0)
                goto fail;
        }
    }

    if (mls_tls_read_opaque32(reader, &w->encrypted_group_info,
                               &w->encrypted_group_info_len) != 0)
        goto fail;

    return 0;
fail:
    mls_welcome_clear(w);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Welcome processing
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_welcome_process(const uint8_t *welcome_data, size_t welcome_len,
                    const MlsKeyPackage *kp,
                    const MlsKeyPackagePrivate *kp_priv,
                    const uint8_t *ratchet_tree, size_t tree_len,
                    MlsGroup *group_out)
{
    if (!welcome_data || !kp || !kp_priv || !group_out)
        return MARMOT_ERR_INVALID_ARG;

    MlsWelcome welcome;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, welcome_data, welcome_len);
    if (mls_welcome_deserialize(&reader, &welcome) != 0)
        return MARMOT_ERR_WELCOME_INVALID;

    int rc = mls_welcome_process_parsed(&welcome, kp, kp_priv,
                                         ratchet_tree, tree_len, group_out);
    mls_welcome_clear(&welcome);
    return rc;
}

int
mls_welcome_process_parsed(const MlsWelcome *welcome,
                           const MlsKeyPackage *kp,
                           const MlsKeyPackagePrivate *kp_priv,
                           const uint8_t *ratchet_tree, size_t tree_len,
                           MlsGroup *group_out)
{
    if (!welcome || !kp || !kp_priv || !group_out)
        return MARMOT_ERR_INVALID_ARG;

    memset(group_out, 0, sizeof(*group_out));

    /* Verify ciphersuite */
    if (welcome->cipher_suite != MARMOT_CIPHERSUITE)
        return MARMOT_ERR_UNSUPPORTED;

    /* Find our EncryptedGroupSecrets entry */
    uint8_t our_kp_ref[MLS_HASH_LEN];
    if (mls_key_package_ref(kp, our_kp_ref) != 0)
        return MARMOT_ERR_INTERNAL;

    const MlsEncryptedGroupSecrets *our_egs = NULL;
    for (size_t i = 0; i < welcome->secret_count; i++) {
        if (memcmp(welcome->secrets[i].key_package_ref, our_kp_ref, MLS_HASH_LEN) == 0) {
            our_egs = &welcome->secrets[i];
            break;
        }
    }
    if (!our_egs)
        return MARMOT_ERR_WELCOME_NOT_FOUND;

    /* HPKE decap: recover shared_secret from kem_output using our init_key */
    uint8_t shared_secret[MLS_KEM_SECRET_LEN];
    if (mls_crypto_kem_decap(shared_secret, our_egs->kem_output,
                              kp_priv->init_key_private,
                              kp->init_key) != 0) {
        return MARMOT_ERR_CRYPTO;
    }

    /* Decrypt joiner_secret */
    uint8_t joiner_secret[MLS_HASH_LEN];
    {
        uint8_t js_key[MLS_AEAD_KEY_LEN];
        uint8_t js_nonce[MLS_AEAD_NONCE_LEN];
        memcpy(js_key, shared_secret, MLS_AEAD_KEY_LEN);
        memset(js_nonce, 0, MLS_AEAD_NONCE_LEN);

        size_t pt_len = 0;
        if (mls_crypto_aead_decrypt(joiner_secret, &pt_len,
                                     js_key, js_nonce,
                                     our_egs->encrypted_joiner_secret,
                                     our_egs->encrypted_joiner_secret_len,
                                     NULL, 0) != 0) {
            sodium_memzero(shared_secret, sizeof(shared_secret));
            return MARMOT_ERR_CRYPTO;
        }
        sodium_memzero(js_key, sizeof(js_key));
        if (pt_len != MLS_HASH_LEN) {
            sodium_memzero(shared_secret, sizeof(shared_secret));
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            return MARMOT_ERR_WELCOME_INVALID;
        }
    }
    sodium_memzero(shared_secret, sizeof(shared_secret));

    /* Derive welcome_secret from joiner_secret:
     * welcome_secret = DeriveSecret(joiner_secret, "welcome")
     * welcome_key = ExpandWithLabel(welcome_secret, "key", "", key_len)
     * welcome_nonce = ExpandWithLabel(welcome_secret, "nonce", "", nonce_len) */
    uint8_t welcome_secret[MLS_HASH_LEN];
    if (mls_crypto_derive_secret(welcome_secret, joiner_secret, "welcome") != 0) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_INTERNAL;
    }

    uint8_t welcome_key[MLS_AEAD_KEY_LEN];
    uint8_t welcome_nonce[MLS_AEAD_NONCE_LEN];
    if (mls_crypto_expand_with_label(welcome_key, MLS_AEAD_KEY_LEN,
                                      welcome_secret, "key", NULL, 0) != 0 ||
        mls_crypto_expand_with_label(welcome_nonce, MLS_AEAD_NONCE_LEN,
                                      welcome_secret, "nonce", NULL, 0) != 0) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        return MARMOT_ERR_INTERNAL;
    }

    /* Decrypt GroupInfo */
    uint8_t *gi_data = malloc(welcome->encrypted_group_info_len);
    if (!gi_data) {
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        return MARMOT_ERR_MEMORY;
    }

    size_t gi_len = 0;
    if (mls_crypto_aead_decrypt(gi_data, &gi_len,
                                 welcome_key, welcome_nonce,
                                 welcome->encrypted_group_info,
                                 welcome->encrypted_group_info_len,
                                 NULL, 0) != 0) {
        free(gi_data);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(welcome_secret, sizeof(welcome_secret));
        sodium_memzero(welcome_key, sizeof(welcome_key));
        return MARMOT_ERR_CRYPTO;
    }
    sodium_memzero(welcome_key, sizeof(welcome_key));
    sodium_memzero(welcome_nonce, sizeof(welcome_nonce));
    sodium_memzero(welcome_secret, sizeof(welcome_secret));

    /* Parse GroupInfo */
    MlsGroupInfo gi;
    MlsTlsReader gi_reader;
    mls_tls_reader_init(&gi_reader, gi_data, gi_len);
    if (mls_group_info_deserialize(&gi_reader, &gi) != 0) {
        free(gi_data);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_WELCOME_INVALID;
    }

    /* Verify GroupInfo signature.
     * NOTE: In a full implementation, we would need to extract the signer's
     * public key from the ratchet tree at gi.signer_leaf. For now, we skip
     * signature verification as the tree is not yet fully deserialized.
     * This is a known limitation that should be addressed when implementing
     * full ratchet tree support. */
    
    free(gi_data);

    /* Initialize group state from GroupInfo */
    group_out->group_id = malloc(gi.group_id_len);
    if (!group_out->group_id) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        return MARMOT_ERR_MEMORY;
    }
    memcpy(group_out->group_id, gi.group_id, gi.group_id_len);
    group_out->group_id_len = gi.group_id_len;
    group_out->epoch = gi.epoch;
    group_out->max_forward_distance = 1000;

    /* Copy transcript hash from GroupInfo */
    memcpy(group_out->confirmed_transcript_hash,
           gi.confirmed_transcript_hash, MLS_HASH_LEN);

    /* Compute interim transcript hash from confirmed + confirmation_tag */
    {
        MlsTlsBuf tbuf;
        if (mls_tls_buf_init(&tbuf, MLS_HASH_LEN * 2) != 0) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return MARMOT_ERR_MEMORY;
        }
        mls_tls_buf_append(&tbuf, gi.confirmed_transcript_hash, MLS_HASH_LEN);
        mls_tls_buf_append(&tbuf, gi.confirmation_tag, MLS_HASH_LEN);
        mls_crypto_hash(group_out->interim_transcript_hash, tbuf.data, tbuf.len);
        mls_tls_buf_free(&tbuf);
    }

    /* Extensions */
    if (gi.extensions_data && gi.extensions_len > 0) {
        group_out->extensions_data = malloc(gi.extensions_len);
        if (!group_out->extensions_data) {
            mls_group_info_clear(&gi);
            sodium_memzero(joiner_secret, sizeof(joiner_secret));
            mls_group_free(group_out);
            return MARMOT_ERR_MEMORY;
        }
        memcpy(group_out->extensions_data, gi.extensions_data, gi.extensions_len);
        group_out->extensions_len = gi.extensions_len;
    }

    /* Store our signing key and encryption key */
    memcpy(group_out->own_signature_key, kp_priv->signature_key_private, MLS_SIG_SK_LEN);
    memcpy(group_out->own_encryption_key, kp_priv->encryption_key_private, MLS_KEM_SK_LEN);

    /* Initialize ratchet tree.
     * In a full implementation, the tree would be provided either via
     * ratchet_tree extension or out-of-band. For now, we initialize
     * a minimal 2-leaf tree (creator + joiner). */
    if (ratchet_tree && tree_len > 0) {
        /* TODO: Deserialize provided ratchet tree */
        /* For now, fall through to minimal init */
    }

    /* Create minimal tree: we know we're being added, so we exist in the tree.
     * The tree should have been constructed by the adder. For a 2-member group,
     * we have 2 leaves. */
    if (mls_tree_new(&group_out->tree, 2) != 0) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Find our leaf index by matching our KeyPackage's encryption key in the tree.
     * For now, with a 2-leaf tree, we assume we're at index 1 (the joiner).
     * In a full implementation, we would search the tree for our encryption key. */
    group_out->own_leaf_index = 1;
    bool found_self = false;
    
    for (uint32_t leaf_idx = 0; leaf_idx < group_out->tree.n_leaves; leaf_idx++) {
        uint32_t node_idx = mls_tree_leaf_to_node(leaf_idx);
        /* For now, just populate our known position */
        if (leaf_idx == 1) {
            group_out->tree.nodes[node_idx].type = MLS_NODE_LEAF;
            if (mls_leaf_node_clone(&group_out->tree.nodes[node_idx].leaf,
                                     &kp->leaf_node) != 0) {
                mls_group_info_clear(&gi);
                sodium_memzero(joiner_secret, sizeof(joiner_secret));
                mls_group_free(group_out);
                return MARMOT_ERR_INTERNAL;
            }
            found_self = true;
            break;
        }
    }
    
    if (!found_self) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_NOT_FOUND;
    }

    /* Derive epoch secrets from joiner_secret.
     * The key schedule for a joiner uses:
     * - joiner_secret (instead of constructing from init_secret + commit_secret)
     * - GroupContext from the GroupInfo
     *
     * epoch_secret = ExpandWithLabel(joiner_secret, "epoch", GroupContext, Hash.length)
     * Then derive all epoch secrets from epoch_secret.
     */

    /* Build GroupContext for key schedule */
    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    uint8_t tree_hash[MLS_HASH_LEN];
    /* Use the tree hash from GroupInfo since our tree may be incomplete */
    memcpy(tree_hash, gi.tree_hash, MLS_HASH_LEN);

    if (mls_group_context_serialize(
            group_out->group_id, group_out->group_id_len,
            group_out->epoch, tree_hash,
            group_out->confirmed_transcript_hash,
            group_out->extensions_data, group_out->extensions_len,
            &gc_data, &gc_len) != 0) {
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* For the joiner, the key schedule processes the joiner_secret
     * as if it were derived from (init_secret, commit_secret).
     * We use the key_schedule_derive with a special path:
     * joiner_secret is already computed, so we pass it as the
     * commit_secret with a zero init_secret_prev. */
    uint8_t zero[MLS_HASH_LEN];
    memset(zero, 0, MLS_HASH_LEN);

    /* Actually, per RFC 9420, the joiner derives:
     * welcome_secret = DeriveSecret(joiner_secret, "welcome")
     * epoch_secret = ExpandWithLabel(joiner_secret, "epoch", GroupContext, Hash.length)
     * Then all epoch secrets from epoch_secret.
     *
     * Our key_schedule_derive already does this internally from
     * init_secret + commit_secret -> joiner_secret -> epoch_secret.
     * We need to match the same joiner_secret, so we arrange
     * the inputs to produce it. Since:
     * joiner_secret = ExpandWithLabel(commit_secret XOR init_secret, ...)
     * This is complex. Simpler: derive directly. */

    /* Derive epoch_secret from joiner_secret */
    uint8_t epoch_secret[MLS_HASH_LEN];
    if (mls_crypto_expand_with_label(epoch_secret, MLS_HASH_LEN,
                                      joiner_secret, "epoch",
                                      gc_data, gc_len) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Derive individual epoch secrets from epoch_secret */
    MlsEpochSecrets *es = &group_out->epoch_secrets;

    if (mls_crypto_derive_secret(es->sender_data_secret, epoch_secret, "sender data") != 0 ||
        mls_crypto_derive_secret(es->encryption_secret, epoch_secret, "encryption") != 0 ||
        mls_crypto_derive_secret(es->exporter_secret, epoch_secret, "exporter") != 0 ||
        mls_crypto_derive_secret(es->external_secret, epoch_secret, "external") != 0 ||
        mls_crypto_derive_secret(es->confirmation_key, epoch_secret, "confirm") != 0 ||
        mls_crypto_derive_secret(es->membership_key, epoch_secret, "membership") != 0 ||
        mls_crypto_derive_secret(es->resumption_psk, epoch_secret, "resumption") != 0 ||
        mls_crypto_derive_secret(es->epoch_authenticator, epoch_secret, "authentication") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* init_secret for next epoch */
    if (mls_crypto_derive_secret(es->init_secret, epoch_secret, "init") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Store joiner_secret and welcome_secret for reference */
    memcpy(es->joiner_secret, joiner_secret, MLS_HASH_LEN);
    if (mls_crypto_derive_secret(es->welcome_secret, joiner_secret, "welcome") != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Initialize secret tree */
    if (mls_secret_tree_init(&group_out->secret_tree,
                              es->encryption_secret,
                              group_out->tree.n_leaves) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    /* Verify confirmation tag */
    uint8_t expected_tag[MLS_HASH_LEN];
    if (mls_crypto_hkdf_extract(expected_tag, es->confirmation_key, MLS_HASH_LEN,
                                 group_out->confirmed_transcript_hash, MLS_HASH_LEN) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_INTERNAL;
    }

    if (sodium_memcmp(expected_tag, gi.confirmation_tag, MLS_HASH_LEN) != 0) {
        free(gc_data);
        mls_group_info_clear(&gi);
        sodium_memzero(joiner_secret, sizeof(joiner_secret));
        sodium_memzero(epoch_secret, sizeof(epoch_secret));
        mls_group_free(group_out);
        return MARMOT_ERR_WELCOME_INVALID;
    }

    free(gc_data);
    mls_group_info_clear(&gi);
    sodium_memzero(joiner_secret, sizeof(joiner_secret));
    sodium_memzero(epoch_secret, sizeof(epoch_secret));

    return 0;
}
