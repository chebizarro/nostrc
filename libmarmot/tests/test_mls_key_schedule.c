/*
 * libmarmot - MLS Key Schedule tests (RFC 9420 §8, §9)
 *
 * Tests key schedule derivation, secret tree, per-sender message key
 * derivation, MLS Exporter, and GroupContext serialization.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_key_schedule.h"
#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Key schedule derivation tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_key_schedule_deterministic(void)
{
    /* Same inputs → same outputs */
    uint8_t commit_secret[MLS_HASH_LEN];
    memset(commit_secret, 0x01, sizeof(commit_secret));

    /* Build a minimal GroupContext */
    uint8_t group_id[] = "test-group";
    uint8_t tree_hash[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(tree_hash, 0x02, sizeof(tree_hash));
    memset(cth, 0x03, sizeof(cth));

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id) - 1,
                                        0, tree_hash, cth,
                                        NULL, 0,
                                        &gc_data, &gc_len) == 0);

    MlsEpochSecrets sec1, sec2;
    assert(mls_key_schedule_derive(NULL, commit_secret, gc_data, gc_len, NULL, &sec1) == 0);
    assert(mls_key_schedule_derive(NULL, commit_secret, gc_data, gc_len, NULL, &sec2) == 0);

    assert(memcmp(&sec1, &sec2, sizeof(MlsEpochSecrets)) == 0);

    free(gc_data);
}

static void test_key_schedule_different_commit_secrets(void)
{
    /* Different commit secrets → different epoch secrets */
    uint8_t cs1[MLS_HASH_LEN], cs2[MLS_HASH_LEN];
    memset(cs1, 0xAA, sizeof(cs1));
    memset(cs2, 0xBB, sizeof(cs2));

    uint8_t group_id[] = "group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x00, sizeof(th));
    memset(cth, 0x00, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, 5, 0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    MlsEpochSecrets sec1, sec2;
    assert(mls_key_schedule_derive(NULL, cs1, gc, gc_len, NULL, &sec1) == 0);
    assert(mls_key_schedule_derive(NULL, cs2, gc, gc_len, NULL, &sec2) == 0);

    assert(memcmp(sec1.sender_data_secret, sec2.sender_data_secret, MLS_HASH_LEN) != 0);
    assert(memcmp(sec1.encryption_secret, sec2.encryption_secret, MLS_HASH_LEN) != 0);
    assert(memcmp(sec1.init_secret, sec2.init_secret, MLS_HASH_LEN) != 0);

    free(gc);
}

static void test_key_schedule_different_epochs(void)
{
    /* Different epoch numbers → different GroupContext → different secrets */
    uint8_t cs[MLS_HASH_LEN];
    memset(cs, 0x11, sizeof(cs));

    uint8_t group_id[] = "group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x00, sizeof(th));
    memset(cth, 0x00, sizeof(cth));

    uint8_t *gc0 = NULL, *gc1 = NULL;
    size_t gc0_len = 0, gc1_len = 0;
    assert(mls_group_context_serialize(group_id, 5, 0, th, cth, NULL, 0, &gc0, &gc0_len) == 0);
    assert(mls_group_context_serialize(group_id, 5, 1, th, cth, NULL, 0, &gc1, &gc1_len) == 0);

    MlsEpochSecrets sec0, sec1;
    assert(mls_key_schedule_derive(NULL, cs, gc0, gc0_len, NULL, &sec0) == 0);
    assert(mls_key_schedule_derive(NULL, cs, gc1, gc1_len, NULL, &sec1) == 0);

    assert(memcmp(sec0.encryption_secret, sec1.encryption_secret, MLS_HASH_LEN) != 0);

    free(gc0);
    free(gc1);
}

static void test_key_schedule_with_init_secret(void)
{
    /* Epoch > 0: provide init_secret from previous epoch */
    uint8_t cs[MLS_HASH_LEN];
    memset(cs, 0x22, sizeof(cs));

    uint8_t group_id[] = "group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x00, sizeof(th));
    memset(cth, 0x00, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, 5, 0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    /* Derive epoch 0 */
    MlsEpochSecrets epoch0;
    assert(mls_key_schedule_derive(NULL, cs, gc, gc_len, NULL, &epoch0) == 0);

    /* Derive epoch 1 using init_secret from epoch 0 */
    free(gc);
    assert(mls_group_context_serialize(group_id, 5, 1, th, cth, NULL, 0, &gc, &gc_len) == 0);

    MlsEpochSecrets epoch1;
    assert(mls_key_schedule_derive(epoch0.init_secret, cs, gc, gc_len, NULL, &epoch1) == 0);

    /* Different from epoch 0 */
    assert(memcmp(epoch0.encryption_secret, epoch1.encryption_secret, MLS_HASH_LEN) != 0);

    /* Verify chain: using init_secret links epochs */
    MlsEpochSecrets epoch1_no_init;
    assert(mls_key_schedule_derive(NULL, cs, gc, gc_len, NULL, &epoch1_no_init) == 0);
    /* Should differ because init_secret != zero */
    assert(memcmp(epoch1.encryption_secret, epoch1_no_init.encryption_secret, MLS_HASH_LEN) != 0);

    free(gc);
}

static void test_key_schedule_with_psk(void)
{
    /* PSK secret changes the derivation */
    uint8_t cs[MLS_HASH_LEN];
    memset(cs, 0x33, sizeof(cs));

    uint8_t group_id[] = "group";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x00, sizeof(th));
    memset(cth, 0x00, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, 5, 0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    uint8_t psk[MLS_HASH_LEN];
    memset(psk, 0x99, sizeof(psk));

    MlsEpochSecrets no_psk, with_psk;
    assert(mls_key_schedule_derive(NULL, cs, gc, gc_len, NULL, &no_psk) == 0);
    assert(mls_key_schedule_derive(NULL, cs, gc, gc_len, psk, &with_psk) == 0);

    assert(memcmp(no_psk.encryption_secret, with_psk.encryption_secret, MLS_HASH_LEN) != 0);

    free(gc);
}

static void test_key_schedule_all_secrets_unique(void)
{
    /* All derived secrets within an epoch should be different */
    uint8_t cs[MLS_HASH_LEN];
    mls_crypto_random(cs, sizeof(cs));

    uint8_t group_id[] = "unique-test";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    mls_crypto_random(th, sizeof(th));
    mls_crypto_random(cth, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id) - 1,
                                        0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    MlsEpochSecrets sec;
    assert(mls_key_schedule_derive(NULL, cs, gc, gc_len, NULL, &sec) == 0);

    /* Collect all 32-byte secrets */
    const uint8_t *secrets[] = {
        sec.sender_data_secret, sec.encryption_secret, sec.exporter_secret,
        sec.external_secret, sec.confirmation_key, sec.membership_key,
        sec.resumption_psk, sec.epoch_authenticator, sec.init_secret,
        sec.welcome_secret, sec.joiner_secret
    };
    int n = sizeof(secrets) / sizeof(secrets[0]);

    /* Pairwise compare */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            assert(memcmp(secrets[i], secrets[j], MLS_HASH_LEN) != 0);
        }
    }

    free(gc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Secret tree tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_secret_tree_init_free(void)
{
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x42, sizeof(enc_secret));

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 4) == 0);
    assert(st.n_leaves == 4);
    assert(st.tree_secrets != NULL);
    assert(st.senders != NULL);
    assert(st.sender_initialized != NULL);

    mls_secret_tree_free(&st);
}

static void test_secret_tree_deterministic(void)
{
    /* Same encryption_secret → same tree secrets */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x77, sizeof(enc_secret));

    MlsSecretTree st1, st2;
    assert(mls_secret_tree_init(&st1, enc_secret, 4) == 0);
    assert(mls_secret_tree_init(&st2, enc_secret, 4) == 0);

    /* Derive keys for leaf 0 in both — should match */
    MlsMessageKeys keys1, keys2;
    assert(mls_secret_tree_derive_keys(&st1, 0, false, &keys1) == 0);
    assert(mls_secret_tree_derive_keys(&st2, 0, false, &keys2) == 0);

    assert(keys1.generation == 0);
    assert(keys2.generation == 0);
    assert(memcmp(keys1.key, keys2.key, MLS_AEAD_KEY_LEN) == 0);
    assert(memcmp(keys1.nonce, keys2.nonce, MLS_AEAD_NONCE_LEN) == 0);

    mls_secret_tree_free(&st1);
    mls_secret_tree_free(&st2);
}

static void test_secret_tree_different_senders(void)
{
    /* Different senders get different keys */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x88, sizeof(enc_secret));

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 4) == 0);

    MlsMessageKeys k0, k1, k2;
    assert(mls_secret_tree_derive_keys(&st, 0, false, &k0) == 0);
    assert(mls_secret_tree_derive_keys(&st, 1, false, &k1) == 0);
    assert(mls_secret_tree_derive_keys(&st, 2, false, &k2) == 0);

    assert(memcmp(k0.key, k1.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k0.key, k2.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k1.key, k2.key, MLS_AEAD_KEY_LEN) != 0);

    /* All at generation 0 */
    assert(k0.generation == 0);
    assert(k1.generation == 0);
    assert(k2.generation == 0);

    mls_secret_tree_free(&st);
}

static void test_secret_tree_generation_advance(void)
{
    /* Each call to derive_keys advances the generation */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x55, sizeof(enc_secret));

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 2) == 0);

    MlsMessageKeys k0, k1, k2;
    assert(mls_secret_tree_derive_keys(&st, 0, false, &k0) == 0);
    assert(k0.generation == 0);

    assert(mls_secret_tree_derive_keys(&st, 0, false, &k1) == 0);
    assert(k1.generation == 1);

    assert(mls_secret_tree_derive_keys(&st, 0, false, &k2) == 0);
    assert(k2.generation == 2);

    /* Each generation has different keys */
    assert(memcmp(k0.key, k1.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k1.key, k2.key, MLS_AEAD_KEY_LEN) != 0);

    mls_secret_tree_free(&st);
}

static void test_secret_tree_handshake_vs_application(void)
{
    /* Handshake and application ratchets are independent */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x66, sizeof(enc_secret));

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 2) == 0);

    MlsMessageKeys hs, app;
    assert(mls_secret_tree_derive_keys(&st, 0, true, &hs) == 0);
    assert(mls_secret_tree_derive_keys(&st, 0, false, &app) == 0);

    /* Both at generation 0 (independent counters) */
    assert(hs.generation == 0);
    assert(app.generation == 0);

    /* Different keys (different derivation paths) */
    assert(memcmp(hs.key, app.key, MLS_AEAD_KEY_LEN) != 0);

    mls_secret_tree_free(&st);
}

static void test_secret_tree_get_keys_for_generation(void)
{
    /* Forward ratchet to a specific generation */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x44, sizeof(enc_secret));

    MlsSecretTree st1, st2;
    assert(mls_secret_tree_init(&st1, enc_secret, 2) == 0);
    assert(mls_secret_tree_init(&st2, enc_secret, 2) == 0);

    /* Derive gen 0,1,2 sequentially in st1 */
    MlsMessageKeys k[3];
    for (int i = 0; i < 3; i++) {
        assert(mls_secret_tree_derive_keys(&st1, 0, false, &k[i]) == 0);
        assert(k[i].generation == (uint32_t)i);
    }

    /* Jump directly to gen 2 in st2 */
    MlsMessageKeys k2;
    assert(mls_secret_tree_get_keys_for_generation(&st2, 0, false, 2, 10, &k2) == 0);
    assert(k2.generation == 2);
    assert(memcmp(k2.key, k[2].key, MLS_AEAD_KEY_LEN) == 0);
    assert(memcmp(k2.nonce, k[2].nonce, MLS_AEAD_NONCE_LEN) == 0);

    mls_secret_tree_free(&st1);
    mls_secret_tree_free(&st2);
}

static void test_secret_tree_max_forward_distance(void)
{
    /* Forward ratchet beyond max distance should fail */
    uint8_t enc_secret[MLS_HASH_LEN];
    memset(enc_secret, 0x33, sizeof(enc_secret));

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 2) == 0);

    MlsMessageKeys k;
    /* Try to jump to generation 100 with max forward distance of 5 */
    int rc = mls_secret_tree_get_keys_for_generation(&st, 0, false, 100, 5, &k);
    assert(rc != 0);

    /* But jumping to generation 5 with max 5 should succeed */
    assert(mls_secret_tree_get_keys_for_generation(&st, 0, false, 5, 5, &k) == 0);

    mls_secret_tree_free(&st);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MLS Exporter tests (RFC 9420 §8.5)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_exporter_deterministic(void)
{
    uint8_t exp_secret[MLS_HASH_LEN];
    memset(exp_secret, 0xAA, sizeof(exp_secret));

    uint8_t out1[32], out2[32];
    assert(mls_exporter(exp_secret, "marmot-nip44-key", NULL, 0, out1, 32) == 0);
    assert(mls_exporter(exp_secret, "marmot-nip44-key", NULL, 0, out2, 32) == 0);
    assert(memcmp(out1, out2, 32) == 0);
}

static void test_exporter_different_labels(void)
{
    uint8_t exp_secret[MLS_HASH_LEN];
    memset(exp_secret, 0xBB, sizeof(exp_secret));

    uint8_t out1[32], out2[32];
    assert(mls_exporter(exp_secret, "label-one", NULL, 0, out1, 32) == 0);
    assert(mls_exporter(exp_secret, "label-two", NULL, 0, out2, 32) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

static void test_exporter_different_contexts(void)
{
    uint8_t exp_secret[MLS_HASH_LEN];
    memset(exp_secret, 0xCC, sizeof(exp_secret));

    uint8_t ctx1[] = "context-a";
    uint8_t ctx2[] = "context-b";
    uint8_t out1[32], out2[32];
    assert(mls_exporter(exp_secret, "same-label", ctx1, sizeof(ctx1) - 1, out1, 32) == 0);
    assert(mls_exporter(exp_secret, "same-label", ctx2, sizeof(ctx2) - 1, out2, 32) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

static void test_exporter_different_lengths(void)
{
    uint8_t exp_secret[MLS_HASH_LEN];
    memset(exp_secret, 0xDD, sizeof(exp_secret));

    uint8_t out16[16], out32[32];
    assert(mls_exporter(exp_secret, "test", NULL, 0, out16, 16) == 0);
    assert(mls_exporter(exp_secret, "test", NULL, 0, out32, 32) == 0);

    /* First 16 bytes might or might not match depending on HKDF-Expand behavior
     * with different output lengths — but both should succeed */
}

/* ══════════════════════════════════════════════════════════════════════════
 * GroupContext serialization tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_group_context_serialize(void)
{
    uint8_t group_id[] = "test-group-id";
    uint8_t tree_hash[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(tree_hash, 0x11, sizeof(tree_hash));
    memset(cth, 0x22, sizeof(cth));

    uint8_t *data = NULL;
    size_t len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id) - 1,
                                        42, tree_hash, cth,
                                        NULL, 0,
                                        &data, &len) == 0);
    assert(data != NULL);
    assert(len > 0);

    /* Verify the TLS structure by reading back */
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);

    /* version: uint16 = 1 (mls10) */
    uint16_t version;
    assert(mls_tls_read_u16(&r, &version) == 0);
    assert(version == 1);

    /* cipher_suite: uint16 = 0x0001 */
    uint16_t cs;
    assert(mls_tls_read_u16(&r, &cs) == 0);
    assert(cs == 0x0001);

    /* group_id: opaque8 */
    uint8_t *gid = NULL;
    size_t gid_len = 0;
    assert(mls_tls_read_opaque8(&r, &gid, &gid_len) == 0);
    assert(gid_len == sizeof(group_id) - 1);
    assert(memcmp(gid, group_id, gid_len) == 0);
    free(gid);

    /* epoch: uint64 */
    uint64_t epoch;
    assert(mls_tls_read_u64(&r, &epoch) == 0);
    assert(epoch == 42);

    /* tree_hash: opaque8 */
    uint8_t *th = NULL;
    size_t th_len = 0;
    assert(mls_tls_read_opaque8(&r, &th, &th_len) == 0);
    assert(th_len == MLS_HASH_LEN);
    assert(memcmp(th, tree_hash, MLS_HASH_LEN) == 0);
    free(th);

    /* confirmed_transcript_hash: opaque8 */
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(mls_tls_read_opaque8(&r, &ct, &ct_len) == 0);
    assert(ct_len == MLS_HASH_LEN);
    assert(memcmp(ct, cth, MLS_HASH_LEN) == 0);
    free(ct);

    /* extensions: opaque32 (should be empty) */
    uint8_t *ext = NULL;
    size_t ext_len = 0;
    assert(mls_tls_read_opaque32(&r, &ext, &ext_len) == 0);
    assert(ext_len == 0);
    free(ext);

    free(data);
}

static void test_group_context_with_extensions(void)
{
    uint8_t group_id[] = "grp";
    uint8_t tree_hash[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(tree_hash, 0x00, sizeof(tree_hash));
    memset(cth, 0x00, sizeof(cth));

    uint8_t ext_data[] = {0xF2, 0xEE, 0x00, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t *data = NULL;
    size_t len = 0;
    assert(mls_group_context_serialize(group_id, 3, 0, tree_hash, cth,
                                        ext_data, sizeof(ext_data),
                                        &data, &len) == 0);
    assert(data != NULL);
    assert(len > 0);

    /* Just verify it's longer than without extensions */
    uint8_t *data2 = NULL;
    size_t len2 = 0;
    assert(mls_group_context_serialize(group_id, 3, 0, tree_hash, cth,
                                        NULL, 0,
                                        &data2, &len2) == 0);
    assert(len > len2);

    free(data);
    free(data2);
}

static void test_group_context_deterministic(void)
{
    uint8_t group_id[] = "determ";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x44, sizeof(th));
    memset(cth, 0x55, sizeof(cth));

    uint8_t *d1 = NULL, *d2 = NULL;
    size_t l1 = 0, l2 = 0;
    assert(mls_group_context_serialize(group_id, 6, 100, th, cth, NULL, 0, &d1, &l1) == 0);
    assert(mls_group_context_serialize(group_id, 6, 100, th, cth, NULL, 0, &d2, &l2) == 0);

    assert(l1 == l2);
    assert(memcmp(d1, d2, l1) == 0);

    free(d1);
    free(d2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Integration: full epoch derivation + message key extraction
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_full_epoch_to_message_keys(void)
{
    /* Simulate a complete flow: key schedule → secret tree → message keys */
    uint8_t commit_secret[MLS_HASH_LEN];
    mls_crypto_random(commit_secret, sizeof(commit_secret));

    uint8_t group_id[] = "integration-test";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    mls_crypto_random(th, sizeof(th));
    mls_crypto_random(cth, sizeof(cth));

    uint8_t *gc = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id) - 1,
                                        0, th, cth, NULL, 0, &gc, &gc_len) == 0);

    /* Derive epoch secrets */
    MlsEpochSecrets secrets;
    assert(mls_key_schedule_derive(NULL, commit_secret, gc, gc_len, NULL, &secrets) == 0);

    /* Initialize secret tree with 4 members */
    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, secrets.encryption_secret, 4) == 0);

    /* Each member derives their first application message key */
    MlsMessageKeys member_keys[4];
    for (uint32_t i = 0; i < 4; i++) {
        assert(mls_secret_tree_derive_keys(&st, i, false, &member_keys[i]) == 0);
        assert(member_keys[i].generation == 0);
    }

    /* All members should have different keys */
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            assert(memcmp(member_keys[i].key, member_keys[j].key, MLS_AEAD_KEY_LEN) != 0);
        }
    }

    /* Derive an exported secret (Marmot NIP-44 key) */
    uint8_t nip44_key[32];
    assert(mls_exporter(secrets.exporter_secret, "marmot-nip44",
                        NULL, 0, nip44_key, 32) == 0);

    /* Non-zero */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (nip44_key[i] != 0) { all_zero = 0; break; }
    }
    assert(!all_zero);

    mls_secret_tree_free(&st);
    free(gc);
}

static void test_multi_epoch_chain(void)
{
    /* Chain multiple epochs using init_secret */
    uint8_t group_id[] = "chain";
    uint8_t th[MLS_HASH_LEN], cth[MLS_HASH_LEN];
    memset(th, 0x00, sizeof(th));
    memset(cth, 0x00, sizeof(cth));

    MlsEpochSecrets prev_sec;
    memset(&prev_sec, 0, sizeof(prev_sec));

    const uint8_t *init = NULL; /* NULL for epoch 0 */

    for (uint64_t epoch = 0; epoch < 5; epoch++) {
        uint8_t cs[MLS_HASH_LEN];
        mls_crypto_random(cs, sizeof(cs));

        uint8_t *gc = NULL;
        size_t gc_len = 0;
        assert(mls_group_context_serialize(group_id, 5, epoch, th, cth,
                                            NULL, 0, &gc, &gc_len) == 0);

        MlsEpochSecrets sec;
        assert(mls_key_schedule_derive(init, cs, gc, gc_len, NULL, &sec) == 0);
        free(gc);

        /* Verify secrets are non-zero */
        int non_zero = 0;
        for (int i = 0; i < MLS_HASH_LEN; i++) {
            if (sec.encryption_secret[i] != 0) { non_zero = 1; break; }
        }
        assert(non_zero);

        /* Chain to next epoch */
        memcpy(&prev_sec, &sec, sizeof(sec));
        init = prev_sec.init_secret;
    }
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

    printf("libmarmot: MLS Key Schedule tests\n");

    printf("\n  --- Key schedule derivation ---\n");
    TEST(test_key_schedule_deterministic);
    TEST(test_key_schedule_different_commit_secrets);
    TEST(test_key_schedule_different_epochs);
    TEST(test_key_schedule_with_init_secret);
    TEST(test_key_schedule_with_psk);
    TEST(test_key_schedule_all_secrets_unique);

    printf("\n  --- Secret tree ---\n");
    TEST(test_secret_tree_init_free);
    TEST(test_secret_tree_deterministic);
    TEST(test_secret_tree_different_senders);
    TEST(test_secret_tree_generation_advance);
    TEST(test_secret_tree_handshake_vs_application);
    TEST(test_secret_tree_get_keys_for_generation);
    TEST(test_secret_tree_max_forward_distance);

    printf("\n  --- MLS Exporter ---\n");
    TEST(test_exporter_deterministic);
    TEST(test_exporter_different_labels);
    TEST(test_exporter_different_contexts);
    TEST(test_exporter_different_lengths);

    printf("\n  --- GroupContext serialization ---\n");
    TEST(test_group_context_serialize);
    TEST(test_group_context_with_extensions);
    TEST(test_group_context_deterministic);

    printf("\n  --- Integration ---\n");
    TEST(test_full_epoch_to_message_keys);
    TEST(test_multi_epoch_chain);

    printf("\nAll MLS Key Schedule tests passed.\n");
    return 0;
}
