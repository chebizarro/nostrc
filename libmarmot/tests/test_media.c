/*
 * libmarmot - MIP-04 encrypted media tests
 *
 * Tests media encryption/decryption, key derivation, and integrity
 * verification via ChaCha20-Poly1305.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

static Marmot *
create_test_marmot(void)
{
    MarmotStorage *storage = marmot_storage_memory_new();
    assert(storage != NULL);

    MarmotConfig config = marmot_config_default();
    Marmot *m = marmot_new_with_config(storage, &config);
    assert(m != NULL);
    return m;
}

static void
setup_group_with_secret(Marmot *m,
                         const MarmotGroupId *gid,
                         uint64_t epoch,
                         const uint8_t secret[32])
{
    /* Save a group at the given epoch */
    MarmotGroup *g = marmot_group_new();
    g->mls_group_id = marmot_group_id_new(gid->data, gid->len);
    memset(g->nostr_group_id, 0xAA, 32);
    g->name = strdup("Media Test Group");
    g->description = strdup("For media tests");
    g->state = MARMOT_GROUP_STATE_ACTIVE;
    g->epoch = epoch;
    assert(m->storage->save_group(m->storage->ctx, g) == MARMOT_OK);
    marmot_group_free(g);

    /* Save the exporter secret */
    assert(m->storage->save_exporter_secret(m->storage->ctx, gid, epoch, secret)
           == MARMOT_OK);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void
test_encrypt_decrypt_roundtrip(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"media_rt", 8);
    uint8_t secret[32];
    memset(secret, 0x42, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    /* Encrypt a test file */
    const char *plaintext = "Hello, encrypted world! This is a test file for MIP-04.";
    size_t pt_len = strlen(plaintext);

    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext, pt_len,
                                            "text/plain", "test.txt",
                                            &result);
    assert(err == MARMOT_OK);
    assert(result.encrypted_data != NULL);
    assert(result.encrypted_len > pt_len); /* ciphertext + tag */
    assert(result.original_size == pt_len);
    assert(result.imeta.epoch == 1);
    assert(strcmp(result.imeta.mime_type, "text/plain") == 0);
    assert(strcmp(result.imeta.filename, "test.txt") == 0);

    /* Decrypt */
    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    free(decrypted);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_encrypt_binary_data(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"media_bin", 9);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 3, secret);

    /* Binary data with null bytes */
    uint8_t binary[256];
    for (int i = 0; i < 256; i++) binary[i] = (uint8_t)i;

    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            binary, sizeof(binary),
                                            "application/octet-stream", NULL,
                                            &result);
    assert(err == MARMOT_OK);
    assert(result.imeta.filename == NULL); /* NULL filename preserved */

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(dec_len == sizeof(binary));
    assert(memcmp(decrypted, binary, sizeof(binary)) == 0);

    free(decrypted);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_encrypt_large_file(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"media_lg", 8);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    /* 1MB of random data */
    size_t file_len = 1024 * 1024;
    uint8_t *file_data = malloc(file_len);
    assert(file_data != NULL);
    randombytes_buf(file_data, file_len);

    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            file_data, file_len,
                                            "image/png", "photo.png",
                                            &result);
    assert(err == MARMOT_OK);
    assert(result.original_size == file_len);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(dec_len == file_len);
    assert(memcmp(decrypted, file_data, file_len) == 0);

    free(decrypted);
    free(file_data);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_decrypt_wrong_key_fails(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"wrong_key", 9);
    uint8_t secret1[32], secret2[32];
    memset(secret1, 0xAA, 32);
    memset(secret2, 0xBB, 32);
    setup_group_with_secret(m, &gid, 1, secret1);

    /* Encrypt with secret1 */
    const char *plaintext = "Secret message";
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext,
                                            strlen(plaintext),
                                            "text/plain", NULL,
                                            &result);
    assert(err == MARMOT_OK);

    /* Replace exporter secret with a different one */
    assert(m->storage->save_exporter_secret(m->storage->ctx, &gid, 1, secret2)
           == MARMOT_OK);

    /* Decrypt should fail (AEAD tag mismatch) */
    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_ERR_MEDIA_DECRYPT);
    assert(decrypted == NULL);

    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_decrypt_tampered_ciphertext(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"tamper", 6);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    const char *plaintext = "Don't tamper with me!";
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext,
                                            strlen(plaintext),
                                            "text/plain", NULL,
                                            &result);
    assert(err == MARMOT_OK);

    /* Flip a byte in the ciphertext */
    result.encrypted_data[0] ^= 0xFF;

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_ERR_MEDIA_DECRYPT);
    assert(decrypted == NULL);

    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_hash_mismatch_detection(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"hash_mm", 7);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    const char *plaintext = "Hash check test";
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext,
                                            strlen(plaintext),
                                            "text/plain", NULL,
                                            &result);
    assert(err == MARMOT_OK);

    /* Corrupt the file hash in imeta — decryption succeeds but hash check fails */
    result.imeta.file_hash[0] ^= 0xFF;

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_ERR_MEDIA_HASH_MISMATCH);
    assert(decrypted == NULL);

    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_null_mime_type(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"null_mt", 7);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    const char *plaintext = "No MIME type";
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext,
                                            strlen(plaintext),
                                            NULL, NULL, &result);
    assert(err == MARMOT_OK);
    assert(result.imeta.mime_type == NULL);

    /* Decrypt with NULL mime_type in imeta */
    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(dec_len == strlen(plaintext));
    assert(memcmp(decrypted, plaintext, dec_len) == 0);

    free(decrypted);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_different_epochs_different_keys(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"epochs", 6);
    uint8_t s1[32], s2[32];
    randombytes_buf(s1, 32);
    randombytes_buf(s2, 32);

    /* Set up group at epoch 1 */
    setup_group_with_secret(m, &gid, 1, s1);

    const char *plaintext = "Epoch-keyed data";
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)plaintext,
                                            strlen(plaintext),
                                            "text/plain", NULL, &result);
    assert(err == MARMOT_OK);
    assert(result.imeta.epoch == 1);

    /* Advance group to epoch 2 with a different secret */
    MarmotGroup *g = NULL;
    m->storage->find_group_by_mls_id(m->storage->ctx, &gid, &g);
    g->epoch = 2;
    m->storage->save_group(m->storage->ctx, g);
    marmot_group_free(g);
    m->storage->save_exporter_secret(m->storage->ctx, &gid, 2, s2);

    /* Can still decrypt epoch-1 data if the secret is retained */
    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(memcmp(decrypted, plaintext, dec_len) == 0);

    free(decrypted);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_empty_file(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"empty", 5);
    uint8_t secret[32];
    randombytes_buf(secret, 32);
    setup_group_with_secret(m, &gid, 1, secret);

    /* Encrypt a zero-length file */
    MarmotEncryptedMedia result;
    MarmotError err = marmot_encrypt_media(m, &gid,
                                            (const uint8_t *)"", 0,
                                            "application/empty", NULL,
                                            &result);
    assert(err == MARMOT_OK);
    assert(result.original_size == 0);
    /* Ciphertext should be just the AEAD tag (16 bytes) */
    assert(result.encrypted_len == crypto_aead_chacha20poly1305_ietf_ABYTES);

    uint8_t *decrypted = NULL;
    size_t dec_len = 0;
    err = marmot_decrypt_media(m, &gid,
                                result.encrypted_data, result.encrypted_len,
                                &result.imeta,
                                &decrypted, &dec_len);
    assert(err == MARMOT_OK);
    assert(dec_len == 0);

    free(decrypted);
    marmot_encrypted_media_clear(&result);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_invalid_args(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"args", 4);

    MarmotEncryptedMedia result;

    /* NULL marmot */
    assert(marmot_encrypt_media(NULL, &gid, (uint8_t *)"x", 1,
                                 "text/plain", NULL, &result)
           == MARMOT_ERR_INVALID_ARG);

    /* NULL group ID */
    assert(marmot_encrypt_media(m, NULL, (uint8_t *)"x", 1,
                                 "text/plain", NULL, &result)
           == MARMOT_ERR_INVALID_ARG);

    /* NULL file data */
    assert(marmot_encrypt_media(m, &gid, NULL, 1,
                                 "text/plain", NULL, &result)
           == MARMOT_ERR_INVALID_ARG);

    /* NULL result */
    assert(marmot_encrypt_media(m, &gid, (uint8_t *)"x", 1,
                                 "text/plain", NULL, NULL)
           == MARMOT_ERR_INVALID_ARG);

    /* Decrypt: NULL args */
    MarmotImetaInfo imeta = {0};
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(marmot_decrypt_media(NULL, &gid, (uint8_t *)"x", 1,
                                 &imeta, &out, &out_len)
           == MARMOT_ERR_INVALID_ARG);
    assert(marmot_decrypt_media(m, &gid, NULL, 1,
                                 &imeta, &out, &out_len)
           == MARMOT_ERR_INVALID_ARG);

    /* Too-short ciphertext (less than AEAD tag) */
    uint8_t short_ct[4] = {0};
    imeta.epoch = 1;
    uint8_t secret[32];
    memset(secret, 0x99, 32);
    setup_group_with_secret(m, &gid, 1, secret);
    assert(marmot_decrypt_media(m, &gid, short_ct, sizeof(short_ct),
                                 &imeta, &out, &out_len)
           == MARMOT_ERR_INVALID_INPUT);

    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_encrypted_media_clear(void)
{
    MarmotEncryptedMedia result;
    memset(&result, 0, sizeof(result));
    result.encrypted_data = malloc(64);
    result.encrypted_len = 64;
    result.imeta.mime_type = strdup("image/jpeg");
    result.imeta.filename = strdup("photo.jpg");
    result.imeta.url = strdup("https://example.com/file");

    marmot_encrypted_media_clear(&result);

    assert(result.encrypted_data == NULL);
    assert(result.encrypted_len == 0);
    assert(result.imeta.mime_type == NULL);
    assert(result.imeta.filename == NULL);
    assert(result.imeta.url == NULL);

    /* Double clear should be safe */
    marmot_encrypted_media_clear(&result);

    /* NULL should be safe */
    marmot_encrypted_media_clear(NULL);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: MIP-04 media encryption tests\n");

    TEST(test_encrypt_decrypt_roundtrip);
    TEST(test_encrypt_binary_data);
    TEST(test_encrypt_large_file);
    TEST(test_decrypt_wrong_key_fails);
    TEST(test_decrypt_tampered_ciphertext);
    TEST(test_hash_mismatch_detection);
    TEST(test_null_mime_type);
    TEST(test_different_epochs_different_keys);
    TEST(test_empty_file);
    TEST(test_invalid_args);
    TEST(test_encrypted_media_clear);

    printf("All MIP-04 media tests passed (11 tests).\n");
    return 0;
}
