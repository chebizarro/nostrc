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
    TEST(test_ed25519_sign_verify);
    TEST(test_ed25519_tampered_sig);
    TEST(test_random_not_zero);
    TEST(test_ref_hash_deterministic);
    printf("All MLS crypto tests passed.\n");
    return 0;
}
