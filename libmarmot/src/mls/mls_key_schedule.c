/*
 * libmarmot - MLS Key Schedule implementation (RFC 9420 §8, §9)
 *
 * Derives epoch secrets from:
 *   init_secret[n-1] + commit_secret --> joiner_secret --> epoch_secret
 *       --> sender_data, encryption, exporter, external, confirmation,
 *           membership, resumption, epoch_authenticator, init[n]
 *
 * Secret tree: derives per-sender message keys from encryption_secret.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_key_schedule.h"
#include "mls_tree.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Key Schedule (RFC 9420 §8)
 *
 * init_secret[n-1]
 *      |
 *      V
 * commit_secret --> KDF.Extract
 *      |
 *      V
 * ExpandWithLabel(., "joiner", GroupContext[n], KDF.Nh)
 *      |
 *      V
 * joiner_secret
 *      |
 *      V
 * psk_secret (or 0) --> KDF.Extract
 *      |
 *      +-> DeriveSecret(., "welcome") = welcome_secret
 *      |
 *      V
 * ExpandWithLabel(., "epoch", GroupContext[n], KDF.Nh)
 *      |
 *      V
 * epoch_secret
 *      |
 *      +-> DeriveSecret(., "sender data")    = sender_data_secret
 *      +-> DeriveSecret(., "encryption")     = encryption_secret
 *      +-> DeriveSecret(., "exporter")       = exporter_secret
 *      +-> DeriveSecret(., "external")       = external_secret
 *      +-> DeriveSecret(., "confirm")        = confirmation_key
 *      +-> DeriveSecret(., "membership")     = membership_key
 *      +-> DeriveSecret(., "resumption")     = resumption_psk
 *      +-> DeriveSecret(., "authentication") = epoch_authenticator
 *      |
 *      V
 * DeriveSecret(., "init") = init_secret[n]
 *
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_key_schedule_derive(const uint8_t *init_secret_prev,
                         const uint8_t commit_secret[MLS_HASH_LEN],
                         const uint8_t *group_context, size_t group_context_len,
                         const uint8_t *psk_secret,
                         MlsEpochSecrets *out)
{
    if (!commit_secret || !group_context || !out) return -1;
    memset(out, 0, sizeof(*out));

    uint8_t zero[MLS_HASH_LEN] = {0};
    uint8_t extracted[MLS_HASH_LEN] = {0};
    uint8_t member_secret[MLS_HASH_LEN] = {0};
    uint8_t epoch_secret[MLS_HASH_LEN] = {0};
    int rc = -1;

    /* Use zero init_secret for epoch 0 if not provided */
    const uint8_t *init_prev = init_secret_prev ? init_secret_prev : zero;

    /* Step 1: KDF.Extract(init_secret_prev, commit_secret)
     * salt = init_secret_prev, ikm = commit_secret */
    if (mls_crypto_hkdf_extract(extracted, init_prev, MLS_HASH_LEN,
                                 commit_secret, MLS_HASH_LEN) != 0)
        goto cleanup;

    /* Step 2: joiner_secret = ExpandWithLabel(extracted, "joiner", GroupContext, Nh) */
    if (mls_crypto_expand_with_label(out->joiner_secret, MLS_HASH_LEN,
                                      extracted, "joiner",
                                      group_context, group_context_len) != 0)
        goto cleanup;

    /* Step 3: KDF.Extract(psk_secret || 0, joiner_secret)
     * salt = joiner_secret, ikm = psk_secret */
    const uint8_t *psk = psk_secret ? psk_secret : zero;
    if (mls_crypto_hkdf_extract(member_secret, out->joiner_secret, MLS_HASH_LEN,
                                 psk, MLS_HASH_LEN) != 0)
        goto cleanup;

    /* Step 3a: welcome_secret = DeriveSecret(member_secret, "welcome") */
    if (mls_crypto_derive_secret(out->welcome_secret, member_secret, "welcome") != 0)
        goto cleanup;

    /* Step 4: epoch_secret = ExpandWithLabel(member_secret, "epoch", GroupContext, Nh) */
    if (mls_crypto_expand_with_label(epoch_secret, MLS_HASH_LEN,
                                      member_secret, "epoch",
                                      group_context, group_context_len) != 0)
        goto cleanup;

    /* Step 5: Derive all epoch secrets */
    if (mls_crypto_derive_secret(out->sender_data_secret, epoch_secret, "sender data") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->encryption_secret, epoch_secret, "encryption") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->exporter_secret, epoch_secret, "exporter") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->external_secret, epoch_secret, "external") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->confirmation_key, epoch_secret, "confirm") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->membership_key, epoch_secret, "membership") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->resumption_psk, epoch_secret, "resumption") != 0)
        goto cleanup;
    if (mls_crypto_derive_secret(out->epoch_authenticator, epoch_secret, "authentication") != 0)
        goto cleanup;

    /* Step 6: init_secret[n] = DeriveSecret(epoch_secret, "init") */
    if (mls_crypto_derive_secret(out->init_secret, epoch_secret, "init") != 0)
        goto cleanup;

    rc = 0;

cleanup:
    /* Always zero intermediate secrets on all exit paths */
    sodium_memzero(extracted, sizeof(extracted));
    sodium_memzero(member_secret, sizeof(member_secret));
    sodium_memzero(epoch_secret, sizeof(epoch_secret));
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Secret Tree (RFC 9420 §9)
 *
 * The tree has the same shape as the ratchet tree. The root holds the
 * encryption_secret. Left children derive with "left", right with "right".
 *
 * Leaf secrets split into handshake and application ratchets.
 * Each ratchet generates (key, nonce, secret) triplets per generation.
 *
 *          encryption_secret
 *             /       \
 *          left       right
 *         /    \     /    \
 *        ...   ... ...    ...
 *        |                  |
 *   leaf_secret[0]     leaf_secret[n-1]
 *     /      \
 *  handshake  application
 * ══════════════════════════════════════════════════════════════════════════ */

/** Derive left child secret: ExpandWithLabel(secret, "tree", "left", Nh) */
static int
derive_tree_left(uint8_t out[MLS_HASH_LEN], const uint8_t secret[MLS_HASH_LEN])
{
    const uint8_t ctx[] = "left";
    return mls_crypto_expand_with_label(out, MLS_HASH_LEN, secret, "tree",
                                         ctx, sizeof(ctx) - 1);
}

/** Derive right child secret: ExpandWithLabel(secret, "tree", "right", Nh) */
static int
derive_tree_right(uint8_t out[MLS_HASH_LEN], const uint8_t secret[MLS_HASH_LEN])
{
    const uint8_t ctx[] = "right";
    return mls_crypto_expand_with_label(out, MLS_HASH_LEN, secret, "tree",
                                         ctx, sizeof(ctx) - 1);
}

/** Recursively populate the secret tree from a node downward. */
static int
populate_tree(uint8_t (*secrets)[MLS_HASH_LEN], uint32_t node_idx,
              uint32_t n_leaves, const uint8_t parent_secret[MLS_HASH_LEN])
{
    uint32_t n_nodes = mls_tree_node_width(n_leaves);
    if (node_idx >= n_nodes) return 0;

    memcpy(secrets[node_idx], parent_secret, MLS_HASH_LEN);

    if (!mls_tree_is_leaf(node_idx)) {
        uint32_t l = mls_tree_left(node_idx);
        uint32_t r = mls_tree_right(node_idx);

        uint8_t left_secret[MLS_HASH_LEN], right_secret[MLS_HASH_LEN];
        if (derive_tree_left(left_secret, parent_secret) != 0) return -1;
        if (derive_tree_right(right_secret, parent_secret) != 0) return -1;

        if (populate_tree(secrets, l, n_leaves, left_secret) != 0) return -1;
        if (populate_tree(secrets, r, n_leaves, right_secret) != 0) return -1;

        sodium_memzero(left_secret, sizeof(left_secret));
        sodium_memzero(right_secret, sizeof(right_secret));
    }
    return 0;
}

int
mls_secret_tree_init(MlsSecretTree *st,
                      const uint8_t encryption_secret[MLS_HASH_LEN],
                      uint32_t n_leaves)
{
    if (!st || !encryption_secret || n_leaves == 0) return -1;
    memset(st, 0, sizeof(*st));

    st->n_leaves = n_leaves;
    uint32_t n_nodes = mls_tree_node_width(n_leaves);

    st->tree_secrets = calloc(n_nodes, MLS_HASH_LEN);
    st->senders = calloc(n_leaves, sizeof(MlsSenderRatchet));
    st->sender_initialized = calloc(n_leaves, sizeof(bool));
    if (!st->tree_secrets || !st->senders || !st->sender_initialized) {
        mls_secret_tree_free(st);
        return -1;
    }

    /* Populate the tree from the root */
    uint32_t root = mls_tree_root(n_leaves);
    if (populate_tree(st->tree_secrets, root, n_leaves, encryption_secret) != 0) {
        mls_secret_tree_free(st);
        return -1;
    }
    return 0;
}

void
mls_secret_tree_free(MlsSecretTree *st)
{
    if (!st) return;
    if (st->tree_secrets) {
        uint32_t n_nodes = mls_tree_node_width(st->n_leaves);
        sodium_memzero(st->tree_secrets, n_nodes * MLS_HASH_LEN);
        free(st->tree_secrets);
    }
    if (st->senders) {
        sodium_memzero(st->senders, st->n_leaves * sizeof(MlsSenderRatchet));
        free(st->senders);
    }
    free(st->sender_initialized);
    memset(st, 0, sizeof(*st));
}

/**
 * Initialize a sender's ratchet from the leaf secret.
 *
 * handshake_secret = ExpandWithLabel(leaf_secret, "handshake", "", Nh)
 * application_secret = ExpandWithLabel(leaf_secret, "application", "", Nh)
 *
 * NOTE: This function is NOT thread-safe. The secret tree must be protected
 * by external synchronization if used in a multi-threaded environment.
 */
static int
init_sender_ratchet(MlsSecretTree *st, uint32_t leaf_index)
{
    if (leaf_index >= st->n_leaves) return -1;
    /* Check-then-act pattern - not thread-safe without external locking */
    if (st->sender_initialized[leaf_index]) return 0;

    uint32_t node_idx = mls_tree_leaf_to_node(leaf_index);
    const uint8_t *leaf_secret = st->tree_secrets[node_idx];

    MlsSenderRatchet *ratchet = &st->senders[leaf_index];
    if (mls_crypto_expand_with_label(ratchet->handshake_secret, MLS_HASH_LEN,
                                      leaf_secret, "handshake", NULL, 0) != 0)
        return -1;
    if (mls_crypto_expand_with_label(ratchet->application_secret, MLS_HASH_LEN,
                                      leaf_secret, "application", NULL, 0) != 0)
        return -1;
    ratchet->handshake_generation = 0;
    ratchet->application_generation = 0;
    st->sender_initialized[leaf_index] = true;
    return 0;
}

/**
 * Derive message keys from a ratchet secret and advance the ratchet.
 *
 * key = ExpandWithLabel(secret[n], "key", "", key_length)
 * nonce = ExpandWithLabel(secret[n], "nonce", "", nonce_length)
 * secret[n+1] = ExpandWithLabel(secret[n], "secret", "", Nh)
 */
static int
ratchet_derive_keys(uint8_t secret[MLS_HASH_LEN], uint32_t *generation,
                    MlsMessageKeys *out)
{
    out->generation = *generation;

    /* Derive key */
    if (mls_crypto_expand_with_label(out->key, MLS_AEAD_KEY_LEN,
                                      secret, "key", NULL, 0) != 0)
        return -1;
    /* Derive nonce */
    if (mls_crypto_expand_with_label(out->nonce, MLS_AEAD_NONCE_LEN,
                                      secret, "nonce", NULL, 0) != 0)
        return -1;

    /* Advance ratchet: secret[n+1] = ExpandWithLabel(secret[n], "secret", "", Nh) */
    uint8_t next_secret[MLS_HASH_LEN];
    if (mls_crypto_expand_with_label(next_secret, MLS_HASH_LEN,
                                      secret, "secret", NULL, 0) != 0)
        return -1;

    memcpy(secret, next_secret, MLS_HASH_LEN);
    sodium_memzero(next_secret, sizeof(next_secret));
    (*generation)++;

    return 0;
}

int
mls_secret_tree_derive_keys(MlsSecretTree *st, uint32_t leaf_index,
                             bool is_handshake, MlsMessageKeys *out)
{
    if (!st || !out) return -1;
    if (leaf_index >= st->n_leaves) return -1;

    if (init_sender_ratchet(st, leaf_index) != 0) return -1;

    MlsSenderRatchet *ratchet = &st->senders[leaf_index];
    if (is_handshake) {
        return ratchet_derive_keys(ratchet->handshake_secret,
                                   &ratchet->handshake_generation, out);
    } else {
        return ratchet_derive_keys(ratchet->application_secret,
                                   &ratchet->application_generation, out);
    }
}

int
mls_secret_tree_get_keys_for_generation(MlsSecretTree *st, uint32_t leaf_index,
                                         bool is_handshake, uint32_t generation,
                                         uint32_t max_forward_distance,
                                         MlsMessageKeys *out)
{
    if (!st || !out) return -1;
    if (leaf_index >= st->n_leaves) return -1;

    if (init_sender_ratchet(st, leaf_index) != 0) return -1;

    MlsSenderRatchet *ratchet = &st->senders[leaf_index];
    uint8_t *secret = is_handshake
                       ? ratchet->handshake_secret
                       : ratchet->application_secret;
    uint32_t *gen = is_handshake
                     ? &ratchet->handshake_generation
                     : &ratchet->application_generation;

    /* Check that the requested generation isn't too far in the past */
    if (generation < *gen) {
        /* Would need out-of-order handling — not implemented yet.
         * For now, return error for past generations. */
        return MARMOT_ERR_WRONG_EPOCH;
    }

    /* Check forward distance */
    if (generation - *gen > max_forward_distance)
        return MARMOT_ERR_MESSAGE;

    /* Advance ratchet to the requested generation */
    while (*gen < generation) {
        MlsMessageKeys dummy;
        if (ratchet_derive_keys(secret, gen, &dummy) != 0)
            return -1;
        sodium_memzero(&dummy, sizeof(dummy));
    }

    /* Now derive keys at the target generation */
    return ratchet_derive_keys(secret, gen, out);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MLS Exporter (RFC 9420 §8.5)
 *
 * MLS-Exporter(label, context, length) =
 *   ExpandWithLabel(DeriveSecret(exporter_secret, label),
 *                   "exported", Hash(context), length)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_exporter(const uint8_t exporter_secret[MLS_HASH_LEN],
             const char *label,
             const uint8_t *context, size_t context_len,
             uint8_t *out, size_t out_len)
{
    if (!exporter_secret || !out) return -1;

    /* Step 1: derived_secret = DeriveSecret(exporter_secret, label) */
    uint8_t derived[MLS_HASH_LEN];
    if (mls_crypto_derive_secret(derived, exporter_secret, label) != 0)
        return -1;

    /* Step 2: context_hash = Hash(context) */
    uint8_t context_hash[MLS_HASH_LEN];
    if (context && context_len > 0) {
        if (mls_crypto_hash(context_hash, context, context_len) != 0)
            return -1;
    } else {
        /* Hash("") */
        if (mls_crypto_hash(context_hash, (const uint8_t *)"", 0) != 0)
            return -1;
    }

    /* Step 3: ExpandWithLabel(derived_secret, "exported", context_hash, length) */
    int rc = mls_crypto_expand_with_label(out, out_len, derived, "exported",
                                           context_hash, MLS_HASH_LEN);

    sodium_memzero(derived, sizeof(derived));
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GroupContext serialization (RFC 9420 §8.1)
 *
 * struct {
 *   ProtocolVersion version = mls10;    // uint16 = 1
 *   CipherSuite cipher_suite;           // uint16 = 0x0001
 *   opaque group_id<V>;
 *   uint64 epoch;
 *   opaque tree_hash<V>;
 *   opaque confirmed_transcript_hash<V>;
 *   Extension extensions<V>;
 * } GroupContext;
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_group_context_serialize(const uint8_t *group_id, size_t group_id_len,
                             uint64_t epoch,
                             const uint8_t tree_hash[MLS_HASH_LEN],
                             const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                             const uint8_t *extensions_data, size_t extensions_len,
                             uint8_t **out_data, size_t *out_len)
{
    if (!group_id || !tree_hash || !confirmed_transcript_hash || !out_data || !out_len)
        return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) return -1;

    /* version: uint16 = mls10 = 1 */
    if (mls_tls_write_u16(&buf, 1) != 0) goto fail;
    /* cipher_suite: uint16 = 0x0001 */
    if (mls_tls_write_u16(&buf, MARMOT_CIPHERSUITE) != 0) goto fail;
    /* group_id: opaque<V> */
    if (mls_tls_write_opaque8(&buf, group_id, group_id_len) != 0) goto fail;
    /* epoch: uint64 */
    if (mls_tls_write_u64(&buf, epoch) != 0) goto fail;
    /* tree_hash: opaque<V> */
    if (mls_tls_write_opaque8(&buf, tree_hash, MLS_HASH_LEN) != 0) goto fail;
    /* confirmed_transcript_hash: opaque<V> */
    if (mls_tls_write_opaque8(&buf, confirmed_transcript_hash, MLS_HASH_LEN) != 0) goto fail;
    /* extensions: opaque<V> */
    if (extensions_data && extensions_len > 0) {
        if (mls_tls_write_opaque32(&buf, extensions_data, extensions_len) != 0) goto fail;
    } else {
        if (mls_tls_write_opaque32(&buf, NULL, 0) != 0) goto fail;
    }

    *out_data = buf.data;
    *out_len = buf.len;
    /* Don't free buf.data — ownership transferred to caller */
    return 0;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}
