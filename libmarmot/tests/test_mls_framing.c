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
 * FramedContent tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_framed_content_serialize_roundtrip(void)
{
    MlsFramedContent fc;
    memset(&fc, 0, sizeof(fc));

    uint8_t gid[] = "test-group";
    fc.group_id = malloc(sizeof(gid) - 1);
    memcpy(fc.group_id, gid, sizeof(gid) - 1);
    fc.group_id_len = sizeof(gid) - 1;
    fc.epoch = 42;
    fc.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    fc.sender.leaf_index = 3;
    fc.content_type = MLS_CONTENT_TYPE_APPLICATION;

    uint8_t body[] = "Hello world";
    fc.content = malloc(sizeof(body) - 1);
    memcpy(fc.content, body, sizeof(body) - 1);
    fc.content_len = sizeof(body) - 1;

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 128) == 0);
    assert(mls_framed_content_serialize(&fc, &buf) == 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsFramedContent fc2;
    assert(mls_framed_content_deserialize(&reader, &fc2) == 0);

    assert(fc2.group_id_len == fc.group_id_len);
    assert(memcmp(fc2.group_id, fc.group_id, fc.group_id_len) == 0);
    assert(fc2.epoch == 42);
    assert(fc2.sender.sender_type == MLS_SENDER_TYPE_MEMBER);
    assert(fc2.sender.leaf_index == 3);
    assert(fc2.content_type == MLS_CONTENT_TYPE_APPLICATION);
    assert(fc2.content_len == fc.content_len);
    assert(memcmp(fc2.content, body, fc.content_len) == 0);

    mls_tls_buf_free(&buf);
    mls_framed_content_clear(&fc);
    mls_framed_content_clear(&fc2);
}

static void test_framed_content_tbs_deterministic(void)
{
    MlsFramedContent fc;
    memset(&fc, 0, sizeof(fc));

    uint8_t gid[] = "grp";
    fc.group_id = malloc(3);
    memcpy(fc.group_id, gid, 3);
    fc.group_id_len = 3;
    fc.epoch = 0;
    fc.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    fc.sender.leaf_index = 0;
    fc.content_type = MLS_CONTENT_TYPE_APPLICATION;
    fc.content = malloc(4);
    memcpy(fc.content, "test", 4);
    fc.content_len = 4;

    /* Need a GroupContext for member senders */
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x11, sizeof(th));
    memset(cth, 0x22, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, 3, 0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    uint8_t *tbs1 = NULL, *tbs2 = NULL;
    size_t len1 = 0, len2 = 0;
    assert(mls_framed_content_tbs_serialize(&fc, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                             gc, gc_len, &tbs1, &len1) == 0);
    assert(mls_framed_content_tbs_serialize(&fc, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                             gc, gc_len, &tbs2, &len2) == 0);

    assert(len1 == len2);
    assert(memcmp(tbs1, tbs2, len1) == 0);

    free(tbs1);
    free(tbs2);
    free(gc);
    mls_framed_content_clear(&fc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Content signing / verification tests
 * ══════════════════════════════════════════════════════════════════════════ */

/** Helper: create a FramedContent for signing tests. Caller must clear. */
static void
make_test_framed_content(MlsFramedContent *fc, const char *body,
                         uint32_t leaf_index, uint64_t epoch)
{
    memset(fc, 0, sizeof(*fc));

    uint8_t gid[] = "sign-test-group";
    fc->group_id = malloc(sizeof(gid) - 1);
    memcpy(fc->group_id, gid, sizeof(gid) - 1);
    fc->group_id_len = sizeof(gid) - 1;
    fc->epoch = epoch;
    fc->sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    fc->sender.leaf_index = leaf_index;
    fc->content_type = MLS_CONTENT_TYPE_APPLICATION;
    size_t blen = strlen(body);
    fc->content = malloc(blen);
    memcpy(fc->content, body, blen);
    fc->content_len = blen;
}

static void test_framed_content_sign_verify(void)
{
    /* Sign and verify a FramedContent */
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    MlsFramedContent fc;
    make_test_framed_content(&fc, "signed message", 0, 0);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 0, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    MlsFramedContentAuthData auth;
    assert(mls_framed_content_sign(&fc, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &auth) == 0);

    /* Verify should succeed with correct key */
    assert(mls_framed_content_verify(&fc, &auth, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                      gc, gc_len, pk) == 0);

    free(gc);
    mls_framed_content_clear(&fc);
}

static void test_framed_content_sign_wrong_key(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    uint8_t sk2[MLS_SIG_SK_LEN], pk2[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk2, pk2) == 0);

    MlsFramedContent fc;
    make_test_framed_content(&fc, "wrong key test", 0, 0);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 0, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    MlsFramedContentAuthData auth;
    assert(mls_framed_content_sign(&fc, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &auth) == 0);

    /* Verify with wrong key should fail */
    assert(mls_framed_content_verify(&fc, &auth, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                      gc, gc_len, pk2) != 0);

    free(gc);
    mls_framed_content_clear(&fc);
}

static void test_framed_content_sign_tampered_content(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    MlsFramedContent fc;
    make_test_framed_content(&fc, "original", 0, 0);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 0, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    MlsFramedContentAuthData auth;
    assert(mls_framed_content_sign(&fc, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &auth) == 0);

    /* Tamper with content */
    fc.content[0] ^= 0xFF;

    /* Verify should fail */
    assert(mls_framed_content_verify(&fc, &auth, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                      gc, gc_len, pk) != 0);

    free(gc);
    mls_framed_content_clear(&fc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Confirmation tag tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_confirmation_tag_deterministic(void)
{
    uint8_t conf_key[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(conf_key, 0xAA, sizeof(conf_key));
    memset(cth, 0xBB, sizeof(cth));

    uint8_t tag1[MLS_HASH_LEN], tag2[MLS_HASH_LEN];
    assert(mls_compute_confirmation_tag(conf_key, cth, tag1) == 0);
    assert(mls_compute_confirmation_tag(conf_key, cth, tag2) == 0);
    assert(memcmp(tag1, tag2, MLS_HASH_LEN) == 0);
}

static void test_confirmation_tag_different_inputs(void)
{
    uint8_t key1[MLS_HASH_LEN], key2[MLS_HASH_LEN];
    memset(key1, 0x11, sizeof(key1));
    memset(key2, 0x22, sizeof(key2));

    uint8_t cth[MLS_HASH_LEN];
    memset(cth, 0x33, sizeof(cth));

    uint8_t tag1[MLS_HASH_LEN], tag2[MLS_HASH_LEN];
    assert(mls_compute_confirmation_tag(key1, cth, tag1) == 0);
    assert(mls_compute_confirmation_tag(key2, cth, tag2) == 0);
    assert(memcmp(tag1, tag2, MLS_HASH_LEN) != 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PublicMessage tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_public_message_serialize_roundtrip(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    MlsPublicMessage msg;
    memset(&msg, 0, sizeof(msg));
    make_test_framed_content(&msg.content, "public msg", 0, 5);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 5, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    /* Sign */
    assert(mls_framed_content_sign(&msg.content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &msg.auth) == 0);

    /* Compute membership tag */
    uint8_t membership_key[MLS_HASH_LEN];
    memset(membership_key, 0xCC, sizeof(membership_key));
    assert(mls_public_message_compute_membership_tag(&msg, membership_key,
                                                      gc, gc_len) == 0);
    assert(msg.has_membership_tag);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_public_message_serialize(&msg, &buf) == 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsPublicMessage msg2;
    assert(mls_public_message_deserialize(&reader, &msg2) == 0);

    /* Verify fields */
    assert(msg2.content.epoch == 5);
    assert(msg2.content.sender.leaf_index == 0);
    assert(msg2.content.content_type == MLS_CONTENT_TYPE_APPLICATION);
    assert(msg2.content.content_len == msg.content.content_len);
    assert(memcmp(msg2.auth.signature, msg.auth.signature, MLS_SIG_LEN) == 0);
    assert(msg2.has_membership_tag);
    assert(memcmp(msg2.membership_tag, msg.membership_tag, MLS_HASH_LEN) == 0);

    /* Verify signature on deserialized message */
    assert(mls_framed_content_verify(&msg2.content, &msg2.auth,
                                      MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                      gc, gc_len, pk) == 0);

    /* Verify membership tag */
    assert(mls_public_message_verify_membership_tag(&msg2, membership_key,
                                                     gc, gc_len) == 0);

    mls_tls_buf_free(&buf);
    mls_public_message_clear(&msg);
    mls_public_message_clear(&msg2);
    free(gc);
}

static void test_public_message_membership_tag_wrong_key(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    (void)pk;
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    MlsPublicMessage msg;
    memset(&msg, 0, sizeof(msg));
    make_test_framed_content(&msg.content, "member tag test", 0, 0);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 0, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    assert(mls_framed_content_sign(&msg.content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &msg.auth) == 0);

    uint8_t key1[MLS_HASH_LEN], key2[MLS_HASH_LEN];
    memset(key1, 0xAA, sizeof(key1));
    memset(key2, 0xBB, sizeof(key2));

    assert(mls_public_message_compute_membership_tag(&msg, key1, gc, gc_len) == 0);

    /* Verify with wrong key should fail */
    assert(mls_public_message_verify_membership_tag(&msg, key2, gc, gc_len) != 0);

    mls_public_message_clear(&msg);
    free(gc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MLSMessage container tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_mls_message_public_roundtrip(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    (void)pk;
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = 0x0001;

    make_test_framed_content(&msg.public_message.content, "mls msg test", 1, 10);

    uint8_t gid[] = "sign-test-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0, sizeof(th));
    memset(cth, 0, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 10, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    assert(mls_framed_content_sign(&msg.public_message.content,
                                    MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk, &msg.public_message.auth) == 0);

    uint8_t mkey[MLS_HASH_LEN];
    memset(mkey, 0xDD, sizeof(mkey));
    assert(mls_public_message_compute_membership_tag(&msg.public_message,
                                                      mkey, gc, gc_len) == 0);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_message_serialize(&msg, &buf) == 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsMLSMessage msg2;
    assert(mls_message_deserialize(&reader, &msg2) == 0);

    assert(msg2.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE);
    assert(msg2.cipher_suite == 0x0001);
    assert(msg2.public_message.content.epoch == 10);
    assert(msg2.public_message.content.sender.leaf_index == 1);

    mls_tls_buf_free(&buf);
    mls_message_clear(&msg);
    mls_message_clear(&msg2);
    free(gc);
}

static void test_mls_message_private_roundtrip(void)
{
    TestEpochCtx ctx;
    setup_epoch(&ctx, 2, 0, 0x90);

    MlsMessageKeys keys;
    assert(mls_secret_tree_derive_keys(&ctx.secret_tree, 0, false, &keys) == 0);

    uint8_t reuse_guard[4];
    mls_crypto_random(reuse_guard, 4);

    const uint8_t pt[] = "private in mls message";

    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PRIVATE_MESSAGE;
    msg.cipher_suite = 0x0001;

    assert(mls_private_message_encrypt(
        ctx.group_id, ctx.group_id_len, ctx.epoch,
        MLS_CONTENT_TYPE_APPLICATION,
        NULL, 0, pt, sizeof(pt) - 1,
        ctx.secrets.sender_data_secret,
        &keys, 0, reuse_guard,
        &msg.private_message) == 0);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_message_serialize(&msg, &buf) == 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsMLSMessage msg2;
    assert(mls_message_deserialize(&reader, &msg2) == 0);

    assert(msg2.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE);
    assert(msg2.cipher_suite == 0x0001);
    assert(msg2.private_message.epoch == ctx.epoch);

    /* Decrypt from deserialized message */
    MlsSecretTree dec_tree;
    assert(mls_secret_tree_init(&dec_tree, ctx.secrets.encryption_secret, 2) == 0);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    MlsSenderData sender;
    assert(mls_private_message_decrypt(
        &msg2.private_message, ctx.secrets.sender_data_secret,
        &dec_tree, 100, &decrypted, &dec_len, &sender) == 0);

    assert(dec_len == sizeof(pt) - 1);
    assert(memcmp(decrypted, pt, dec_len) == 0);

    free(decrypted);
    mls_tls_buf_free(&buf);
    mls_message_clear(&msg);
    mls_message_clear(&msg2);
    mls_secret_tree_free(&dec_tree);
    teardown_epoch(&ctx);
}

static void test_mls_message_wrong_version(void)
{
    /* Manually create a buffer with wrong version */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    mls_tls_write_u16(&buf, 99); /* wrong version */
    mls_tls_write_u16(&buf, 0x0001);
    mls_tls_write_u16(&buf, MLS_WIRE_FORMAT_PUBLIC_MESSAGE);

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsMLSMessage msg;
    assert(mls_message_deserialize(&reader, &msg) != 0);

    mls_tls_buf_free(&buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Integration: full sign → public message → serialize → verify
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_full_public_message_flow(void)
{
    /* Simulate: member signs a proposal, serializes as MLSMessage,
     * receiver deserializes, verifies signature and membership tag */
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    /* Build group context */
    uint8_t gid[] = "integration-group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    mls_crypto_random(th, sizeof(th));
    mls_crypto_random(cth, sizeof(cth));
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(gid, sizeof(gid) - 1, 7, th, cth,
                                        NULL, 0, &gc, &gc_len) == 0);

    /* Derive epoch secrets for membership key */
    uint8_t commit_secret[MLS_HASH_LEN];
    mls_crypto_random(commit_secret, sizeof(commit_secret));
    MlsEpochSecrets secrets;
    assert(mls_key_schedule_derive(NULL, commit_secret, gc, gc_len, NULL, &secrets) == 0);

    /* Build content (simulate a proposal body) */
    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = 0x0001;

    const uint8_t proposal_body[5] = {0x03 /* Remove */, 0x00, 0x00, 0x00, 0x02};
    const size_t proposal_body_len = sizeof(proposal_body);
    msg.public_message.content.group_id = malloc(sizeof(gid) - 1);
    memcpy(msg.public_message.content.group_id, gid, sizeof(gid) - 1);
    msg.public_message.content.group_id_len = sizeof(gid) - 1;
    msg.public_message.content.epoch = 7;
    msg.public_message.content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    msg.public_message.content.sender.leaf_index = 0;
    msg.public_message.content.content_type = MLS_CONTENT_TYPE_PROPOSAL;
    msg.public_message.content.content = malloc(proposal_body_len);
    memcpy(msg.public_message.content.content, proposal_body, proposal_body_len);
    msg.public_message.content.content_len = proposal_body_len;

    /* Sign */
    assert(mls_framed_content_sign(&msg.public_message.content,
                                    MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                    gc, gc_len, sk,
                                    &msg.public_message.auth) == 0);

    /* Membership tag */
    assert(mls_public_message_compute_membership_tag(&msg.public_message,
                                                      secrets.membership_key,
                                                      gc, gc_len) == 0);

    /* Serialize as MLSMessage */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_message_serialize(&msg, &buf) == 0);

    /* ── Receiver side ── */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsMLSMessage received;
    assert(mls_message_deserialize(&reader, &received) == 0);

    assert(received.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE);
    MlsPublicMessage *pm = &received.public_message;

    /* Verify signature */
    assert(mls_framed_content_verify(&pm->content, &pm->auth,
                                      MLS_WIRE_FORMAT_PUBLIC_MESSAGE,
                                      gc, gc_len, pk) == 0);

    /* Verify membership tag */
    assert(mls_public_message_verify_membership_tag(pm, secrets.membership_key,
                                                     gc, gc_len) == 0);

    /* Verify content */
    assert(pm->content.content_type == MLS_CONTENT_TYPE_PROPOSAL);
    assert(pm->content.content_len == proposal_body_len);
    assert(memcmp(pm->content.content, proposal_body, proposal_body_len) == 0);

    mls_tls_buf_free(&buf);
    mls_message_clear(&msg);
    mls_message_clear(&received);
    free(gc);
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

    printf("\n  --- FramedContent ---\n");
    TEST(test_framed_content_serialize_roundtrip);
    TEST(test_framed_content_tbs_deterministic);

    printf("\n  --- Content signing ---\n");
    TEST(test_framed_content_sign_verify);
    TEST(test_framed_content_sign_wrong_key);
    TEST(test_framed_content_sign_tampered_content);

    printf("\n  --- Confirmation tag ---\n");
    TEST(test_confirmation_tag_deterministic);
    TEST(test_confirmation_tag_different_inputs);

    printf("\n  --- PublicMessage ---\n");
    TEST(test_public_message_serialize_roundtrip);
    TEST(test_public_message_membership_tag_wrong_key);

    printf("\n  --- MLSMessage container ---\n");
    TEST(test_mls_message_public_roundtrip);
    TEST(test_mls_message_private_roundtrip);
    TEST(test_mls_message_wrong_version);

    printf("\n  --- Integration ---\n");
    TEST(test_full_public_message_flow);

    printf("\nAll MLS Message Framing tests passed.\n");
    return 0;
}
