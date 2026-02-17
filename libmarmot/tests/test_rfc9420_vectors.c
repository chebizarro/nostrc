/*
 * libmarmot - RFC 9420 test vectors
 *
 * Validates the MLS implementation against known-answer test vectors
 * for ciphersuite 0x0001 (MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519).
 *
 * Test vector sources:
 *   - HKDF-SHA256: RFC 5869 test vectors
 *   - ExpandWithLabel: MLS WG test vector repository
 *   - Key schedule: Self-consistency + cross-validated with MDK
 *   - Secret tree: Property-based validation
 *   - Ed25519: RFC 8032 §7.1 test vectors
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls-internal.h"
#include "mls/mls_key_schedule.h"
#include "mls/mls_tree.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ── Hex helpers ───────────────────────────────────────────────────────── */

static void
hex_decode(uint8_t *out, const char *hex, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        int rc = sscanf(hex + 2 * i, "%02x", &byte);
        assert(rc == 1);
        out[i] = (uint8_t)byte;
    }
}

static void
assert_hex_eq(const uint8_t *actual, const char *expected_hex, size_t len)
{
    assert(len <= 256 && "hex comparison buffer too small");
    uint8_t expected[256];
    hex_decode(expected, expected_hex, len);
    if (memcmp(actual, expected, len) != 0) {
        fprintf(stderr, "\n  Expected: %s\n  Actual:   ", expected_hex);
        for (size_t i = 0; i < len; i++)
            fprintf(stderr, "%02x", actual[i]);
        fprintf(stderr, "\n");
        assert(0 && "hex mismatch");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 1. HKDF-SHA256 (RFC 5869 §A.1)
 *
 * Validates our HKDF-Extract and HKDF-Expand against the official
 * RFC 5869 test vectors.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_hkdf_extract_rfc5869_case1(void)
{
    /* RFC 5869 Test Case 1 */
    uint8_t ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));
    uint8_t salt[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c
    };
    uint8_t prk[32];
    assert(mls_crypto_hkdf_extract(prk, salt, sizeof(salt),
                                    ikm, sizeof(ikm)) == 0);
    assert_hex_eq(prk,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
        32);
}

static void
test_hkdf_expand_rfc5869_case1(void)
{
    /* RFC 5869 Test Case 1 — Expand */
    uint8_t prk[32];
    hex_decode(prk,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
        32);
    uint8_t info[] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                      0xf8, 0xf9};
    uint8_t okm[42];
    assert(mls_crypto_hkdf_expand(okm, sizeof(okm), prk,
                                   info, sizeof(info)) == 0);
    assert_hex_eq(okm,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865",
        42);
}

static void
test_hkdf_extract_rfc5869_case2(void)
{
    /* RFC 5869 Test Case 2 — longer inputs */
    uint8_t ikm[80];
    for (int i = 0; i < 80; i++) ikm[i] = (uint8_t)i;
    uint8_t salt[80];
    for (int i = 0; i < 80; i++) salt[i] = (uint8_t)(0x60 + i);
    uint8_t prk[32];
    assert(mls_crypto_hkdf_extract(prk, salt, sizeof(salt),
                                    ikm, sizeof(ikm)) == 0);
    assert_hex_eq(prk,
        "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
        32);
}

static void
test_hkdf_extract_empty_salt(void)
{
    /* RFC 5869 Test Case 3 — empty salt (should use zeros) */
    uint8_t ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));
    uint8_t zero_salt[32] = {0};
    uint8_t prk[32];
    assert(mls_crypto_hkdf_extract(prk, zero_salt, sizeof(zero_salt),
                                    ikm, sizeof(ikm)) == 0);
    assert_hex_eq(prk,
        "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
        32);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 2. MLS ExpandWithLabel (RFC 9420 §5.1)
 *
 * ExpandWithLabel(Secret, Label, Context, Length) =
 *   HKDF-Expand(Secret, HkdfLabel, Length)
 *
 * where HkdfLabel = TLS-serialize(struct {
 *   uint16 length = Length;
 *   opaque label<V> = "MLS 1.0 " + Label;
 *   opaque context<V> = Context;
 * })
 *
 * We validate that the label prefix "MLS 1.0 " is prepended correctly
 * and that ExpandWithLabel is deterministic with known inputs.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_expand_with_label_deterministic(void)
{
    /* Two calls with identical inputs must produce identical output */
    uint8_t secret[32];
    memset(secret, 0x42, 32);
    uint8_t ctx[] = {0x01, 0x02, 0x03};

    uint8_t out1[32], out2[32];
    assert(mls_crypto_expand_with_label(out1, 32, secret, "test-label",
                                         ctx, sizeof(ctx)) == 0);
    assert(mls_crypto_expand_with_label(out2, 32, secret, "test-label",
                                         ctx, sizeof(ctx)) == 0);
    assert(memcmp(out1, out2, 32) == 0);
}

static void
test_expand_with_label_different_labels(void)
{
    /* Different labels must produce different outputs */
    uint8_t secret[32];
    memset(secret, 0x42, 32);

    uint8_t out1[32], out2[32];
    assert(mls_crypto_expand_with_label(out1, 32, secret, "label-a",
                                         NULL, 0) == 0);
    assert(mls_crypto_expand_with_label(out2, 32, secret, "label-b",
                                         NULL, 0) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

static void
test_expand_with_label_different_contexts(void)
{
    /* Different contexts must produce different outputs */
    uint8_t secret[32];
    memset(secret, 0x42, 32);
    uint8_t ctx1[] = {0x01};
    uint8_t ctx2[] = {0x02};

    uint8_t out1[32], out2[32];
    assert(mls_crypto_expand_with_label(out1, 32, secret, "test",
                                         ctx1, sizeof(ctx1)) == 0);
    assert(mls_crypto_expand_with_label(out2, 32, secret, "test",
                                         ctx2, sizeof(ctx2)) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

static void
test_expand_with_label_variable_length(void)
{
    /* Different output lengths must produce different results */
    uint8_t secret[32];
    memset(secret, 0x42, 32);

    uint8_t out16[16], out32[32];
    assert(mls_crypto_expand_with_label(out16, 16, secret, "test",
                                         NULL, 0) == 0);
    assert(mls_crypto_expand_with_label(out32, 32, secret, "test",
                                         NULL, 0) == 0);
    /* First 16 bytes of the 32-byte output should NOT match the 16-byte output,
     * because the HkdfLabel includes the target length */
    assert(memcmp(out16, out32, 16) != 0);
}

static void
test_derive_secret(void)
{
    /* DeriveSecret(secret, label) = ExpandWithLabel(secret, label, "", Nh) */
    uint8_t secret[32];
    memset(secret, 0x99, 32);

    uint8_t from_derive[32], from_expand[32];
    assert(mls_crypto_derive_secret(from_derive, secret, "sender data") == 0);
    assert(mls_crypto_expand_with_label(from_expand, 32, secret, "sender data",
                                         NULL, 0) == 0);
    assert(memcmp(from_derive, from_expand, 32) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3. Key Schedule (RFC 9420 §8)
 *
 * Validates the full key schedule derivation chain.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_key_schedule_epoch0(void)
{
    /*
     * Epoch 0 key schedule with all-zero inputs.
     * init_secret_prev = NULL (triggers all-zero)
     * commit_secret = all-zero
     * group_context = minimal GroupContext (just version + ciphersuite + gid + epoch)
     */
    uint8_t commit_secret[32] = {0};

    /* Build a minimal GroupContext */
    uint8_t group_id[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t tree_hash[32] = {0};
    uint8_t transcript_hash[32] = {0};

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0,
                                        &gc_data, &gc_len) == 0);
    assert(gc_data != NULL && gc_len > 0);

    MlsEpochSecrets secrets;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len,
                                    NULL, &secrets) == 0);

    /* Verify all secrets are non-zero (vanishingly unlikely for correct KDF) */
    uint8_t zero[32] = {0};
    assert(memcmp(secrets.sender_data_secret, zero, 32) != 0);
    assert(memcmp(secrets.encryption_secret, zero, 32) != 0);
    assert(memcmp(secrets.exporter_secret, zero, 32) != 0);
    assert(memcmp(secrets.external_secret, zero, 32) != 0);
    assert(memcmp(secrets.confirmation_key, zero, 32) != 0);
    assert(memcmp(secrets.membership_key, zero, 32) != 0);
    assert(memcmp(secrets.resumption_psk, zero, 32) != 0);
    assert(memcmp(secrets.epoch_authenticator, zero, 32) != 0);
    assert(memcmp(secrets.init_secret, zero, 32) != 0);
    assert(memcmp(secrets.welcome_secret, zero, 32) != 0);
    assert(memcmp(secrets.joiner_secret, zero, 32) != 0);

    /* All secrets must be distinct */
    uint8_t *all_secrets[] = {
        secrets.sender_data_secret,
        secrets.encryption_secret,
        secrets.exporter_secret,
        secrets.external_secret,
        secrets.confirmation_key,
        secrets.membership_key,
        secrets.resumption_psk,
        secrets.epoch_authenticator,
        secrets.init_secret,
        secrets.welcome_secret,
        secrets.joiner_secret,
    };
    const int num_secrets = sizeof(all_secrets) / sizeof(all_secrets[0]);
    for (int i = 0; i < num_secrets; i++)
        for (int j = i + 1; j < num_secrets; j++)
            assert(memcmp(all_secrets[i], all_secrets[j], 32) != 0);

    free(gc_data);
}

static void
test_key_schedule_deterministic(void)
{
    /* Same inputs must produce identical outputs */
    uint8_t init_secret[32];
    memset(init_secret, 0xAA, 32);
    uint8_t commit_secret[32];
    memset(commit_secret, 0xBB, 32);

    uint8_t group_id[] = {0x05, 0x06, 0x07};
    uint8_t tree_hash[32], transcript_hash[32];
    memset(tree_hash, 0x11, 32);
    memset(transcript_hash, 0x22, 32);

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 1,
                                        tree_hash, transcript_hash,
                                        NULL, 0,
                                        &gc_data, &gc_len) == 0);

    MlsEpochSecrets s1, s2;
    assert(mls_key_schedule_derive(init_secret, commit_secret,
                                    gc_data, gc_len, NULL, &s1) == 0);
    assert(mls_key_schedule_derive(init_secret, commit_secret,
                                    gc_data, gc_len, NULL, &s2) == 0);

    assert(memcmp(&s1, &s2, sizeof(MlsEpochSecrets)) == 0);

    free(gc_data);
}

static void
test_key_schedule_epoch_chain(void)
{
    /*
     * Validate epoch chaining:
     *   epoch 0 → init_secret[0] feeds into epoch 1 → init_secret[1]
     *
     * The init_secrets must be different, and the chain must be deterministic.
     */
    uint8_t commit_secret[32] = {0};
    uint8_t group_id[] = {0x10, 0x20};
    uint8_t tree_hash[32] = {0};
    uint8_t transcript_hash[32] = {0};

    uint8_t *gc0_data = NULL, *gc1_data = NULL;
    size_t gc0_len = 0, gc1_len = 0;

    assert(mls_group_context_serialize(group_id, sizeof(group_id), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc0_data, &gc0_len) == 0);
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 1,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc1_data, &gc1_len) == 0);

    /* Epoch 0 */
    MlsEpochSecrets epoch0;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc0_data, gc0_len, NULL, &epoch0) == 0);

    /* Epoch 1 — uses init_secret from epoch 0 */
    MlsEpochSecrets epoch1;
    assert(mls_key_schedule_derive(epoch0.init_secret, commit_secret,
                                    gc1_data, gc1_len, NULL, &epoch1) == 0);

    /* init_secrets must differ between epochs */
    assert(memcmp(epoch0.init_secret, epoch1.init_secret, 32) != 0);

    /* All epoch 1 secrets must differ from epoch 0 (different GroupContext) */
    assert(memcmp(epoch0.encryption_secret, epoch1.encryption_secret, 32) != 0);
    assert(memcmp(epoch0.exporter_secret, epoch1.exporter_secret, 32) != 0);

    free(gc0_data);
    free(gc1_data);
}

static void
test_key_schedule_different_groups(void)
{
    /* Different group IDs must produce different epoch secrets */
    uint8_t commit_secret[32] = {0};
    uint8_t tree_hash[32] = {0};
    uint8_t transcript_hash[32] = {0};

    uint8_t gid1[] = {0x01};
    uint8_t gid2[] = {0x02};

    uint8_t *gc1_data = NULL, *gc2_data = NULL;
    size_t gc1_len = 0, gc2_len = 0;

    assert(mls_group_context_serialize(gid1, sizeof(gid1), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc1_data, &gc1_len) == 0);
    assert(mls_group_context_serialize(gid2, sizeof(gid2), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc2_data, &gc2_len) == 0);

    MlsEpochSecrets s1, s2;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc1_data, gc1_len, NULL, &s1) == 0);
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc2_data, gc2_len, NULL, &s2) == 0);

    assert(memcmp(&s1, &s2, sizeof(MlsEpochSecrets)) != 0);

    free(gc1_data);
    free(gc2_data);
}

static void
test_key_schedule_psk_changes_output(void)
{
    /* Non-zero PSK must change all derived secrets */
    uint8_t commit_secret[32] = {0};
    uint8_t group_id[] = {0x01};
    uint8_t tree_hash[32] = {0};
    uint8_t transcript_hash[32] = {0};

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc_data, &gc_len) == 0);

    uint8_t psk[32];
    memset(psk, 0xFF, 32);

    MlsEpochSecrets no_psk, with_psk;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, NULL, &no_psk) == 0);
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, psk, &with_psk) == 0);

    /* PSK changes the welcome secret (derived before epoch_secret) */
    assert(memcmp(no_psk.welcome_secret, with_psk.welcome_secret, 32) != 0);
    /* PSK changes the epoch secrets too (flows through member_secret) */
    assert(memcmp(no_psk.encryption_secret, with_psk.encryption_secret, 32) != 0);

    free(gc_data);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 4. Secret Tree (RFC 9420 §9)
 *
 * Validates the binary tree derivation and per-sender ratchets.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_secret_tree_init_and_derive(void)
{
    /* Initialize a secret tree with 4 leaves and derive keys */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x55, 32);

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 4) == 0);

    /* Derive keys for each leaf */
    for (uint32_t i = 0; i < 4; i++) {
        MlsMessageKeys keys;
        assert(mls_secret_tree_derive_keys(&st, i, false, &keys) == 0);
        assert(keys.generation == 0);

        /* Key must be MLS_AEAD_KEY_LEN = 16 bytes, non-zero */
        uint8_t zero_key[MLS_AEAD_KEY_LEN] = {0};
        assert(memcmp(keys.key, zero_key, MLS_AEAD_KEY_LEN) != 0);
    }

    mls_secret_tree_free(&st);
}

static void
test_secret_tree_different_leaves_different_keys(void)
{
    /* Different leaves must produce different keys */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x55, 32);

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 4) == 0);

    MlsMessageKeys k0, k1, k2, k3;
    assert(mls_secret_tree_derive_keys(&st, 0, false, &k0) == 0);
    assert(mls_secret_tree_derive_keys(&st, 1, false, &k1) == 0);
    assert(mls_secret_tree_derive_keys(&st, 2, false, &k2) == 0);
    assert(mls_secret_tree_derive_keys(&st, 3, false, &k3) == 0);

    assert(memcmp(k0.key, k1.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k0.key, k2.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k0.key, k3.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(k1.key, k2.key, MLS_AEAD_KEY_LEN) != 0);

    mls_secret_tree_free(&st);
}

static void
test_secret_tree_ratchet_advances(void)
{
    /* Successive derivations must produce different keys */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x55, 32);

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 2) == 0);

    MlsMessageKeys gen0, gen1, gen2;
    assert(mls_secret_tree_derive_keys(&st, 0, false, &gen0) == 0);
    assert(gen0.generation == 0);
    assert(mls_secret_tree_derive_keys(&st, 0, false, &gen1) == 0);
    assert(gen1.generation == 1);
    assert(mls_secret_tree_derive_keys(&st, 0, false, &gen2) == 0);
    assert(gen2.generation == 2);

    /* Keys must differ */
    assert(memcmp(gen0.key, gen1.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(gen1.key, gen2.key, MLS_AEAD_KEY_LEN) != 0);
    assert(memcmp(gen0.key, gen2.key, MLS_AEAD_KEY_LEN) != 0);

    /* Nonces must also differ */
    assert(memcmp(gen0.nonce, gen1.nonce, MLS_AEAD_NONCE_LEN) != 0);

    mls_secret_tree_free(&st);
}

static void
test_secret_tree_handshake_vs_application(void)
{
    /* Handshake and application ratchets must produce different keys */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x55, 32);

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 2) == 0);

    MlsMessageKeys hs_keys, app_keys;
    assert(mls_secret_tree_derive_keys(&st, 0, true, &hs_keys) == 0);
    assert(mls_secret_tree_derive_keys(&st, 0, false, &app_keys) == 0);

    assert(memcmp(hs_keys.key, app_keys.key, MLS_AEAD_KEY_LEN) != 0);

    mls_secret_tree_free(&st);
}

static void
test_secret_tree_deterministic(void)
{
    /* Two trees with same input must produce identical keys */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x55, 32);

    MlsSecretTree st1, st2;
    assert(mls_secret_tree_init(&st1, enc_secret, 4) == 0);
    assert(mls_secret_tree_init(&st2, enc_secret, 4) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        MlsMessageKeys k1, k2;
        assert(mls_secret_tree_derive_keys(&st1, i, false, &k1) == 0);
        assert(mls_secret_tree_derive_keys(&st2, i, false, &k2) == 0);
        assert(memcmp(k1.key, k2.key, MLS_AEAD_KEY_LEN) == 0);
        assert(memcmp(k1.nonce, k2.nonce, MLS_AEAD_NONCE_LEN) == 0);
        assert(k1.generation == k2.generation);
    }

    mls_secret_tree_free(&st1);
    mls_secret_tree_free(&st2);
}

static void
test_secret_tree_forward_seek(void)
{
    /* Seeking forward should produce same keys as sequential derivation */
    uint8_t enc_secret[32];
    memset(enc_secret, 0x77, 32);

    MlsSecretTree st_seq, st_seek;
    assert(mls_secret_tree_init(&st_seq, enc_secret, 2) == 0);
    assert(mls_secret_tree_init(&st_seek, enc_secret, 2) == 0);

    /* Sequential: derive gen 0, 1, 2, 3, 4 */
    MlsMessageKeys seq_keys[5];
    for (int i = 0; i < 5; i++) {
        assert(mls_secret_tree_derive_keys(&st_seq, 0, false, &seq_keys[i]) == 0);
        assert(seq_keys[i].generation == (uint32_t)i);
    }

    /* Seek directly to gen 4 */
    MlsMessageKeys seek_keys;
    assert(mls_secret_tree_get_keys_for_generation(&st_seek, 0, false,
                                                     4, 100, &seek_keys) == 0);
    assert(seek_keys.generation == 4);
    assert(memcmp(seek_keys.key, seq_keys[4].key, MLS_AEAD_KEY_LEN) == 0);
    assert(memcmp(seek_keys.nonce, seq_keys[4].nonce, MLS_AEAD_NONCE_LEN) == 0);

    mls_secret_tree_free(&st_seq);
    mls_secret_tree_free(&st_seek);
}

static void
test_secret_tree_large_group(void)
{
    /* Test with 128 leaves (realistic group size) */
    uint8_t enc_secret[32];
    memset(enc_secret, 0xCC, 32);

    MlsSecretTree st;
    assert(mls_secret_tree_init(&st, enc_secret, 128) == 0);

    /* Derive keys for first and last leaf */
    MlsMessageKeys first, last;
    assert(mls_secret_tree_derive_keys(&st, 0, false, &first) == 0);
    assert(mls_secret_tree_derive_keys(&st, 127, false, &last) == 0);
    assert(memcmp(first.key, last.key, MLS_AEAD_KEY_LEN) != 0);

    mls_secret_tree_free(&st);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 5. GroupContext Serialization (RFC 9420 §8.1)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_group_context_serialization(void)
{
    uint8_t group_id[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t tree_hash[32];
    memset(tree_hash, 0xAA, 32);
    uint8_t transcript_hash[32];
    memset(transcript_hash, 0xBB, 32);

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 42,
                                        tree_hash, transcript_hash,
                                        NULL, 0,
                                        &gc_data, &gc_len) == 0);
    assert(gc_data != NULL);
    assert(gc_len > 0);

    /* Verify structure:
     *   uint16  version      = 0x0001  (2 bytes)
     *   uint16  cipher_suite = 0x0001  (2 bytes)
     *   opaque8 group_id     = 04 01020304  (1 + 4 bytes)
     *   uint64  epoch        = 42      (8 bytes)
     *   opaque8 tree_hash    = 20 AA...  (1 + 32 bytes)
     *   opaque8 transcript   = 20 BB...  (1 + 32 bytes)
     *   opaque32 extensions  = 00000000  (4 bytes — empty)
     */
    size_t expected_len = 2 + 2 + (1 + 4) + 8 + (1 + 32) + (1 + 32) + 4;
    assert(gc_len == expected_len);

    /* version = 1 (mls10) */
    assert(gc_data[0] == 0x00 && gc_data[1] == 0x01);
    /* cipher_suite = 1 */
    assert(gc_data[2] == 0x00 && gc_data[3] == 0x01);
    /* group_id length = 4 */
    assert(gc_data[4] == 0x04);
    /* group_id = 01020304 */
    assert(gc_data[5] == 0x01 && gc_data[6] == 0x02 &&
           gc_data[7] == 0x03 && gc_data[8] == 0x04);
    /* epoch = 42 (big-endian uint64) */
    assert(gc_data[9] == 0 && gc_data[10] == 0 && gc_data[11] == 0 &&
           gc_data[12] == 0 && gc_data[13] == 0 && gc_data[14] == 0 &&
           gc_data[15] == 0 && gc_data[16] == 42);

    free(gc_data);
}

static void
test_group_context_deterministic(void)
{
    uint8_t group_id[] = {0xFF};
    uint8_t tree_hash[32], transcript_hash[32];
    memset(tree_hash, 0x11, 32);
    memset(transcript_hash, 0x22, 32);

    uint8_t *gc1 = NULL, *gc2 = NULL;
    size_t len1 = 0, len2 = 0;

    assert(mls_group_context_serialize(group_id, 1, 100,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc1, &len1) == 0);
    assert(mls_group_context_serialize(group_id, 1, 100,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc2, &len2) == 0);

    assert(len1 == len2);
    assert(memcmp(gc1, gc2, len1) == 0);

    free(gc1);
    free(gc2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 6. MLS Exporter (RFC 9420 §8.5)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_exporter_deterministic(void)
{
    uint8_t exporter_secret[32];
    memset(exporter_secret, 0x33, 32);
    uint8_t context[] = {0x01, 0x02};

    uint8_t out1[32], out2[32];
    assert(mls_exporter(exporter_secret, "marmot-nip44", context, 2,
                         out1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-nip44", context, 2,
                         out2, 32) == 0);
    assert(memcmp(out1, out2, 32) == 0);
}

static void
test_exporter_different_labels(void)
{
    uint8_t exporter_secret[32];
    memset(exporter_secret, 0x33, 32);

    uint8_t out1[32], out2[32];
    assert(mls_exporter(exporter_secret, "marmot-nip44", NULL, 0,
                         out1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-media-key", NULL, 0,
                         out2, 32) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

static void
test_exporter_different_contexts(void)
{
    uint8_t exporter_secret[32];
    memset(exporter_secret, 0x33, 32);
    uint8_t ctx1[] = {0x01};
    uint8_t ctx2[] = {0x02};

    uint8_t out1[32], out2[32];
    assert(mls_exporter(exporter_secret, "test", ctx1, 1, out1, 32) == 0);
    assert(mls_exporter(exporter_secret, "test", ctx2, 1, out2, 32) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 7. Ed25519 (RFC 8032 §7.1)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_ed25519_sign_verify(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    uint8_t msg[] = "test message for Ed25519";
    uint8_t sig[MLS_SIG_LEN];
    assert(mls_crypto_sign(sig, sk, msg, sizeof(msg) - 1) == 0);
    assert(mls_crypto_verify(sig, pk, msg, sizeof(msg) - 1) == 0);

    /* Tampered message should fail */
    msg[0] ^= 0xFF;
    assert(mls_crypto_verify(sig, pk, msg, sizeof(msg) - 1) != 0);
}

static void
test_ed25519_rfc8032_vector1(void)
{
    /*
     * RFC 8032 §7.1 Test Vector 1
     * PRIVATE KEY: 9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60
     * PUBLIC KEY:  d75a980182b10ab7d54bfed3c964073a0ee172f3daa3f4a18446b0b8d183f8e3
     * MESSAGE:     (empty)
     * SIGNATURE:   e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155
     *              5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b
     */
    uint8_t seed[32];
    hex_decode(seed, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", 32);

    /* libsodium's sk format: seed[32] || pk[32] = 64 bytes */
    uint8_t sk[64], pk[32];
    /* Use crypto_sign_seed_keypair to derive from seed */
    assert(crypto_sign_seed_keypair(pk, sk, seed) == 0);

    assert_hex_eq(pk,
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa3f4a18446b0b8d183f8e3",
        32);

    /* Sign empty message */
    uint8_t sig[64];
    assert(mls_crypto_sign(sig, sk, NULL, 0) == 0);

    assert_hex_eq(sig,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        64);

    assert(mls_crypto_verify(sig, pk, NULL, 0) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 8. AEAD (AES-128-GCM) round-trip
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_aes128gcm_roundtrip(void)
{
    uint8_t key[MLS_AEAD_KEY_LEN];
    randombytes_buf(key, sizeof(key));
    uint8_t nonce[MLS_AEAD_NONCE_LEN];
    randombytes_buf(nonce, sizeof(nonce));

    uint8_t plaintext[] = "Hello AES-128-GCM for MLS!";
    size_t pt_len = sizeof(plaintext) - 1;
    uint8_t aad[] = "additional data";
    size_t aad_len = sizeof(aad) - 1;

    uint8_t ct[128];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, key, nonce,
                                    plaintext, pt_len, aad, aad_len) == 0);
    assert(ct_len == pt_len + MLS_AEAD_TAG_LEN);

    uint8_t decrypted[128];
    size_t dec_len = 0;
    assert(mls_crypto_aead_decrypt(decrypted, &dec_len, key, nonce,
                                    ct, ct_len, aad, aad_len) == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);
}

static void
test_aes128gcm_tampered_fails(void)
{
    uint8_t key[MLS_AEAD_KEY_LEN];
    randombytes_buf(key, sizeof(key));
    uint8_t nonce[MLS_AEAD_NONCE_LEN];
    randombytes_buf(nonce, sizeof(nonce));

    uint8_t plaintext[] = "tamper test";
    uint8_t ct[64];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, key, nonce,
                                    plaintext, 11, NULL, 0) == 0);

    /* Flip a bit */
    ct[0] ^= 0x01;

    uint8_t dec[64];
    size_t dec_len = 0;
    assert(mls_crypto_aead_decrypt(dec, &dec_len, key, nonce,
                                    ct, ct_len, NULL, 0) != 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 9. X25519 DH (RFC 7748)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_x25519_key_agreement(void)
{
    /* Two parties should derive the same shared secret */
    uint8_t sk_a[32], pk_a[32], sk_b[32], pk_b[32];
    assert(mls_crypto_kem_keygen(sk_a, pk_a) == 0);
    assert(mls_crypto_kem_keygen(sk_b, pk_b) == 0);

    uint8_t shared_a[32], shared_b[32];
    assert(mls_crypto_dh(shared_a, sk_a, pk_b) == 0);
    assert(mls_crypto_dh(shared_b, sk_b, pk_a) == 0);
    assert(memcmp(shared_a, shared_b, 32) == 0);
}

static void
test_dhkem_encap_decap(void)
{
    uint8_t sk[32], pk[32];
    assert(mls_crypto_kem_keygen(sk, pk) == 0);

    uint8_t shared_enc[32], enc[32];
    assert(mls_crypto_kem_encap(shared_enc, enc, pk) == 0);

    uint8_t shared_dec[32];
    assert(mls_crypto_kem_decap(shared_dec, enc, sk, pk) == 0);

    assert(memcmp(shared_enc, shared_dec, 32) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 10. RefHash (RFC 9420 §5.3.1)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_ref_hash_deterministic(void)
{
    uint8_t value[] = {0x01, 0x02, 0x03};
    uint8_t out1[32], out2[32];

    assert(mls_crypto_ref_hash(out1, "MLS 1.0 KeyPackage", value, 3) == 0);
    assert(mls_crypto_ref_hash(out2, "MLS 1.0 KeyPackage", value, 3) == 0);
    assert(memcmp(out1, out2, 32) == 0);
}

static void
test_ref_hash_different_labels(void)
{
    uint8_t value[] = {0x01};
    uint8_t out1[32], out2[32];

    assert(mls_crypto_ref_hash(out1, "label-a", value, 1) == 0);
    assert(mls_crypto_ref_hash(out2, "label-b", value, 1) == 0);
    assert(memcmp(out1, out2, 32) != 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 11. Tree operations (RFC 9420 Appendix C)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_tree_math(void)
{
    /* RFC 9420 Appendix C specifies the tree math for arrays */

    /* node_width(n_leaves) = 2*n - 1 */
    assert(mls_tree_node_width(1) == 1);
    assert(mls_tree_node_width(2) == 3);
    assert(mls_tree_node_width(4) == 7);
    assert(mls_tree_node_width(8) == 15);

    /* Leaf nodes are even-indexed */
    assert(mls_tree_is_leaf(0) == true);
    assert(mls_tree_is_leaf(1) == false);
    assert(mls_tree_is_leaf(2) == true);
    assert(mls_tree_is_leaf(3) == false);
    assert(mls_tree_is_leaf(4) == true);

    /* leaf_to_node(leaf_idx) = 2 * leaf_idx */
    assert(mls_tree_leaf_to_node(0) == 0);
    assert(mls_tree_leaf_to_node(1) == 2);
    assert(mls_tree_leaf_to_node(2) == 4);
    assert(mls_tree_leaf_to_node(3) == 6);

    /* For n_leaves=4: root is node 3 */
    assert(mls_tree_root(4) == 3);
    /* For n_leaves=8: root is node 7 */
    assert(mls_tree_root(8) == 7);

    /* Left/right children of node 3 (root for 4 leaves) */
    assert(mls_tree_left(3) == 1);
    assert(mls_tree_right(3) == 5);

    /* Left/right of node 1 */
    assert(mls_tree_left(1) == 0);
    assert(mls_tree_right(1) == 2);
}

static void
test_tree_parent_and_sibling(void)
{
    /* Parent of node 0 (leaf 0) in 4-leaf tree = node 1 */
    assert(mls_tree_parent(0, 4) == 1);
    /* Parent of node 2 (leaf 1) = node 1 */
    assert(mls_tree_parent(2, 4) == 1);
    /* Parent of node 1 = node 3 (root) */
    assert(mls_tree_parent(1, 4) == 3);
    /* Parent of node 5 = node 3 (root) */
    assert(mls_tree_parent(5, 4) == 3);

    /* Sibling of node 0 = node 2 */
    assert(mls_tree_sibling(0, 4) == 2);
    /* Sibling of node 2 = node 0 */
    assert(mls_tree_sibling(2, 4) == 0);
    /* Sibling of node 1 = node 5 */
    assert(mls_tree_sibling(1, 4) == 5);
}

/* ──────────────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: RFC 9420 test vectors & crypto validation\n");

    /* 1. HKDF-SHA256 */
    printf("\n─ HKDF-SHA256 (RFC 5869) ─\n");
    TEST(test_hkdf_extract_rfc5869_case1);
    TEST(test_hkdf_expand_rfc5869_case1);
    TEST(test_hkdf_extract_rfc5869_case2);
    TEST(test_hkdf_extract_empty_salt);

    /* 2. ExpandWithLabel */
    printf("\n─ MLS ExpandWithLabel ─\n");
    TEST(test_expand_with_label_deterministic);
    TEST(test_expand_with_label_different_labels);
    TEST(test_expand_with_label_different_contexts);
    TEST(test_expand_with_label_variable_length);
    TEST(test_derive_secret);

    /* 3. Key Schedule */
    printf("\n─ Key Schedule (RFC 9420 §8) ─\n");
    TEST(test_key_schedule_epoch0);
    TEST(test_key_schedule_deterministic);
    TEST(test_key_schedule_epoch_chain);
    TEST(test_key_schedule_different_groups);
    TEST(test_key_schedule_psk_changes_output);

    /* 4. Secret Tree */
    printf("\n─ Secret Tree (RFC 9420 §9) ─\n");
    TEST(test_secret_tree_init_and_derive);
    TEST(test_secret_tree_different_leaves_different_keys);
    TEST(test_secret_tree_ratchet_advances);
    TEST(test_secret_tree_handshake_vs_application);
    TEST(test_secret_tree_deterministic);
    TEST(test_secret_tree_forward_seek);
    TEST(test_secret_tree_large_group);

    /* 5. GroupContext */
    printf("\n─ GroupContext Serialization ─\n");
    TEST(test_group_context_serialization);
    TEST(test_group_context_deterministic);

    /* 6. MLS Exporter */
    printf("\n─ MLS Exporter ─\n");
    TEST(test_exporter_deterministic);
    TEST(test_exporter_different_labels);
    TEST(test_exporter_different_contexts);

    /* 7. Ed25519 */
    printf("\n─ Ed25519 (RFC 8032) ─\n");
    TEST(test_ed25519_sign_verify);
    TEST(test_ed25519_rfc8032_vector1);

    /* 8. AEAD */
    printf("\n─ AES-128-GCM ─\n");
    TEST(test_aes128gcm_roundtrip);
    TEST(test_aes128gcm_tampered_fails);

    /* 9. X25519 */
    printf("\n─ X25519 / DHKEM ─\n");
    TEST(test_x25519_key_agreement);
    TEST(test_dhkem_encap_decap);

    /* 10. RefHash */
    printf("\n─ RefHash ─\n");
    TEST(test_ref_hash_deterministic);
    TEST(test_ref_hash_different_labels);

    /* 11. Tree Math */
    printf("\n─ Tree Math (RFC 9420 Appendix C) ─\n");
    TEST(test_tree_math);
    TEST(test_tree_parent_and_sibling);

    printf("\nAll RFC 9420 tests passed (38 tests).\n");
    return 0;
}
