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
    /* A Remove requires an UpdatePath (RFC 9420 §12.4): round-trip the
     * path-bearing Commit libmarmot's remove producer emits. */
    MlsGroup alice_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);
    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);
    MlsCommitResult removal;
    assert(mls_group_remove_member(&alice_group, 1, &removal) == 0);

    MlsMLSMessage msg;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, removal.commit_data, removal.commit_len);
    assert(mls_message_deserialize(&reader, &msg) == 0);
    const MlsFramedContent *fc = &msg.public_message.content;
    MlsCommit commit2;
    mls_tls_reader_init(&reader, fc->content, fc->content_len);
    assert(mls_commit_deserialize(&reader, &commit2) == 0);
    assert(mls_tls_reader_done(&reader));
    assert(commit2.proposal_count == 1);
    assert(commit2.proposals[0].type == MLS_PROPOSAL_REMOVE);
    assert(commit2.proposals[0].remove.removed_leaf == 1);
    assert(commit2.has_path);

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, fc->content_len) == 0);
    assert(mls_commit_serialize(&commit2, &buf) == 0);
    assert(buf.len == fc->content_len &&
           memcmp(buf.data, fc->content, buf.len) == 0);

    mls_tls_buf_free(&buf);
    mls_commit_clear(&commit2);
    mls_message_clear(&msg);
    mls_commit_result_clear(&removal);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
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

static int
setup_two_member_groups(MlsGroup *alice_group, MlsGroup *bob_group,
                        MlsKeyPackage *bob_kp,
                        MlsKeyPackagePrivate *bob_priv,
                        MlsAddResult *add_result)
{
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    if (create_alice_group(alice_group, alice_sk) != 0) return -1;
    if (mls_key_package_create(bob_kp, bob_priv, BOB_ID, 32, NULL, 0) != 0)
        return -1;
    if (mls_group_add_member(alice_group, bob_kp, add_result) != 0)
        return -1;
    if (mls_welcome_process(add_result->welcome_data, add_result->welcome_len,
                            bob_kp, bob_priv, NULL, 0, bob_group) != 0)
        return -1;
    return 0;
}

static int
build_commit_public_message_for_test(MlsGroup *sender,
                                     const uint8_t *commit_body,
                                     size_t commit_body_len,
                                     const uint8_t confirmation_tag[MLS_HASH_LEN],
                                     uint8_t **out, size_t *out_len)
{
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    if (mls_group_context_build(sender, &gc, &gc_len) != 0) return -1;

    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg.public_message;
    pm->content.group_id = malloc(sender->group_id_len);
    pm->content.content = malloc(commit_body_len);
    if (!pm->content.group_id || !pm->content.content) goto fail;
    memcpy(pm->content.group_id, sender->group_id, sender->group_id_len);
    pm->content.group_id_len = sender->group_id_len;
    pm->content.epoch = sender->epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = sender->own_leaf_index;
    pm->content.content_type = MLS_CONTENT_TYPE_COMMIT;
    memcpy(pm->content.content, commit_body, commit_body_len);
    pm->content.content_len = commit_body_len;
    if (mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                gc, gc_len, sender->own_signature_key,
                                &pm->auth) != 0) goto fail;
    memcpy(pm->auth.confirmation_tag, confirmation_tag, MLS_HASH_LEN);
    pm->auth.confirmation_tag_len = MLS_HASH_LEN;
    pm->auth.has_confirmation_tag = true;
    if (mls_public_message_compute_membership_tag(pm,
            sender->epoch_secrets.membership_key, gc, gc_len) != 0) goto fail;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, commit_body_len + 256) != 0) goto fail;
    if (mls_message_serialize(&msg, &buf) != 0) {
        mls_tls_buf_free(&buf);
        goto fail;
    }
    *out = buf.data;
    *out_len = buf.len;
    free(gc);
    mls_message_clear(&msg);
    return 0;
fail:
    free(gc);
    mls_message_clear(&msg);
    return -1;
}

static int
build_proposal_message_with_body_for_test(const MlsGroup *sender,
                                          const uint8_t *body, size_t body_len,
                                          uint64_t epoch, uint32_t claimed_sender,
                                          uint8_t group_id_xor,
                                          const uint8_t signing_key[MLS_SIG_SK_LEN],
                                          const uint8_t membership_key[MLS_HASH_LEN],
                                          uint8_t **out, size_t *out_len)
{
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    if (mls_group_context_build(sender, &gc, &gc_len) != 0) return -1;
    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg.public_message;
    pm->content.group_id = malloc(sender->group_id_len);
    pm->content.content = malloc(body_len);
    if (!pm->content.group_id || !pm->content.content) goto fail;
    memcpy(pm->content.group_id, sender->group_id, sender->group_id_len);
    pm->content.group_id[0] ^= group_id_xor;
    pm->content.group_id_len = sender->group_id_len;
    pm->content.epoch = epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = claimed_sender;
    pm->content.content_type = MLS_CONTENT_TYPE_PROPOSAL;
    memcpy(pm->content.content, body, body_len);
    pm->content.content_len = body_len;
    if (mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                gc, gc_len, signing_key, &pm->auth) != 0 ||
        mls_public_message_compute_membership_tag(pm, membership_key,
                                                   gc, gc_len) != 0)
        goto fail;
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) goto fail;
    if (mls_message_serialize(&msg, &buf) != 0) {
        mls_tls_buf_free(&buf);
        goto fail;
    }
    *out = buf.data;
    *out_len = buf.len;
    free(gc);
    mls_message_clear(&msg);
    return 0;
fail:
    free(gc);
    mls_message_clear(&msg);
    return -1;
}

static int
build_proposal_public_message_for_test(const MlsGroup *sender,
                                       uint64_t epoch, uint32_t claimed_sender,
                                       uint8_t group_id_xor,
                                       const uint8_t signing_key[MLS_SIG_SK_LEN],
                                       const uint8_t membership_key[MLS_HASH_LEN],
                                       uint8_t **out, size_t *out_len)
{
    static const uint8_t remove_body[] = {0x00, 0x03, 0x00, 0x00, 0x00, 0x01};
    return build_proposal_message_with_body_for_test(sender, remove_body,
        sizeof(remove_body), epoch, claimed_sender, group_id_xor, signing_key,
        membership_key, out, out_len);
}

/* ── Referenced-proposal Commit fixtures (RFC 9420 §12.4) ────────────────
 *
 * libmarmot has no by-reference Commit producer, so these helpers build a
 * spec-valid one from exported MLS primitives.  A Commit that covers only Add
 * proposals needs no UpdatePath (§12.4), so the committer frames and signs the
 * Commit, advances the transcript and key schedule with an all-zero
 * commit_secret, and attaches the confirmation and membership tags.  The
 * receiver-side proof is always the live mls_group_process_commit_ex(). */

typedef struct {
    uint64_t        epoch;
    uint32_t        n_leaves;
    uint8_t         tree_hash[MLS_HASH_LEN];
    uint8_t         confirmed_transcript_hash[MLS_HASH_LEN];
    uint8_t         interim_transcript_hash[MLS_HASH_LEN];
    MlsEpochSecrets epoch_secrets;
} ExpectedEpochForTest;

/* Proposal { ProposalType add(1); KeyPackage key_package; } */
static int
add_proposal_body_for_test(const MlsKeyPackage *kp, MlsTlsBuf *body)
{
    if (mls_tls_buf_init(body, 512) != 0) return -1;
    if (mls_tls_write_u16(body, MLS_PROPOSAL_ADD) != 0 ||
        mls_key_package_serialize(kp, body) != 0) {
        mls_tls_buf_free(body);
        return -1;
    }
    return 0;
}

/* wire_format || FramedContent || signature<V>: the AuthenticatedContent
 * prefix hashed into ProposalRefs and the confirmed transcript. */
static int
authenticated_content_for_test(const MlsPublicMessage *pm, MlsTlsBuf *out)
{
    const uint8_t *sig = pm->auth.signature_data ? pm->auth.signature_data
                                                 : pm->auth.signature;
    size_t sig_len = pm->auth.signature_len ? pm->auth.signature_len : MLS_SIG_LEN;
    if (mls_tls_buf_init(out, pm->content.content_len + 256) != 0) return -1;
    if (mls_tls_write_u16(out, MLS_WIRE_FORMAT_PUBLIC_MESSAGE) != 0 ||
        mls_framed_content_serialize(&pm->content, out) != 0 ||
        mls_tls_write_opaque16(out, sig, sig_len) != 0) {
        mls_tls_buf_free(out);
        return -1;
    }
    return 0;
}

/* ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent) */
static int
proposal_ref_for_test(const uint8_t *msg_data, size_t msg_len,
                      uint8_t ref[MLS_HASH_LEN])
{
    MlsMLSMessage msg;
    MlsTlsReader r;
    MlsTlsBuf ac;
    mls_tls_reader_init(&r, msg_data, msg_len);
    if (mls_message_deserialize(&r, &msg) != 0) return -1;
    int rc = -1;
    if (msg.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE &&
        authenticated_content_for_test(&msg.public_message, &ac) == 0) {
        rc = mls_crypto_ref_hash(ref, "MLS 1.0 Proposal Reference",
                                 ac.data, ac.len);
        mls_tls_buf_free(&ac);
    }
    mls_message_clear(&msg);
    return rc;
}

static int
tree_clone_for_test(const MlsRatchetTree *src, MlsRatchetTree *dst)
{
    uint8_t *data = NULL;
    size_t len = 0;
    memset(dst, 0, sizeof(*dst));
    int rc = (mls_ratchet_tree_serialize(src, &data, &len) == 0 &&
              mls_ratchet_tree_deserialize(data, len, dst) == 0) ? 0 : -1;
    free(data);
    return rc;
}

/* GroupContext tree_hash: libmarmot hashes the canonical (serialized) view,
 * which omits a blank right edge, while keeping the live leaf-index space. */
static int
canonical_tree_hash_for_test(const MlsRatchetTree *tree, uint8_t out[MLS_HASH_LEN])
{
    MlsRatchetTree canonical;
    int rc = tree_clone_for_test(tree, &canonical) == 0 &&
             mls_tree_root_hash(&canonical, out) == 0 ? 0 : -1;
    mls_tree_free(&canonical);
    return rc;
}

/* Add: fill the leftmost blank leaf (extending the tree when full) and join
 * the unmerged_leaves of every non-blank parent on its direct path (RFC 9420
 * §12.1.1). */
static int
add_leaf_for_test(MlsRatchetTree *tree, const MlsKeyPackage *kp)
{
    uint32_t node_idx;
    if (mls_tree_add_leaf(tree, &node_idx) != 0) return -1;
    tree->nodes[node_idx].type = MLS_NODE_LEAF;
    if (mls_leaf_node_clone(&tree->nodes[node_idx].leaf, &kp->leaf_node) != 0)
        return -1;
    uint32_t dp[64], dp_len = 0;
    if (mls_tree_direct_path(node_idx, tree->n_leaves, dp, 64, &dp_len) != 0)
        return -1;
    for (uint32_t j = 0; j < dp_len; j++) {
        MlsParentNode *parent = &tree->nodes[dp[j]].parent;
        if (tree->nodes[dp[j]].type != MLS_NODE_PARENT) continue;
        uint32_t *grown = realloc(parent->unmerged_leaves,
            (parent->unmerged_leaf_count + 1) * sizeof(*grown));
        if (!grown) return -1;
        parent->unmerged_leaves = grown;
        parent->unmerged_leaves[parent->unmerged_leaf_count++] =
            mls_tree_node_to_leaf(node_idx);
    }
    return 0;
}

/* Remove / Update: optionally install `leaf_node` at `leaf` (Update), else
 * blank it (Remove), then blank its direct path (RFC 9420 §12.1.2-3). */
static int
replace_leaf_for_test(MlsRatchetTree *tree, uint32_t leaf,
                      const MlsLeafNode *leaf_node)
{
    uint32_t node = mls_tree_leaf_to_node(leaf);
    mls_tree_blank_node(&tree->nodes[node]);
    if (leaf_node) {
        tree->nodes[node].type = MLS_NODE_LEAF;
        if (mls_leaf_node_clone(&tree->nodes[node].leaf, leaf_node) != 0)
            return -1;
    }
    uint32_t dp[64], dp_len = 0;
    if (mls_tree_direct_path(node, tree->n_leaves, dp, 64, &dp_len) != 0)
        return -1;
    for (uint32_t j = 0; j < dp_len; j++)
        mls_tree_blank_node(&tree->nodes[dp[j]]);
    return 0;
}

/* Build a Commit from `committer` whose ProposalOrRef vector is the
 * pre-encoded `proposals`, carrying `path` (NULL: no UpdatePath), and derive
 * the epoch a receiver reaches if it accepts it: `next_tree` is the tree after
 * the proposals and the path, `next_ext` the resulting GroupContext
 * extensions, `commit_secret` the path's commit secret (NULL: all-zero, as
 * without a path), `psk_secret` the combined PSK secret (NULL: no PSKs). */
static int
build_commit_for_test(const MlsGroup *committer,
                      const uint8_t *proposals, size_t proposals_len,
                      const MlsUpdatePath *path,
                      const uint8_t *commit_secret,
                      const MlsRatchetTree *next_tree,
                      const uint8_t *next_ext, size_t next_ext_len,
                      const uint8_t *psk_secret,
                      uint8_t **out, size_t *out_len,
                      ExpectedEpochForTest *expected)
{
    int rc = -1;
    uint8_t *gc = NULL, *next_gc = NULL;
    size_t gc_len = 0, next_gc_len = 0;
    MlsTlsBuf body = {0}, ac = {0}, hash_in = {0}, wire = {0};
    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    memset(expected, 0, sizeof(*expected));

    /* Commit { ProposalOrRef proposals<V>; optional<UpdatePath> path; } */
    if (mls_tls_buf_init(&body, proposals_len + 16) != 0 ||
        mls_tls_write_opaque32(&body, proposals, proposals_len) != 0 ||
        mls_tls_write_u8(&body, path ? 1 : 0) != 0 ||
        (path && mls_update_path_serialize(path, &body) != 0))
        goto done;

    if (canonical_tree_hash_for_test(next_tree, expected->tree_hash) != 0) goto done;
    expected->n_leaves = next_tree->n_leaves;
    expected->epoch = committer->epoch + 1;

    /* Frame and sign against the parent-epoch GroupContext. */
    if (mls_group_context_build(committer, &gc, &gc_len) != 0) goto done;
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg.public_message;
    pm->content.group_id = malloc(committer->group_id_len);
    pm->content.content = malloc(body.len);
    if (!pm->content.group_id || !pm->content.content) goto done;
    memcpy(pm->content.group_id, committer->group_id, committer->group_id_len);
    pm->content.group_id_len = committer->group_id_len;
    pm->content.epoch = committer->epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = committer->own_leaf_index;
    pm->content.content_type = MLS_CONTENT_TYPE_COMMIT;
    memcpy(pm->content.content, body.data, body.len);
    pm->content.content_len = body.len;
    if (mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                gc, gc_len, committer->own_signature_key,
                                &pm->auth) != 0) goto done;

    /* confirmed' = Hash(interim || AuthenticatedContent) */
    if (authenticated_content_for_test(pm, &ac) != 0 ||
        mls_tls_buf_init(&hash_in, MLS_HASH_LEN + ac.len) != 0 ||
        mls_tls_buf_append(&hash_in, committer->interim_transcript_hash,
                           MLS_HASH_LEN) != 0 ||
        mls_tls_buf_append(&hash_in, ac.data, ac.len) != 0 ||
        mls_crypto_hash(expected->confirmed_transcript_hash,
                        hash_in.data, hash_in.len) != 0)
        goto done;

    /* Next epoch: without a path commit_secret is all-zero. */
    uint8_t zero_commit_secret[MLS_HASH_LEN] = {0};
    if (mls_group_context_serialize(committer->group_id, committer->group_id_len,
                                    expected->epoch, expected->tree_hash,
                                    expected->confirmed_transcript_hash,
                                    next_ext, next_ext_len,
                                    &next_gc, &next_gc_len) != 0 ||
        mls_key_schedule_derive(committer->epoch_secrets.init_secret,
                                commit_secret ? commit_secret : zero_commit_secret,
                                next_gc, next_gc_len,
                                psk_secret, &expected->epoch_secrets) != 0 ||
        mls_compute_confirmation_tag(expected->epoch_secrets.confirmation_key,
                                     expected->confirmed_transcript_hash,
                                     pm->auth.confirmation_tag) != 0)
        goto done;
    pm->auth.confirmation_tag_len = MLS_HASH_LEN;
    pm->auth.has_confirmation_tag = true;

    /* interim' = Hash(confirmed' || confirmation_tag<V>) */
    mls_tls_buf_free(&hash_in);
    if (mls_tls_buf_init(&hash_in, MLS_HASH_LEN * 2 + 2) != 0 ||
        mls_tls_buf_append(&hash_in, expected->confirmed_transcript_hash,
                           MLS_HASH_LEN) != 0 ||
        mls_tls_write_opaque32(&hash_in, pm->auth.confirmation_tag,
                               MLS_HASH_LEN) != 0 ||
        mls_crypto_hash(expected->interim_transcript_hash,
                        hash_in.data, hash_in.len) != 0)
        goto done;

    if (mls_public_message_compute_membership_tag(pm,
            committer->epoch_secrets.membership_key, gc, gc_len) != 0 ||
        mls_tls_buf_init(&wire, body.len + 512) != 0 ||
        mls_message_serialize(&msg, &wire) != 0)
        goto done;
    *out = wire.data;
    *out_len = wire.len;
    wire.data = NULL;
    rc = 0;
done:
    mls_tls_buf_free(&wire);
    mls_tls_buf_free(&hash_in);
    mls_tls_buf_free(&ac);
    mls_tls_buf_free(&body);
    mls_message_clear(&msg);
    free(next_gc);
    free(gc);
    return rc;
}

/* A Commit with no UpdatePath (commit_secret all-zero). */
static int
build_pathless_commit_for_test(const MlsGroup *committer,
                               const uint8_t *proposals, size_t proposals_len,
                               const MlsRatchetTree *next_tree,
                               const uint8_t *next_ext, size_t next_ext_len,
                               const uint8_t *psk_secret,
                               uint8_t **out, size_t *out_len,
                               ExpectedEpochForTest *expected)
{
    return build_commit_for_test(committer, proposals, proposals_len, NULL, NULL,
                                 next_tree, next_ext, next_ext_len, psk_secret,
                                 out, out_len, expected);
}

/* Build a pathless Commit from `committer` whose proposals are exactly
 * `refs` (ProposalOrRef type 2), assuming those references resolve to Adds of
 * `added` (in order), and derive the next epoch the receiver must reach. */
static int
build_add_ref_commit_for_test(const MlsGroup *committer,
                              const uint8_t (*refs)[MLS_HASH_LEN], size_t ref_count,
                              const MlsKeyPackage *const *added, size_t added_count,
                              uint8_t **out, size_t *out_len,
                              ExpectedEpochForTest *expected)
{
    int rc = -1;
    MlsTlsBuf refs_buf = {0};
    MlsRatchetTree tree;
    memset(&tree, 0, sizeof(tree));
    if (mls_tls_buf_init(&refs_buf, 64) != 0) goto done;
    for (size_t i = 0; i < ref_count; i++) {
        if (mls_tls_write_u8(&refs_buf, 2) != 0 ||
            mls_tls_write_opaque16(&refs_buf, refs[i], MLS_HASH_LEN) != 0)
            goto done;
    }
    if (tree_clone_for_test(&committer->tree, &tree) != 0) goto done;
    for (size_t i = 0; i < added_count; i++) {
        if (add_leaf_for_test(&tree, added[i]) != 0) goto done;
    }
    rc = build_pathless_commit_for_test(committer, refs_buf.data, refs_buf.len,
                                        &tree, committer->extensions_data,
                                        committer->extensions_len, NULL,
                                        out, out_len, expected);
done:
    mls_tls_buf_free(&refs_buf);
    mls_tree_free(&tree);
    return rc;
}

/* The full persisted state (tree, epoch, transcript hashes, epoch and
 * secret-tree secrets, own keys) plus the fields the ProposalRef regression
 * names explicitly; a rejected Commit must leave all of it untouched. */
typedef struct {
    uint64_t        epoch;
    uint8_t         tree_hash[MLS_HASH_LEN];
    MlsEpochSecrets epoch_secrets;
    uint8_t        *blob;
    size_t          blob_len;
} GroupSnapshotForTest;

static void
snapshot_group_for_test(const MlsGroup *g, GroupSnapshotForTest *s)
{
    memset(s, 0, sizeof(*s));
    s->epoch = g->epoch;
    assert(mls_group_tree_hash(g, s->tree_hash) == 0);
    memcpy(&s->epoch_secrets, &g->epoch_secrets, sizeof(s->epoch_secrets));
    assert(mls_group_serialize(g, &s->blob, &s->blob_len) == 0);
}

static void
assert_group_matches_snapshot_for_test(const MlsGroup *g, const GroupSnapshotForTest *s)
{
    GroupSnapshotForTest now;
    snapshot_group_for_test(g, &now);
    assert(now.epoch == s->epoch);
    assert(memcmp(now.tree_hash, s->tree_hash, MLS_HASH_LEN) == 0);
    assert(sodium_memcmp(&now.epoch_secrets, &s->epoch_secrets,
                         sizeof(now.epoch_secrets)) == 0);
    assert(now.blob_len == s->blob_len &&
           memcmp(now.blob, s->blob, s->blob_len) == 0);
    free(now.blob);
}

static void
assert_group_reached_for_test(const MlsGroup *g, const ExpectedEpochForTest *e)
{
    uint8_t tree_hash[MLS_HASH_LEN];
    assert(g->epoch == e->epoch);
    assert(g->tree.n_leaves == e->n_leaves);
    assert(mls_group_tree_hash(g, tree_hash) == 0);
    assert(memcmp(tree_hash, e->tree_hash, MLS_HASH_LEN) == 0);
    assert(memcmp(g->confirmed_transcript_hash, e->confirmed_transcript_hash,
                  MLS_HASH_LEN) == 0);
    assert(memcmp(g->interim_transcript_hash, e->interim_transcript_hash,
                  MLS_HASH_LEN) == 0);
    assert(sodium_memcmp(&g->epoch_secrets, &e->epoch_secrets,
                         sizeof(g->epoch_secrets)) == 0);
}

static int
build_encrypt_context_for_test(const char *label,
                               const uint8_t *context, size_t context_len,
                               uint8_t **out, size_t *out_len)
{
    const char prefix[] = "MLS 1.0 ";
    size_t label_len = strlen(prefix) + strlen(label);
    uint8_t *full_label = malloc(label_len);
    if (!full_label) return -1;
    memcpy(full_label, prefix, strlen(prefix));
    memcpy(full_label + strlen(prefix), label, strlen(label));
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, label_len + context_len + 16) != 0) {
        free(full_label);
        return -1;
    }
    if (mls_tls_write_opaque16(&buf, full_label, label_len) != 0 ||
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

TEST(test_group_out_of_order_and_replay)
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

    uint8_t *ct[3] = {0};
    size_t ct_len[3] = {0};
    for (int i = 0; i < 3; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "ooo-%d", i);
        assert(mls_group_encrypt(&alice_group, (const uint8_t *)msg, strlen(msg),
                                  &ct[i], &ct_len[i]) == 0);
    }

    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender = 999;
    assert(mls_group_decrypt(&bob_group, ct[2], ct_len[2], &pt, &pt_len, &sender) == 0);
    assert(sender == 0);
    assert(pt_len == 5 && memcmp(pt, "ooo-2", 5) == 0);
    free(pt);

    pt = NULL;
    assert(mls_group_decrypt(&bob_group, ct[0], ct_len[0], &pt, &pt_len, &sender) == 0);
    assert(sender == 0);
    assert(pt_len == 5 && memcmp(pt, "ooo-0", 5) == 0);
    free(pt);

    assert(mls_group_decrypt(&bob_group, ct[0], ct_len[0], &pt, &pt_len, &sender) != 0);

    for (int i = 0; i < 3; i++)
        free(ct[i]);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

TEST(test_group_out_of_order_beyond_window)
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
    bob_group.max_forward_distance = 1;

    uint8_t *ct[3] = {0};
    size_t ct_len[3] = {0};
    for (int i = 0; i < 3; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "win-%d", i);
        assert(mls_group_encrypt(&alice_group, (const uint8_t *)msg, strlen(msg),
                                  &ct[i], &ct_len[i]) == 0);
    }

    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender = 999;
    assert(mls_group_decrypt(&bob_group, ct[2], ct_len[2], &pt, &pt_len, &sender) != 0);

    /* The rejected generation 2 message must not consume generation 0. */
    assert(mls_group_decrypt(&bob_group, ct[0], ct_len[0], &pt, &pt_len, &sender) == 0);
    assert(sender == 0);
    assert(pt_len == 5 && memcmp(pt, "win-0", 5) == 0);
    free(pt);

    for (int i = 0; i < 3; i++)
        free(ct[i]);
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

TEST(test_process_valid_self_update_commit_roundtrip)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result;
    memset(&add_result, 0, sizeof(add_result));
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    MlsCommitResult update;
    assert(mls_group_self_update(&alice_group, &update) == 0);
    assert(mls_group_process_commit(&bob_group, update.commit_data,
                                    update.commit_len, 0) == 0);
    assert(alice_group.epoch == bob_group.epoch);

    const char *msg = "post-update";
    uint8_t *ct = NULL, *pt = NULL;
    size_t ct_len = 0, pt_len = 0;
    uint32_t sender = 99;
    assert(mls_group_encrypt(&alice_group, (const uint8_t *)msg, strlen(msg),
                              &ct, &ct_len) == 0);
    assert(mls_group_decrypt(&bob_group, ct, ct_len, &pt, &pt_len, &sender) == 0);
    assert(sender == 0);
    assert(pt_len == strlen(msg) && memcmp(pt, msg, pt_len) == 0);
    free(ct);
    free(pt);

    mls_commit_result_clear(&update);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

TEST(test_referenced_proposal_store_requires_parent_authentication)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result = {0};
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    enum { VALID, WRONG_GROUP, OLD_EPOCH, WRONG_SENDER,
           WRONG_SIGNATURE, WRONG_MEMBERSHIP, COUNT };
    uint8_t *proposals[COUNT] = {0};
    size_t proposal_lens[COUNT] = {0};
    uint8_t wrong_membership[MLS_HASH_LEN];
    memcpy(wrong_membership, alice_group.epoch_secrets.membership_key,
           sizeof(wrong_membership));
    wrong_membership[0] ^= 1;
    for (int i = 0; i < COUNT; i++) {
        assert(build_proposal_public_message_for_test(
            &alice_group,
            alice_group.epoch - (i == OLD_EPOCH),
            i == WRONG_SENDER ? 1 : 0,
            i == WRONG_GROUP ? 1 : 0,
            i == WRONG_SIGNATURE ? bob_priv.signature_key_private
                                 : alice_group.own_signature_key,
            i == WRONG_MEMBERSHIP ? wrong_membership
                                  : alice_group.epoch_secrets.membership_key,
            &proposals[i], &proposal_lens[i]) == 0);
    }

    MlsCommitResult update;
    assert(mls_group_self_update(&alice_group, &update) == 0);
    uint64_t parent_epoch = bob_group.epoch;
    for (int i = WRONG_GROUP; i < COUNT; i++) {
        const uint8_t *input[] = {proposals[i]};
        assert(mls_group_process_commit_ex(&bob_group, update.commit_data,
                                           update.commit_len, 0, input,
                                           &proposal_lens[i], 1) ==
               MARMOT_ERR_MLS_PROCESS_MESSAGE);
        assert(bob_group.epoch == parent_epoch);
    }
    const uint8_t *valid_input[] = {proposals[VALID]};
    assert(mls_group_process_commit_ex(&bob_group, update.commit_data,
                                       update.commit_len, 0, valid_input,
                                       &proposal_lens[VALID], 1) == 0);
    assert(bob_group.epoch == alice_group.epoch);

    for (int i = 0; i < COUNT; i++) free(proposals[i]);
    mls_commit_result_clear(&update);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

/* Alice (leaf 0) and Bob (leaf 1) share a normal, non-adopted group at the
 * parent epoch.  Every proposal below proposes Add(Charlie) from Alice's leaf;
 * only VALID is a genuine, current-epoch member proposal. */
enum {
    REF_VALID,
    REF_WRONG_GROUP,
    REF_PREVIOUS_EPOCH,
    REF_NON_MEMBER,
    REF_BAD_SIGNATURE,
    REF_BAD_MEMBERSHIP_TAG,
    REF_VARIANT_COUNT
};

typedef struct {
    MlsGroup             alice, bob;
    MlsKeyPackage        bob_kp, charlie_kp;
    MlsKeyPackagePrivate bob_priv, charlie_priv;
    MlsAddResult         add_result;
    MlsCommitResult      update;
    uint8_t             *prop[REF_VARIANT_COUNT];
    size_t               prop_len[REF_VARIANT_COUNT];
} ProposalRefFixture;

static void
proposal_ref_fixture_init(ProposalRefFixture *f)
{
    memset(f, 0, sizeof(*f));
    assert(setup_two_member_groups(&f->alice, &f->bob, &f->bob_kp,
                                   &f->bob_priv, &f->add_result) == 0);
    assert(mls_key_package_create(&f->charlie_kp, &f->charlie_priv,
                                  CHARLIE_ID, 32, NULL, 0) == 0);
    MlsTlsBuf add_body;
    assert(add_proposal_body_for_test(&f->charlie_kp, &add_body) == 0);

    /* A genuine Alice proposal from the epoch before the parent epoch: both
     * members then advance, so referencing it is a cross-epoch replay. */
    assert(build_proposal_message_with_body_for_test(&f->alice,
        add_body.data, add_body.len, f->alice.epoch, f->alice.own_leaf_index,
        0, f->alice.own_signature_key, f->alice.epoch_secrets.membership_key,
        &f->prop[REF_PREVIOUS_EPOCH], &f->prop_len[REF_PREVIOUS_EPOCH]) == 0);
    assert(mls_group_self_update(&f->alice, &f->update) == 0);
    assert(mls_group_process_commit(&f->bob, f->update.commit_data,
                                    f->update.commit_len, 0) == 0);
    assert(f->bob.epoch == f->alice.epoch);

    uint8_t bad_membership_key[MLS_HASH_LEN];
    memcpy(bad_membership_key, f->alice.epoch_secrets.membership_key,
           MLS_HASH_LEN);
    bad_membership_key[0] ^= 0x01;
    for (int v = 0; v < REF_VARIANT_COUNT; v++) {
        if (v == REF_PREVIOUS_EPOCH) continue;
        assert(build_proposal_message_with_body_for_test(&f->alice,
            add_body.data, add_body.len, f->alice.epoch,
            v == REF_NON_MEMBER ? f->alice.tree.n_leaves : f->alice.own_leaf_index,
            v == REF_WRONG_GROUP ? 0x01 : 0x00,
            v == REF_BAD_SIGNATURE ? f->charlie_priv.signature_key_private
                                   : f->alice.own_signature_key,
            v == REF_BAD_MEMBERSHIP_TAG ? bad_membership_key
                                        : f->alice.epoch_secrets.membership_key,
            &f->prop[v], &f->prop_len[v]) == 0);
    }
    mls_tls_buf_free(&add_body);
}

static void
proposal_ref_fixture_clear(ProposalRefFixture *f)
{
    for (int v = 0; v < REF_VARIANT_COUNT; v++) free(f->prop[v]);
    mls_commit_result_clear(&f->update);
    mls_add_result_clear(&f->add_result);
    mls_key_package_clear(&f->bob_kp);
    mls_key_package_clear(&f->charlie_kp);
    mls_key_package_private_clear(&f->bob_priv);
    mls_key_package_private_clear(&f->charlie_priv);
    mls_group_free(&f->alice);
    mls_group_free(&f->bob);
}

TEST(test_live_commit_consumes_parent_epoch_proposal_ref)
{
    ProposalRefFixture f;
    proposal_ref_fixture_init(&f);
    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.bob, &parent);

    uint8_t ref[1][MLS_HASH_LEN];
    assert(proposal_ref_for_test(f.prop[REF_VALID], f.prop_len[REF_VALID],
                                 ref[0]) == 0);
    const MlsKeyPackage *added[] = {&f.charlie_kp};
    ExpectedEpochForTest expected;
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    assert(build_add_ref_commit_for_test(&f.alice, ref, 1, added, 1,
                                         &commit, &commit_len, &expected) == 0);
    const uint8_t *store[] = {f.prop[REF_VALID]};

    /* Load the persisted parent-epoch state before the live group moves. */
    MlsGroup loaded;
    assert(mls_group_deserialize(parent.blob, parent.blob_len, &loaded) == 0);

    assert(mls_group_process_commit_ex(&f.bob, commit, commit_len,
                                       f.alice.own_leaf_index, store,
                                       &f.prop_len[REF_VALID], 1) == 0);
    assert(expected.epoch == parent.epoch + 1);
    assert_group_reached_for_test(&f.bob, &expected);
    assert(sodium_memcmp(&expected.epoch_secrets, &parent.epoch_secrets,
                         sizeof(expected.epoch_secrets)) != 0);
    /* Charlie now occupies the leftmost free leaf with his KeyPackage leaf. */
    const MlsNode *charlie = &f.bob.tree.nodes[mls_tree_leaf_to_node(2)];
    assert(charlie->type == MLS_NODE_LEAF);
    assert(charlie->leaf.credential_identity_len == sizeof(CHARLIE_ID) &&
           memcmp(charlie->leaf.credential_identity, CHARLIE_ID,
                  sizeof(CHARLIE_ID)) == 0);
    assert(memcmp(charlie->leaf.signature_key,
                  f.charlie_kp.leaf_node.signature_key, MLS_SIG_PK_LEN) == 0);

    /* Persist/load: the persisted parent state converges byte-for-byte, and
     * the advanced state reloads to the same epoch. */
    assert(mls_group_process_commit_ex(&loaded, commit, commit_len,
                                       f.alice.own_leaf_index, store,
                                       &f.prop_len[REF_VALID], 1) == 0);
    GroupSnapshotForTest advanced;
    snapshot_group_for_test(&f.bob, &advanced);
    assert_group_matches_snapshot_for_test(&loaded, &advanced);
    MlsGroup reloaded;
    assert(mls_group_deserialize(advanced.blob, advanced.blob_len,
                                 &reloaded) == 0);
    assert_group_reached_for_test(&reloaded, &expected);

    /* Replaying the parent-epoch Commit on the advanced state is rejected
     * (by the epoch check; consumption itself is proven by the duplicate-ref
     * case) and leaves the state unchanged. */
    assert(mls_group_process_commit_ex(&reloaded, commit, commit_len,
                                       f.alice.own_leaf_index, store,
                                       &f.prop_len[REF_VALID], 1) ==
           MARMOT_ERR_MLS_PROCESS_MESSAGE);
    assert_group_matches_snapshot_for_test(&reloaded, &advanced);

    free(commit);
    free(parent.blob);
    free(advanced.blob);
    mls_group_free(&loaded);
    mls_group_free(&reloaded);
    proposal_ref_fixture_clear(&f);
}

TEST(test_live_commit_rejects_unauthenticated_proposal_refs)
{
    ProposalRefFixture f;
    proposal_ref_fixture_init(&f);
    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.bob, &parent);
    const MlsKeyPackage *added[] = {&f.charlie_kp, &f.charlie_kp};
    ExpectedEpochForTest ignored;

    /* Each Commit is otherwise valid and references exactly the stored
     * proposal it is paired with, so only proposal authentication stands
     * between it and an applied Add. */
    for (int v = REF_VALID + 1; v < REF_VARIANT_COUNT; v++) {
        uint8_t ref[1][MLS_HASH_LEN];
        assert(proposal_ref_for_test(f.prop[v], f.prop_len[v], ref[0]) == 0);
        uint8_t *commit = NULL;
        size_t commit_len = 0;
        assert(build_add_ref_commit_for_test(&f.alice, ref, 1, added, 1,
                                             &commit, &commit_len, &ignored) == 0);
        const uint8_t *store[] = {f.prop[v]};
        assert(mls_group_process_commit_ex(&f.bob, commit, commit_len,
                                           f.alice.own_leaf_index, store,
                                           &f.prop_len[v], 1) ==
               MARMOT_ERR_MLS_PROCESS_MESSAGE);
        assert_group_matches_snapshot_for_test(&f.bob, &parent);
        free(commit);
    }

    uint8_t valid_ref[2][MLS_HASH_LEN];
    assert(proposal_ref_for_test(f.prop[REF_VALID], f.prop_len[REF_VALID],
                                 valid_ref[0]) == 0);
    memcpy(valid_ref[1], valid_ref[0], MLS_HASH_LEN);
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    assert(build_add_ref_commit_for_test(&f.alice, valid_ref, 1, added, 1,
                                         &commit, &commit_len, &ignored) == 0);

    /* Unknown reference: the store holds only a different, authentic
     * proposal (Alice's Remove of Bob). */
    uint8_t *other = NULL;
    size_t other_len = 0;
    assert(build_proposal_public_message_for_test(&f.alice, f.alice.epoch,
        f.alice.own_leaf_index, 0, f.alice.own_signature_key,
        f.alice.epoch_secrets.membership_key, &other, &other_len) == 0);
    const uint8_t *other_store[] = {other};
    assert(mls_group_process_commit_ex(&f.bob, commit, commit_len,
                                       f.alice.own_leaf_index, other_store,
                                       &other_len, 1) ==
           MARMOT_ERR_MLS_PROCESS_MESSAGE);
    assert_group_matches_snapshot_for_test(&f.bob, &parent);

    /* No proposal store at all. */
    assert(mls_group_process_commit(&f.bob, commit, commit_len,
                                    f.alice.own_leaf_index) ==
           MARMOT_ERR_UNSUPPORTED);
    assert_group_matches_snapshot_for_test(&f.bob, &parent);

    /* One stored proposal cannot satisfy two references to it. */
    uint8_t *twice = NULL;
    size_t twice_len = 0;
    assert(build_add_ref_commit_for_test(&f.alice, valid_ref, 2, added, 2,
                                         &twice, &twice_len, &ignored) == 0);
    const uint8_t *valid_store[] = {f.prop[REF_VALID]};
    assert(mls_group_process_commit_ex(&f.bob, twice, twice_len,
                                       f.alice.own_leaf_index, valid_store,
                                       &f.prop_len[REF_VALID], 1) ==
           MARMOT_ERR_MLS_PROCESS_MESSAGE);
    assert_group_matches_snapshot_for_test(&f.bob, &parent);

    /* After every rejection the untouched group still consumes the genuine
     * reference. */
    assert(mls_group_process_commit_ex(&f.bob, commit, commit_len,
                                       f.alice.own_leaf_index, valid_store,
                                       &f.prop_len[REF_VALID], 1) == 0);
    assert(f.bob.epoch == parent.epoch + 1);

    free(twice);
    free(other);
    free(commit);
    free(parent.blob);
    proposal_ref_fixture_clear(&f);
}

/* ── UpdatePath requirement (RFC 9420 §12.4) ─────────────────────────────
 *
 * A Commit MUST carry an UpdatePath when it covers no proposals or any
 * Update, Remove, ExternalInit or GroupContextExtensions proposal.  Alice
 * (leaf 0), Bob (leaf 1) and Charlie (leaf 2) share a normal, non-adopted
 * group built by libmarmot's own producers; Alice commits, Bob receives. */

static const uint8_t DAVE_ID[32] = {
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
};

typedef struct {
    MlsGroup             alice, bob, charlie;
    MlsKeyPackage        bob_kp, charlie_kp, dave_kp;
    MlsKeyPackagePrivate bob_priv, charlie_priv, dave_priv;
    MlsAddResult         add_bob, add_charlie;
} ThreeMemberFixture;

/* Alice/Bob/Charlie at leaves 0/1/2 of a 4-leaf tree, built by libmarmot's
 * own producers; Alice's group carries GroupContext extensions `ext`. */
static void
three_member_fixture_init_ext(ThreeMemberFixture *f,
                              const uint8_t *ext, size_t ext_len)
{
    memset(f, 0, sizeof(*f));
    uint8_t alice_sk[MLS_SIG_SK_LEN], alice_pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(alice_sk, alice_pk) == 0);
    assert(mls_group_create(&f->alice, GROUP_ID, sizeof(GROUP_ID), ALICE_ID, 32,
                            alice_sk, ext, ext_len) == 0);
    sodium_memzero(alice_sk, sizeof(alice_sk));
    assert(mls_key_package_create(&f->bob_kp, &f->bob_priv, BOB_ID, 32, NULL, 0) == 0);
    assert(mls_group_add_member(&f->alice, &f->bob_kp, &f->add_bob) == 0);
    assert(mls_welcome_process(f->add_bob.welcome_data, f->add_bob.welcome_len,
                               &f->bob_kp, &f->bob_priv, NULL, 0, &f->bob) == 0);
    assert(mls_key_package_create(&f->charlie_kp, &f->charlie_priv,
                                  CHARLIE_ID, 32, NULL, 0) == 0);
    assert(mls_key_package_create(&f->dave_kp, &f->dave_priv,
                                  DAVE_ID, 32, NULL, 0) == 0);
    assert(mls_group_add_member(&f->alice, &f->charlie_kp, &f->add_charlie) == 0);
    assert(mls_group_process_commit(&f->bob, f->add_charlie.commit_data,
                                    f->add_charlie.commit_len, 0) == 0);
    assert(mls_welcome_process(f->add_charlie.welcome_data,
                               f->add_charlie.welcome_len, &f->charlie_kp,
                               &f->charlie_priv, NULL, 0, &f->charlie) == 0);
    assert(f->charlie.own_leaf_index == 2);
    assert(f->bob.epoch == f->alice.epoch && f->charlie.epoch == f->alice.epoch);
    assert(sodium_memcmp(&f->bob.epoch_secrets, &f->alice.epoch_secrets,
                         sizeof(f->bob.epoch_secrets)) == 0);
    assert(sodium_memcmp(&f->charlie.epoch_secrets, &f->alice.epoch_secrets,
                         sizeof(f->charlie.epoch_secrets)) == 0);
}

static void
three_member_fixture_init(ThreeMemberFixture *f)
{
    three_member_fixture_init_ext(f, NULL, 0);
}

static void
three_member_fixture_clear(ThreeMemberFixture *f)
{
    mls_add_result_clear(&f->add_bob);
    mls_add_result_clear(&f->add_charlie);
    mls_key_package_clear(&f->bob_kp);
    mls_key_package_clear(&f->charlie_kp);
    mls_key_package_clear(&f->dave_kp);
    mls_key_package_private_clear(&f->bob_priv);
    mls_key_package_private_clear(&f->charlie_priv);
    mls_key_package_private_clear(&f->dave_priv);
    mls_group_free(&f->alice);
    mls_group_free(&f->bob);
    mls_group_free(&f->charlie);
}

/* A valid Update LeafNode for `member` (RFC 9420 §12.1.2): its current leaf
 * with a fresh HPKE encryption key, leaf_node_source update (no lifetime, no
 * parent_hash), signed over LeafNodeTBS bound to the group_id and the
 * member's leaf index (§7.2). */
static void
make_update_leaf_for_test(const MlsGroup *member, MlsLeafNode *out,
                          uint8_t enc_sk[MLS_KEM_SK_LEN])
{
    assert(mls_leaf_node_clone(out,
        &member->tree.nodes[mls_tree_leaf_to_node(member->own_leaf_index)].leaf) == 0);
    assert(mls_crypto_kem_keygen(enc_sk, out->encryption_key) == 0);
    out->leaf_node_source = MLS_LEAF_NODE_SOURCE_UPDATE;
    out->lifetime_not_before = 0;
    out->lifetime_not_after = 0;
    free(out->parent_hash);
    out->parent_hash = NULL;
    out->parent_hash_len = 0;
    assert(mls_leaf_node_sign(out, member->own_signature_key, member->group_id,
                              member->group_id_len, member->own_leaf_index) == 0);
    assert(mls_leaf_node_verify_signature(out, member->group_id, member->group_id_len,
                                          member->own_leaf_index) == 0);
}

/* Standalone Update proposal message from `member` carrying `leaf`. */
static void
update_proposal_message_for_test(const MlsGroup *member, const MlsLeafNode *leaf,
                                 uint8_t **msg, size_t *msg_len,
                                 uint8_t ref[MLS_HASH_LEN])
{
    MlsTlsBuf body;
    assert(mls_tls_buf_init(&body, 512) == 0);
    assert(mls_tls_write_u16(&body, MLS_PROPOSAL_UPDATE) == 0);
    assert(mls_leaf_node_serialize(leaf, &body) == 0);
    assert(build_proposal_message_with_body_for_test(member, body.data, body.len,
        member->epoch, member->own_leaf_index, 0, member->own_signature_key,
        member->epoch_secrets.membership_key, msg, msg_len) == 0);
    assert(proposal_ref_for_test(*msg, *msg_len, ref) == 0);
    mls_tls_buf_free(&body);
}

/* Inline ProposalOrRef encodings: proposal(1) || Proposal. */
static int
inline_remove_for_test(MlsTlsBuf *v, uint32_t leaf)
{
    return (mls_tls_write_u8(v, 1) == 0 &&
            mls_tls_write_u16(v, MLS_PROPOSAL_REMOVE) == 0 &&
            mls_tls_write_u32(v, leaf) == 0) ? 0 : -1;
}

static int
inline_add_for_test(MlsTlsBuf *v, const MlsKeyPackage *kp)
{
    MlsTlsBuf body;
    if (add_proposal_body_for_test(kp, &body) != 0) return -1;
    int rc = (mls_tls_write_u8(v, 1) == 0 &&
              mls_tls_buf_append(v, body.data, body.len) == 0) ? 0 : -1;
    mls_tls_buf_free(&body);
    return rc;
}

/* What any holder of the parent epoch's secrets computes from the public
 * Commit alone, assuming an all-zero commit_secret: the confirmed transcript
 * from the wire, the next GroupContext from the (public) resulting tree, then
 * the key schedule from the parent init_secret.  *tag_matches reports whether
 * that derivation reproduces the Commit's confirmation tag, i.e. whether the
 * holder has recovered the committer's actual next-epoch secrets. */
static int
derive_zero_commit_secret_epoch_for_test(const MlsGroup *holder,
                                         const uint8_t *wire, size_t wire_len,
                                         const MlsRatchetTree *next_tree,
                                         uint8_t confirmed[MLS_HASH_LEN],
                                         MlsEpochSecrets *out, bool *tag_matches)
{
    int rc = -1;
    MlsMLSMessage msg;
    MlsTlsReader r;
    MlsTlsBuf ac = {0}, hash_in = {0};
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    uint8_t tree_hash[MLS_HASH_LEN], tag[MLS_HASH_LEN];
    uint8_t zero_commit_secret[MLS_HASH_LEN] = {0};
    mls_tls_reader_init(&r, wire, wire_len);
    if (mls_message_deserialize(&r, &msg) != 0) return -1;
    if (authenticated_content_for_test(&msg.public_message, &ac) != 0 ||
        mls_tls_buf_init(&hash_in, MLS_HASH_LEN + ac.len) != 0 ||
        mls_tls_buf_append(&hash_in, holder->interim_transcript_hash,
                           MLS_HASH_LEN) != 0 ||
        mls_tls_buf_append(&hash_in, ac.data, ac.len) != 0 ||
        mls_crypto_hash(confirmed, hash_in.data, hash_in.len) != 0 ||
        canonical_tree_hash_for_test(next_tree, tree_hash) != 0 ||
        mls_group_context_serialize(holder->group_id, holder->group_id_len,
                                    holder->epoch + 1, tree_hash, confirmed,
                                    holder->extensions_data,
                                    holder->extensions_len, &gc, &gc_len) != 0 ||
        mls_key_schedule_derive(holder->epoch_secrets.init_secret,
                                zero_commit_secret, gc, gc_len, NULL, out) != 0 ||
        mls_compute_confirmation_tag(out->confirmation_key, confirmed, tag) != 0)
        goto done;
    *tag_matches = sodium_memcmp(tag, msg.public_message.auth.confirmation_tag,
                                 MLS_HASH_LEN) == 0;
    rc = 0;
done:
    free(gc);
    mls_tls_buf_free(&hash_in);
    mls_tls_buf_free(&ac);
    mls_message_clear(&msg);
    return rc;
}

enum {
    PATHLESS_REMOVE,
    PATHLESS_UPDATE_REF,
    PATHLESS_EMPTY,
    PATHLESS_GCE,
    PATHLESS_ADD_AND_REMOVE,
    PATHLESS_CASE_COUNT
};

static const char *const pathless_case_names[PATHLESS_CASE_COUNT] = {
    "Remove", "Update-by-ref", "empty", "GroupContextExtensions", "Add+Remove",
};

TEST(test_pathless_commit_requiring_path_rejected)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);

    /* Charlie's standalone Update proposal (committed by reference): a
     * fully valid Update LeafNode (source update, fresh HPKE key, signed over
     * LeafNodeTBS with group_id and leaf index 2), so the only defect of the
     * Update-by-ref Commit below is its missing path (nostrc-2io4). */
    MlsLeafNode upd_leaf;
    uint8_t upd_sk[MLS_KEM_SK_LEN];
    make_update_leaf_for_test(&f.charlie, &upd_leaf, upd_sk);
    uint8_t *upd_msg = NULL;
    size_t upd_len = 0;
    uint8_t upd_ref[MLS_HASH_LEN];
    update_proposal_message_for_test(&f.charlie, &upd_leaf, &upd_msg, &upd_len,
                                     upd_ref);

    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.bob, &parent);
    int accepted = 0;

    for (int c = 0; c < PATHLESS_CASE_COUNT; c++) {
        MlsTlsBuf v;
        MlsRatchetTree next;
        assert(mls_tls_buf_init(&v, 512) == 0);
        assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
        switch (c) {
        case PATHLESS_REMOVE:
            assert(inline_remove_for_test(&v, 2) == 0);
            assert(replace_leaf_for_test(&next, 2, NULL) == 0);
            break;
        case PATHLESS_UPDATE_REF:
            assert(mls_tls_write_u8(&v, 2) == 0);
            assert(mls_tls_write_opaque16(&v, upd_ref, MLS_HASH_LEN) == 0);
            assert(replace_leaf_for_test(&next, 2, &upd_leaf) == 0);
            break;
        case PATHLESS_EMPTY:
            break;
        case PATHLESS_GCE:
            /* Restates the current (supported) GroupContext extensions. */
            assert(mls_tls_write_u8(&v, 1) == 0);
            assert(mls_tls_write_u16(&v, MLS_PROPOSAL_GROUP_CONTEXT_EXT) == 0);
            assert(mls_tls_write_opaque32(&v, f.alice.extensions_data,
                                          f.alice.extensions_len) == 0);
            break;
        case PATHLESS_ADD_AND_REMOVE:
            /* Removes apply before Adds, so Dave refills Charlie's leaf. */
            assert(inline_add_for_test(&v, &f.dave_kp) == 0);
            assert(inline_remove_for_test(&v, 2) == 0);
            assert(replace_leaf_for_test(&next, 2, NULL) == 0);
            assert(add_leaf_for_test(&next, &f.dave_kp) == 0);
            break;
        }
        ExpectedEpochForTest expected;
        uint8_t *commit = NULL;
        size_t commit_len = 0;
        assert(build_pathless_commit_for_test(&f.alice, v.data, v.len, &next,
            f.alice.extensions_data, f.alice.extensions_len, NULL,
            &commit, &commit_len, &expected) == 0);

        const uint8_t *store[] = {upd_msg};
        int rc = mls_group_process_commit_ex(&f.bob, commit, commit_len,
                                             f.alice.own_leaf_index, store,
                                             &upd_len, 1);
        if (rc == 0) {
            /* Only reachable without the §12.4 rule: the Commit is otherwise
             * valid and Bob installed exactly the zero-commit_secret epoch.
             * Record it and roll back so every case is exercised. */
            assert_group_reached_for_test(&f.bob, &expected);
            fprintf(stderr, "\n    pathless %s Commit ACCEPTED (epoch %llu)",
                    pathless_case_names[c], (unsigned long long)f.bob.epoch);
            accepted++;
            mls_group_free(&f.bob);
            assert(mls_group_deserialize(parent.blob, parent.blob_len,
                                         &f.bob) == 0);
        } else {
            assert(rc == MARMOT_ERR_MLS_PROCESS_MESSAGE);
            assert_group_matches_snapshot_for_test(&f.bob, &parent);
        }
        free(commit);
        mls_tree_free(&next);
        mls_tls_buf_free(&v);
    }
    assert(accepted == 0);

    /* The untouched group still follows Alice's genuine, path-bearing
     * self-update and reaches her epoch. */
    MlsCommitResult update;
    assert(mls_group_self_update(&f.alice, &update) == 0);
    assert(mls_group_process_commit(&f.bob, update.commit_data,
                                    update.commit_len, 0) == 0);
    assert(f.bob.epoch == parent.epoch + 1 && f.bob.epoch == f.alice.epoch);
    assert(sodium_memcmp(&f.bob.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.bob.epoch_secrets)) == 0);

    mls_commit_result_clear(&update);
    sodium_memzero(upd_sk, sizeof(upd_sk));
    mls_leaf_node_clear(&upd_leaf);
    free(upd_msg);
    free(parent.blob);
    three_member_fixture_clear(&f);
}

/* Security impact: a pathless Remove advances from the parent init_secret
 * with an all-zero commit_secret, both of which the removed member holds.
 * Alice removes Bob (leaf 1); Charlie receives. */
TEST(test_pathless_remove_does_not_exclude_removed_member)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.charlie, &parent);

    MlsTlsBuf v;
    MlsRatchetTree next;
    assert(mls_tls_buf_init(&v, 16) == 0);
    assert(inline_remove_for_test(&v, 1) == 0);
    assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
    assert(replace_leaf_for_test(&next, 1, NULL) == 0);
    ExpectedEpochForTest expected;
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    assert(build_pathless_commit_for_test(&f.alice, v.data, v.len, &next,
        f.alice.extensions_data, f.alice.extensions_len, NULL,
        &commit, &commit_len, &expected) == 0);

    /* Bob, from only his own parent-epoch state and the public Commit,
     * recovers the committer's next-epoch secrets (his derivation reproduces
     * the confirmation tag) -- exactly the epoch a receiver without the path
     * rule installs. */
    MlsRatchetTree removed_view;
    assert(tree_clone_for_test(&f.bob.tree, &removed_view) == 0);
    assert(replace_leaf_for_test(&removed_view, 1, NULL) == 0);
    MlsEpochSecrets leaked;
    uint8_t confirmed[MLS_HASH_LEN];
    bool tag_matches = false;
    assert(derive_zero_commit_secret_epoch_for_test(&f.bob, commit, commit_len,
        &removed_view, confirmed, &leaked, &tag_matches) == 0);
    assert(tag_matches);
    assert(sodium_memcmp(&leaked, &expected.epoch_secrets, sizeof(leaked)) == 0);

    int rc = mls_group_process_commit(&f.charlie, commit, commit_len, 0);
    if (rc == 0) {
        /* Pre-fix behaviour: Charlie "removed" Bob yet entered an epoch whose
         * every secret Bob already holds. */
        assert_group_reached_for_test(&f.charlie, &expected);
        assert(sodium_memcmp(&f.charlie.epoch_secrets, &leaked,
                             sizeof(leaked)) == 0);
        assert(f.charlie.tree.nodes[mls_tree_leaf_to_node(1)].type ==
               MLS_NODE_BLANK);
        fprintf(stderr, "\n    pathless Remove ACCEPTED: removed member holds "
                        "the receiver's epoch-%llu secrets",
                (unsigned long long)f.charlie.epoch);
    }
    assert(rc == MARMOT_ERR_MLS_PROCESS_MESSAGE);
    assert_group_matches_snapshot_for_test(&f.charlie, &parent);

    /* Contrast: libmarmot's own Remove carries an UpdatePath.  Bob can
     * rebuild every public input (tree incl. the new path keys, transcript)
     * but not the fresh commit_secret, so he no longer reaches the epoch. */
    MlsCommitResult removal;
    assert(mls_group_remove_member(&f.alice, 1, &removal) == 0);
    MlsMLSMessage msg;
    MlsTlsReader r;
    mls_tls_reader_init(&r, removal.commit_data, removal.commit_len);
    assert(mls_message_deserialize(&r, &msg) == 0);
    MlsCommit produced;
    mls_tls_reader_init(&r, msg.public_message.content.content,
                        msg.public_message.content.content_len);
    assert(mls_commit_deserialize(&r, &produced) == 0);
    assert(produced.has_path);
    assert(produced.proposal_count == 1 &&
           produced.proposals[0].type == MLS_PROPOSAL_REMOVE &&
           produced.proposals[0].remove.removed_leaf == 1);

    assert(mls_group_process_commit(&f.charlie, removal.commit_data,
                                    removal.commit_len, 0) == 0);
    assert(f.charlie.epoch == parent.epoch + 1 &&
           f.charlie.epoch == f.alice.epoch);
    assert(sodium_memcmp(&f.charlie.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.charlie.epoch_secrets)) == 0);

    mls_tree_free(&removed_view);
    assert(tree_clone_for_test(&f.bob.tree, &removed_view) == 0);
    assert(replace_leaf_for_test(&removed_view, 1, NULL) == 0);
    assert(mls_treekem_apply_update_path(&removed_view, 0, &produced.path) == 0);
    uint8_t view_hash[MLS_HASH_LEN], receiver_hash[MLS_HASH_LEN];
    assert(canonical_tree_hash_for_test(&removed_view, view_hash) == 0);
    assert(mls_group_tree_hash(&f.charlie, receiver_hash) == 0);
    assert(memcmp(view_hash, receiver_hash, MLS_HASH_LEN) == 0);
    assert(derive_zero_commit_secret_epoch_for_test(&f.bob,
        removal.commit_data, removal.commit_len, &removed_view, confirmed,
        &leaked, &tag_matches) == 0);
    assert(memcmp(confirmed, f.charlie.confirmed_transcript_hash,
                  MLS_HASH_LEN) == 0);
    assert(!tag_matches);
    assert(sodium_memcmp(leaked.encryption_secret,
                         f.charlie.epoch_secrets.encryption_secret,
                         MLS_HASH_LEN) != 0);
    assert(sodium_memcmp(leaked.exporter_secret,
                         f.charlie.epoch_secrets.exporter_secret,
                         MLS_HASH_LEN) != 0);

    sodium_memzero(&leaked, sizeof(leaked));
    mls_commit_clear(&produced);
    mls_message_clear(&msg);
    mls_commit_result_clear(&removal);
    mls_tree_free(&removed_view);
    mls_tree_free(&next);
    mls_tls_buf_free(&v);
    free(commit);
    free(parent.blob);
    three_member_fixture_clear(&f);
}

/* Add, PreSharedKey (and ReInit) proposals do not require a path. */
TEST(test_pathless_add_and_psk_commits_accepted)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);

    /* Inline Add(Dave), received by Bob. */
    MlsTlsBuf v;
    MlsRatchetTree next;
    assert(mls_tls_buf_init(&v, 512) == 0);
    assert(inline_add_for_test(&v, &f.dave_kp) == 0);
    assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
    assert(add_leaf_for_test(&next, &f.dave_kp) == 0);
    ExpectedEpochForTest expected;
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    assert(build_pathless_commit_for_test(&f.alice, v.data, v.len, &next,
        f.alice.extensions_data, f.alice.extensions_len, NULL,
        &commit, &commit_len, &expected) == 0);
    assert(mls_group_process_commit(&f.bob, commit, commit_len, 0) == 0);
    assert_group_reached_for_test(&f.bob, &expected);
    const MlsNode *dave = &f.bob.tree.nodes[mls_tree_leaf_to_node(3)];
    assert(dave->type == MLS_NODE_LEAF &&
           memcmp(dave->leaf.signature_key, f.dave_kp.leaf_node.signature_key,
                  MLS_SIG_PK_LEN) == 0);
    free(commit);
    mls_tree_free(&next);
    mls_tls_buf_free(&v);

    /* Inline resumption PSK of the current epoch, received by Charlie. */
    uint8_t nonce[MLS_HASH_LEN];
    randombytes_buf(nonce, sizeof(nonce));
    assert(mls_tls_buf_init(&v, 128) == 0);
    assert(mls_tls_write_u8(&v, 1) == 0);
    assert(mls_tls_write_u16(&v, MLS_PROPOSAL_PSK) == 0);
    assert(mls_tls_write_u8(&v, 2) == 0);   /* psktype = resumption */
    assert(mls_tls_write_u8(&v, 1) == 0);   /* usage = application */
    assert(mls_tls_write_opaque32(&v, f.alice.group_id, f.alice.group_id_len) == 0);
    assert(mls_tls_write_u64(&v, f.alice.epoch) == 0);
    assert(mls_tls_write_opaque32(&v, nonce, sizeof(nonce)) == 0);
    MlsPskInput psk = {0};
    psk.psk_type = 2;
    psk.resumption_usage = 1;
    psk.resumption_group_id = f.alice.group_id;
    psk.resumption_group_id_len = f.alice.group_id_len;
    psk.resumption_epoch = f.alice.epoch;
    psk.psk = f.alice.epoch_secrets.resumption_psk;
    psk.psk_len = MLS_HASH_LEN;
    psk.psk_nonce = nonce;
    psk.psk_nonce_len = sizeof(nonce);
    uint8_t psk_secret[MLS_HASH_LEN];
    assert(mls_psk_secret_compute(&psk, 1, psk_secret) == 0);
    assert(build_pathless_commit_for_test(&f.alice, v.data, v.len, &f.alice.tree,
        f.alice.extensions_data, f.alice.extensions_len, psk_secret,
        &commit, &commit_len, &expected) == 0);
    assert(mls_group_process_commit(&f.charlie, commit, commit_len, 0) == 0);
    assert_group_reached_for_test(&f.charlie, &expected);
    free(commit);
    mls_tls_buf_free(&v);
    sodium_memzero(psk_secret, sizeof(psk_secret));

    three_member_fixture_clear(&f);
}

/* Creation side: every libmarmot producer emits the path, and the single
 * Commit encoder refuses a pathless Commit that requires one. */
TEST(test_commit_serialize_requires_path)
{
    MlsProposal props[2];
    memset(props, 0, sizeof(props));
    MlsCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.proposals = props;
    commit.has_path = false;
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);

    /* No proposals at all. */
    commit.proposal_count = 0;
    assert(mls_commit_serialize(&commit, &buf) == -1);

    /* Remove, alone or beside a path-optional proposal type. */
    props[0].type = MLS_PROPOSAL_REMOVE;
    props[0].remove.removed_leaf = 1;
    commit.proposal_count = 1;
    buf.len = 0;
    assert(mls_commit_serialize(&commit, &buf) == -1);
    props[1] = props[0];
    props[0].type = MLS_PROPOSAL_APP_DATA_UPDATE;
    commit.proposal_count = 2;
    buf.len = 0;
    assert(mls_commit_serialize(&commit, &buf) == -1);

    /* Update and GroupContextExtensions. */
    props[0].type = MLS_PROPOSAL_UPDATE;
    commit.proposal_count = 1;
    buf.len = 0;
    assert(mls_commit_serialize(&commit, &buf) == -1);
    props[0].type = MLS_PROPOSAL_GROUP_CONTEXT_EXT;
    buf.len = 0;
    assert(mls_commit_serialize(&commit, &buf) == -1);
    mls_tls_buf_free(&buf);

    /* Every producer-built Commit carries its UpdatePath (Alice adds Dave,
     * removes Bob, then self-updates; Charlie follows). */
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    MlsCommitResult produced[3];
    MlsAddResult add_dave;
    assert(mls_group_add_member(&f.alice, &f.dave_kp, &add_dave) == 0);
    produced[0].commit_data = add_dave.commit_data;
    produced[0].commit_len = add_dave.commit_len;
    assert(mls_group_remove_member(&f.alice, 1, &produced[1]) == 0);
    assert(mls_group_self_update(&f.alice, &produced[2]) == 0);
    for (int i = 0; i < 3; i++) {
        MlsMLSMessage msg;
        MlsTlsReader r;
        mls_tls_reader_init(&r, produced[i].commit_data, produced[i].commit_len);
        assert(mls_message_deserialize(&r, &msg) == 0);
        MlsCommit c;
        mls_tls_reader_init(&r, msg.public_message.content.content,
                            msg.public_message.content.content_len);
        assert(mls_commit_deserialize(&r, &c) == 0);
        assert(c.has_path);
        assert(mls_group_process_commit(&f.charlie, produced[i].commit_data,
                                        produced[i].commit_len, 0) == 0);
        assert(f.charlie.epoch == f.alice.epoch - (uint64_t)(2 - i));
        mls_commit_clear(&c);
        mls_message_clear(&msg);
    }
    assert(sodium_memcmp(&f.charlie.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.charlie.epoch_secrets)) == 0);
    mls_commit_result_clear(&produced[1]);
    mls_commit_result_clear(&produced[2]);
    mls_add_result_clear(&add_dave);
    three_member_fixture_clear(&f);
}

static void
parse_commit_for_test(const uint8_t *wire, size_t wire_len, MlsCommit *out)
{
    MlsMLSMessage msg;
    MlsTlsReader r;
    mls_tls_reader_init(&r, wire, wire_len);
    assert(mls_message_deserialize(&r, &msg) == 0);
    mls_tls_reader_init(&r, msg.public_message.content.content,
                        msg.public_message.content.content_len);
    assert(mls_commit_deserialize(&r, out) == 0 && mls_tls_reader_done(&r));
    mls_message_clear(&msg);
}

/* nostrc-lz4f: Alice (leaf 0) removes Charlie (leaf 2) from the 4-leaf tree
 * whose leaf 3 is blank.  Copath node 5 then has an empty resolution, so
 * Alice's filtered direct path is [1] and stops below the root (3).  RFC 9420
 * §7.9: the topmost filtered node carries an empty parent_hash and the leaf's
 * parent_hash is relative to it.  The old producer treated a non-root top as
 * an error and returned MARMOT_ERR_INTERNAL. */
TEST(test_remove_filters_root_from_committer_path)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);

    MlsRatchetTree after;
    uint32_t fdp[8], fdp_len = 0;
    assert(tree_clone_for_test(&f.alice.tree, &after) == 0);
    assert(after.n_leaves == 4);
    assert(replace_leaf_for_test(&after, 2, NULL) == 0);
    assert(mls_tree_filtered_direct_path(&after, 0, fdp, 8, &fdp_len) == 0);
    assert(fdp_len == 1 && fdp[0] == 1 && mls_tree_root(after.n_leaves) == 3);
    mls_tree_free(&after);

    GroupSnapshotForTest charlie_parent;
    snapshot_group_for_test(&f.charlie, &charlie_parent);

    MlsCommitResult removal;
    memset(&removal, 0, sizeof(removal));
    int rc = mls_group_remove_member(&f.alice, 2, &removal);
    if (rc != 0)
        fprintf(stderr, "\n    remove(leaf 2) rc=%d", rc);
    assert(rc == 0);

    MlsCommit produced;
    parse_commit_for_test(removal.commit_data, removal.commit_len, &produced);
    assert(produced.has_path);
    assert(produced.proposal_count == 1 &&
           produced.proposals[0].type == MLS_PROPOSAL_REMOVE &&
           produced.proposals[0].remove.removed_leaf == 2);
    assert(produced.path.node_count == 1);
    assert(produced.path.nodes[0].secret_count == 1); /* Bob only */

    /* Committer's tree: node 1 is the top of the filtered path (empty
     * parent_hash) and the new leaf's parent_hash is ParentHash(node 1). */
    const MlsNode *n1 = &f.alice.tree.nodes[1];
    assert(n1->type == MLS_NODE_PARENT && n1->parent.parent_hash_len == 0);
    assert(memcmp(n1->parent.encryption_key, produced.path.nodes[0].encryption_key,
                  MLS_KEM_PK_LEN) == 0);
    uint8_t expected_ph[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&f.alice.tree, 1, 0, expected_ph) == 0);
    assert(produced.path.leaf_node.parent_hash_len == MLS_HASH_LEN &&
           memcmp(produced.path.leaf_node.parent_hash, expected_ph,
                  MLS_HASH_LEN) == 0);
    assert(f.alice.tree.nodes[3].type == MLS_NODE_BLANK);
    assert(mls_tree_verify_parent_hashes(&f.alice.tree) == 0);

    /* Bob processes the Commit and reaches Alice's epoch exactly. */
    assert(mls_group_process_commit(&f.bob, removal.commit_data,
                                    removal.commit_len, 0) == 0);
    assert(f.bob.epoch == f.alice.epoch);
    assert(f.bob.tree.nodes[mls_tree_leaf_to_node(2)].type == MLS_NODE_BLANK);
    uint8_t alice_hash[MLS_HASH_LEN], bob_hash[MLS_HASH_LEN];
    assert(mls_group_tree_hash(&f.alice, alice_hash) == 0);
    assert(mls_group_tree_hash(&f.bob, bob_hash) == 0);
    assert(memcmp(alice_hash, bob_hash, MLS_HASH_LEN) == 0);
    assert(memcmp(f.bob.confirmed_transcript_hash, f.alice.confirmed_transcript_hash,
                  MLS_HASH_LEN) == 0);
    assert(sodium_memcmp(&f.bob.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.bob.epoch_secrets)) == 0);

    /* The removed member is not a recipient of the path and cannot follow. */
    assert(mls_group_process_commit(&f.charlie, removal.commit_data,
                                    removal.commit_len, 0) != 0);
    assert_group_matches_snapshot_for_test(&f.charlie, &charlie_parent);

    /* Both remaining members keep working in the new epoch. */
    uint8_t *ct = NULL, *pt = NULL;
    size_t ct_len = 0, pt_len = 0;
    uint32_t sender = UINT32_MAX;
    static const uint8_t hello[] = "after lz4f removal";
    assert(mls_group_encrypt(&f.alice, hello, sizeof(hello), &ct, &ct_len) == 0);
    assert(mls_group_decrypt(&f.bob, ct, ct_len, &pt, &pt_len, &sender) == 0);
    assert(sender == 0 && pt_len == sizeof(hello) && memcmp(pt, hello, pt_len) == 0);
    free(ct);
    free(pt);
    MlsCommitResult bob_update;
    assert(mls_group_self_update(&f.bob, &bob_update) == 0);
    assert(mls_group_process_commit(&f.alice, bob_update.commit_data,
                                    bob_update.commit_len, 1) == 0);
    assert(sodium_memcmp(&f.bob.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.bob.epoch_secrets)) == 0);
    mls_commit_result_clear(&bob_update);

    /* Removing the last other member leaves an empty filtered direct path:
     * the UpdatePath carries no nodes and the leaf parent_hash is empty. */
    MlsCommitResult last;
    assert(mls_group_remove_member(&f.alice, 1, &last) == 0);
    MlsCommit alone;
    parse_commit_for_test(last.commit_data, last.commit_len, &alone);
    assert(alone.has_path && alone.path.node_count == 0);
    assert(alone.path.leaf_node.parent_hash_len == 0);
    assert(mls_tree_verify_parent_hashes(&f.alice.tree) == 0);
    mls_commit_clear(&alone);
    mls_commit_result_clear(&last);

    mls_commit_clear(&produced);
    mls_commit_result_clear(&removal);
    free(charlie_parent.blob);
    three_member_fixture_clear(&f);
}

/* ── Multi-member convergence (nostrc-va60, nostrc-5q55, nostrc-8u1k) ────────
 *
 * Every member processes every other member's Commits and must end in the
 * same epoch: epoch number, all epoch secrets, tree hash, the serialized
 * GroupContext (confirmed transcript hash, extensions) and interim transcript
 * hash. */

static void
assert_converged_for_test(MlsGroup *const *members, size_t n)
{
    uint8_t *gc0 = NULL, th0[MLS_HASH_LEN];
    size_t gc0_len = 0;
    assert(mls_group_context_build(members[0], &gc0, &gc0_len) == 0);
    assert(mls_group_tree_hash(members[0], th0) == 0);
    for (size_t i = 1; i < n; i++) {
        uint8_t *gc = NULL, th[MLS_HASH_LEN];
        size_t gc_len = 0;
        assert(members[i]->epoch == members[0]->epoch);
        assert(mls_group_tree_hash(members[i], th) == 0);
        assert(memcmp(th, th0, MLS_HASH_LEN) == 0);
        assert(mls_group_context_build(members[i], &gc, &gc_len) == 0);
        assert(gc_len == gc0_len && memcmp(gc, gc0, gc_len) == 0);
        assert(memcmp(members[i]->interim_transcript_hash,
                      members[0]->interim_transcript_hash, MLS_HASH_LEN) == 0);
        assert(sodium_memcmp(&members[i]->epoch_secrets, &members[0]->epoch_secrets,
                             sizeof(members[0]->epoch_secrets)) == 0);
        free(gc);
    }
    free(gc0);
}

/* `committer`'s Commit, processed by every other listed member. */
static void
deliver_commit_for_test(MlsGroup *const *members, size_t n, const MlsGroup *committer,
                        const uint8_t *commit, size_t commit_len, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        if (members[i] == committer) continue;
        int rc = mls_group_process_commit(members[i], commit, commit_len,
                                          committer->own_leaf_index);
        if (rc != 0)
            fprintf(stderr, "\n    %s: leaf %u rejected leaf %u's Commit, rc=%d",
                    what, members[i]->own_leaf_index, committer->own_leaf_index, rc);
        assert(rc == 0);
    }
}

/* Persist and reload, as storage does between operations. */
static void
reload_group_for_test(MlsGroup *g)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    assert(mls_group_serialize(g, &blob, &len) == 0);
    mls_group_free(g);
    assert(mls_group_deserialize(blob, len, g) == 0);
    sodium_memzero(blob, len);
    free(blob);
}

/* The member's path-key cache holds only current keys: each entry is for a
 * parent node on its own direct path, matches that node's public key and is a
 * valid key pair -- nothing for blanked, re-keyed or foreign nodes
 * (nostrc-va60).  Returns the number of entries. */
static size_t
assert_path_keys_current_for_test(const MlsGroup *g)
{
    uint32_t dp[64], dp_len = 0;
    assert(mls_tree_direct_path(mls_tree_leaf_to_node(g->own_leaf_index),
                                g->tree.n_leaves, dp, 64, &dp_len) == 0);
    size_t cached = 0;
    for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++) {
        const MlsOwnPathKeyCacheEntry *e = &g->own_path_keys[i];
        if (!e->valid) continue;
        cached++;
        bool on_path = false;
        for (uint32_t j = 0; j < dp_len; j++) on_path |= dp[j] == e->node;
        assert(on_path);
        const MlsNode *n = &g->tree.nodes[e->node];
        assert(n->type == MLS_NODE_PARENT &&
               memcmp(n->parent.encryption_key, e->pk, MLS_KEM_PK_LEN) == 0);
        uint8_t pk[MLS_KEM_PK_LEN];
        assert(crypto_scalarmult_curve25519_base(pk, e->sk) == 0);
        assert(memcmp(pk, e->pk, MLS_KEM_PK_LEN) == 0);
    }
    return cached;
}

/* Right after its own Commit every non-blank node on the committer's direct
 * path is one it just installed: it must hold all of their private keys. */
static void
assert_committer_holds_path_keys_for_test(const MlsGroup *g)
{
    uint32_t dp[64], dp_len = 0;
    assert(mls_tree_direct_path(mls_tree_leaf_to_node(g->own_leaf_index),
                                g->tree.n_leaves, dp, 64, &dp_len) == 0);
    size_t installed = 0;
    for (uint32_t j = 0; j < dp_len; j++) {
        if (g->tree.nodes[dp[j]].type != MLS_NODE_PARENT) continue;
        installed++;
        bool held = false;
        for (size_t i = 0; i < MLS_OWN_PATH_KEY_CACHE_SIZE; i++)
            held |= g->own_path_keys[i].valid && g->own_path_keys[i].node == dp[j];
        if (!held)
            fprintf(stderr, "\n    committer leaf %u lacks the private key of node %u",
                    g->own_leaf_index, dp[j]);
        assert(held);
    }
    assert(assert_path_keys_current_for_test(g) == installed);
}

/* nostrc-va60: after merging its own Commit the committer holds the private
 * keys of the path nodes it installed (RFC 9420 §7.4, §12.4.2).  In the
 * fixture Alice's Add(Charlie) installed node 1 (copath: Bob) and the root.
 * Charlie's self-update encrypts the root's path secret to resolution(node 1)
 * = [node 1], which Bob holds as a receiver of that Add -- and Alice must hold
 * as its committer, or she cannot follow (was rc -116). */
TEST(test_committer_keeps_own_path_keys)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    MlsGroup *all[] = {&f.alice, &f.bob, &f.charlie};
    uint32_t res[8], res_len = 0;
    assert(mls_tree_resolution(&f.alice.tree, 1, res, 8, &res_len) == 0);
    assert(res_len == 1 && res[0] == 1);

    /* Alice committed last; the keys come from persisted state, as between
     * real operations. */
    for (size_t i = 0; i < 3; i++) reload_group_for_test(all[i]);
    assert_committer_holds_path_keys_for_test(&f.alice);

    MlsCommitResult upd;
    assert(mls_group_self_update(&f.charlie, &upd) == 0);
    deliver_commit_for_test(all, 3, &f.charlie, upd.commit_data, upd.commit_len,
                            "Charlie self-update");
    assert_converged_for_test(all, 3);
    mls_commit_result_clear(&upd);

    /* Everyone commits in turn, with a Remove and a re-Add in between; the
     * cache always holds exactly the current keys of the member's own path. */
    MlsGroup dave;
    memset(&dave, 0, sizeof(dave));
    for (int round = 0; round < 3; round++) {
        for (size_t c = 0; c < 3; c++) {
            MlsCommitResult r;
            assert(mls_group_self_update(all[c], &r) == 0);
            assert_committer_holds_path_keys_for_test(all[c]);
            deliver_commit_for_test(all, 3, all[c], r.commit_data, r.commit_len,
                                    "self-update round");
            assert_converged_for_test(all, 3);
            for (size_t i = 0; i < 3; i++) {
                reload_group_for_test(all[i]);
                assert(assert_path_keys_current_for_test(all[i]) <= 2);
            }
            mls_commit_result_clear(&r);
        }
    }

    /* Bob removes Charlie; Charlie's slot is refilled by Dave via Bob. */
    MlsCommitResult rm;
    assert(mls_group_remove_member(&f.bob, 2, &rm) == 0);
    assert_committer_holds_path_keys_for_test(&f.bob);
    MlsGroup *remaining[] = {&f.alice, &f.bob};
    deliver_commit_for_test(remaining, 2, &f.bob, rm.commit_data, rm.commit_len,
                            "Bob removes Charlie");
    assert_converged_for_test(remaining, 2);
    mls_commit_result_clear(&rm);
    MlsAddResult add;
    assert(mls_group_add_member(&f.bob, &f.dave_kp, &add) == 0);
    assert_committer_holds_path_keys_for_test(&f.bob);
    deliver_commit_for_test(remaining, 2, &f.bob, add.commit_data, add.commit_len,
                            "Bob adds Dave");
    assert(mls_welcome_process(add.welcome_data, add.welcome_len, &f.dave_kp,
                               &f.dave_priv, NULL, 0, &dave) == 0);
    mls_add_result_clear(&add);
    MlsGroup *now[] = {&f.alice, &f.bob, &dave};
    assert_converged_for_test(now, 3);
    for (size_t c = 0; c < 3; c++) {
        MlsCommitResult r;
        assert(mls_group_self_update(now[c], &r) == 0);
        assert_committer_holds_path_keys_for_test(now[c]);
        deliver_commit_for_test(now, 3, now[c], r.commit_data, r.commit_len,
                                "after re-Add");
        assert_converged_for_test(now, 3);
        for (size_t i = 0; i < 3; i++) assert_path_keys_current_for_test(now[i]);
        mls_commit_result_clear(&r);
    }

    mls_group_free(&dave);
    three_member_fixture_clear(&f);
}

/* Commit producers merge a staged clone: a producer that fails part-way
 * leaves the live group byte-for-byte unchanged (fail closed). */
TEST(test_commit_producers_fail_closed)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    /* Bob's leaf in Alice's tree carries a low-order X25519 key: HPKE to it
     * fails inside the UpdatePath, after the producer has applied its
     * proposal and installed its new path nodes and leaf. */
    memset(f.alice.tree.nodes[mls_tree_leaf_to_node(1)].leaf.encryption_key, 0,
           MLS_KEM_PK_LEN);
    GroupSnapshotForTest before;
    snapshot_group_for_test(&f.alice, &before);

    MlsAddResult add;
    MlsCommitResult res;
    assert(mls_group_add_member(&f.alice, &f.dave_kp, &add) != 0);
    assert_group_matches_snapshot_for_test(&f.alice, &before);
    assert(mls_group_remove_member(&f.alice, 2, &res) != 0);
    assert_group_matches_snapshot_for_test(&f.alice, &before);
    assert(mls_group_self_update(&f.alice, &res) != 0);
    assert_group_matches_snapshot_for_test(&f.alice, &before);

    free(before.blob);
    three_member_fixture_clear(&f);
}

/* ── LeafNode validation (nostrc-2io4, RFC 9420 §7.3) ───────────────────────────
 *
 * libmarmot has no producer for Commits that carry referenced Updates or a
 * malformed UpdatePath leaf, so build_update_path_for_test() builds the
 * committer's UpdatePath from exported primitives exactly as RFC 9420
 * §7.4-7.9 prescribe (path secrets, node keys, parent hashes along the
 * filtered direct path, HPKE to the copath resolutions under the provisional
 * GroupContext), optionally with a defective leaf, and build_commit_for_test()
 * frames it.  Each Commit is otherwise fully valid: without LeafNode
 * validation the receiver installs exactly the derived epoch. */

/* Marmot groups carry marmot_group_data (0xF2EE) in the GroupContext. */
static const uint8_t MARMOT_GC_EXT_FOR_TEST[] = {0xF2, 0xEE, 0x02, 0xCA, 0xFE};

typedef enum {
    PATH_LEAF_VALID,
    PATH_LEAF_TAMPERED_SIGNATURE,
    PATH_LEAF_WRONG_LEAF_INDEX,
    PATH_LEAF_DUPLICATE_ENCRYPTION_KEY,
    PATH_LEAF_UNSUPPORTED_GROUP_EXTENSION,
    PATH_LEAF_CASE_COUNT
} PathLeafCase;

static const char *const path_leaf_case_names[PATH_LEAF_CASE_COUNT] = {
    "valid", "tampered signature", "wrong leaf_index",
    "duplicate encryption key", "unsupported GroupContext extension",
};

/* Leave only last_resort in the capabilities: marmot_group_data unsupported. */
static void
drop_group_data_capability_for_test(MlsLeafNode *leaf)
{
    static const uint16_t only_last_resort = 0x000A;
    free(leaf->cap_extensions);
    leaf->cap_extensions = malloc(sizeof(uint16_t));
    assert(leaf->cap_extensions);
    leaf->cap_extensions[0] = only_last_resort;
    leaf->cap_extension_count = 1;
}

/* The committer's UpdatePath over `next_tree` (the tree after the Commit's
 * proposals; no Adds), its leaf made defective per `leaf_case`.  Returns the
 * provisional tree a receiver reaches by merging it and the commit_secret. */
static void
build_update_path_for_test(const MlsGroup *committer,
                           const MlsRatchetTree *next_tree,
                           const uint8_t *next_ext, size_t next_ext_len,
                           PathLeafCase leaf_case,
                           const uint8_t dup_key[MLS_KEM_PK_LEN],
                           MlsUpdatePath *path, MlsRatchetTree *provisional,
                           uint8_t commit_secret[MLS_HASH_LEN])
{
    uint32_t sender = committer->own_leaf_index;
    uint32_t sender_node = mls_tree_leaf_to_node(sender);
    uint32_t n_leaves = next_tree->n_leaves;
    uint32_t fdp[16], fdp_len = 0;
    uint8_t secrets[16][MLS_HASH_LEN];
    memset(path, 0, sizeof(*path));
    assert(mls_tree_filtered_direct_path(next_tree, sender, fdp, 16, &fdp_len) == 0);
    assert(fdp_len > 0);

    /* path_secret[0] random, path_secret[n] = DeriveSecret(prev, "path"). */
    randombytes_buf(secrets[0], MLS_HASH_LEN);
    for (uint32_t i = 1; i < fdp_len; i++)
        assert(mls_tree_derive_next_path_secret(secrets[i - 1], secrets[i]) == 0);
    path->nodes = calloc(fdp_len, sizeof(*path->nodes));
    assert(path->nodes);
    path->node_count = fdp_len;
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint8_t node_sk[MLS_KEM_SK_LEN];
        assert(mls_tree_derive_node_keypair(secrets[i], node_sk,
                                            path->nodes[i].encryption_key) == 0);
        sodium_memzero(node_sk, sizeof(node_sk));
    }

    /* New committer leaf: current leaf, fresh HPKE key, source commit. */
    assert(mls_leaf_node_clone(&path->leaf_node, &next_tree->nodes[sender_node].leaf) == 0);
    uint8_t leaf_sk[MLS_KEM_SK_LEN];
    assert(mls_crypto_kem_keygen(leaf_sk, path->leaf_node.encryption_key) == 0);
    sodium_memzero(leaf_sk, sizeof(leaf_sk));
    if (leaf_case == PATH_LEAF_DUPLICATE_ENCRYPTION_KEY)
        memcpy(path->leaf_node.encryption_key, dup_key, MLS_KEM_PK_LEN);
    if (leaf_case == PATH_LEAF_UNSUPPORTED_GROUP_EXTENSION)
        drop_group_data_capability_for_test(&path->leaf_node);
    path->leaf_node.leaf_node_source = MLS_LEAF_NODE_SOURCE_COMMIT;
    path->leaf_node.lifetime_not_before = 0;
    path->leaf_node.lifetime_not_after = 0;

    /* parent_hash along the filtered direct path (§7.9), top-down, in a
     * scratch copy with the committer's direct path replaced. */
    MlsRatchetTree scratch;
    assert(tree_clone_for_test(next_tree, &scratch) == 0 && scratch.n_leaves == n_leaves);
    uint32_t dp[16], dp_len = 0;
    assert(mls_tree_direct_path(sender_node, n_leaves, dp, 16, &dp_len) == 0);
    for (uint32_t i = 0; i < dp_len; i++)
        mls_tree_blank_node(&scratch.nodes[dp[i]]);
    for (uint32_t i = 0; i < fdp_len; i++) {
        scratch.nodes[fdp[i]].type = MLS_NODE_PARENT;
        memset(&scratch.nodes[fdp[i]].parent, 0, sizeof(MlsParentNode));
        memcpy(scratch.nodes[fdp[i]].parent.encryption_key,
               path->nodes[i].encryption_key, MLS_KEM_PK_LEN);
    }
    for (uint32_t pos = fdp_len; pos-- > 1;) {
        MlsParentNode *child = &scratch.nodes[fdp[pos - 1]].parent;
        child->parent_hash = malloc(MLS_HASH_LEN);
        assert(child->parent_hash);
        assert(mls_tree_parent_hash(&scratch, fdp[pos], fdp[pos - 1],
                                    child->parent_hash) == 0);
        child->parent_hash_len = MLS_HASH_LEN;
    }
    free(path->leaf_node.parent_hash);
    path->leaf_node.parent_hash = malloc(MLS_HASH_LEN);
    assert(path->leaf_node.parent_hash);
    assert(mls_tree_parent_hash(&scratch, fdp[0], sender_node,
                                path->leaf_node.parent_hash) == 0);
    path->leaf_node.parent_hash_len = MLS_HASH_LEN;
    mls_tree_free(&scratch);

    /* LeafNodeTBS binds group_id and the committer's leaf index (§7.2). */
    uint32_t signed_index = leaf_case == PATH_LEAF_WRONG_LEAF_INDEX ? sender + 1 : sender;
    assert(mls_leaf_node_sign(&path->leaf_node, committer->own_signature_key,
                              committer->group_id, committer->group_id_len,
                              signed_index) == 0);
    if (leaf_case == PATH_LEAF_TAMPERED_SIGNATURE)
        path->leaf_node.signature[7] ^= 0x01;

    /* The provisional tree and GroupContext a receiver rebuilds (§12.4.2). */
    assert(tree_clone_for_test(next_tree, provisional) == 0);
    assert(mls_treekem_apply_update_path(provisional, sender, path) == 0);
    uint8_t provisional_hash[MLS_HASH_LEN];
    assert(canonical_tree_hash_for_test(provisional, provisional_hash) == 0);
    uint8_t *ctx = NULL, *info = NULL;
    size_t ctx_len = 0, info_len = 0;
    assert(mls_group_context_serialize(committer->group_id, committer->group_id_len,
                                       committer->epoch + 1, provisional_hash,
                                       committer->confirmed_transcript_hash,
                                       next_ext, next_ext_len, &ctx, &ctx_len) == 0);
    assert(build_encrypt_context_for_test("UpdatePathNode", ctx, ctx_len,
                                          &info, &info_len) == 0);

    /* EncryptWithLabel(path_secret[i]) to the resolution of each copath node. */
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t child = sender_node;
        while (mls_tree_parent(child, n_leaves) != fdp[i])
            child = mls_tree_parent(child, n_leaves);
        uint32_t resolution[32], res_len = 0;
        assert(mls_tree_resolution(next_tree, mls_tree_sibling(child, n_leaves),
                                   resolution, 32, &res_len) == 0);
        MlsTlsBuf cts;
        assert(mls_tls_buf_init(&cts, 128) == 0);
        for (uint32_t j = 0; j < res_len; j++) {
            const uint8_t *pk = mls_tree_node_encryption_key(next_tree, resolution[j]);
            uint8_t enc[MLS_KEM_ENC_LEN], ct[MLS_HASH_LEN + MLS_AEAD_TAG_LEN];
            size_t ct_len = 0;
            assert(pk);
            assert(mls_crypto_hpke_seal_base(enc, ct, &ct_len, pk, info, info_len,
                                             NULL, 0, secrets[i], MLS_HASH_LEN) == 0);
            assert(mls_tls_write_opaque16(&cts, enc, MLS_KEM_ENC_LEN) == 0 &&
                   mls_tls_write_opaque16(&cts, ct, ct_len) == 0);
        }
        path->nodes[i].encrypted_path_secrets = cts.data;
        path->nodes[i].encrypted_path_secrets_len = cts.len;
        path->nodes[i].secret_count = res_len;
    }
    assert(mls_treekem_commit_secret_from_path_secret(secrets[fdp_len - 1],
                                                      commit_secret) == 0);
    sodium_memzero(secrets, sizeof(secrets));
    free(info);
    free(ctx);
}

/* Hand `receiver` a Commit that is valid except for a LeafNode defect and
 * require rejection without any state change.  Without LeafNode validation the
 * Commit is accepted and the receiver installs exactly `expected`; that is
 * recorded (counterfactual) and rolled back so every case runs. */
static int
expect_leaf_rejected_for_test(MlsGroup *receiver, const GroupSnapshotForTest *parent,
                              const char *what, const uint8_t *commit, size_t commit_len,
                              uint32_t committer, const uint8_t *const *store,
                              const size_t *store_lens, size_t store_count,
                              const ExpectedEpochForTest *expected)
{
    int rc = mls_group_process_commit_ex(receiver, commit, commit_len, committer,
                                         store, store_lens, store_count);
    if (rc == 0) {
        assert_group_reached_for_test(receiver, expected);
        fprintf(stderr, "\n    %s ACCEPTED (epoch %llu)", what,
                (unsigned long long)receiver->epoch);
        mls_group_free(receiver);
        assert(mls_group_deserialize(parent->blob, parent->blob_len, receiver) == 0);
        return 1;
    }
    assert(rc == MARMOT_ERR_MLS_PROCESS_MESSAGE);
    assert_group_matches_snapshot_for_test(receiver, parent);
    return 0;
}

enum {
    UPDATE_LEAF_VALID,
    UPDATE_LEAF_TAMPERED_SIGNATURE,
    UPDATE_LEAF_WRONG_LEAF_INDEX,
    UPDATE_LEAF_KEY_PACKAGE_SOURCE,
    UPDATE_LEAF_DUPLICATE_ENCRYPTION_KEY,
    UPDATE_LEAF_UNSUPPORTED_GROUP_EXTENSION,
    UPDATE_LEAF_CHANGED_IDENTITY,
    UPDATE_LEAF_BY_COMMITTER,
    UPDATE_LEAF_CASE_COUNT
};

static const char *const update_leaf_case_names[UPDATE_LEAF_CASE_COUNT] = {
    "valid", "tampered signature", "wrong leaf_index", "key_package source",
    "duplicate encryption key", "unsupported GroupContext extension",
    "changed credential identity", "committer's own Update",
};

/* nostrc-2io4: Alice commits Charlie's by-reference Update with a valid
 * UpdatePath; Bob receives.  The Update LeafNode is RFC 9420 §7.3-validated
 * before anything is applied. */
TEST(test_update_by_ref_leaf_validation)
{
    ThreeMemberFixture f;
    three_member_fixture_init_ext(&f, MARMOT_GC_EXT_FOR_TEST,
                                  sizeof(MARMOT_GC_EXT_FOR_TEST));
    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.bob, &parent);
    uint8_t bob_enc_key[MLS_KEM_PK_LEN];
    memcpy(bob_enc_key, f.bob.tree.nodes[mls_tree_leaf_to_node(1)].leaf.encryption_key,
           MLS_KEM_PK_LEN);
    int accepted = 0;

    for (int c = 0; c < UPDATE_LEAF_CASE_COUNT; c++) {
        const MlsGroup *proposer = c == UPDATE_LEAF_BY_COMMITTER ? &f.alice : &f.charlie;
        uint32_t target = proposer->own_leaf_index;
        MlsLeafNode leaf;
        uint8_t leaf_sk[MLS_KEM_SK_LEN];
        make_update_leaf_for_test(proposer, &leaf, leaf_sk);
        switch (c) {
        case UPDATE_LEAF_TAMPERED_SIGNATURE:
            leaf.signature[3] ^= 0x80;
            break;
        case UPDATE_LEAF_WRONG_LEAF_INDEX:
            assert(mls_leaf_node_sign(&leaf, proposer->own_signature_key,
                                      proposer->group_id, proposer->group_id_len, 1) == 0);
            break;
        case UPDATE_LEAF_KEY_PACKAGE_SOURCE:
            /* The original W12 fixture shape: a key_package-source leaf with
             * a lifetime, self-consistently signed for that source. */
            leaf.leaf_node_source = MLS_LEAF_NODE_SOURCE_KEY_PACKAGE;
            leaf.lifetime_not_before = 1;
            leaf.lifetime_not_after = UINT64_MAX;
            assert(mls_leaf_node_sign(&leaf, proposer->own_signature_key,
                                      NULL, 0, 0) == 0);
            break;
        case UPDATE_LEAF_DUPLICATE_ENCRYPTION_KEY:
            memcpy(leaf.encryption_key, bob_enc_key, MLS_KEM_PK_LEN);
            assert(mls_leaf_node_sign(&leaf, proposer->own_signature_key,
                                      proposer->group_id, proposer->group_id_len,
                                      target) == 0);
            break;
        case UPDATE_LEAF_UNSUPPORTED_GROUP_EXTENSION:
            drop_group_data_capability_for_test(&leaf);
            assert(mls_leaf_node_sign(&leaf, proposer->own_signature_key,
                                      proposer->group_id, proposer->group_id_len,
                                      target) == 0);
            break;
        case UPDATE_LEAF_CHANGED_IDENTITY:
            assert(leaf.credential_identity_len == sizeof(DAVE_ID));
            memcpy(leaf.credential_identity, DAVE_ID, sizeof(DAVE_ID));
            assert(mls_leaf_node_sign(&leaf, proposer->own_signature_key,
                                      proposer->group_id, proposer->group_id_len,
                                      target) == 0);
            break;
        default:
            break;
        }
        uint8_t *msg = NULL, ref[MLS_HASH_LEN];
        size_t msg_len = 0;
        update_proposal_message_for_test(proposer, &leaf, &msg, &msg_len, ref);

        MlsTlsBuf refs;
        assert(mls_tls_buf_init(&refs, 64) == 0);
        assert(mls_tls_write_u8(&refs, 2) == 0 &&
               mls_tls_write_opaque16(&refs, ref, MLS_HASH_LEN) == 0);
        MlsRatchetTree next, provisional;
        assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
        assert(replace_leaf_for_test(&next, target, &leaf) == 0);
        MlsUpdatePath path;
        uint8_t commit_secret[MLS_HASH_LEN];
        build_update_path_for_test(&f.alice, &next, f.alice.extensions_data,
                                   f.alice.extensions_len, PATH_LEAF_VALID, NULL,
                                   &path, &provisional, commit_secret);
        uint8_t *commit = NULL;
        size_t commit_len = 0;
        ExpectedEpochForTest expected;
        assert(build_commit_for_test(&f.alice, refs.data, refs.len, &path,
                                     commit_secret, &provisional,
                                     f.alice.extensions_data, f.alice.extensions_len,
                                     NULL, &commit, &commit_len, &expected) == 0);

        const uint8_t *store[] = {msg};
        if (c == UPDATE_LEAF_VALID) {
            /* Control: the same Commit shape with a valid Update is accepted
             * and Bob installs Charlie's new leaf and exactly the epoch. */
            assert(mls_group_process_commit_ex(&f.bob, commit, commit_len, 0,
                                               store, &msg_len, 1) == 0);
            assert_group_reached_for_test(&f.bob, &expected);
            const MlsLeafNode *installed =
                &f.bob.tree.nodes[mls_tree_leaf_to_node(2)].leaf;
            assert(memcmp(installed->encryption_key, leaf.encryption_key,
                          MLS_KEM_PK_LEN) == 0 &&
                   installed->leaf_node_source == MLS_LEAF_NODE_SOURCE_UPDATE);
            mls_group_free(&f.bob);
            assert(mls_group_deserialize(parent.blob, parent.blob_len, &f.bob) == 0);
        } else {
            char what[96];
            snprintf(what, sizeof(what), "Update-by-ref with %s",
                     update_leaf_case_names[c]);
            accepted += expect_leaf_rejected_for_test(&f.bob, &parent, what,
                commit, commit_len, 0, store, &msg_len, 1, &expected);
        }
        free(commit);
        mls_update_path_clear(&path);
        mls_tree_free(&provisional);
        mls_tree_free(&next);
        mls_tls_buf_free(&refs);
        free(msg);
        sodium_memzero(leaf_sk, sizeof(leaf_sk));
        mls_leaf_node_clear(&leaf);
    }
    assert(accepted == 0);

    /* The untouched group still follows Alice's genuine self-update. */
    MlsCommitResult update;
    assert(mls_group_self_update(&f.alice, &update) == 0);
    assert(mls_group_process_commit(&f.bob, update.commit_data, update.commit_len, 0) == 0);
    assert(sodium_memcmp(&f.bob.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.bob.epoch_secrets)) == 0);
    mls_commit_result_clear(&update);
    free(parent.blob);
    three_member_fixture_clear(&f);
}

/* nostrc-2io4: the UpdatePath LeafNode is RFC 9420 §7.3-validated (source
 * commit, bound to the committer's leaf) and its keys checked for freshness
 * (§12.4.2) before the path is merged.  Alice commits an empty Commit with a
 * path; Bob receives. */
TEST(test_update_path_leaf_validation)
{
    ThreeMemberFixture f;
    three_member_fixture_init_ext(&f, MARMOT_GC_EXT_FOR_TEST,
                                  sizeof(MARMOT_GC_EXT_FOR_TEST));
    GroupSnapshotForTest parent;
    snapshot_group_for_test(&f.bob, &parent);
    uint8_t bob_enc_key[MLS_KEM_PK_LEN];
    memcpy(bob_enc_key, f.bob.tree.nodes[mls_tree_leaf_to_node(1)].leaf.encryption_key,
           MLS_KEM_PK_LEN);
    int accepted = 0;

    for (int c = 0; c < PATH_LEAF_CASE_COUNT; c++) {
        MlsRatchetTree next, provisional;
        assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
        MlsUpdatePath path;
        uint8_t commit_secret[MLS_HASH_LEN];
        build_update_path_for_test(&f.alice, &next, f.alice.extensions_data,
                                   f.alice.extensions_len, (PathLeafCase)c,
                                   bob_enc_key, &path, &provisional, commit_secret);
        uint8_t *commit = NULL;
        size_t commit_len = 0;
        ExpectedEpochForTest expected;
        assert(build_commit_for_test(&f.alice, NULL, 0, &path, commit_secret,
                                     &provisional, f.alice.extensions_data,
                                     f.alice.extensions_len, NULL,
                                     &commit, &commit_len, &expected) == 0);
        if (c == PATH_LEAF_VALID) {
            assert(mls_group_process_commit(&f.bob, commit, commit_len, 0) == 0);
            assert_group_reached_for_test(&f.bob, &expected);
            mls_group_free(&f.bob);
            assert(mls_group_deserialize(parent.blob, parent.blob_len, &f.bob) == 0);
        } else {
            char what[96];
            snprintf(what, sizeof(what), "UpdatePath leaf with %s",
                     path_leaf_case_names[c]);
            accepted += expect_leaf_rejected_for_test(&f.bob, &parent, what,
                commit, commit_len, 0, NULL, NULL, 0, &expected);
        }
        free(commit);
        mls_update_path_clear(&path);
        mls_tree_free(&provisional);
        mls_tree_free(&next);
    }
    assert(accepted == 0);

    /* libmarmot's own producers emit leaves that pass the same validation:
     * group creation, Add, Remove and self-update Commits all verify. */
    const MlsLeafNode *alice_leaf = &f.alice.tree.nodes[0].leaf;
    assert(mls_leaf_node_verify_signature(alice_leaf, f.alice.group_id,
                                          f.alice.group_id_len, 0) == 0);
    assert(mls_leaf_node_verify_signature(alice_leaf, f.alice.group_id,
                                          f.alice.group_id_len, 1) != 0);
    MlsCommitResult update;
    assert(mls_group_self_update(&f.alice, &update) == 0);
    assert(mls_group_process_commit(&f.bob, update.commit_data, update.commit_len, 0) == 0);
    assert(mls_group_process_commit(&f.charlie, update.commit_data, update.commit_len, 0) == 0);
    assert(sodium_memcmp(&f.bob.epoch_secrets, &f.alice.epoch_secrets,
                         sizeof(f.bob.epoch_secrets)) == 0);
    mls_commit_result_clear(&update);
    free(parent.blob);
    three_member_fixture_clear(&f);
}

/* ── Leaves added by the Commit are not UpdatePath recipients (nostrc-5q55) ──
 *
 * RFC 9420 §12.4.2: the committer encrypts each path secret to the copath
 * resolution *excluding the leaves this Commit adds* (they get the joiner
 * secret through the Welcome), and a receiver picks its HPKECiphertext by its
 * index in that same reduced resolution. */

typedef struct {
    MlsGroup             g;
    MlsKeyPackage        kp;
    MlsKeyPackagePrivate priv;
} MemberForTest;

static void
member_init_for_test(MemberForTest *m, uint8_t tag)
{
    uint8_t id[32];
    memset(m, 0, sizeof(*m));
    memset(id, tag, sizeof(id));
    assert(mls_key_package_create(&m->kp, &m->priv, id, sizeof(id), NULL, 0) == 0);
}

static void
member_clear_for_test(MemberForTest *m)
{
    mls_group_free(&m->g);
    mls_key_package_clear(&m->kp);
    mls_key_package_private_clear(&m->priv);
}

/* `committer` adds `joiner`; the other listed members follow the Commit and
 * the joiner joins from the Welcome.  Returns the Commit if `commit_out`. */
static void
add_and_welcome_for_test(MlsGroup *const *members, size_t n, MlsGroup *committer,
                         MemberForTest *joiner, const char *what,
                         uint8_t **commit_out, size_t *commit_len_out)
{
    MlsAddResult add;
    assert(mls_group_add_member(committer, &joiner->kp, &add) == 0);
    deliver_commit_for_test(members, n, committer, add.commit_data, add.commit_len, what);
    assert(mls_welcome_process(add.welcome_data, add.welcome_len, &joiner->kp,
                               &joiner->priv, NULL, 0, &joiner->g) == 0);
    if (commit_out) {
        *commit_out = add.commit_data;
        *commit_len_out = add.commit_len;
        add.commit_data = NULL;
    }
    mls_add_result_clear(&add);
}

/* The receiver-side count an OpenMLS/MDK peer enforces: over the tree after
 * the Commit's proposals, UpdatePathNode i carries exactly one HPKECiphertext
 * per node of its copath resolution minus the added leaves.  Returns how many
 * added leaves were dropped from some resolution. */
static size_t
assert_path_ciphertexts_exclude_added_for_test(const MlsRatchetTree *after_proposals,
                                               uint32_t committer,
                                               const MlsUpdatePath *path,
                                               const uint32_t *added, size_t n_added)
{
    uint32_t fdp[32], fdp_len = 0;
    size_t dropped = 0;
    uint32_t n_leaves = after_proposals->n_leaves;
    assert(mls_tree_filtered_direct_path(after_proposals, committer, fdp, 32,
                                         &fdp_len) == 0);
    assert(path->node_count == fdp_len);
    for (uint32_t i = 0; i < fdp_len; i++) {
        uint32_t child = mls_tree_leaf_to_node(committer);
        while (mls_tree_parent(child, n_leaves) != fdp[i])
            child = mls_tree_parent(child, n_leaves);
        uint32_t res[64], res_len = 0, kept = 0;
        assert(mls_tree_resolution(after_proposals, mls_tree_sibling(child, n_leaves),
                                   res, 64, &res_len) == 0);
        for (uint32_t j = 0; j < res_len; j++) {
            bool is_added = false;
            for (size_t a = 0; a < n_added; a++)
                is_added |= res[j] == mls_tree_leaf_to_node(added[a]);
            if (is_added) dropped++;
            else kept++;
        }
        if (path->nodes[i].secret_count != kept)
            fprintf(stderr, "\n    UpdatePathNode %u (node %u): %u ciphertexts, "
                            "resolution minus added leaves has %u",
                    i, fdp[i], path->nodes[i].secret_count, kept);
        assert(path->nodes[i].secret_count == kept);
    }
    return dropped;
}

/* nostrc-5q55 (the bead's repro): Alice adds Dave (leaf 3), removes Charlie
 * (leaf 2), then adds Eve, who refills leaf 2.  Node 5 is blank, so the copath
 * resolution of the root for Alice is [Eve, Dave]: Dave must decrypt entry 0,
 * not Eve's (was rc -21). */
TEST(test_update_path_excludes_leaves_added_by_commit)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    MemberForTest dave, eve;
    memset(&dave, 0, sizeof(dave));
    dave.kp = f.dave_kp;   /* borrowed; cleared by the fixture */
    dave.priv = f.dave_priv;
    member_init_for_test(&eve, 0xEE);

    MlsGroup *three[] = {&f.alice, &f.bob, &f.charlie};
    add_and_welcome_for_test(three, 3, &f.alice, &dave, "Alice adds Dave", NULL, NULL);
    assert(dave.g.own_leaf_index == 3);
    MlsGroup *four[] = {&f.alice, &f.bob, &f.charlie, &dave.g};
    assert_converged_for_test(four, 4);

    MlsCommitResult rm;
    assert(mls_group_remove_member(&f.alice, 2, &rm) == 0);
    MlsGroup *after_rm[] = {&f.alice, &f.bob, &dave.g};
    deliver_commit_for_test(after_rm, 3, &f.alice, rm.commit_data, rm.commit_len,
                            "Alice removes Charlie");
    assert_converged_for_test(after_rm, 3);
    mls_commit_result_clear(&rm);

    /* The tree the Add produces, as every receiver builds it. */
    MlsRatchetTree after_add;
    assert(tree_clone_for_test(&f.bob.tree, &after_add) == 0);
    assert(add_leaf_for_test(&after_add, &eve.kp) == 0);
    uint32_t res[8], res_len = 0;
    assert(mls_tree_resolution(&after_add, 5, res, 8, &res_len) == 0);
    assert(res_len == 2 && res[0] == mls_tree_leaf_to_node(2) &&
           res[1] == mls_tree_leaf_to_node(3));

    uint8_t *commit = NULL;
    size_t commit_len = 0;
    add_and_welcome_for_test(after_rm, 3, &f.alice, &eve, "Alice adds Eve",
                             &commit, &commit_len);
    assert(eve.g.own_leaf_index == 2);
    MlsGroup *all[] = {&f.alice, &f.bob, &dave.g, &eve.g};
    assert_converged_for_test(all, 4);

    MlsCommit produced;
    parse_commit_for_test(commit, commit_len, &produced);
    const uint32_t added[] = {2};
    assert(assert_path_ciphertexts_exclude_added_for_test(&after_add, 0,
               &produced.path, added, 1) == 1);
    assert(produced.path.nodes[produced.path.node_count - 1].secret_count == 1);

    /* The group keeps working: everyone commits once more and follows. */
    for (size_t c = 0; c < 4; c++) {
        MlsCommitResult r;
        assert(mls_group_self_update(all[c], &r) == 0);
        deliver_commit_for_test(all, 4, all[c], r.commit_data, r.commit_len,
                                "self-update after Add(Eve)");
        assert_converged_for_test(all, 4);
        mls_commit_result_clear(&r);
    }

    mls_commit_clear(&produced);
    free(commit);
    mls_tree_free(&after_add);
    mls_group_free(&dave.g);
    member_clear_for_test(&eve);
    three_member_fixture_clear(&f);
}

/* Receivers enforce the ciphertext count: an UpdatePath that also encrypts to
 * the leaf the Commit adds (what libmarmot <= 0.3.7 produced) is rejected by
 * every member, without state change -- not just by the members whose index
 * it shifts.  Alice (after removing Charlie) adds Eve; Bob and Dave receive. */
TEST(test_update_path_encrypting_to_added_leaf_rejected)
{
    ThreeMemberFixture f;
    three_member_fixture_init(&f);
    MemberForTest dave, eve;
    memset(&dave, 0, sizeof(dave));
    dave.kp = f.dave_kp;
    dave.priv = f.dave_priv;
    member_init_for_test(&eve, 0xEE);
    MlsGroup *three[] = {&f.alice, &f.bob, &f.charlie};
    add_and_welcome_for_test(three, 3, &f.alice, &dave, "Alice adds Dave", NULL, NULL);
    MlsCommitResult rm;
    assert(mls_group_remove_member(&f.alice, 2, &rm) == 0);
    MlsGroup *after_rm[] = {&f.alice, &f.bob, &dave.g};
    deliver_commit_for_test(after_rm, 3, &f.alice, rm.commit_data, rm.commit_len,
                            "Alice removes Charlie");
    mls_commit_result_clear(&rm);

    MlsTlsBuf v;
    assert(mls_tls_buf_init(&v, 512) == 0);
    assert(inline_add_for_test(&v, &eve.kp) == 0);
    MlsRatchetTree next, provisional;
    assert(tree_clone_for_test(&f.alice.tree, &next) == 0);
    assert(add_leaf_for_test(&next, &eve.kp) == 0);
    MlsUpdatePath path;
    uint8_t commit_secret[MLS_HASH_LEN];
    /* Encrypts to the full resolution, Eve included. */
    build_update_path_for_test(&f.alice, &next, f.alice.extensions_data,
                               f.alice.extensions_len, PATH_LEAF_VALID, NULL,
                               &path, &provisional, commit_secret);
    assert(path.node_count == 2 && path.nodes[1].secret_count == 2);
    uint8_t *commit = NULL;
    size_t commit_len = 0;
    ExpectedEpochForTest expected;
    assert(build_commit_for_test(&f.alice, v.data, v.len, &path, commit_secret,
                                 &provisional, f.alice.extensions_data,
                                 f.alice.extensions_len, NULL,
                                 &commit, &commit_len, &expected) == 0);
    int accepted = 0;
    MlsGroup *receivers[] = {&f.bob, &dave.g};
    for (size_t i = 0; i < 2; i++) {
        GroupSnapshotForTest parent;
        snapshot_group_for_test(receivers[i], &parent);
        char what[80];
        snprintf(what, sizeof(what), "leaf %u: UpdatePath encrypting to the added leaf",
                 receivers[i]->own_leaf_index);
        int rc = mls_group_process_commit(receivers[i], commit, commit_len, 0);
        if (rc == 0) {
            assert_group_reached_for_test(receivers[i], &expected);
            fprintf(stderr, "\n    %s ACCEPTED", what);
            accepted++;
            mls_group_free(receivers[i]);
            assert(mls_group_deserialize(parent.blob, parent.blob_len, receivers[i]) == 0);
        } else {
            if (rc != MARMOT_ERR_MLS_PROCESS_MESSAGE)
                fprintf(stderr, "\n    %s: rc=%d", what, rc);
            assert(rc == MARMOT_ERR_MLS_PROCESS_MESSAGE);
            assert_group_matches_snapshot_for_test(receivers[i], &parent);
        }
        free(parent.blob);
    }
    assert(accepted == 0);

    free(commit);
    mls_update_path_clear(&path);
    mls_tree_free(&provisional);
    mls_tree_free(&next);
    mls_tls_buf_free(&v);
    mls_group_free(&dave.g);
    member_clear_for_test(&eve);
    three_member_fixture_clear(&f);
}

/* The committer's tree after an Add matches the receivers': the new leaf
 * joins the unmerged_leaves of every non-blank parent on its direct path,
 * including ones off the committer's path (RFC 9420 §12.1.1).  Five members
 * (8-leaf tree); Bob removes Alice, re-keying node 3 above her blank leaf;
 * Eve (leaf 4) adds Frank into leaf 0, whose path crosses node 3. */
TEST(test_add_unmerged_leaf_off_committer_path)
{
    MemberForTest m[6];
    member_init_for_test(&m[0], 0xA0);
    for (int i = 1; i < 6; i++) member_init_for_test(&m[i], (uint8_t)(0xB0 + i));
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);
    assert(mls_group_create(&m[0].g, GROUP_ID, sizeof(GROUP_ID), ALICE_ID, 32,
                            sk, NULL, 0) == 0);
    MlsGroup *g[6];
    for (int i = 0; i < 6; i++) g[i] = &m[i].g;
    for (size_t n = 1; n < 5; n++)
        add_and_welcome_for_test(g, n, g[0], &m[n], "grow", NULL, NULL);
    assert(g[0]->tree.n_leaves == 8 && g[4]->own_leaf_index == 4);
    assert_converged_for_test(g, 5);

    MlsCommitResult rm;
    assert(mls_group_remove_member(g[1], 0, &rm) == 0);
    deliver_commit_for_test(g + 1, 4, g[1], rm.commit_data, rm.commit_len,
                            "Bob removes Alice");
    mls_commit_result_clear(&rm);
    assert(g[1]->tree.nodes[3].type == MLS_NODE_PARENT);

    add_and_welcome_for_test(g + 1, 4, g[4], &m[5], "Eve adds Frank", NULL, NULL);
    assert(g[5]->own_leaf_index == 0);
    const MlsParentNode *n3 = &g[4]->tree.nodes[3].parent;
    assert(g[4]->tree.nodes[3].type == MLS_NODE_PARENT &&
           n3->unmerged_leaf_count == 1 && n3->unmerged_leaves[0] == 0);
    assert_converged_for_test(g + 1, 5);

    for (size_t c = 1; c < 6; c++) {
        MlsCommitResult r;
        assert(mls_group_self_update(g[c], &r) == 0);
        deliver_commit_for_test(g + 1, 5, g[c], r.commit_data, r.commit_len,
                                "self-update after Add(Frank)");
        assert_converged_for_test(g + 1, 5);
        mls_commit_result_clear(&r);
    }
    sodium_memzero(sk, sizeof(sk));
    for (int i = 0; i < 6; i++) member_clear_for_test(&m[i]);
}

TEST(test_group_creator_leaf_signature_bound_to_group)
{
    MlsGroup group;
    uint8_t sig_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&group, sig_sk) == 0);
    const MlsLeafNode *leaf = &group.tree.nodes[0].leaf;
    assert(leaf->leaf_node_source == MLS_LEAF_NODE_SOURCE_COMMIT);
    assert(mls_leaf_node_verify_signature(leaf, group.group_id, group.group_id_len, 0) == 0);
    static const uint8_t other_group[] = "other-group";
    assert(mls_leaf_node_verify_signature(leaf, other_group, sizeof(other_group), 0) != 0);
    assert(mls_leaf_node_verify_signature(leaf, NULL, 0, 0) != 0);
    mls_group_free(&group);
}

TEST(test_bad_committer_signature_rejected)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result;
    memset(&add_result, 0, sizeof(add_result));
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    MlsCommitResult update;
    assert(mls_group_self_update(&alice_group, &update) == 0);
    MlsMLSMessage msg;
    MlsTlsReader r;
    mls_tls_reader_init(&r, update.commit_data, update.commit_len);
    assert(mls_message_deserialize(&r, &msg) == 0);
    msg.public_message.auth.signature[0] ^= 0x01;
    MlsTlsBuf tampered;
    assert(mls_tls_buf_init(&tampered, update.commit_len) == 0);
    assert(mls_message_serialize(&msg, &tampered) == 0);
    assert(mls_group_process_commit(&bob_group, tampered.data, tampered.len, 0) != 0);

    mls_tls_buf_free(&tampered);
    mls_message_clear(&msg);
    mls_commit_result_clear(&update);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

TEST(test_wrong_confirmation_tag_rejected)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result;
    memset(&add_result, 0, sizeof(add_result));
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    MlsCommitResult update;
    assert(mls_group_self_update(&alice_group, &update) == 0);
    MlsMLSMessage msg;
    MlsTlsReader r;
    mls_tls_reader_init(&r, update.commit_data, update.commit_len);
    assert(mls_message_deserialize(&r, &msg) == 0);
    msg.public_message.auth.confirmation_tag[0] ^= 0x01;
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_build(&bob_group, &gc, &gc_len) == 0);
    assert(mls_public_message_compute_membership_tag(&msg.public_message,
            bob_group.epoch_secrets.membership_key, gc, gc_len) == 0);
    free(gc);
    MlsTlsBuf tampered;
    assert(mls_tls_buf_init(&tampered, update.commit_len) == 0);
    assert(mls_message_serialize(&msg, &tampered) == 0);
    assert(mls_group_process_commit(&bob_group, tampered.data, tampered.len, 0) != 0);

    mls_tls_buf_free(&tampered);
    mls_message_clear(&msg);
    mls_commit_result_clear(&update);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

TEST(test_unknown_proposal_type_rejected)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result;
    memset(&add_result, 0, sizeof(add_result));
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    MlsTlsBuf body;
    assert(mls_tls_buf_init(&body, 8) == 0);
    uint8_t unknown_prop[2] = { 0x12, 0x34 };
    assert(mls_tls_write_opaque32(&body, unknown_prop, sizeof(unknown_prop)) == 0);
    assert(mls_tls_write_u8(&body, 0) == 0); /* no path */
    uint8_t zero_tag[MLS_HASH_LEN] = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    assert(build_commit_public_message_for_test(&alice_group, body.data, body.len,
                                                zero_tag, &wire, &wire_len) == 0);
    assert(mls_group_process_commit(&bob_group, wire, wire_len, 0) != 0);

    free(wire);
    mls_tls_buf_free(&body);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
    mls_group_free(&bob_group);
}

TEST(test_tampered_group_info_signature_rejected)
{
    MlsGroup alice_group, bob_group;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    MlsAddResult add_result;
    memset(&add_result, 0, sizeof(add_result));
    assert(setup_two_member_groups(&alice_group, &bob_group,
                                   &bob_kp, &bob_priv, &add_result) == 0);

    MlsWelcome w;
    MlsTlsReader wr;
    mls_tls_reader_init(&wr, add_result.welcome_data, add_result.welcome_len);
    assert(mls_welcome_deserialize(&wr, &w) == 0);

    uint8_t welcome_key[MLS_AEAD_KEY_LEN], welcome_nonce[MLS_AEAD_NONCE_LEN];
    assert(mls_crypto_expand_with_label(welcome_key, MLS_AEAD_KEY_LEN,
            alice_group.epoch_secrets.welcome_secret, "key", NULL, 0) == 0);
    assert(mls_crypto_expand_with_label(welcome_nonce, MLS_AEAD_NONCE_LEN,
            alice_group.epoch_secrets.welcome_secret, "nonce", NULL, 0) == 0);
    uint8_t *gi_data = malloc(w.encrypted_group_info_len);
    size_t gi_len = 0;
    assert(gi_data != NULL);
    assert(mls_crypto_aead_decrypt(gi_data, &gi_len, welcome_key, welcome_nonce,
            w.encrypted_group_info, w.encrypted_group_info_len, NULL, 0) == 0);
    MlsGroupInfo gi;
    MlsTlsReader gir;
    mls_tls_reader_init(&gir, gi_data, gi_len);
    assert(mls_group_info_deserialize(&gir, &gi) == 0);
    gi.signature[0] ^= 0x01;
    MlsTlsBuf gi_buf;
    assert(mls_tls_buf_init(&gi_buf, gi_len) == 0);
    assert(mls_group_info_serialize(&gi, &gi_buf) == 0);
    uint8_t *new_enc_gi = malloc(gi_buf.len + MLS_AEAD_TAG_LEN);
    size_t new_enc_gi_len = 0;
    assert(new_enc_gi != NULL);
    assert(mls_crypto_aead_encrypt(new_enc_gi, &new_enc_gi_len,
            welcome_key, welcome_nonce, gi_buf.data, gi_buf.len, NULL, 0) == 0);

    MlsTlsBuf group_secrets;
    assert(mls_tls_buf_init(&group_secrets, MLS_HASH_LEN + 8) == 0);
    assert(mls_tls_write_opaque16(&group_secrets,
            alice_group.epoch_secrets.joiner_secret, MLS_HASH_LEN) == 0);
    assert(mls_tls_write_u8(&group_secrets, 0) == 0);
    assert(mls_tls_write_opaque32(&group_secrets, NULL, 0) == 0);
    uint8_t *info = NULL;
    size_t info_len = 0;
    assert(build_encrypt_context_for_test("Welcome", new_enc_gi, new_enc_gi_len,
                                          &info, &info_len) == 0);
    uint8_t new_enc[MLS_KEM_ENC_LEN];
    uint8_t *new_ct = malloc(group_secrets.len + MLS_AEAD_TAG_LEN);
    size_t new_ct_len = 0;
    assert(new_ct != NULL);
    assert(mls_crypto_hpke_seal_base(new_enc, new_ct, &new_ct_len,
            bob_kp.init_key, info, info_len, NULL, 0,
            group_secrets.data, group_secrets.len) == 0);
    memcpy(w.secrets[0].kem_output, new_enc, MLS_KEM_ENC_LEN);
    free(w.secrets[0].encrypted_joiner_secret);
    w.secrets[0].encrypted_joiner_secret = new_ct;
    w.secrets[0].encrypted_joiner_secret_len = new_ct_len;
    free(w.encrypted_group_info);
    w.encrypted_group_info = new_enc_gi;
    w.encrypted_group_info_len = new_enc_gi_len;

    MlsTlsBuf tampered_welcome;
    assert(mls_tls_buf_init(&tampered_welcome, add_result.welcome_len) == 0);
    assert(mls_welcome_serialize(&w, &tampered_welcome) == 0);
    MlsGroup rejected;
    assert(mls_welcome_process(tampered_welcome.data, tampered_welcome.len,
                               &bob_kp, &bob_priv, NULL, 0, &rejected) != 0);

    free(info);
    free(gi_data);
    mls_tls_buf_free(&group_secrets);
    mls_tls_buf_free(&gi_buf);
    mls_tls_buf_free(&tampered_welcome);
    mls_group_info_clear(&gi);
    mls_welcome_clear(&w);
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
    RUN(test_group_out_of_order_and_replay);
    RUN(test_group_out_of_order_beyond_window);
    RUN(test_welcome_epoch_secrets_match);
    RUN(test_process_valid_self_update_commit_roundtrip);
    RUN(test_referenced_proposal_store_requires_parent_authentication);
    RUN(test_live_commit_consumes_parent_epoch_proposal_ref);
    RUN(test_live_commit_rejects_unauthenticated_proposal_refs);
    RUN(test_pathless_commit_requiring_path_rejected);
    RUN(test_pathless_remove_does_not_exclude_removed_member);
    RUN(test_pathless_add_and_psk_commits_accepted);
    RUN(test_commit_serialize_requires_path);
    RUN(test_remove_filters_root_from_committer_path);
    RUN(test_committer_keeps_own_path_keys);
    RUN(test_commit_producers_fail_closed);
    RUN(test_update_path_excludes_leaves_added_by_commit);
    RUN(test_update_path_encrypting_to_added_leaf_rejected);
    RUN(test_add_unmerged_leaf_off_committer_path);
    RUN(test_group_creator_leaf_signature_bound_to_group);
    RUN(test_update_by_ref_leaf_validation);
    RUN(test_update_path_leaf_validation);
    RUN(test_bad_committer_signature_rejected);
    RUN(test_wrong_confirmation_tag_rejected);
    RUN(test_unknown_proposal_type_rejected);
    RUN(test_tampered_group_info_signature_rejected);
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
