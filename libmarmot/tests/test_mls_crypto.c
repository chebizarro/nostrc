/*
 * libmarmot - MLS crypto primitives tests
 *
 * Tests SHA-256, HKDF, AES-128-GCM, X25519, Ed25519 for ciphersuite 0x0001.
 */

#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── SHA-256 known vector ──────────────────────────────────────────────── */

static void test_sha256_empty(void)
{
    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    uint8_t expected[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    uint8_t out[MLS_HASH_LEN];
    assert(mls_crypto_hash(out, (const uint8_t *)"", 0) == 0);
    assert(memcmp(out, expected, 32) == 0);
}

static void test_sha256_abc(void)
{
    /* SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t out[MLS_HASH_LEN];
    assert(mls_crypto_hash(out, (const uint8_t *)"abc", 3) == 0);
    assert(memcmp(out, expected, 32) == 0);
}

/* ── HKDF-Extract + Expand round-trip ──────────────────────────────────── */

static void test_hkdf_deterministic(void)
{
    uint8_t ikm[32], salt[32];
    memset(ikm, 0x0b, sizeof(ikm));
    memset(salt, 0x00, sizeof(salt));

    uint8_t prk1[MLS_KDF_EXTRACT_LEN], prk2[MLS_KDF_EXTRACT_LEN];
    assert(mls_crypto_hkdf_extract(prk1, salt, sizeof(salt), ikm, sizeof(ikm)) == 0);
    assert(mls_crypto_hkdf_extract(prk2, salt, sizeof(salt), ikm, sizeof(ikm)) == 0);
    assert(memcmp(prk1, prk2, MLS_KDF_EXTRACT_LEN) == 0);
}

static void test_hkdf_expand(void)
{
    uint8_t prk[MLS_KDF_EXTRACT_LEN];
    memset(prk, 0x07, sizeof(prk));

    uint8_t out1[64], out2[64];
    uint8_t info[] = "test info";
    assert(mls_crypto_hkdf_expand(out1, sizeof(out1), prk, info, sizeof(info) - 1) == 0);
    assert(mls_crypto_hkdf_expand(out2, sizeof(out2), prk, info, sizeof(info) - 1) == 0);
    assert(memcmp(out1, out2, sizeof(out1)) == 0);

    /* Different info → different output */
    uint8_t info2[] = "other info";
    uint8_t out3[64];
    assert(mls_crypto_hkdf_expand(out3, sizeof(out3), prk, info2, sizeof(info2) - 1) == 0);
    assert(memcmp(out1, out3, sizeof(out1)) != 0);
}

/* ── ExpandWithLabel ──────────────────────────────────────────────────── */

static void test_expand_with_label(void)
{
    uint8_t secret[MLS_HASH_LEN];
    memset(secret, 0xAA, sizeof(secret));

    uint8_t out1[32], out2[32], out3[32];
    assert(mls_crypto_expand_with_label(out1, 32, secret, "sender", NULL, 0) == 0);
    assert(mls_crypto_expand_with_label(out2, 32, secret, "sender", NULL, 0) == 0);
    assert(memcmp(out1, out2, 32) == 0);

    /* Different label → different output */
    assert(mls_crypto_expand_with_label(out3, 32, secret, "receiver", NULL, 0) == 0);
    assert(memcmp(out1, out3, 32) != 0);
}

/* ── AES-128-GCM round-trip ──────────────────────────────────────────── */

static void test_aead_roundtrip(void)
{
    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
    mls_crypto_random(key, sizeof(key));
    mls_crypto_random(nonce, sizeof(nonce));

    const uint8_t pt[] = "hello, MLS group messaging!";
    const uint8_t aad[] = "associated data";
    size_t pt_len = sizeof(pt) - 1;

    uint8_t ct[sizeof(pt) + MLS_AEAD_TAG_LEN];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, key, nonce,
                                    pt, pt_len, aad, sizeof(aad) - 1) == 0);
    assert(ct_len == pt_len + MLS_AEAD_TAG_LEN);

    uint8_t decrypted[sizeof(pt)];
    size_t dec_len = 0;
    assert(mls_crypto_aead_decrypt(decrypted, &dec_len, key, nonce,
                                    ct, ct_len, aad, sizeof(aad) - 1) == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, pt, pt_len) == 0);
}

static void test_aead_tamper_detection(void)
{
    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
    mls_crypto_random(key, sizeof(key));
    mls_crypto_random(nonce, sizeof(nonce));

    const uint8_t pt[] = "secret message";
    uint8_t ct[sizeof(pt) + MLS_AEAD_TAG_LEN];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, key, nonce,
                                    pt, sizeof(pt) - 1, NULL, 0) == 0);

    /* Tamper with ciphertext */
    ct[0] ^= 0xFF;
    uint8_t decrypted[sizeof(pt)];
    size_t dec_len = 0;
    assert(mls_crypto_aead_decrypt(decrypted, &dec_len, key, nonce,
                                    ct, ct_len, NULL, 0) != 0);
}

static void test_aead_wrong_aad(void)
{
    uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
    mls_crypto_random(key, sizeof(key));
    mls_crypto_random(nonce, sizeof(nonce));

    const uint8_t pt[] = "message";
    const uint8_t aad1[] = "correct aad";
    const uint8_t aad2[] = "wrong aad";

    uint8_t ct[sizeof(pt) + MLS_AEAD_TAG_LEN];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, key, nonce,
                                    pt, sizeof(pt) - 1, aad1, sizeof(aad1) - 1) == 0);

    uint8_t decrypted[sizeof(pt)];
    size_t dec_len = 0;
    assert(mls_crypto_aead_decrypt(decrypted, &dec_len, key, nonce,
                                    ct, ct_len, aad2, sizeof(aad2) - 1) != 0);
}

/* ── X25519 DH ────────────────────────────────────────────────────────── */

static void test_x25519_dh_shared_secret(void)
{
    uint8_t sk_a[MLS_KEM_SK_LEN], pk_a[MLS_KEM_PK_LEN];
    uint8_t sk_b[MLS_KEM_SK_LEN], pk_b[MLS_KEM_PK_LEN];

    assert(mls_crypto_kem_keygen(sk_a, pk_a) == 0);
    assert(mls_crypto_kem_keygen(sk_b, pk_b) == 0);

    uint8_t shared_ab[MLS_KEM_SECRET_LEN], shared_ba[MLS_KEM_SECRET_LEN];
    assert(mls_crypto_dh(shared_ab, sk_a, pk_b) == 0);
    assert(mls_crypto_dh(shared_ba, sk_b, pk_a) == 0);
    assert(memcmp(shared_ab, shared_ba, MLS_KEM_SECRET_LEN) == 0);
}

/* ── DHKEM Encap/Decap ────────────────────────────────────────────────── */

static void test_kem_encap_decap(void)
{
    uint8_t sk[MLS_KEM_SK_LEN], pk[MLS_KEM_PK_LEN];
    assert(mls_crypto_kem_keygen(sk, pk) == 0);

    uint8_t shared_enc[MLS_KEM_SECRET_LEN];
    uint8_t enc[MLS_KEM_ENC_LEN];
    assert(mls_crypto_kem_encap(shared_enc, enc, pk) == 0);

    uint8_t shared_dec[MLS_KEM_SECRET_LEN];
    assert(mls_crypto_kem_decap(shared_dec, enc, sk, pk) == 0);
    assert(memcmp(shared_enc, shared_dec, MLS_KEM_SECRET_LEN) == 0);
}

/* ── RFC 9180 DHKEM/HPKE vectors ──────────────────────────────────────── */

static void test_rfc9180_dhkem_x25519_vectors(void)
{
    /* RFC 9180 Appendix A.1.1: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, AES-128-GCM */
    const uint8_t ikm_e[32] = {
        0x72,0x68,0x60,0x0d,0x40,0x3f,0xce,0x43,
        0x15,0x61,0xae,0xf5,0x83,0xee,0x16,0x13,
        0x52,0x7c,0xff,0x65,0x5c,0x13,0x43,0xf2,
        0x98,0x12,0xe6,0x67,0x06,0xdf,0x32,0x34
    };
    const uint8_t expected_pk_e[32] = {
        0x37,0xfd,0xa3,0x56,0x7b,0xdb,0xd6,0x28,
        0xe8,0x86,0x68,0xc3,0xc8,0xd7,0xe9,0x7d,
        0x1d,0x12,0x53,0xb6,0xd4,0xea,0x6d,0x44,
        0xc1,0x50,0xf7,0x41,0xf1,0xbf,0x44,0x31
    };
    const uint8_t expected_sk_e[32] = {
        0x52,0xc4,0xa7,0x58,0xa8,0x02,0xcd,0x8b,
        0x93,0x6e,0xce,0xea,0x31,0x44,0x32,0x79,
        0x8d,0x5b,0xaf,0x2d,0x7e,0x92,0x35,0xdc,
        0x08,0x4a,0xb1,0xb9,0xcf,0xa2,0xf7,0x36
    };
    const uint8_t ikm_r[32] = {
        0x6d,0xb9,0xdf,0x30,0xaa,0x07,0xdd,0x42,
        0xee,0x5e,0x81,0x81,0xaf,0xdb,0x97,0x7e,
        0x53,0x8f,0x5e,0x1f,0xec,0x8a,0x06,0x22,
        0x3f,0x33,0xf7,0x01,0x3e,0x52,0x50,0x37
    };
    const uint8_t expected_pk_r[32] = {
        0x39,0x48,0xcf,0xe0,0xad,0x1d,0xdb,0x69,
        0x5d,0x78,0x0e,0x59,0x07,0x71,0x95,0xda,
        0x6c,0x56,0x50,0x6b,0x02,0x73,0x29,0x79,
        0x4a,0xb0,0x2b,0xca,0x80,0x81,0x5c,0x4d
    };
    const uint8_t expected_sk_r[32] = {
        0x46,0x12,0xc5,0x50,0x26,0x3f,0xc8,0xad,
        0x58,0x37,0x5d,0xf3,0xf5,0x57,0xaa,0xc5,
        0x31,0xd2,0x68,0x50,0x90,0x3e,0x55,0xa9,
        0xf2,0x3f,0x21,0xd8,0x53,0x4e,0x8a,0xc8
    };
    const uint8_t expected_shared_secret[32] = {
        0xfe,0x0e,0x18,0xc9,0xf0,0x24,0xce,0x43,
        0x79,0x9a,0xe3,0x93,0xc7,0xe8,0xfe,0x8f,
        0xce,0x9d,0x21,0x88,0x75,0xe8,0x22,0x7b,
        0x01,0x87,0xc0,0x4e,0x7d,0x2e,0xa1,0xfc
    };
    const uint8_t info[] = {
        0x4f,0x64,0x65,0x20,0x6f,0x6e,0x20,0x61,
        0x20,0x47,0x72,0x65,0x63,0x69,0x61,0x6e,
        0x20,0x55,0x72,0x6e
    };
    const uint8_t expected_key[MLS_AEAD_KEY_LEN] = {
        0x45,0x31,0x68,0x5d,0x41,0xd6,0x5f,0x03,
        0xdc,0x48,0xf6,0xb8,0x30,0x2c,0x05,0xb0
    };
    const uint8_t expected_base_nonce[MLS_AEAD_NONCE_LEN] = {
        0x56,0xd8,0x90,0xe5,0xac,0xca,0xaf,0x01,
        0x1c,0xff,0x4b,0x7d
    };
    const uint8_t expected_exporter_secret[MLS_HASH_LEN] = {
        0x45,0xff,0x1c,0x2e,0x22,0x0d,0xb5,0x87,
        0x17,0x19,0x52,0xc0,0x59,0x2d,0x5f,0x5e,
        0xbe,0x10,0x3f,0x15,0x61,0xa2,0x61,0x4e,
        0x38,0xf2,0xff,0xd4,0x7e,0x99,0xe3,0xf8
    };
    const uint8_t pt[] = {
        0x42,0x65,0x61,0x75,0x74,0x79,0x20,0x69,
        0x73,0x20,0x74,0x72,0x75,0x74,0x68,0x2c,
        0x20,0x74,0x72,0x75,0x74,0x68,0x20,0x62,
        0x65,0x61,0x75,0x74,0x79
    };
    const uint8_t aad[] = {0x43,0x6f,0x75,0x6e,0x74,0x2d,0x30};
    const uint8_t expected_ct[] = {
        0xf9,0x38,0x55,0x8b,0x5d,0x72,0xf1,0xa2,
        0x38,0x10,0xb4,0xbe,0x2a,0xb4,0xf8,0x43,
        0x31,0xac,0xc0,0x2f,0xc9,0x7b,0xab,0xc5,
        0x3a,0x52,0xae,0x82,0x18,0xa3,0x55,0xa9,
        0x6d,0x87,0x70,0xac,0x83,0xd0,0x7b,0xea,
        0x87,0xe1,0x3c,0x51,0x2a
    };

    uint8_t sk_e[MLS_KEM_SK_LEN], pk_e[MLS_KEM_PK_LEN];
    assert(mls_crypto_kem_derive_keypair(ikm_e, sizeof(ikm_e), sk_e, pk_e) == 0);
    assert(memcmp(sk_e, expected_sk_e, sizeof(expected_sk_e)) == 0);
    assert(memcmp(pk_e, expected_pk_e, sizeof(expected_pk_e)) == 0);

    uint8_t sk_r[MLS_KEM_SK_LEN], pk_r[MLS_KEM_PK_LEN];
    assert(mls_crypto_kem_derive_keypair(ikm_r, sizeof(ikm_r), sk_r, pk_r) == 0);
    assert(memcmp(sk_r, expected_sk_r, sizeof(expected_sk_r)) == 0);
    assert(memcmp(pk_r, expected_pk_r, sizeof(expected_pk_r)) == 0);

    uint8_t shared_enc[MLS_KEM_SECRET_LEN], enc[MLS_KEM_ENC_LEN];
    assert(mls_crypto_kem_encap_derand(shared_enc, enc, pk_r, ikm_e, sizeof(ikm_e)) == 0);
    assert(memcmp(enc, expected_pk_e, sizeof(expected_pk_e)) == 0);
    assert(memcmp(shared_enc, expected_shared_secret, sizeof(expected_shared_secret)) == 0);

    uint8_t shared_dec[MLS_KEM_SECRET_LEN];
    assert(mls_crypto_kem_decap(shared_dec, enc, sk_r, pk_r) == 0);
    assert(memcmp(shared_dec, expected_shared_secret, sizeof(expected_shared_secret)) == 0);

    MlsHpkeContext ctx;
    assert(mls_crypto_hpke_key_schedule_base(&ctx, expected_shared_secret,
                                             info, sizeof(info)) == 0);
    assert(memcmp(ctx.key, expected_key, sizeof(expected_key)) == 0);
    assert(memcmp(ctx.base_nonce, expected_base_nonce, sizeof(expected_base_nonce)) == 0);
    assert(memcmp(ctx.exporter_secret, expected_exporter_secret,
                  sizeof(expected_exporter_secret)) == 0);

    uint8_t ct[sizeof(expected_ct)];
    size_t ct_len = 0;
    assert(mls_crypto_aead_encrypt(ct, &ct_len, ctx.key, ctx.base_nonce,
                                    pt, sizeof(pt), aad, sizeof(aad)) == 0);
    assert(ct_len == sizeof(expected_ct));
    assert(memcmp(ct, expected_ct, sizeof(expected_ct)) == 0);

    sodium_memzero(sk_e, sizeof(sk_e));
    sodium_memzero(sk_r, sizeof(sk_r));
    sodium_memzero(shared_enc, sizeof(shared_enc));
    sodium_memzero(shared_dec, sizeof(shared_dec));
    sodium_memzero(&ctx, sizeof(ctx));
}

static void test_hpke_seal_open_base_roundtrip(void)
{
    uint8_t sk[MLS_KEM_SK_LEN], pk[MLS_KEM_PK_LEN];
    assert(mls_crypto_kem_keygen(sk, pk) == 0);

    const uint8_t info[] = "MLS HPKE base info";
    const uint8_t aad[] = "MLS aad";
    const uint8_t pt[] = "path secret or joiner secret";

    uint8_t enc[MLS_KEM_ENC_LEN];
    uint8_t ct[sizeof(pt) - 1 + MLS_AEAD_TAG_LEN];
    size_t ct_len = 0;
    assert(mls_crypto_hpke_seal_base(enc, ct, &ct_len, pk,
                                      info, sizeof(info) - 1,
                                      aad, sizeof(aad) - 1,
                                      pt, sizeof(pt) - 1) == 0);
    assert(ct_len == sizeof(pt) - 1 + MLS_AEAD_TAG_LEN);

    uint8_t decrypted[sizeof(pt)];
    size_t dec_len = 0;
    assert(mls_crypto_hpke_open_base(decrypted, &dec_len, enc, sk, pk,
                                      info, sizeof(info) - 1,
                                      aad, sizeof(aad) - 1,
                                      ct, ct_len) == 0);
    assert(dec_len == sizeof(pt) - 1);
    assert(memcmp(decrypted, pt, dec_len) == 0);

    ct[0] ^= 0x01;
    assert(mls_crypto_hpke_open_base(decrypted, &dec_len, enc, sk, pk,
                                      info, sizeof(info) - 1,
                                      aad, sizeof(aad) - 1,
                                      ct, ct_len) != 0);
}

/* ── Ed25519 Sign/Verify ──────────────────────────────────────────────── */

static void test_ed25519_sign_verify(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    const uint8_t msg[] = "Sign this message for the MLS group";
    uint8_t sig[MLS_SIG_LEN];
    assert(mls_crypto_sign(sig, sk, msg, sizeof(msg) - 1) == 0);
    assert(mls_crypto_verify(sig, pk, msg, sizeof(msg) - 1) == 0);

    /* Wrong message → verification fails */
    const uint8_t wrong[] = "Different message";
    assert(mls_crypto_verify(sig, pk, wrong, sizeof(wrong) - 1) != 0);
}

static void test_ed25519_tampered_sig(void)
{
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    assert(mls_crypto_sign_keygen(sk, pk) == 0);

    const uint8_t msg[] = "Test";
    uint8_t sig[MLS_SIG_LEN];
    assert(mls_crypto_sign(sig, sk, msg, sizeof(msg) - 1) == 0);

    sig[0] ^= 0x01; /* flip one bit */
    assert(mls_crypto_verify(sig, pk, msg, sizeof(msg) - 1) != 0);
}

/* ── Random ──────────────────────────────────────────────────────────── */

static void test_random_not_zero(void)
{
    uint8_t buf[64] = {0};
    mls_crypto_random(buf, sizeof(buf));

    /* Statistical check: extremely unlikely to be all zeros */
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) { all_zero = 0; break; }
    }
    assert(!all_zero);
}

/* ── RefHash ──────────────────────────────────────────────────────────── */

static void test_ref_hash_deterministic(void)
{
    uint8_t value[] = "KeyPackage data";
    uint8_t out1[MLS_HASH_LEN], out2[MLS_HASH_LEN];

    assert(mls_crypto_ref_hash(out1, "MLS 1.0 KeyPackage", value, sizeof(value) - 1) == 0);
    assert(mls_crypto_ref_hash(out2, "MLS 1.0 KeyPackage", value, sizeof(value) - 1) == 0);
    assert(memcmp(out1, out2, MLS_HASH_LEN) == 0);

    /* Different label → different hash */
    uint8_t out3[MLS_HASH_LEN];
    assert(mls_crypto_ref_hash(out3, "MLS 1.0 Proposal", value, sizeof(value) - 1) == 0);
    assert(memcmp(out1, out3, MLS_HASH_LEN) != 0);
}

int main(void)
{
    /* libsodium must be initialized before use */
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: MLS crypto tests\n");
    TEST(test_sha256_empty);
    TEST(test_sha256_abc);
    TEST(test_hkdf_deterministic);
    TEST(test_hkdf_expand);
    TEST(test_expand_with_label);
    TEST(test_aead_roundtrip);
    TEST(test_aead_tamper_detection);
    TEST(test_aead_wrong_aad);
    TEST(test_x25519_dh_shared_secret);
    TEST(test_kem_encap_decap);
    TEST(test_rfc9180_dhkem_x25519_vectors);
    TEST(test_hpke_seal_open_base_roundtrip);
    TEST(test_ed25519_sign_verify);
    TEST(test_ed25519_tampered_sig);
    TEST(test_random_not_zero);
    TEST(test_ref_hash_deterministic);
    printf("All MLS crypto tests passed.\n");
    return 0;
}
