/*
 * libmarmot - MLS KeyPackage tests
 *
 * Tests for key package creation, serialization, validation, and ref.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_key_package.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

static const uint8_t TEST_IDENTITY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

/* ── Tests ─────────────────────────────────────────────────────────────── */

TEST(test_create_basic)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;

    int rc = mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0);
    assert(rc == 0);
    assert(kp.version == 1);
    assert(kp.cipher_suite == MARMOT_CIPHERSUITE);
    assert(kp.leaf_node.credential_type == MLS_CREDENTIAL_BASIC);
    assert(kp.leaf_node.credential_identity_len == 32);
    assert(memcmp(kp.leaf_node.credential_identity, TEST_IDENTITY, 32) == 0);
    assert(kp.signature_len == MLS_SIG_LEN);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_create_with_extensions)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    uint8_t ext[] = { 0xF2, 0xEE, 0x00, 0x04, 0xDE, 0xAD, 0xBE, 0xEF };

    int rc = mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, ext, sizeof(ext));
    assert(rc == 0);
    assert(kp.extensions_len == sizeof(ext));
    assert(memcmp(kp.extensions_data, ext, sizeof(ext)) == 0);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_validate_valid)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    int rc = mls_key_package_validate(&kp);
    assert(rc == 0);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_validate_bad_version)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    kp.version = 99;
    int rc = mls_key_package_validate(&kp);
    assert(rc != 0);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_validate_bad_ciphersuite)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    kp.cipher_suite = 0x9999;
    int rc = mls_key_package_validate(&kp);
    assert(rc != 0);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_validate_bad_signature)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    /* Corrupt signature */
    kp.signature[0] ^= 0xFF;
    int rc = mls_key_package_validate(&kp);
    assert(rc == MARMOT_ERR_SIGNATURE);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_serialize_roundtrip)
{
    MlsKeyPackage kp, kp2;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_key_package_deserialize(&reader, &kp2) == 0);

    /* Verify fields match */
    assert(kp2.version == kp.version);
    assert(kp2.cipher_suite == kp.cipher_suite);
    assert(memcmp(kp2.init_key, kp.init_key, MLS_KEM_PK_LEN) == 0);
    assert(kp2.signature_len == kp.signature_len);
    assert(memcmp(kp2.signature, kp.signature, kp.signature_len) == 0);
    assert(kp2.leaf_node.credential_identity_len == kp.leaf_node.credential_identity_len);

    /* Deserialized should also validate */
    assert(mls_key_package_validate(&kp2) == 0);

    mls_tls_buf_free(&buf);
    mls_key_package_clear(&kp);
    mls_key_package_clear(&kp2);
    mls_key_package_private_clear(&priv);
}

TEST(test_key_package_ref)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    uint8_t ref1[MLS_HASH_LEN], ref2[MLS_HASH_LEN];
    assert(mls_key_package_ref(&kp, ref1) == 0);
    assert(mls_key_package_ref(&kp, ref2) == 0);

    /* Same key package should produce same ref */
    assert(memcmp(ref1, ref2, MLS_HASH_LEN) == 0);

    /* Ref should not be all zeros */
    uint8_t zeros[MLS_HASH_LEN] = {0};
    assert(memcmp(ref1, zeros, MLS_HASH_LEN) != 0);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

TEST(test_different_kp_different_ref)
{
    MlsKeyPackage kp1, kp2;
    MlsKeyPackagePrivate priv1, priv2;

    assert(mls_key_package_create(&kp1, &priv1, TEST_IDENTITY, 32, NULL, 0) == 0);
    uint8_t other_id[32] = {0xFF};
    assert(mls_key_package_create(&kp2, &priv2, other_id, 32, NULL, 0) == 0);

    uint8_t ref1[MLS_HASH_LEN], ref2[MLS_HASH_LEN];
    assert(mls_key_package_ref(&kp1, ref1) == 0);
    assert(mls_key_package_ref(&kp2, ref2) == 0);
    assert(memcmp(ref1, ref2, MLS_HASH_LEN) != 0);

    mls_key_package_clear(&kp1);
    mls_key_package_clear(&kp2);
    mls_key_package_private_clear(&priv1);
    mls_key_package_private_clear(&priv2);
}

TEST(test_unique_keys_per_creation)
{
    MlsKeyPackage kp1, kp2;
    MlsKeyPackagePrivate priv1, priv2;

    assert(mls_key_package_create(&kp1, &priv1, TEST_IDENTITY, 32, NULL, 0) == 0);
    assert(mls_key_package_create(&kp2, &priv2, TEST_IDENTITY, 32, NULL, 0) == 0);

    /* Different init keys */
    assert(memcmp(kp1.init_key, kp2.init_key, MLS_KEM_PK_LEN) != 0);

    /* Different encryption keys */
    assert(memcmp(kp1.leaf_node.encryption_key, kp2.leaf_node.encryption_key, MLS_KEM_PK_LEN) != 0);

    /* Different signing keys */
    assert(memcmp(kp1.leaf_node.signature_key, kp2.leaf_node.signature_key, MLS_SIG_PK_LEN) != 0);

    mls_key_package_clear(&kp1);
    mls_key_package_clear(&kp2);
    mls_key_package_private_clear(&priv1);
    mls_key_package_private_clear(&priv2);
}

TEST(test_clear_zeroes)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, TEST_IDENTITY, 32, NULL, 0) == 0);

    mls_key_package_private_clear(&priv);
    /* Private keys should be zeroed */
    uint8_t zeros_sk[MLS_KEM_SK_LEN] = {0};
    assert(memcmp(priv.init_key_private, zeros_sk, MLS_KEM_SK_LEN) == 0);
    assert(memcmp(priv.encryption_key_private, zeros_sk, MLS_KEM_SK_LEN) == 0);

    mls_key_package_clear(&kp);
}

TEST(test_null_args)
{
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;

    assert(mls_key_package_create(NULL, &priv, TEST_IDENTITY, 32, NULL, 0) != 0);
    assert(mls_key_package_create(&kp, NULL, TEST_IDENTITY, 32, NULL, 0) != 0);
    assert(mls_key_package_create(&kp, &priv, NULL, 32, NULL, 0) != 0);
    assert(mls_key_package_validate(NULL) != 0);
    assert(mls_key_package_ref(NULL, (uint8_t[32]){0}) != 0);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("test_mls_key_package:\n");
    RUN(test_create_basic);
    RUN(test_create_with_extensions);
    RUN(test_validate_valid);
    RUN(test_validate_bad_version);
    RUN(test_validate_bad_ciphersuite);
    RUN(test_validate_bad_signature);
    RUN(test_serialize_roundtrip);
    RUN(test_key_package_ref);
    RUN(test_different_kp_different_ref);
    RUN(test_unique_keys_per_creation);
    RUN(test_clear_zeroes);
    RUN(test_null_args);
    printf("All key package tests passed.\n");
    return 0;
}
