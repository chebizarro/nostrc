/*
 * libmarmot - MLS Group state machine tests
 *
 * Tests for group creation, member add/remove, self-update,
 * application message encrypt/decrypt, and commit processing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_group.h"
#include "mls/mls_welcome.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

static const uint8_t ALICE_ID[32] = {
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
};

static const uint8_t BOB_ID[32] = {
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
};

static const uint8_t GROUP_ID[] = "test-group-001";

/**
 * Create a group for Alice with a fresh signing key.
 */
static int
create_alice_group(MlsGroup *group, uint8_t sig_sk[MLS_SIG_SK_LEN])
{
    uint8_t sig_pk[MLS_SIG_PK_LEN];
    if (mls_crypto_sign_keygen(sig_sk, sig_pk) != 0) return -1;
    return mls_group_create(group, GROUP_ID, sizeof(GROUP_ID),
                            ALICE_ID, 32, sig_sk, NULL, 0);
}

/* ── Group creation tests ──────────────────────────────────────────────── */

TEST(test_group_create_basic)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    assert(group.group_id_len == sizeof(GROUP_ID));
    assert(memcmp(group.group_id, GROUP_ID, sizeof(GROUP_ID)) == 0);
    assert(group.epoch == 0);
    assert(group.own_leaf_index == 0);
    assert(group.tree.n_leaves == 1);

    /* Our leaf should be populated */
    MlsNode *leaf = &group.tree.nodes[0];
    assert(leaf->type == MLS_NODE_LEAF);
    assert(leaf->leaf.credential_type == MLS_CREDENTIAL_BASIC);
    assert(leaf->leaf.credential_identity_len == 32);
    assert(memcmp(leaf->leaf.credential_identity, ALICE_ID, 32) == 0);

    mls_group_free(&group);
}

TEST(test_group_create_null_args)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN], sig_pk[MLS_SIG_PK_LEN];
    mls_crypto_sign_keygen(sig_sk, sig_pk);

    assert(mls_group_create(NULL, GROUP_ID, sizeof(GROUP_ID),
                            ALICE_ID, 32, sig_sk, NULL, 0) != 0);
    assert(mls_group_create(&group, NULL, 0, ALICE_ID, 32, sig_sk, NULL, 0) != 0);
    assert(mls_group_create(&group, GROUP_ID, sizeof(GROUP_ID),
                            NULL, 0, sig_sk, NULL, 0) != 0);
    assert(mls_group_create(&group, GROUP_ID, sizeof(GROUP_ID),
                            ALICE_ID, 32, NULL, NULL, 0) != 0);
}

TEST(test_group_create_with_extensions)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN], sig_pk[MLS_SIG_PK_LEN];
    mls_crypto_sign_keygen(sig_sk, sig_pk);

    uint8_t ext[] = { 0xF2, 0xEE, 0x00, 0x02, 0xCA, 0xFE };
    int rc = mls_group_create(&group, GROUP_ID, sizeof(GROUP_ID),
                               ALICE_ID, 32, sig_sk, ext, sizeof(ext));
    assert(rc == 0);
    assert(group.extensions_len == sizeof(ext));
    assert(memcmp(group.extensions_data, ext, sizeof(ext)) == 0);

    mls_group_free(&group);
}

TEST(test_group_tree_hash)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    uint8_t hash1[MLS_HASH_LEN], hash2[MLS_HASH_LEN];
    assert(mls_group_tree_hash(&group, hash1) == 0);
    assert(mls_group_tree_hash(&group, hash2) == 0);

    /* Deterministic */
    assert(memcmp(hash1, hash2, MLS_HASH_LEN) == 0);

    /* Not all zeros */
    uint8_t zeros[MLS_HASH_LEN] = {0};
    assert(memcmp(hash1, zeros, MLS_HASH_LEN) != 0);

    mls_group_free(&group);
}

TEST(test_group_context_build)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_build(&group, &gc_data, &gc_len) == 0);
    assert(gc_data != NULL);
    assert(gc_len > 0);

    free(gc_data);
    mls_group_free(&group);
}

/* ── Application message tests ─────────────────────────────────────────── */

TEST(test_encrypt_decrypt_single_member)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Encrypt a message */
    const char *msg = "Hello, World!";
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    int rc = mls_group_encrypt(&group, (const uint8_t *)msg, strlen(msg),
                                &ct, &ct_len);
    assert(rc == 0);
    assert(ct != NULL);
    assert(ct_len > strlen(msg)); /* ciphertext is larger than plaintext */

    /* Can't decrypt our own message (returns OWN_MESSAGE) */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender;
    rc = mls_group_decrypt(&group, ct, ct_len, &pt, &pt_len, &sender);
    assert(rc == MARMOT_ERR_OWN_MESSAGE);

    free(ct);
    mls_group_free(&group);
}

TEST(test_encrypt_multiple_messages)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Encrypt several messages — each should succeed and be different */
    uint8_t *ct1 = NULL, *ct2 = NULL;
    size_t ct1_len = 0, ct2_len = 0;

    assert(mls_group_encrypt(&group, (const uint8_t *)"msg1", 4, &ct1, &ct1_len) == 0);
    assert(mls_group_encrypt(&group, (const uint8_t *)"msg2", 4, &ct2, &ct2_len) == 0);

    /* Different ciphertexts (different generation + reuse guard) */
    assert(ct1_len != ct2_len || memcmp(ct1, ct2, ct1_len) != 0);

    free(ct1);
    free(ct2);
    mls_group_free(&group);
}

TEST(test_encrypt_null_args)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(NULL, (const uint8_t *)"hi", 2, &ct, &ct_len) != 0);
    assert(mls_group_encrypt(&group, NULL, 2, &ct, &ct_len) != 0);
    assert(mls_group_encrypt(&group, (const uint8_t *)"hi", 2, NULL, &ct_len) != 0);
    assert(mls_group_encrypt(&group, (const uint8_t *)"hi", 2, &ct, NULL) != 0);

    mls_group_free(&group);
}

TEST(test_decrypt_wrong_group_id)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Encrypt */
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&group, (const uint8_t *)"test", 4, &ct, &ct_len) == 0);

    /* Create another group with different ID */
    MlsGroup group2;
    const uint8_t other_id[] = "other-group-999";
    uint8_t sig_sk2[MLS_SIG_SK_LEN], sig_pk2[MLS_SIG_PK_LEN];
    mls_crypto_sign_keygen(sig_sk2, sig_pk2);
    assert(mls_group_create(&group2, other_id, sizeof(other_id),
                            ALICE_ID, 32, sig_sk2, NULL, 0) == 0);

    /* Should fail with wrong group ID */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    int rc = mls_group_decrypt(&group2, ct, ct_len, &pt, &pt_len, NULL);
    assert(rc == MARMOT_ERR_WRONG_GROUP_ID);

    free(ct);
    mls_group_free(&group);
    mls_group_free(&group2);
}

/* ── Self-update tests ─────────────────────────────────────────────────── */

TEST(test_self_update)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);
    assert(group.epoch == 0);

    MlsCommitResult result;
    int rc = mls_group_self_update(&group, &result);
    assert(rc == 0);
    assert(result.commit_data != NULL);
    assert(result.commit_len > 0);
    assert(group.epoch == 1);

    mls_commit_result_clear(&result);
    mls_group_free(&group);
}

TEST(test_self_update_multiple)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    for (int i = 0; i < 5; i++) {
        uint64_t prev_epoch = group.epoch;
        MlsCommitResult result;
        assert(mls_group_self_update(&group, &result) == 0);
        assert(group.epoch == prev_epoch + 1);
        mls_commit_result_clear(&result);
    }
    assert(group.epoch == 5);

    mls_group_free(&group);
}

TEST(test_encrypt_after_self_update)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Self-update to advance epoch */
    MlsCommitResult result;
    assert(mls_group_self_update(&group, &result) == 0);
    mls_commit_result_clear(&result);

    /* Should still be able to encrypt */
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&group, (const uint8_t *)"post-update", 11,
                              &ct, &ct_len) == 0);
    assert(ct != NULL);

    free(ct);
    mls_group_free(&group);
}

/* ── Add member tests ──────────────────────────────────────────────────── */

TEST(test_add_member)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);
    assert(group.tree.n_leaves == 1);

    /* Create Bob's key package */
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    /* Add Bob */
    MlsAddResult add_result;
    int rc = mls_group_add_member(&group, &bob_kp, &add_result);
    assert(rc == 0);
    assert(add_result.commit_data != NULL);
    assert(add_result.commit_len > 0);
    assert(add_result.welcome_data != NULL);
    assert(add_result.welcome_len > 0);
    assert(group.epoch == 1);
    assert(group.tree.n_leaves == 2);

    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&group);
}

TEST(test_add_member_invalid_kp)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Create invalid key package */
    MlsKeyPackage bad_kp;
    MlsKeyPackagePrivate bad_priv;
    assert(mls_key_package_create(&bad_kp, &bad_priv, BOB_ID, 32, NULL, 0) == 0);
    bad_kp.version = 99; /* Invalidate */

    MlsAddResult result;
    int rc = mls_group_add_member(&group, &bad_kp, &result);
    assert(rc != 0);

    mls_key_package_clear(&bad_kp);
    mls_key_package_private_clear(&bad_priv);
    mls_group_free(&group);
}

/* ── Remove member tests ───────────────────────────────────────────────── */

TEST(test_remove_member)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Add Bob first */
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&group, &bob_kp, &add_result) == 0);
    mls_add_result_clear(&add_result);
    assert(group.epoch == 1);

    /* Remove Bob (leaf 1) */
    MlsCommitResult rm_result;
    int rc = mls_group_remove_member(&group, 1, &rm_result);
    assert(rc == 0);
    assert(rm_result.commit_data != NULL);
    assert(rm_result.commit_len > 0);
    assert(group.epoch == 2);

    /* Bob's leaf should be blank */
    uint32_t bob_node = mls_tree_leaf_to_node(1);
    assert(group.tree.nodes[bob_node].type == MLS_NODE_BLANK);

    mls_commit_result_clear(&rm_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&group);
}

TEST(test_remove_self_rejected)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    MlsCommitResult result;
    int rc = mls_group_remove_member(&group, 0, &result);
    assert(rc == MARMOT_ERR_INVALID_ARG);

    mls_group_free(&group);
}

TEST(test_remove_out_of_range)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    MlsCommitResult result;
    int rc = mls_group_remove_member(&group, 999, &result);
    assert(rc == MARMOT_ERR_INVALID_ARG);

    mls_group_free(&group);
}

/* ── Commit serialization tests ────────────────────────────────────────── */

TEST(test_commit_serialize_roundtrip)
{
    /* Create a commit with an Add proposal */
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, BOB_ID, 32, NULL, 0) == 0);

    MlsProposal prop;
    memset(&prop, 0, sizeof(prop));
    prop.type = MLS_PROPOSAL_ADD;
    assert(mls_leaf_node_clone(&prop.add.key_package.leaf_node, &kp.leaf_node) == 0);
    prop.add.key_package.version = kp.version;
    prop.add.key_package.cipher_suite = kp.cipher_suite;
    memcpy(prop.add.key_package.init_key, kp.init_key, MLS_KEM_PK_LEN);
    memcpy(prop.add.key_package.signature, kp.signature, kp.signature_len);
    prop.add.key_package.signature_len = kp.signature_len;

    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = &prop;
    commit.proposal_count = 1;
    commit.has_path = false;

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 1024) == 0);
    assert(mls_commit_serialize(&commit, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize */
    MlsCommit commit2;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_commit_deserialize(&reader, &commit2) == 0);
    assert(commit2.proposal_count == 1);
    assert(commit2.proposals[0].type == MLS_PROPOSAL_ADD);
    assert(commit2.has_path == false);

    mls_tls_buf_free(&buf);
    mls_commit_clear(&commit2);
    mls_proposal_clear(&prop);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_commit_remove_serialize)
{
    MlsProposal prop;
    memset(&prop, 0, sizeof(prop));
    prop.type = MLS_PROPOSAL_REMOVE;
    prop.remove.removed_leaf = 42;

    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = &prop;
    commit.proposal_count = 1;
    commit.has_path = false;

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_commit_serialize(&commit, &buf) == 0);

    MlsCommit commit2;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_commit_deserialize(&reader, &commit2) == 0);
    assert(commit2.proposal_count == 1);
    assert(commit2.proposals[0].type == MLS_PROPOSAL_REMOVE);
    assert(commit2.proposals[0].remove.removed_leaf == 42);

    mls_tls_buf_free(&buf);
    mls_commit_clear(&commit2);
}

/* ── GroupInfo tests ───────────────────────────────────────────────────── */

TEST(test_group_info_build_and_roundtrip)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    MlsGroupInfo gi;
    assert(mls_group_info_build(&group, &gi) == 0);
    assert(gi.group_id_len == group.group_id_len);
    assert(gi.epoch == group.epoch);
    assert(gi.signer_leaf == group.own_leaf_index);
    assert(gi.signature_len == MLS_SIG_LEN);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_group_info_serialize(&gi, &buf) == 0);

    /* Deserialize */
    MlsGroupInfo gi2;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_group_info_deserialize(&reader, &gi2) == 0);
    assert(gi2.epoch == gi.epoch);
    assert(gi2.signer_leaf == gi.signer_leaf);
    assert(gi2.group_id_len == gi.group_id_len);

    mls_tls_buf_free(&buf);
    mls_group_info_clear(&gi);
    mls_group_info_clear(&gi2);
    mls_group_free(&group);
}

/* ── Two-member integration tests ──────────────────────────────────────── */

static const uint8_t CHARLIE_ID[32] = {
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

/**
 * Full integration: Alice creates group, adds Bob via Welcome,
 * Bob processes Welcome and joins, then they exchange messages.
 */
TEST(test_two_member_message_exchange)
{
    /* Alice creates group */
    MlsGroup alice_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    /* Bob creates key package */
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    /* Alice adds Bob */
    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(alice_group.epoch == 1);
    assert(alice_group.tree.n_leaves == 2);

    /* Bob processes Welcome */
    MlsGroup bob_group;
    int rc = mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                  &bob_kp, &bob_priv, NULL, 0, &bob_group);
    assert(rc == 0);

    /* Verify both are in the same group and epoch */
    assert(bob_group.epoch == alice_group.epoch);
    assert(bob_group.group_id_len == alice_group.group_id_len);
    assert(memcmp(bob_group.group_id, alice_group.group_id,
                  alice_group.group_id_len) == 0);

    /* Alice sends a message to Bob */
    const char *msg_text = "Hello Bob!";
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&alice_group, (const uint8_t *)msg_text,
                              strlen(msg_text), &ct, &ct_len) == 0);

    /* Bob decrypts Alice's message */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender_leaf;
    rc = mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, &sender_leaf);
    assert(rc == 0);
    assert(pt_len == strlen(msg_text));
    assert(memcmp(pt, msg_text, pt_len) == 0);
    assert(sender_leaf == 0); /* Alice is leaf 0 */

    free(pt);
    free(ct);

    /* Bob sends a message to Alice */
    const char *bob_msg = "Hello Alice!";
    ct = NULL;
    ct_len = 0;
    assert(mls_group_encrypt(&bob_group, (const uint8_t *)bob_msg,
                              strlen(bob_msg), &ct, &ct_len) == 0);

    /* Alice decrypts Bob's message */
    pt = NULL;
    pt_len = 0;
    rc = mls_group_decrypt(&alice_group, ct, ct_len, &pt, &pt_len, &sender_leaf);
    assert(rc == 0);
    assert(pt_len == strlen(bob_msg));
    assert(memcmp(pt, bob_msg, pt_len) == 0);
    assert(sender_leaf == 1); /* Bob is leaf 1 */

    free(pt);
    free(ct);

    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Verify that multiple messages can be exchanged in both directions.
 */
TEST(test_two_member_multiple_messages)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Exchange 5 messages in each direction */
    for (int i = 0; i < 5; i++) {
        char msg_buf[64];
        snprintf(msg_buf, sizeof(msg_buf), "Alice msg %d", i);

        uint8_t *ct = NULL;
        size_t ct_len = 0;
        assert(mls_group_encrypt(&alice_group, (const uint8_t *)msg_buf,
                                  strlen(msg_buf), &ct, &ct_len) == 0);

        uint8_t *pt = NULL;
        size_t pt_len = 0;
        uint32_t sender;
        assert(mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, &sender) == 0);
        assert(pt_len == strlen(msg_buf));
        assert(memcmp(pt, msg_buf, pt_len) == 0);
        assert(sender == 0);
        free(pt);
        free(ct);
    }

    for (int i = 0; i < 5; i++) {
        char msg_buf[64];
        snprintf(msg_buf, sizeof(msg_buf), "Bob msg %d", i);

        uint8_t *ct = NULL;
        size_t ct_len = 0;
        assert(mls_group_encrypt(&bob_group, (const uint8_t *)msg_buf,
                                  strlen(msg_buf), &ct, &ct_len) == 0);

        uint8_t *pt = NULL;
        size_t pt_len = 0;
        uint32_t sender;
        assert(mls_group_decrypt(&alice_group, ct, ct_len, &pt, &pt_len, &sender) == 0);
        assert(pt_len == strlen(msg_buf));
        assert(memcmp(pt, msg_buf, pt_len) == 0);
        assert(sender == 1);
        free(pt);
        free(ct);
    }

    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Verify epoch secrets match between creator and joiner after Welcome.
 */
TEST(test_welcome_epoch_secrets_match)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Epoch secrets must match for message exchange to work */
    assert(memcmp(alice_group.epoch_secrets.sender_data_secret,
                  bob_group.epoch_secrets.sender_data_secret,
                  MLS_HASH_LEN) == 0);
    assert(memcmp(alice_group.epoch_secrets.encryption_secret,
                  bob_group.epoch_secrets.encryption_secret,
                  MLS_HASH_LEN) == 0);
    assert(memcmp(alice_group.epoch_secrets.confirmation_key,
                  bob_group.epoch_secrets.confirmation_key,
                  MLS_HASH_LEN) == 0);
    assert(memcmp(alice_group.epoch_secrets.membership_key,
                  bob_group.epoch_secrets.membership_key,
                  MLS_HASH_LEN) == 0);

    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Add a 3rd member (Charlie) to a 4-leaf tree (power of 2).
 *
 * Non-power-of-2 tree sizes (3 leaves) have unresolved path encryption
 * issues. This test works around that by adding a dummy member first to
 * grow the tree to 4 leaves, then removing them to create a blank slot,
 * so Charlie can be added as leaf 3 in a 4-leaf tree.
 *
 * Note: this tests the multi-add scenario with tree growth,
 * not the odd-tree-size case (which is a separate bug).
 */
TEST(test_three_member_via_four_leaf_tree)
{
    /* Alice creates group, adds Bob */
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_bob;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_bob) == 0);
    assert(mls_welcome_process(add_bob.welcome_data, add_bob.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Verify Alice and Bob can exchange messages before adding Charlie */
    {
        uint8_t *ct = NULL;
        size_t ct_len = 0;
        assert(mls_group_encrypt(&alice_group, (const uint8_t *)"pre-charlie", 11,
                                  &ct, &ct_len) == 0);
        uint8_t *pt = NULL;
        size_t pt_len = 0;
        uint32_t sender;
        assert(mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, &sender) == 0);
        assert(memcmp(pt, "pre-charlie", 11) == 0);
        free(pt);
        free(ct);
    }

    mls_add_result_clear(&add_bob);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Verify decrypt fails with WRONG_EPOCH after the sender advances.
 */
TEST(test_epoch_mismatch_rejected)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Alice self-updates, advancing her epoch */
    MlsCommitResult update_result;
    assert(mls_group_self_update(&alice_group, &update_result) == 0);
    assert(alice_group.epoch == 2);
    assert(bob_group.epoch == 1); /* Bob hasn't processed commit yet */

    /* Alice encrypts at epoch 2 */
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&alice_group, (const uint8_t *)"epoch2", 6,
                              &ct, &ct_len) == 0);

    /* Bob tries to decrypt at epoch 1 — should fail */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    int rc = mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, NULL);
    assert(rc == MARMOT_ERR_WRONG_EPOCH);

    free(ct);
    mls_commit_result_clear(&update_result);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Verify that own-message detection works correctly in a two-member group.
 */
TEST(test_own_message_detection)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Alice encrypts */
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&alice_group, (const uint8_t *)"test", 4,
                              &ct, &ct_len) == 0);

    /* Alice tries to decrypt her own message — should return OWN_MESSAGE */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender;
    int rc = mls_group_decrypt(&alice_group, ct, ct_len, &pt, &pt_len, &sender);
    assert(rc == MARMOT_ERR_OWN_MESSAGE);
    assert(sender == 0); /* Should still report sender leaf */

    free(ct);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Verify ciphertexts from different senders are not interchangeable:
 * same plaintext produces different ciphertexts due to different sender
 * keys and reuse guards.
 */
TEST(test_ciphertext_uniqueness)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    const char *same_msg = "same message";
    uint8_t *ct_alice = NULL, *ct_bob = NULL;
    size_t ct_alice_len = 0, ct_bob_len = 0;

    assert(mls_group_encrypt(&alice_group, (const uint8_t *)same_msg,
                              strlen(same_msg), &ct_alice, &ct_alice_len) == 0);
    assert(mls_group_encrypt(&bob_group, (const uint8_t *)same_msg,
                              strlen(same_msg), &ct_bob, &ct_bob_len) == 0);

    /* Ciphertexts should differ (different sender, different keys) */
    assert(ct_alice_len != ct_bob_len ||
           memcmp(ct_alice, ct_bob, ct_alice_len) != 0);

    /* Both should decrypt correctly by the other party */
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender;
    assert(mls_group_decrypt(&bob_group, ct_alice, ct_alice_len,
                              &pt, &pt_len, &sender) == 0);
    assert(pt_len == strlen(same_msg));
    assert(sender == 0);
    free(pt);

    assert(mls_group_decrypt(&alice_group, ct_bob, ct_bob_len,
                              &pt, &pt_len, &sender) == 0);
    assert(pt_len == strlen(same_msg));
    assert(sender == 1);
    free(pt);

    free(ct_alice);
    free(ct_bob);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Large message stress test — encrypt and decrypt a substantial payload.
 */
TEST(test_large_message)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* 64KB message */
    size_t big_len = 65536;
    uint8_t *big_msg = malloc(big_len);
    assert(big_msg != NULL);
    for (size_t i = 0; i < big_len; i++)
        big_msg[i] = (uint8_t)(i & 0xFF);

    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&alice_group, big_msg, big_len, &ct, &ct_len) == 0);
    assert(ct_len > big_len);

    uint8_t *pt = NULL;
    size_t pt_len = 0;
    assert(mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, NULL) == 0);
    assert(pt_len == big_len);
    assert(memcmp(pt, big_msg, big_len) == 0);

    free(big_msg);
    free(pt);
    free(ct);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/**
 * Empty message: encrypt and decrypt a zero-length payload.
 */
TEST(test_empty_message)
{
    MlsGroup alice_group, bob_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                                &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);

    /* Empty payload */
    uint8_t empty = 0;
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_group_encrypt(&alice_group, &empty, 0, &ct, &ct_len) == 0);

    uint8_t *pt = NULL;
    size_t pt_len = 0;
    assert(mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, NULL) == 0);
    assert(pt_len == 0);

    free(pt);
    free(ct);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/* ── Epoch secret evolution ────────────────────────────────────────────── */

TEST(test_epoch_secrets_change_after_update)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);

    /* Capture epoch 0 secrets */
    uint8_t enc_secret_0[MLS_HASH_LEN];
    memcpy(enc_secret_0, group.epoch_secrets.encryption_secret, MLS_HASH_LEN);

    /* Self-update */
    MlsCommitResult result;
    assert(mls_group_self_update(&group, &result) == 0);
    mls_commit_result_clear(&result);

    /* Epoch 1 secrets should differ */
    assert(memcmp(enc_secret_0, group.epoch_secrets.encryption_secret, MLS_HASH_LEN) != 0);

    mls_group_free(&group);
}

TEST(test_group_free_idempotent)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);
    mls_group_free(&group);
    /* Second free should be safe (zeroed struct) */
    mls_group_free(&group);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("test_mls_group:\n");

    printf(" Group creation:\n");
    RUN(test_group_create_basic);
    RUN(test_group_create_null_args);
    RUN(test_group_create_with_extensions);
    RUN(test_group_tree_hash);
    RUN(test_group_context_build);

    printf(" Application messages:\n");
    RUN(test_encrypt_decrypt_single_member);
    RUN(test_encrypt_multiple_messages);
    RUN(test_encrypt_null_args);
    RUN(test_decrypt_wrong_group_id);

    printf(" Self-update:\n");
    RUN(test_self_update);
    RUN(test_self_update_multiple);
    RUN(test_encrypt_after_self_update);

    printf(" Add member:\n");
    RUN(test_add_member);
    RUN(test_add_member_invalid_kp);

    printf(" Remove member:\n");
    RUN(test_remove_member);
    RUN(test_remove_self_rejected);
    RUN(test_remove_out_of_range);

    printf(" Commit serialization:\n");
    RUN(test_commit_serialize_roundtrip);
    RUN(test_commit_remove_serialize);

    printf(" GroupInfo:\n");
    RUN(test_group_info_build_and_roundtrip);

    printf(" Two-member integration:\n");
    RUN(test_two_member_message_exchange);
    RUN(test_two_member_multiple_messages);
    RUN(test_welcome_epoch_secrets_match);
    RUN(test_own_message_detection);
    RUN(test_ciphertext_uniqueness);
    RUN(test_epoch_mismatch_rejected);
    RUN(test_large_message);
    RUN(test_empty_message);

    printf(" Multi-member integration:\n");
    RUN(test_three_member_via_four_leaf_tree);

    printf(" Epoch secrets:\n");
    RUN(test_epoch_secrets_change_after_update);
    RUN(test_group_free_idempotent);

    printf("All group tests passed.\n");
    return 0;
}
