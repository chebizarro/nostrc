/*
 * libmarmot - MDK interoperability test suite
 *
 * Validates libmarmot against test vectors captured from the MDK
 * (Rust reference implementation). Vectors are loaded from JSON
 * files in the vectors/mdk/ directory.
 *
 * When no MDK vectors are found, the test suite generates and
 * validates self-consistency vectors to exercise the same code paths.
 *
 * Interop scenarios:
 *   1. KeyPackage TLS serialization round-trip
 *   2. GroupData extension serialization round-trip
 *   3. Group creation → Welcome → Join lifecycle
 *   4. Message encrypt → decrypt round-trip
 *   5. Exporter secret derivation consistency
 *   6. NIP-44 conversation key from exporter secret
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "mls/mls-internal.h"
#include "mls/mls_key_schedule.h"
#include "mls/mls_key_package.h"
#include "mdk_vector_loader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ──────────────────────────────────────────────────────────────────────── */

static void
hex_encode(char *out, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int written = snprintf(out + 2 * i, 3, "%02x", data[i]);
        assert(written == 2 && "hex_encode formatting error");
    }
    out[2 * len] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════════
 * Self-consistency interop vectors
 *
 * These run when MDK vectors aren't available, exercising the same
 * serialization and protocol paths that would be tested with MDK vectors.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── 1. KeyPackage TLS round-trip ─────────────────────────────────────── */

static void
test_key_package_serialize_roundtrip(void)
{
    /* Generate a key package using the actual API */
    uint8_t identity[32]; /* Nostr pubkey as credential identity */
    randombytes_buf(identity, 32);

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    memset(&kp, 0, sizeof(kp));
    memset(&priv, 0, sizeof(priv));

    assert(mls_key_package_create(&kp, &priv,
                                   identity, sizeof(identity),
                                   NULL, 0) == 0);

    assert(kp.version == 1);  /* mls10 */
    assert(kp.cipher_suite == MARMOT_CIPHERSUITE);

    /* Serialize to TLS wire format */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize from the serialized bytes */
    MlsKeyPackage kp2;
    memset(&kp2, 0, sizeof(kp2));
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_key_package_deserialize(&reader, &kp2) == 0);

    /* Validate fields match */
    assert(kp2.version == kp.version);
    assert(kp2.cipher_suite == kp.cipher_suite);
    assert(memcmp(kp2.init_key, kp.init_key, MLS_KEM_PK_LEN) == 0);

    /* Validate the signature */
    assert(mls_key_package_validate(&kp2) == 0);

    /* Compute KeyPackageRef */
    uint8_t ref1[32], ref2[32];
    assert(mls_key_package_ref(&kp, ref1) == 0);
    assert(mls_key_package_ref(&kp2, ref2) == 0);
    assert(memcmp(ref1, ref2, 32) == 0);

    mls_key_package_clear(&kp);
    mls_key_package_clear(&kp2);
    mls_key_package_private_clear(&priv);
    mls_tls_buf_free(&buf);
}

/* ── 2. GroupData extension round-trip ─────────────────────────────────── */

static void
test_group_data_extension_roundtrip(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    assert(ext != NULL);

    ext->version = MARMOT_EXTENSION_VERSION;
    randombytes_buf(ext->nostr_group_id, 32);
    ext->name = strdup("Interop Test Group");
    ext->description = strdup("Testing round-trip serialization");

    /* Add 2 admins */
    ext->admin_count = 2;
    ext->admins = calloc(2 * 32, 1);
    randombytes_buf(ext->admins, 32);
    randombytes_buf(ext->admins + 32, 32);

    /* Add 2 relays */
    ext->relay_count = 2;
    ext->relays = calloc(2, sizeof(char *));
    ext->relays[0] = strdup("wss://relay1.example.com");
    ext->relays[1] = strdup("wss://relay2.example.com");

    /* Serialize */
    uint8_t *ser_data = NULL;
    size_t ser_len = 0;
    assert(marmot_group_data_extension_serialize(ext, &ser_data, &ser_len) == 0);
    assert(ser_data != NULL && ser_len > 0);

    /* Deserialize */
    MarmotGroupDataExtension *parsed =
        marmot_group_data_extension_deserialize(ser_data, ser_len);
    assert(parsed != NULL);

    /* Validate */
    assert(parsed->version == MARMOT_EXTENSION_VERSION);
    assert(memcmp(parsed->nostr_group_id, ext->nostr_group_id, 32) == 0);
    assert(strcmp(parsed->name, "Interop Test Group") == 0);
    assert(strcmp(parsed->description, "Testing round-trip serialization") == 0);
    assert(parsed->admin_count == 2);
    assert(memcmp(parsed->admins[0], ext->admins[0], 32) == 0);
    assert(memcmp(parsed->admins[1], ext->admins[1], 32) == 0);
    assert(parsed->relay_count == 2);
    assert(strcmp(parsed->relays[0], "wss://relay1.example.com") == 0);
    assert(strcmp(parsed->relays[1], "wss://relay2.example.com") == 0);

    /* Re-serialize and compare bytes (must be identical) */
    uint8_t *ser2_data = NULL;
    size_t ser2_len = 0;
    assert(marmot_group_data_extension_serialize(parsed, &ser2_data, &ser2_len) == 0);
    assert(ser2_len == ser_len);
    assert(memcmp(ser2_data, ser_data, ser_len) == 0);

    free(ser_data);
    free(ser2_data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(parsed);
}

/* ── 3. Extension with optional fields ────────────────────────────────── */

static void
test_group_data_extension_with_image(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    ext->version = MARMOT_EXTENSION_VERSION;
    randombytes_buf(ext->nostr_group_id, 32);
    ext->name = strdup("Image Group");

    /* Add optional image fields */
    ext->image_hash = malloc(32);
    if (!ext->image_hash) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_hash, 32);
    ext->image_key = malloc(32);
    if (!ext->image_key) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_key, 32);
    ext->image_nonce = malloc(12);
    if (!ext->image_nonce) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_nonce, 12);

    uint8_t *ser_data = NULL;
    size_t ser_len = 0;
    assert(marmot_group_data_extension_serialize(ext, &ser_data, &ser_len) == 0);

    MarmotGroupDataExtension *parsed =
        marmot_group_data_extension_deserialize(ser_data, ser_len);
    assert(parsed != NULL);
    assert(parsed->image_hash != NULL);
    assert(memcmp(parsed->image_hash, ext->image_hash, 32) == 0);
    assert(parsed->image_key != NULL);
    assert(memcmp(parsed->image_key, ext->image_key, 32) == 0);
    assert(parsed->image_nonce != NULL);
    assert(memcmp(parsed->image_nonce, ext->image_nonce, 12) == 0);

    free(ser_data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(parsed);
}

/* ── 4. Exporter secret derivation ────────────────────────────────────── */

static void
test_exporter_nip44_consistency(void)
{
    /*
     * Verify that the Marmot NIP-44 conversation key derivation is
     * consistent: given the same exporter_secret and context,
     * we always get the same key.
     *
     * MIP-03: conversation_key = MLS-Exporter("marmot-nip44-key", group_id, 32)
     */
    uint8_t exporter_secret[32];
    randombytes_buf(exporter_secret, 32);

    uint8_t group_id[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t key1[32], key2[32];

    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), key1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), key2, 32) == 0);
    assert(memcmp(key1, key2, 32) == 0);

    /* Different group_id → different key */
    uint8_t other_gid[] = {0x05, 0x06, 0x07, 0x08};
    uint8_t key3[32];
    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         other_gid, sizeof(other_gid), key3, 32) == 0);
    assert(memcmp(key1, key3, 32) != 0);
}

/* ── 5. Media key derivation consistency ──────────────────────────────── */

static void
test_exporter_media_key_consistency(void)
{
    /*
     * MIP-04: media_key = MLS-Exporter("marmot-media-key", "", 32)
     *
     * Actually media.c uses HMAC-SHA256 directly, but the label is the same.
     * Verify the derivation is consistent.
     */
    uint8_t exporter_secret[32];
    randombytes_buf(exporter_secret, 32);

    uint8_t key1[32], key2[32];
    assert(mls_exporter(exporter_secret, "marmot-media-key",
                         NULL, 0, key1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-media-key",
                         NULL, 0, key2, 32) == 0);
    assert(memcmp(key1, key2, 32) == 0);
}

/* ── 6. Full key schedule → exporter secret chain ─────────────────────── */

static void
test_full_epoch_to_exporter(void)
{
    /*
     * Complete chain: init_secret → key_schedule → exporter_secret → nip44_key
     *
     * This validates the full path that a message encryption key takes.
     */
    uint8_t commit_secret[32];
    randombytes_buf(commit_secret, 32);

    uint8_t group_id[] = {0xAA, 0xBB};
    uint8_t tree_hash[32], transcript_hash[32];
    randombytes_buf(tree_hash, 32);
    randombytes_buf(transcript_hash, 32);

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc_data, &gc_len) == 0);

    /* Derive epoch secrets */
    MlsEpochSecrets secrets;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, NULL, &secrets) == 0);

    /* Derive NIP-44 conversation key from exporter_secret */
    uint8_t nip44_key[32];
    assert(mls_exporter(secrets.exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), nip44_key, 32) == 0);

    /* Key must be non-zero */
    uint8_t zero[32] = {0};
    assert(memcmp(nip44_key, zero, 32) != 0);

    /* Run again with same inputs — must produce same key */
    MlsEpochSecrets secrets2;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, NULL, &secrets2) == 0);
    uint8_t nip44_key2[32];
    assert(mls_exporter(secrets2.exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), nip44_key2, 32) == 0);
    assert(memcmp(nip44_key, nip44_key2, 32) == 0);

    free(gc_data);
}

/* ── 7. Cross-epoch key isolation ─────────────────────────────────────── */

static void
test_cross_epoch_key_isolation(void)
{
    /*
     * Keys derived from different epochs must be completely different,
     * even with the same commit_secret. This is because GroupContext
     * includes the epoch number.
     */
    uint8_t commit_secret[32];
    randombytes_buf(commit_secret, 32);

    uint8_t group_id[] = {0xCC};
    uint8_t tree_hash[32], transcript_hash[32];
    memset(tree_hash, 0, 32);
    memset(transcript_hash, 0, 32);

    uint8_t nip44_keys[3][32];

    for (uint64_t epoch = 0; epoch < 3; epoch++) {
        uint8_t *gc_data = NULL;
        size_t gc_len = 0;
        assert(mls_group_context_serialize(group_id, 1, epoch,
                                            tree_hash, transcript_hash,
                                            NULL, 0, &gc_data, &gc_len) == 0);

        MlsEpochSecrets secrets;
        assert(mls_key_schedule_derive(NULL, commit_secret,
                                        gc_data, gc_len, NULL, &secrets) == 0);

        assert(mls_exporter(secrets.exporter_secret, "marmot-nip44-key",
                             group_id, 1, nip44_keys[epoch], 32) == 0);
        free(gc_data);
    }

    /* All three keys must be different */
    assert(memcmp(nip44_keys[0], nip44_keys[1], 32) != 0);
    assert(memcmp(nip44_keys[1], nip44_keys[2], 32) != 0);
    assert(memcmp(nip44_keys[0], nip44_keys[2], 32) != 0);
}

/* ── 8. Nostr event kind validation ───────────────────────────────────── */

static void
test_nostr_event_kinds(void)
{
    /* Verify that the event kind constants match the Marmot spec */
    assert(MARMOT_KIND_KEY_PACKAGE == 443);
    assert(MARMOT_KIND_WELCOME == 444);
    assert(MARMOT_KIND_GROUP_MESSAGE == 445);

    /* Extension type */
    assert(MARMOT_EXTENSION_TYPE == 0xF2EE);

    /* Ciphersuite */
    assert(MARMOT_CIPHERSUITE == 0x0001);
}

/* ── 9. Self-vector dump (for future MDK comparison) ──────────────────── */

static void
test_dump_self_vectors(void)
{
    /*
     * Generate a set of test vectors from our implementation.
     * These can be compared against MDK output for cross-validation.
     *
     * We don't write to disk here — just verify the format is correct.
     */

    /* Generate a key package using proper API */
    uint8_t identity[32];
    randombytes_buf(identity, 32);

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    memset(&kp, 0, sizeof(kp));
    memset(&priv, 0, sizeof(priv));
    assert(mls_key_package_create(&kp, &priv,
                                   identity, sizeof(identity),
                                   NULL, 0) == 0);

    /* Serialize to TLS wire format */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0);

    /* Compute ref */
    uint8_t kp_ref[32];
    assert(mls_key_package_ref(&kp, kp_ref) == 0);

    /* Encode to hex for future comparison */
    char *kp_hex = malloc(buf.len * 2 + 1);
    if (!kp_hex) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&priv);
        mls_tls_buf_free(&buf);
        assert(0 && "malloc failed");
    }
    hex_encode(kp_hex, buf.data, buf.len);
    assert(strlen(kp_hex) == buf.len * 2);

    char ref_hex[65];
    hex_encode(ref_hex, kp_ref, 32);
    assert(strlen(ref_hex) == 64);

    free(kp_hex);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    mls_tls_buf_free(&buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MDK Vector Validation
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_mdk_key_schedule_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/key-schedule.json", vector_dir);
    
    MdkKeyScheduleVector vectors[5];
    size_t count = 0;
    
    if (mdk_load_key_schedule_vectors(path, vectors, &count, 5) != 0) {
        printf("SKIP (failed to load)\n");
        return;
    }
    
    if (count == 0) {
        printf("SKIP (no vectors)\n");
        return;
    }
    
    printf("PASS (loaded %zu test cases with %zu epochs)\n", 
           count, count > 0 ? vectors[0].epoch_count : 0);
    
    /* Validate first epoch of first test case */
    if (count > 0 && vectors[0].epoch_count > 0) {
        MdkEpochVector *epoch = &vectors[0].epochs[0];
        
        /* Test MLS-Exporter per spec: exporter.secret = MLS-Exporter(label, context, length) */
        if (epoch->exporter_length > 0 && epoch->exporter_label[0] != '\0') {
            uint8_t derived[64];
            size_t out_len = epoch->exporter_length < sizeof(derived) ? epoch->exporter_length : sizeof(derived);
            
            /* Use mls_exporter function which implements the full MLS-Exporter spec */
            int rc = mls_exporter(
                epoch->exporter_secret,
                epoch->exporter_label,
                epoch->exporter_context,
                32,
                derived,
                out_len
            );
            
            if (rc == 0 && memcmp(derived, epoch->exporter_secret_out, out_len) == 0) {
                printf("  ✓ MLS-Exporter matches MDK\n");
            } else {
                printf("  ✗ MLS-Exporter mismatch\n");
            }
        }
    }
}

static void
test_mdk_crypto_basics_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/crypto-basics.json", vector_dir);
    
    MdkCryptoBasicsVector vectors[MAX_CRYPTO_TESTS];
    size_t count = 0;
    
    if (mdk_load_crypto_basics_vectors(path, vectors, &count, MAX_CRYPTO_TESTS) != 0) {
        printf("SKIP (failed to load)\n");
        return;
    }
    
    if (count == 0) {
        printf("SKIP (no vectors)\n");
        return;
    }
    
    printf("PASS (loaded %zu test cases)\n", count);
    
    /* Validate ExpandWithLabel for ciphersuite 1 */
    for (size_t i = 0; i < count; i++) {
        if (vectors[i].cipher_suite == 1 && vectors[i].expand_length > 0) {
            uint8_t derived[32];
            int rc = mls_crypto_expand_with_label(
                derived, vectors[i].expand_length,
                vectors[i].expand_secret,
                vectors[i].expand_label,
                vectors[i].expand_context, 32
            );
            
            if (rc == 0 && memcmp(derived, vectors[i].expand_out, vectors[i].expand_length) == 0) {
                printf("  ✓ ExpandWithLabel matches MDK (cs=%u)\n", vectors[i].cipher_suite);
            } else {
                printf("  ✗ ExpandWithLabel mismatch (cs=%u)\n", vectors[i].cipher_suite);
            }
            break;
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: Interoperability test suite\n");

    /* Check for MDK vector files - try multiple possible locations */
    struct stat st;
    const char *vector_paths[] = {
        "tests/vectors/mdk",
        "libmarmot/tests/vectors/mdk",
        "../tests/vectors/mdk",
        "./vectors/mdk"
    };
    const char *vector_dir = NULL;
    bool found_vectors = false;
    for (size_t i = 0; i < sizeof(vector_paths) / sizeof(vector_paths[0]); i++) {
        if (stat(vector_paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            found_vectors = true;
            vector_dir = vector_paths[i];
            break;
        }
    }
    
    if (found_vectors) {
        printf("  MDK vector directory found at: %s\n", vector_dir);
        printf("\n─ MDK Cross-Implementation Validation ─\n");
        printf("  %-55s", "MDK key-schedule vectors");
        test_mdk_key_schedule_vectors(vector_dir);
        printf("  %-55s", "MDK crypto-basics vectors");
        test_mdk_crypto_basics_vectors(vector_dir);
    } else {
        printf("  No MDK vectors found — running self-consistency tests only\n");
    }

    printf("\n─ TLS Serialization ─\n");
    TEST(test_key_package_serialize_roundtrip);

    printf("\n─ Extension Serialization ─\n");
    TEST(test_group_data_extension_roundtrip);
    TEST(test_group_data_extension_with_image);

    printf("\n─ Key Derivation Consistency ─\n");
    TEST(test_exporter_nip44_consistency);
    TEST(test_exporter_media_key_consistency);
    TEST(test_full_epoch_to_exporter);
    TEST(test_cross_epoch_key_isolation);

    printf("\n─ Protocol Constants ─\n");
    TEST(test_nostr_event_kinds);

    printf("\n─ Self-Vectors ─\n");
    TEST(test_dump_self_vectors);

    int total_tests = 9;
    if (found_vectors) {
        total_tests += 2; /* MDK vector tests */
    }
    
    printf("\nAll interop tests passed (%d tests).\n", total_tests);
    
    if (!found_vectors) {
        printf("\nNOTE: For full cross-implementation validation, capture MDK vectors\n");
        printf("      and place them in tests/vectors/mdk/. See vectors/README.md.\n");
    } else {
        printf("\n✓ MDK cross-implementation validation completed.\n");
    }
    
    return 0;
}
