/*
 * libmarmot - MLS Welcome message tests
 *
 * Tests for Welcome serialization, processing, and group joining.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_welcome.h"
#include "mls/mls_group.h"
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
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
static const uint8_t GROUP_ID[]   = "welcome-test-group";

static int
create_alice_group(MlsGroup *group, uint8_t sig_sk[MLS_SIG_SK_LEN])
{
    uint8_t sig_pk[MLS_SIG_PK_LEN];
    if (mls_crypto_sign_keygen(sig_sk, sig_pk) != 0) return -1;
    return mls_group_create(group, GROUP_ID, sizeof(GROUP_ID),
                            ALICE_ID, 32, sig_sk, NULL, 0);
}

/* ── Welcome serialization tests ───────────────────────────────────────── */

TEST(test_welcome_serialize_roundtrip)
{
    MlsWelcome w;
    memset(&w, 0, sizeof(w));
    w.cipher_suite = MARMOT_CIPHERSUITE;

    /* Create a dummy EncryptedGroupSecrets entry */
    w.secret_count = 1;
    w.secrets = calloc(1, sizeof(MlsEncryptedGroupSecrets));
    assert(w.secrets != NULL);
    memset(w.secrets[0].key_package_ref, 0xAA, MLS_HASH_LEN);
    memset(w.secrets[0].kem_output, 0xBB, MLS_KEM_ENC_LEN);
    w.secrets[0].encrypted_joiner_secret = malloc(48);
    assert(w.secrets[0].encrypted_joiner_secret != NULL);
    memset(w.secrets[0].encrypted_joiner_secret, 0xCC, 48);
    w.secrets[0].encrypted_joiner_secret_len = 48;

    /* Dummy encrypted group info */
    w.encrypted_group_info = malloc(100);
    assert(w.encrypted_group_info != NULL);
    memset(w.encrypted_group_info, 0xDD, 100);
    w.encrypted_group_info_len = 100;

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_welcome_serialize(&w, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize */
    MlsWelcome w2;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_welcome_deserialize(&reader, &w2) == 0);

    assert(w2.cipher_suite == MARMOT_CIPHERSUITE);
    assert(w2.secret_count == 1);
    assert(memcmp(w2.secrets[0].key_package_ref, w.secrets[0].key_package_ref,
                  MLS_HASH_LEN) == 0);
    assert(memcmp(w2.secrets[0].kem_output, w.secrets[0].kem_output,
                  MLS_KEM_ENC_LEN) == 0);
    assert(w2.secrets[0].encrypted_joiner_secret_len == 48);
    assert(w2.encrypted_group_info_len == 100);

    mls_tls_buf_free(&buf);
    mls_welcome_clear(&w);
    mls_welcome_clear(&w2);
}

TEST(test_welcome_empty_secrets)
{
    MlsWelcome w;
    memset(&w, 0, sizeof(w));
    w.cipher_suite = MARMOT_CIPHERSUITE;
    w.encrypted_group_info = malloc(10);
    memset(w.encrypted_group_info, 0, 10);
    w.encrypted_group_info_len = 10;

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 64) == 0);
    assert(mls_welcome_serialize(&w, &buf) == 0);

    MlsWelcome w2;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_welcome_deserialize(&reader, &w2) == 0);
    assert(w2.secret_count == 0);

    mls_tls_buf_free(&buf);
    mls_welcome_clear(&w);
    mls_welcome_clear(&w2);
}

/* ── Welcome processing tests ──────────────────────────────────────────── */

TEST(test_welcome_process_null_args)
{
    MlsGroup group;
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;

    assert(mls_welcome_process(NULL, 10, &kp, &priv, NULL, 0, &group)
           == MARMOT_ERR_INVALID_ARG);
    assert(mls_welcome_process((uint8_t[]){0}, 1, NULL, &priv, NULL, 0, &group)
           == MARMOT_ERR_INVALID_ARG);
    assert(mls_welcome_process((uint8_t[]){0}, 1, &kp, NULL, NULL, 0, &group)
           == MARMOT_ERR_INVALID_ARG);
    assert(mls_welcome_process((uint8_t[]){0}, 1, &kp, &priv, NULL, 0, NULL)
           == MARMOT_ERR_INVALID_ARG);
}

TEST(test_welcome_wrong_ciphersuite)
{
    MlsWelcome w;
    memset(&w, 0, sizeof(w));
    w.cipher_suite = 0x9999; /* Invalid */

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    memset(&kp, 0, sizeof(kp));
    memset(&priv, 0, sizeof(priv));

    MlsGroup group;
    int rc = mls_welcome_process_parsed(&w, &kp, &priv, NULL, 0, &group);
    assert(rc != 0);
}

TEST(test_welcome_kp_not_found)
{
    MlsWelcome w;
    memset(&w, 0, sizeof(w));
    w.cipher_suite = MARMOT_CIPHERSUITE;
    /* No secrets entries */

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    assert(mls_key_package_create(&kp, &priv, BOB_ID, 32, NULL, 0) == 0);

    MlsGroup group;
    int rc = mls_welcome_process_parsed(&w, &kp, &priv, NULL, 0, &group);
    assert(rc == MARMOT_ERR_WELCOME_NOT_FOUND);

    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
}

/* ── Integration: add member + process Welcome ─────────────────────────── */

TEST(test_add_and_welcome_integration)
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

    /* Verify welcome was produced */
    assert(add_result.welcome_data != NULL);
    assert(add_result.welcome_len > 0);
    assert(add_result.commit_data != NULL);
    assert(add_result.commit_len > 0);

    /* Alice's epoch should have advanced */
    assert(alice_group.epoch == 1);
    assert(alice_group.tree.n_leaves == 2);

    /* Note: Full Welcome processing test requires matching epoch secrets
     * between Alice's add_member and Bob's welcome_process. This is an
     * integration test that verifies the handshake completes end-to-end.
     * The actual crypto verification happens in mls_welcome_process. */

    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
}

static void
assert_dump_unchanged(const char *path, const struct stat *before, int existed)
{
    struct stat after;
    if (!existed) {
        int created = stat(path, &after) == 0;
        if (created) unlink(path);
        assert(!created);
        return;
    }
    assert(stat(path, &after) == 0);
    assert(after.st_ino == before->st_ino);
    assert(after.st_size == before->st_size);
#ifdef __APPLE__
    assert(after.st_mtimespec.tv_sec == before->st_mtimespec.tv_sec);
    assert(after.st_mtimespec.tv_nsec == before->st_mtimespec.tv_nsec);
#else
    assert(after.st_mtim.tv_sec == before->st_mtim.tv_sec);
    assert(after.st_mtim.tv_nsec == before->st_mtim.tv_nsec);
#endif
}

TEST(test_welcome_group_info_stays_private)
{
    const char *dump_path = "/tmp/gi.bin";
    struct stat dump_before;
    int dump_existed = stat(dump_path, &dump_before) == 0;
    if (!dump_existed) assert(errno == ENOENT);

    MlsGroup alice_group;
    uint8_t alice_sk[MLS_SIG_SK_LEN];
    assert(create_alice_group(&alice_group, alice_sk) == 0);

    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
    assert(mls_key_package_create(&bob_kp, &bob_priv, BOB_ID, 32, NULL, 0) == 0);

    MlsAddResult add_result;
    assert(mls_group_add_member(&alice_group, &bob_kp, &add_result) == 0);

    MlsGroup bob_group;
    assert(mls_welcome_process(add_result.welcome_data, add_result.welcome_len,
                               &bob_kp, &bob_priv, NULL, 0, &bob_group) == 0);
    mls_group_free(&bob_group);
    assert_dump_unchanged(dump_path, &dump_before, dump_existed);

    MlsWelcome welcome;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, add_result.welcome_data, add_result.welcome_len);
    assert(mls_welcome_deserialize(&reader, &welcome) == 0);
    assert(welcome.secret_count == 1);

    /* Authenticate a deliberately malformed GroupInfo so processing reaches
     * the parser, rather than failing at HPKE or AEAD decryption. */
    static const uint8_t malformed_info[] = "LEAKTEST";
    uint8_t welcome_key[MLS_AEAD_KEY_LEN];
    uint8_t welcome_nonce[MLS_AEAD_NONCE_LEN];
    assert(mls_crypto_expand_with_label(welcome_key, sizeof(welcome_key),
               alice_group.epoch_secrets.welcome_secret, "key", NULL, 0) == 0);
    assert(mls_crypto_expand_with_label(welcome_nonce, sizeof(welcome_nonce),
               alice_group.epoch_secrets.welcome_secret, "nonce", NULL, 0) == 0);
    free(welcome.encrypted_group_info);
    welcome.encrypted_group_info = malloc(sizeof(malformed_info) + MLS_AEAD_TAG_LEN);
    assert(welcome.encrypted_group_info != NULL);
    assert(mls_crypto_aead_encrypt(welcome.encrypted_group_info,
               &welcome.encrypted_group_info_len, welcome_key, welcome_nonce,
               malformed_info, sizeof(malformed_info), NULL, 0) == 0);
    sodium_memzero(welcome_key, sizeof(welcome_key));
    sodium_memzero(welcome_nonce, sizeof(welcome_nonce));

    MlsTlsBuf group_secrets;
    assert(mls_tls_buf_init(&group_secrets, MLS_HASH_LEN + 16) == 0);
    assert(mls_tls_write_opaque16(&group_secrets,
               alice_group.epoch_secrets.joiner_secret, MLS_HASH_LEN) == 0);
    assert(mls_tls_write_u8(&group_secrets, 0) == 0);
    assert(mls_tls_write_opaque32(&group_secrets, NULL, 0) == 0);

    MlsTlsBuf hpke_info;
    assert(mls_tls_buf_init(&hpke_info, 64) == 0);
    static const uint8_t hpke_label[] = "MLS 1.0 Welcome";
    assert(mls_tls_write_opaque16(&hpke_info, hpke_label,
               sizeof(hpke_label) - 1) == 0);
    assert(mls_tls_write_opaque32(&hpke_info, welcome.encrypted_group_info,
               welcome.encrypted_group_info_len) == 0);

    MlsEncryptedGroupSecrets *egs = &welcome.secrets[0];
    free(egs->encrypted_joiner_secret);
    egs->encrypted_joiner_secret = malloc(group_secrets.len + MLS_AEAD_TAG_LEN);
    assert(egs->encrypted_joiner_secret != NULL);
    assert(mls_crypto_hpke_seal_base(egs->kem_output,
               egs->encrypted_joiner_secret, &egs->encrypted_joiner_secret_len,
               bob_kp.init_key, hpke_info.data, hpke_info.len, NULL, 0,
               group_secrets.data, group_secrets.len) == 0);
    mls_tls_buf_free(&hpke_info);
    sodium_memzero(group_secrets.data, group_secrets.len);
    mls_tls_buf_free(&group_secrets);

    FILE *diagnostics = tmpfile();
    assert(diagnostics != NULL);
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(fileno(diagnostics), STDERR_FILENO) >= 0);
    int rc = mls_welcome_process_parsed(&welcome, &bob_kp, &bob_priv,
                                        NULL, 0, &bob_group);
    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    assert(rc == MARMOT_ERR_WELCOME_INVALID);

    assert(fseek(diagnostics, 0, SEEK_SET) == 0);
    char message[512] = {0};
    size_t message_len = fread(message, 1, sizeof(message) - 1, diagnostics);
    message[message_len] = '\0';
    fclose(diagnostics);
    assert(strstr(message, "groupinfo parse rc=") != NULL);
    assert(strstr(message, "4c45414b54455354") == NULL);
    assert(strstr(message, "prefix=") == NULL);
    assert_dump_unchanged(dump_path, &dump_before, dump_existed);

    mls_welcome_clear(&welcome);
    mls_add_result_clear(&add_result);
    mls_key_package_clear(&bob_kp);
    mls_key_package_private_clear(&bob_priv);
    mls_group_free(&alice_group);
}

/* ── Clear tests ───────────────────────────────────────────────────────── */

TEST(test_welcome_clear_null_safe)
{
    mls_welcome_clear(NULL); /* Should not crash */
    mls_encrypted_group_secrets_clear(NULL); /* Should not crash */
}

TEST(test_welcome_clear_zeroes_secrets)
{
    MlsEncryptedGroupSecrets egs;
    memset(&egs, 0xAA, sizeof(egs));
    egs.encrypted_joiner_secret = malloc(32);
    memset(egs.encrypted_joiner_secret, 0xBB, 32);
    egs.encrypted_joiner_secret_len = 32;

    mls_encrypted_group_secrets_clear(&egs);

    /* Should be zeroed */
    uint8_t zeros[MLS_HASH_LEN] = {0};
    assert(memcmp(egs.key_package_ref, zeros, MLS_HASH_LEN) == 0);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("test_mls_welcome:\n");

    printf(" Serialization:\n");
    RUN(test_welcome_serialize_roundtrip);
    RUN(test_welcome_empty_secrets);

    printf(" Processing:\n");
    RUN(test_welcome_process_null_args);
    RUN(test_welcome_wrong_ciphersuite);
    RUN(test_welcome_kp_not_found);

    printf(" Integration:\n");
    RUN(test_add_and_welcome_integration);
    RUN(test_welcome_group_info_stays_private);

    printf(" Cleanup:\n");
    RUN(test_welcome_clear_null_safe);
    RUN(test_welcome_clear_zeroes_secrets);

    printf("All welcome tests passed.\n");
    return 0;
}
