/*
 * libmarmot - MLS Message Framing tests (RFC 9420 §6)
 *
 * Tests PrivateMessage encryption/decryption, sender data encryption,
 * content AAD construction, reuse guard, and TLS serialization.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_framing.h"
#include "mls/mls_key_schedule.h"
#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Helper: set up epoch secrets and secret tree
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    MlsEpochSecrets secrets;
    MlsSecretTree   secret_tree;
    uint8_t         group_id[16];
    size_t          group_id_len;
    uint64_t        epoch;
    uint32_t        n_leaves;
} TestEpochCtx;

static void
setup_epoch(TestEpochCtx *ctx, uint32_t n_leaves, uint64_t epoch, uint8_t seed)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->n_leaves = n_leaves;
    ctx->epoch = epoch;

    /* Deterministic group ID */
    memset(ctx->group_id, seed, sizeof(ctx->group_id));
    ctx->group_id_len = sizeof(ctx->group_id);

    /* Derive epoch secrets */
    uint8_t commit_secret[MLS_HASH_LEN];
    memset(commit_secret, seed + 1, sizeof(commit_secret));

    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, seed + 2, sizeof(th));
    memset(cth, seed + 3, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(ctx->group_id, ctx->group_id_len,
                                        epoch, th, cth, NULL, 0,
                                        &gc, &gc_len) == 0);

    assert(mls_key_schedule_derive(NULL, commit_secret, gc, gc_len, NULL, &ctx->secrets) == 0);
    free(gc);

    /* Initialize secret tree */
    assert(mls_secret_tree_init(&ctx->secret_tree, ctx->secrets.encryption_secret, n_leaves) == 0);
}

static void
teardown_epoch(TestEpochCtx *ctx)
{
    mls_secret_tree_free(&ctx->secret_tree);
    sodium_memzero(ctx, sizeof(*ctx));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Reuse guard tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_reuse_guard_xor(void)
{
    uint8_t nonce[MLS_AEAD_NONCE_LEN] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB
    };
    uint8_t guard[4] = {0xFF, 0xFF, 0x00, 0x01};
    uint8_t original[MLS_AEAD_NONCE_LEN];
    memcpy(original, nonce, sizeof(nonce));

    mls_apply_reuse_guard(nonce, guard);

    /* First 4 bytes XORed */
    assert(nonce[0] == (original[0] ^ 0xFF));
    assert(nonce[1] == (original[1] ^ 0xFF));
    assert(nonce[2] == (original[2] ^ 0x00));
    assert(nonce[3] == (original[3] ^ 0x01));

    /* Remaining bytes unchanged */
    for (int i = 4; i < MLS_AEAD_NONCE_LEN; i++) {
        assert(nonce[i] == original[i]);
    }
}

static void test_reuse_guard_double_apply(void)
{
    /* Applying the same guard twice should restore original nonce */
    uint8_t nonce[MLS_AEAD_NONCE_LEN];
    mls_crypto_random(nonce, sizeof(nonce));

    uint8_t original[MLS_AEAD_NONCE_LEN];
    memcpy(original, nonce, sizeof(nonce));

    uint8_t guard[4];
    mls_crypto_random(guard, sizeof(guard));

    mls_apply_reuse_guard(nonce, guard);
    mls_apply_reuse_guard(nonce, guard);

    assert(memcmp(nonce, original, sizeof(nonce)) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Content AAD tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_content_aad_construction(void)
{
    uint8_t group_id[] = "test-group";
    uint64_t epoch = 7;
    uint8_t content_type = MLS_CONTENT_TYPE_APPLICATION;

    uint8_t *aad = NULL;
    size_t aad_len = 0;
    assert(mls_build_content_aad(group_id, sizeof(group_id) - 1,
                                  epoch, content_type,
                                  NULL, 0,
                                  &aad, &aad_len) == 0);
    assert(aad != NULL);
    assert(aad_len > 0);

    /* Parse back the TLS structure */
    MlsTlsReader r;
    mls_tls_reader_init(&r, aad, aad_len);

    uint8_t *gid = NULL;
    size_t gid_len = 0;
    assert(mls_tls_read_opaque8(&r, &gid, &gid_len) == 0);
    assert(gid_len == sizeof(group_id) - 1);
    assert(memcmp(gid, group_id, gid_len) == 0);
    free(gid);

    uint64_t ep;
    assert(mls_tls_read_u64(&r, &ep) == 0);
    assert(ep == 7);

    uint8_t ct;
    assert(mls_tls_read_u8(&r, &ct) == 0);
    assert(ct == MLS_CONTENT_TYPE_APPLICATION);

    free(aad);
}

static void test_content_aad_deterministic(void)
{
    uint8_t group_id[] = "grp";
    uint8_t *aad1 = NULL, *aad2 = NULL;
    size_t len1 = 0, len2 = 0;

    assert(mls_build_content_aad(group_id, 3, 0, MLS_CONTENT_TYPE_COMMIT,
                                  NULL, 0, &aad1, &len1) == 0);
    assert(mls_build_content_aad(group_id, 3, 0, MLS_CONTENT_TYPE_COMMIT,
                                  NULL, 0, &aad2, &len2) == 0);

    assert(len1 == len2);
    assert(memcmp(aad1, aad2, len1) == 0);

    free(aad1);
    free(aad2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Sender data encryption tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_sender_data_roundtrip(void)
{
    uint8_t sds[MLS_HASH_LEN];
    memset(sds, 0xAA, sizeof(sds));

    uint8_t ciphertext_sample[MLS_AEAD_KEY_LEN];
    mls_crypto_random(ciphertext_sample, sizeof(ciphertext_sample));

    MlsSenderData sd_in = {
        .leaf_index = 42,
        .generation = 17,
        .reuse_guard = {0x01, 0x02, 0x03, 0x04}
    };

    /* Encrypt */
    uint8_t encrypted[12 + MLS_AEAD_TAG_LEN + 4]; /* some headroom */
    size_t enc_len = 0;
    assert(mls_sender_data_encrypt(sds, ciphertext_sample, sizeof(ciphertext_sample),
                                    &sd_in, encrypted, &enc_len) == 0);
    assert(enc_len == 12 + MLS_AEAD_TAG_LEN);

    /* Decrypt */
    MlsSenderData sd_out;
    assert(mls_sender_data_decrypt(sds, ciphertext_sample, sizeof(ciphertext_sample),
                                    encrypted, enc_len, &sd_out) == 0);

    assert(sd_out.leaf_index == 42);
    assert(sd_out.generation == 17);
    assert(memcmp(sd_out.reuse_guard, sd_in.reuse_guard, 4) == 0);
}

static void test_sender_data_wrong_secret(void)
{
    uint8_t sds1[MLS_HASH_LEN], sds2[MLS_HASH_LEN];
    memset(sds1, 0xBB, sizeof(sds1));
    memset(sds2, 0xCC, sizeof(sds2));

    uint8_t sample[MLS_AEAD_KEY_LEN] = {0};
    MlsSenderData sd = { .leaf_index = 1, .generation = 0 };

    uint8_t encrypted[28 + 4];
    size_t enc_len = 0;
    assert(mls_sender_data_encrypt(sds1, sample, sizeof(sample),
                                    &sd, encrypted, &enc_len) == 0);

    /* Decrypt with wrong secret should fail */
    MlsSenderData sd_out;
    assert(mls_sender_data_decrypt(sds2, sample, sizeof(sample),
                                    encrypted, enc_len, &sd_out) != 0);
}

static void test_sender_data_wrong_sample(void)
{
    uint8_t sds[MLS_HASH_LEN];
    memset(sds, 0xDD, sizeof(sds));

    uint8_t sample1[MLS_AEAD_KEY_LEN], sample2[MLS_AEAD_KEY_LEN];
    memset(sample1, 0x11, sizeof(sample1));
    memset(sample2, 0x22, sizeof(sample2));

    MlsSenderData sd = { .leaf_index = 0, .generation = 0 };

    uint8_t encrypted[28 + 4];
    size_t enc_len = 0;
    assert(mls_sender_data_encrypt(sds, sample1, sizeof(sample1),
                                    &sd, encrypted, &enc_len) == 0);

    /* Decrypt with wrong ciphertext sample should fail */
    MlsSenderData sd_out;
    assert(mls_sender_data_decrypt(sds, sample2, sizeof(sample2),
                                    encrypted, enc_len, &sd_out) != 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage encryption/decryption tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_private_message_roundtrip(void)
{
    TestEpochCtx ctx;
    setup_epoch(&ctx, 4, 0, 0x10);

    /* Sender is leaf 0 */
    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, sizeof(reuse_guard));

    const uint8_t plaintext[] = "Hello, MLS group!";
    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0,
        plaintext, sizeof(plaintext) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard,
        &msg) == 0);

    assert(msg.ciphertext != NULL);
    assert(msg.ciphertext_len > 0);
    assert(msg.encrypted_sender_data != NULL);

    /* Decrypt: need a fresh secret tree (to get the same keys at generation 0) */
    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 4) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg,
        ctx.secrets.sender_data_secret,
        &dec_tree,
        100, /* max forward */
        &decrypted, &dec_len,
        &sender) == 0);

    assert(dec_len == sizeof(plaintext) - 1);
    assert(memcmp(decrypted, plaintext, dec_len) == 0);
    assert(sender.leaf_index == 0);
    assert(sender.generation == 0);

    free(decrypted);
    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_with_aad(void)
{
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 5, 0x20);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 1, false, &keys) == 0);

    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, sizeof(reuse_guard));

    const uint8_t plaintext[] = "Message with AAD";
    const uint8_t aad[] = "authenticated extra data";

    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        aad, sizeof(aad) - 1,
        plaintext, sizeof(plaintext) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 1, reuse_guard,
        &msg) == 0);

    assert(msg.authenticated_data != NULL);
    assert(msg.authenticated_data_len == sizeof(aad) - 1);

    /* Decrypt */
    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg, ctx.secrets.sender_data_secret, &dec_tree, 100,
        &decrypted, &dec_len, &sender) == 0);

    assert(dec_len == sizeof(plaintext) - 1);
    assert(memcmp(decrypted, plaintext, dec_len) == 0);
    assert(sender.leaf_index == 1);

    free(decrypted);
    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_wrong_epoch_secret(void)
{
    /* Decrypting with wrong sender_data_secret should fail */
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 0, 0x30);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4] = {0};
    const uint8_t pt[] = "secret";

    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0, pt, sizeof(pt) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard, &msg) == 0);

    /* Use wrong sender_data_secret */
    uint8_t wrong_sds[MLS_HASH_LEN];
    memset(wrong_sds, 0xFF, sizeof(wrong_sds));

    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    int rc = mls_private_message_decrypt(
        &msg, wrong_sds, &dec_tree, 100,
        &decrypted, &dec_len, &sender);
    assert(rc != 0);

    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_multiple_senders(void)
{
    /* Multiple senders can encrypt and decrypt within the same epoch */
    TestEpochCtx ctx;
    setup_epoch(&ctx, 4, 0, 0x40);

    /* Each sender encrypts a message */
    MlsPrivateMessage msgs[4];
    uint8_t reuse_guards[4][4];

    for (uint32_t i = 0; i < 4; i++) {
        MlsMessageKeys keys;
        assert(mls_secret_tree_derive_keys(&ctx.secret_tree, i, false, &keys) == 0);

        mls_crypto_random(reuse_guards[i], 4);

        char pt[32];
        snprintf(pt, sizeof(pt), "Message from sender %u", i);

        assert(mls_private_message_encrypt(
            ctx.group_id, ctx.group_id_len, ctx.epoch,
            MLS_CONTENT_TYPE_APPLICATION,
            NULL, 0, (uint8_t *)pt, strlen(pt),
            ctx.secrets.sender_data_secret,
            &keys, i, reuse_guards[i], &msgs[i]) == 0);
    }

    /* Decrypt all messages with a fresh tree */
    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 4) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *decrypted = NULL;
        size_t dec_len = 0;
        MlsSenderData sender;
        assert(mls_private_message_decrypt(
            &msgs[i], ctx.secrets.sender_data_secret, &dec_tree, 100,
            &decrypted, &dec_len, &sender) == 0);

        assert(sender.leaf_index == i);

        char expected[32];
        snprintf(expected, sizeof(expected), "Message from sender %u", i);
        assert(dec_len == strlen(expected));
        assert(memcmp(decrypted, expected, dec_len) == 0);

        free(decrypted);
        mls_private_message_clear(&msgs[i]);
    }

    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_handshake_type(void)
{
    /* Commit messages use is_handshake=true for key derivation */
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 0, 0x50);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, true, &keys) == 0);

    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, 4);

    const uint8_t pt[] = "commit data";
    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_COMMIT,
        NULL, 0, pt, sizeof(pt) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard, &msg) == 0);

    assert(msg.content_type == MLS_CONTENT_TYPE_COMMIT);

    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg, ctx.secrets.sender_data_secret, &dec_tree, 100,
        &decrypted, &dec_len, &sender) == 0);

    assert(dec_len == sizeof(pt) - 1);
    assert(memcmp(decrypted, pt, dec_len) == 0);

    free(decrypted);
    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PrivateMessage TLS serialization tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_private_message_serialize_roundtrip(void)
{
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 3, 0x60);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, 4);

    const uint8_t pt[] = "serialize me";
    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0, pt, sizeof(pt) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard, &msg) == 0);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_private_message_serialize(&msg, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsPrivateMessage msg2;
    assert(mls_private_message_deserialize(&reader, &msg2) == 0);

    /* Verify fields */
    assert(msg2.group_id_len == msg.group_id_len);
    assert(memcmp(msg2.group_id, msg.group_id, msg.group_id_len) == 0);
    assert(msg2.epoch == msg.epoch);
    assert(msg2.content_type == msg.content_type);
    assert(msg2.ciphertext_len == msg.ciphertext_len);
    assert(memcmp(msg2.ciphertext, msg.ciphertext, msg.ciphertext_len) == 0);
    assert(msg2.encrypted_sender_data_len == msg.encrypted_sender_data_len);
    assert(memcmp(msg2.encrypted_sender_data, msg.encrypted_sender_data,
                  msg.encrypted_sender_data_len) == 0);

    /* Decrypt the deserialized message */
    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg2, ctx.secrets.sender_data_secret, &dec_tree, 100,
        &decrypted, &dec_len, &sender) == 0);

    assert(dec_len == sizeof(pt) - 1);
    assert(memcmp(decrypted, pt, dec_len) == 0);

    free(decrypted);
    mls_tls_buf_free(&buf);
    mls_private_message_clear(&msg);
    mls_private_message_clear(&msg2);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_empty_plaintext(void)
{
    /* Edge case: empty plaintext */
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 0, 0x70);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4] = {0};
    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0,
        (const uint8_t *)"", 0,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard, &msg) == 0);

    /* Ciphertext should be just the AEAD tag (16 bytes) */
    assert(msg.ciphertext_len == MLS_AEAD_TAG_LEN);

    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg, ctx.secrets.sender_data_secret, &dec_tree, 100,
        &decrypted, &dec_len, &sender) == 0);
    assert(dec_len == 0);

    free(decrypted);
    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_private_message_tamper_ciphertext(void)
{
    /* Tampering with ciphertext should cause decryption failure */
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 0, 0x80);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4] = {0};
    const uint8_t pt[] = "tamper test";
    MlsPrivateMessage msg;
    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0, pt, sizeof(pt) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard, &msg) == 0);

    /* Tamper with ciphertext byte */
    if (msg.ciphertext_len > 0) {
        msg.ciphertext[msg.ciphertext_len / 2] ^= 0xFF;
    }

    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    int rc = mls_private_message_decrypt(
        &msg, ctx.secrets.sender_data_secret, &dec_tree, 100,
        &decrypted, &dec_len, &sender);
    /* Tamper detection: should fail. Note: tampering with the first 16 bytes
     * also affects sender data decryption since those bytes are the sample.
     * Either way, decryption should fail. */
    assert(rc != 0);

    mls_private_message_clear(&msg);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: MLS Message Framing tests\n");

    printf("\n  --- Reuse guard ---\n");
    TEST(test_reuse_guard_xor);
    TEST(test_reuse_guard_double_apply);

    printf("\n  --- Content AAD ---\n");
    TEST(test_content_aad_construction);
    TEST(test_content_aad_deterministic);

    printf("\n  --- Sender data encryption ---\n");
    TEST(test_sender_data_roundtrip);
    TEST(test_sender_data_wrong_secret);
    TEST(test_sender_data_wrong_sample);

    printf("\n  --- PrivateMessage encrypt/decrypt ---\n");
    TEST(test_private_message_roundtrip);
    TEST(test_private_message_with_aad);
    TEST(test_private_message_wrong_epoch_secret);
    TEST(test_private_message_multiple_senders);
    TEST(test_private_message_handshake_type);

    printf("\n  --- PrivateMessage TLS serialization ---\n");
    TEST(test_private_message_serialize_roundtrip);
    TEST(test_private_message_empty_plaintext);
    TEST(test_private_message_tamper_ciphertext);

    printf("\nAll MLS Message Framing tests passed.\n");
    return 0;
}
