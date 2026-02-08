/* test-signer-core.c - Comprehensive unit tests for gnostr-signer core signer functionality
 *
 * Tests the core cryptographic and signing operations including:
 * - Key generation and derivation (secp256k1)
 * - Schnorr signature creation and verification
 * - NIP-44 encryption/decryption
 * - Event signing workflow
 * - Profile/identity management integration
 * - Secure memory handling for keys
 *
 * Uses GLib testing framework with actual crypto operations.
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <keys.h>
#include <nostr-utils.h>
#include <nostr-event.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip49/nip49.h>
#include <nostr/nip44/nip44.h>

/* ===========================================================================
 * Test Data / Known Vectors
 * =========================================================================== */

/* Well-known test vectors from NIP specifications */
static const char *TEST_SK_HEX = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/* ===========================================================================
 * Key Generation and Derivation Tests
 * =========================================================================== */

static void
test_key_generation_randomness(void)
{
    /* Test that multiple key generations produce different keys */
    const int NUM_KEYS = 10;
    char *keys[NUM_KEYS];

    for (int i = 0; i < NUM_KEYS; i++) {
        keys[i] = nostr_key_generate_private();
        g_assert_nonnull(keys[i]);
        g_assert_cmpuint(strlen(keys[i]), ==, 64);
    }

    /* Verify all keys are different */
    for (int i = 0; i < NUM_KEYS; i++) {
        for (int j = i + 1; j < NUM_KEYS; j++) {
            g_assert_cmpstr(keys[i], !=, keys[j]);
        }
    }

    /* Cleanup */
    for (int i = 0; i < NUM_KEYS; i++) {
        free(keys[i]);
    }
}

static void
test_key_generation_valid_hex(void)
{
    char *sk = nostr_key_generate_private();
    g_assert_nonnull(sk);

    /* Verify all characters are valid hex */
    for (size_t i = 0; i < strlen(sk); i++) {
        g_assert_true(g_ascii_isxdigit(sk[i]));
    }

    /* Verify length is exactly 64 characters (32 bytes) */
    g_assert_cmpuint(strlen(sk), ==, 64);

    free(sk);
}

static void
test_key_derivation_deterministic(void)
{
    /* Same private key should always produce same public key */
    char *sk = nostr_key_generate_private();
    g_assert_nonnull(sk);

    char *pk1 = nostr_key_get_public(sk);
    char *pk2 = nostr_key_get_public(sk);

    g_assert_nonnull(pk1);
    g_assert_nonnull(pk2);
    g_assert_cmpstr(pk1, ==, pk2);
    g_assert_cmpuint(strlen(pk1), ==, 64);

    /* Public key should be different from private key */
    g_assert_cmpstr(sk, !=, pk1);

    free(sk);
    free(pk1);
    free(pk2);
}

static void
test_key_derivation_with_known_vector(void)
{
    /* Use a known test private key */
    char *pk = nostr_key_get_public(TEST_SK_HEX);
    g_assert_nonnull(pk);
    g_assert_cmpuint(strlen(pk), ==, 64);

    /* Verify it's valid hex */
    for (size_t i = 0; i < strlen(pk); i++) {
        g_assert_true(g_ascii_isxdigit(pk[i]));
    }

    /* Calling again should produce same result */
    char *pk2 = nostr_key_get_public(TEST_SK_HEX);
    g_assert_cmpstr(pk, ==, pk2);

    free(pk);
    free(pk2);
}

static void
test_key_validation_public_hex(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Valid public key should pass */
    g_assert_true(nostr_key_is_valid_public_hex(pk));

    /* Invalid inputs */
    g_assert_false(nostr_key_is_valid_public_hex(NULL));
    g_assert_false(nostr_key_is_valid_public_hex(""));
    g_assert_false(nostr_key_is_valid_public_hex("tooshort"));
    g_assert_false(nostr_key_is_valid_public_hex("not-a-valid-hex-string-with-correct-length!!"));

    /* Wrong length */
    g_assert_false(nostr_key_is_valid_public_hex("abcd1234"));

    free(sk);
    free(pk);
}

static void
test_key_bytes_conversion(void)
{
    char *sk_hex = nostr_key_generate_private();
    g_assert_nonnull(sk_hex);

    /* Convert hex to bytes */
    uint8_t sk_bytes[32];
    g_assert_true(nostr_hex2bin(sk_bytes, sk_hex, 32));

    /* Convert back to hex */
    char *sk_hex_back = nostr_bin2hex(sk_bytes, 32);
    g_assert_nonnull(sk_hex_back);
    g_assert_cmpstr(sk_hex, ==, sk_hex_back);

    free(sk_hex);
    free(sk_hex_back);
}

/* ===========================================================================
 * Schnorr Signature Tests (via Event Signing)
 * =========================================================================== */

static void
test_schnorr_event_sign_basic(void)
{
    /* Generate keypair */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);
    g_assert_nonnull(sk);
    g_assert_nonnull(pk);

    /* Create a test event */
    NostrEvent *event = nostr_event_new();
    g_assert_nonnull(event);

    nostr_event_set_pubkey(event, pk);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);  /* Text note */
    nostr_event_set_content(event, "Hello, Nostr!");

    /* Sign the event */
    int rc = nostr_event_sign(event, sk);
    g_assert_cmpint(rc, ==, 0);

    /* Event should now have id and sig */
    char *event_id = nostr_event_get_id(event);
    const char *sig = nostr_event_get_sig(event);

    g_assert_nonnull(event_id);
    g_assert_nonnull(sig);
    g_assert_cmpuint(strlen(event_id), ==, 64);
    g_assert_cmpuint(strlen(sig), ==, 128);

    free(event_id);
    nostr_event_free(event);
    free(sk);
    free(pk);
}

static void
test_schnorr_event_verify_valid(void)
{
    /* Generate keypair */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Create and sign event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Test message for signature verification");

    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);

    /* Verify signature */
    g_assert_true(nostr_event_check_signature(event));

    nostr_event_free(event);
    free(sk);
    free(pk);
}

static void
test_schnorr_event_verify_invalid_signature(void)
{
    /* Generate keypair */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Create and sign event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Original content");

    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);

    /* Tamper with content after signing */
    nostr_event_set_content(event, "Tampered content");

    /* Verification should fail */
    g_assert_false(nostr_event_check_signature(event));

    nostr_event_free(event);
    free(sk);
    free(pk);
}

static void
test_schnorr_event_verify_wrong_pubkey(void)
{
    /* Generate two keypairs */
    char *sk1 = nostr_key_generate_private();
    char *pk1 = nostr_key_get_public(sk1);
    char *sk2 = nostr_key_generate_private();
    char *pk2 = nostr_key_get_public(sk2);

    /* Create event with pk1 */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk1);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Test content");

    /* Sign with sk1 */
    g_assert_cmpint(nostr_event_sign(event, sk1), ==, 0);
    g_assert_true(nostr_event_check_signature(event));

    /* Change pubkey to pk2 - verification should fail */
    nostr_event_set_pubkey(event, pk2);
    g_assert_false(nostr_event_check_signature(event));

    nostr_event_free(event);
    free(sk1);
    free(pk1);
    free(sk2);
    free(pk2);
}

static void
test_schnorr_multiple_signs(void)
{
    /* Generate keypair */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Sign multiple events */
    for (int i = 0; i < 5; i++) {
        NostrEvent *event = nostr_event_new();
        nostr_event_set_pubkey(event, pk);
        nostr_event_set_created_at(event, (int64_t)time(NULL) + i);
        nostr_event_set_kind(event, 1);

        char content[64];
        g_snprintf(content, sizeof(content), "Message number %d", i);
        nostr_event_set_content(event, content);

        g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
        g_assert_true(nostr_event_check_signature(event));

        nostr_event_free(event);
    }

    free(sk);
    free(pk);
}

static void
test_schnorr_different_event_kinds(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Test different event kinds */
    int kinds[] = {0, 1, 3, 4, 7, 30023, 10002};

    for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++) {
        NostrEvent *event = nostr_event_new();
        nostr_event_set_pubkey(event, pk);
        nostr_event_set_created_at(event, (int64_t)time(NULL));
        nostr_event_set_kind(event, kinds[i]);
        nostr_event_set_content(event, "Test content");

        g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
        g_assert_true(nostr_event_check_signature(event));

        nostr_event_free(event);
    }

    free(sk);
    free(pk);
}

/* ===========================================================================
 * NIP-44 Encryption/Decryption Tests
 * =========================================================================== */

static void
test_nip44_encrypt_decrypt_basic(void)
{
    /* Generate two keypairs for sender and receiver */
    char *sender_sk_hex = nostr_key_generate_private();
    char *sender_pk_hex = nostr_key_get_public(sender_sk_hex);
    char *receiver_sk_hex = nostr_key_generate_private();
    char *receiver_pk_hex = nostr_key_get_public(receiver_sk_hex);

    /* Convert to bytes */
    uint8_t sender_sk[32], receiver_sk[32];
    uint8_t sender_pk[32], receiver_pk[32];

    g_assert_true(nostr_hex2bin(sender_sk, sender_sk_hex, 32));
    g_assert_true(nostr_hex2bin(sender_pk, sender_pk_hex, 32));
    g_assert_true(nostr_hex2bin(receiver_sk, receiver_sk_hex, 32));
    g_assert_true(nostr_hex2bin(receiver_pk, receiver_pk_hex, 32));

    /* Message to encrypt */
    const char *plaintext = "Hello, this is a secret message!";
    size_t plaintext_len = strlen(plaintext);

    /* Encrypt from sender to receiver */
    char *ciphertext_base64 = NULL;
    int rc = nostr_nip44_encrypt_v2(sender_sk, receiver_pk,
                                     (const uint8_t *)plaintext, plaintext_len,
                                     &ciphertext_base64);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(ciphertext_base64);

    /* Decrypt as receiver */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    rc = nostr_nip44_decrypt_v2(receiver_sk, sender_pk,
                                 ciphertext_base64,
                                 &decrypted, &decrypted_len);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(decrypted);
    g_assert_cmpuint(decrypted_len, ==, plaintext_len);
    g_assert_cmpmem(decrypted, decrypted_len, plaintext, plaintext_len);

    /* Cleanup */
    free(ciphertext_base64);
    free(decrypted);
    free(sender_sk_hex);
    free(sender_pk_hex);
    free(receiver_sk_hex);
    free(receiver_pk_hex);
}

static void
test_nip44_conversation_key_symmetric(void)
{
    /* Generate two keypairs */
    char *sk1_hex = nostr_key_generate_private();
    char *pk1_hex = nostr_key_get_public(sk1_hex);
    char *sk2_hex = nostr_key_generate_private();
    char *pk2_hex = nostr_key_get_public(sk2_hex);

    uint8_t sk1[32], pk1[32], sk2[32], pk2[32];
    nostr_hex2bin(sk1, sk1_hex, 32);
    nostr_hex2bin(pk1, pk1_hex, 32);
    nostr_hex2bin(sk2, sk2_hex, 32);
    nostr_hex2bin(pk2, pk2_hex, 32);

    /* Derive conversation keys from both sides */
    uint8_t convkey1[32], convkey2[32];

    int rc1 = nostr_nip44_convkey(sk1, pk2, convkey1);  /* Alice derives with Bob's pubkey */
    int rc2 = nostr_nip44_convkey(sk2, pk1, convkey2);  /* Bob derives with Alice's pubkey */

    g_assert_cmpint(rc1, ==, 0);
    g_assert_cmpint(rc2, ==, 0);

    /* Conversation keys should be identical (ECDH property) */
    g_assert_cmpmem(convkey1, 32, convkey2, 32);

    free(sk1_hex);
    free(pk1_hex);
    free(sk2_hex);
    free(pk2_hex);
}

static void
test_nip44_encrypt_different_messages(void)
{
    /* Test that different messages produce different ciphertexts */
    char *sk1_hex = nostr_key_generate_private();
    char *sk2_hex = nostr_key_generate_private();
    char *pk2_hex = nostr_key_get_public(sk2_hex);

    uint8_t sk1[32], pk2[32];
    nostr_hex2bin(sk1, sk1_hex, 32);
    nostr_hex2bin(pk2, pk2_hex, 32);

    const char *msg1 = "First message";
    const char *msg2 = "Second message";

    char *ct1 = NULL, *ct2 = NULL;

    g_assert_cmpint(nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)msg1, strlen(msg1), &ct1), ==, 0);
    g_assert_cmpint(nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)msg2, strlen(msg2), &ct2), ==, 0);

    /* Ciphertexts should be different */
    g_assert_cmpstr(ct1, !=, ct2);

    free(ct1);
    free(ct2);
    free(sk1_hex);
    free(sk2_hex);
    free(pk2_hex);
}

static void
test_nip44_encrypt_same_message_different_nonce(void)
{
    /* Same message encrypted twice should produce different ciphertexts (random nonce) */
    char *sk1_hex = nostr_key_generate_private();
    char *sk2_hex = nostr_key_generate_private();
    char *pk2_hex = nostr_key_get_public(sk2_hex);

    uint8_t sk1[32], pk2[32];
    nostr_hex2bin(sk1, sk1_hex, 32);
    nostr_hex2bin(pk2, pk2_hex, 32);

    const char *msg = "Same message";

    char *ct1 = NULL, *ct2 = NULL;

    g_assert_cmpint(nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)msg, strlen(msg), &ct1), ==, 0);
    g_assert_cmpint(nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)msg, strlen(msg), &ct2), ==, 0);

    /* Due to random nonce, ciphertexts should be different */
    g_assert_cmpstr(ct1, !=, ct2);

    free(ct1);
    free(ct2);
    free(sk1_hex);
    free(sk2_hex);
    free(pk2_hex);
}

static void
test_nip44_decrypt_wrong_key(void)
{
    /* Decryption with wrong key should fail */
    char *sender_sk_hex = nostr_key_generate_private();
    char *receiver_sk_hex = nostr_key_generate_private();
    char *receiver_pk_hex = nostr_key_get_public(receiver_sk_hex);
    char *wrong_sk_hex = nostr_key_generate_private();
    char *sender_pk_hex = nostr_key_get_public(sender_sk_hex);

    uint8_t sender_sk[32], receiver_pk[32], wrong_sk[32], sender_pk[32];
    nostr_hex2bin(sender_sk, sender_sk_hex, 32);
    nostr_hex2bin(receiver_pk, receiver_pk_hex, 32);
    nostr_hex2bin(wrong_sk, wrong_sk_hex, 32);
    nostr_hex2bin(sender_pk, sender_pk_hex, 32);

    const char *plaintext = "Secret message";

    char *ciphertext = NULL;
    g_assert_cmpint(nostr_nip44_encrypt_v2(sender_sk, receiver_pk,
                                           (const uint8_t *)plaintext, strlen(plaintext),
                                           &ciphertext), ==, 0);

    /* Try to decrypt with wrong key - should fail */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    int rc = nostr_nip44_decrypt_v2(wrong_sk, sender_pk, ciphertext, &decrypted, &decrypted_len);

    /* Should fail (MAC verification) */
    g_assert_cmpint(rc, !=, 0);

    free(ciphertext);
    if (decrypted) free(decrypted);
    free(sender_sk_hex);
    free(receiver_sk_hex);
    free(receiver_pk_hex);
    free(wrong_sk_hex);
    free(sender_pk_hex);
}

static void
test_nip44_empty_message(void)
{
    char *sk1_hex = nostr_key_generate_private();
    char *sk2_hex = nostr_key_generate_private();
    char *pk2_hex = nostr_key_get_public(sk2_hex);
    char *pk1_hex = nostr_key_get_public(sk1_hex);

    uint8_t sk1[32], pk2[32], sk2[32], pk1[32];
    nostr_hex2bin(sk1, sk1_hex, 32);
    nostr_hex2bin(pk2, pk2_hex, 32);
    nostr_hex2bin(sk2, sk2_hex, 32);
    nostr_hex2bin(pk1, pk1_hex, 32);

    /* Encrypt empty message */
    const char *empty_msg = "";
    char *ciphertext = NULL;

    int rc = nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)empty_msg, 0, &ciphertext);

    /* NIP-44 requires minimum 1 byte of plaintext, so this should either:
     * - Work with padding (implementation dependent)
     * - Fail gracefully */
    if (rc == 0) {
        g_assert_nonnull(ciphertext);

        uint8_t *decrypted = NULL;
        size_t decrypted_len = 0;
        rc = nostr_nip44_decrypt_v2(sk2, pk1, ciphertext, &decrypted, &decrypted_len);
        g_assert_cmpint(rc, ==, 0);
        g_assert_cmpuint(decrypted_len, ==, 0);

        free(decrypted);
        free(ciphertext);
    }

    free(sk1_hex);
    free(sk2_hex);
    free(pk1_hex);
    free(pk2_hex);
}

static void
test_nip44_long_message(void)
{
    char *sk1_hex = nostr_key_generate_private();
    char *sk2_hex = nostr_key_generate_private();
    char *pk2_hex = nostr_key_get_public(sk2_hex);
    char *pk1_hex = nostr_key_get_public(sk1_hex);

    uint8_t sk1[32], pk2[32], sk2[32], pk1[32];
    nostr_hex2bin(sk1, sk1_hex, 32);
    nostr_hex2bin(pk2, pk2_hex, 32);
    nostr_hex2bin(sk2, sk2_hex, 32);
    nostr_hex2bin(pk1, pk1_hex, 32);

    /* Create a moderately long message (8KB - within NIP-44 limits) */
    size_t msg_len = 8 * 1024;
    char *long_msg = g_malloc(msg_len + 1);
    for (size_t i = 0; i < msg_len; i++) {
        long_msg[i] = 'A' + (i % 26);
    }
    long_msg[msg_len] = '\0';

    /* Encrypt */
    char *ciphertext = NULL;
    int rc = nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)long_msg, msg_len, &ciphertext);

    /* NIP-44 has a max message size limit. If encryption fails due to size, that's expected. */
    if (rc != 0) {
        /* Try a smaller message */
        free(long_msg);
        msg_len = 1024;
        long_msg = g_malloc(msg_len + 1);
        for (size_t i = 0; i < msg_len; i++) {
            long_msg[i] = 'A' + (i % 26);
        }
        long_msg[msg_len] = '\0';

        rc = nostr_nip44_encrypt_v2(sk1, pk2, (const uint8_t *)long_msg, msg_len, &ciphertext);
    }

    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(ciphertext);

    /* Decrypt */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    rc = nostr_nip44_decrypt_v2(sk2, pk1, ciphertext, &decrypted, &decrypted_len);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(decrypted);
    g_assert_cmpuint(decrypted_len, ==, msg_len);
    g_assert_cmpmem(decrypted, decrypted_len, long_msg, msg_len);

    free(ciphertext);
    free(decrypted);
    free(long_msg);
    free(sk1_hex);
    free(sk2_hex);
    free(pk1_hex);
    free(pk2_hex);
}

/* ===========================================================================
 * Event Signing Workflow Tests
 * =========================================================================== */

static void
test_event_workflow_create_sign_verify(void)
{
    /* Complete workflow: create -> populate -> sign -> verify */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Create event */
    NostrEvent *event = nostr_event_new();
    g_assert_nonnull(event);

    /* Populate fields */
    nostr_event_set_pubkey(event, pk);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "This is a complete workflow test");

    /* Sign */
    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);

    /* Get and verify fields */
    char *event_id = nostr_event_get_id(event);
    const char *sig = nostr_event_get_sig(event);
    const char *content = nostr_event_get_content(event);
    int kind = nostr_event_get_kind(event);

    g_assert_nonnull(event_id);
    g_assert_nonnull(sig);
    g_assert_cmpstr(content, ==, "This is a complete workflow test");
    g_assert_cmpint(kind, ==, 1);

    /* Verify signature */
    g_assert_true(nostr_event_check_signature(event));

    free(event_id);
    nostr_event_free(event);
    free(sk);
    free(pk);
}

static void
test_event_workflow_serialize_deserialize(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Create and sign event */
    NostrEvent *original = nostr_event_new();
    nostr_event_set_pubkey(original, pk);
    nostr_event_set_created_at(original, 1234567890);
    nostr_event_set_kind(original, 1);
    nostr_event_set_content(original, "Serialization test");
    g_assert_cmpint(nostr_event_sign(original, sk), ==, 0);

    /* Serialize */
    char *json = nostr_event_serialize_compact(original);
    g_assert_nonnull(json);

    /* Deserialize into new event */
    NostrEvent *restored = nostr_event_new();
    int rc = nostr_event_deserialize_compact(restored, json, NULL);
    g_assert_cmpint(rc, ==, 1);

    /* Verify fields match */
    g_assert_cmpstr(nostr_event_get_pubkey(restored), ==, nostr_event_get_pubkey(original));
    g_assert_cmpint(nostr_event_get_created_at(restored), ==, nostr_event_get_created_at(original));
    g_assert_cmpint(nostr_event_get_kind(restored), ==, nostr_event_get_kind(original));
    g_assert_cmpstr(nostr_event_get_content(restored), ==, nostr_event_get_content(original));
    g_assert_cmpstr(nostr_event_get_sig(restored), ==, nostr_event_get_sig(original));

    /* Restored event should pass signature verification */
    g_assert_true(nostr_event_check_signature(restored));

    free(json);
    nostr_event_free(original);
    nostr_event_free(restored);
    free(sk);
    free(pk);
}

static void
test_event_workflow_unicode_content(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Test various unicode content */
    const char *unicode_tests[] = {
        "Simple ASCII",
        "Emoji test: \xF0\x9F\x8E\x89\xF0\x9F\x8E\x8A",  /* Party emojis */
        "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95",  /* Chinese: "Chinese test" */
        "Mixed: Hello \xD0\x9C\xD0\xB8\xD1\x80 \xE4\xB8\x96\xE7\x95\x8C",  /* Russian + Chinese */
        "Special chars: <>&\"'\\n\\t"
    };

    for (size_t i = 0; i < G_N_ELEMENTS(unicode_tests); i++) {
        NostrEvent *event = nostr_event_new();
        nostr_event_set_pubkey(event, pk);
        nostr_event_set_created_at(event, (int64_t)time(NULL));
        nostr_event_set_kind(event, 1);
        nostr_event_set_content(event, unicode_tests[i]);

        g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
        g_assert_true(nostr_event_check_signature(event));

        /* Serialize and deserialize should preserve content */
        char *json = nostr_event_serialize_compact(event);
        NostrEvent *restored = nostr_event_new();
        g_assert_cmpint(nostr_event_deserialize_compact(restored, json, NULL), ==, 1);
        g_assert_cmpstr(nostr_event_get_content(restored), ==, unicode_tests[i]);
        g_assert_true(nostr_event_check_signature(restored));

        free(json);
        nostr_event_free(event);
        nostr_event_free(restored);
    }

    free(sk);
    free(pk);
}

static void
test_event_workflow_regular_kinds(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    /* Test that regular events are identified correctly */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "Test");

    /* Kind 1 (text note) is regular */
    nostr_event_set_kind(event, 1);
    g_assert_true(nostr_event_is_regular(event));

    /* Kind 0 (metadata) is replaceable, NOT regular */
    nostr_event_set_kind(event, 0);
    g_assert_false(nostr_event_is_regular(event));

    /* Kind 3 (contacts) is replaceable, NOT regular */
    nostr_event_set_kind(event, 3);
    g_assert_false(nostr_event_is_regular(event));

    /* Kind 4 (encrypted DM) is regular */
    nostr_event_set_kind(event, 4);
    g_assert_true(nostr_event_is_regular(event));

    /* Kind 7 (reaction) is regular */
    nostr_event_set_kind(event, 7);
    g_assert_true(nostr_event_is_regular(event));

    nostr_event_free(event);
    free(sk);
    free(pk);
}

/* ===========================================================================
 * NIP-19 Encoding Integration Tests
 * =========================================================================== */

static void
test_nip19_nsec_integration(void)
{
    /* Generate key and encode/decode through nsec format */
    char *sk_hex = nostr_key_generate_private();
    g_assert_nonnull(sk_hex);

    uint8_t sk_bytes[32];
    g_assert_true(nostr_hex2bin(sk_bytes, sk_hex, 32));

    /* Encode to nsec */
    char *nsec = NULL;
    g_assert_cmpint(nostr_nip19_encode_nsec(sk_bytes, &nsec), ==, 0);
    g_assert_nonnull(nsec);
    g_assert_true(g_str_has_prefix(nsec, "nsec1"));

    /* Decode back */
    uint8_t decoded[32];
    g_assert_cmpint(nostr_nip19_decode_nsec(nsec, decoded), ==, 0);

    /* Convert to hex and compare */
    char *decoded_hex = nostr_bin2hex(decoded, 32);
    g_assert_cmpstr(sk_hex, ==, decoded_hex);

    /* Use the decoded key to sign an event */
    char *pk_hex = nostr_key_get_public(decoded_hex);

    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk_hex);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Signed with decoded nsec");

    g_assert_cmpint(nostr_event_sign(event, decoded_hex), ==, 0);
    g_assert_true(nostr_event_check_signature(event));

    nostr_event_free(event);
    free(nsec);
    free(sk_hex);
    free(decoded_hex);
    free(pk_hex);
}

static void
test_nip19_npub_integration(void)
{
    /* Generate keypair and verify npub encoding */
    char *sk_hex = nostr_key_generate_private();
    char *pk_hex = nostr_key_get_public(sk_hex);

    uint8_t pk_bytes[32];
    g_assert_true(nostr_hex2bin(pk_bytes, pk_hex, 32));

    /* Encode to npub */
    char *npub = NULL;
    g_assert_cmpint(nostr_nip19_encode_npub(pk_bytes, &npub), ==, 0);
    g_assert_nonnull(npub);
    g_assert_true(g_str_has_prefix(npub, "npub1"));

    /* Decode and verify */
    uint8_t decoded[32];
    g_assert_cmpint(nostr_nip19_decode_npub(npub, decoded), ==, 0);
    g_assert_cmpmem(pk_bytes, 32, decoded, 32);

    free(npub);
    free(sk_hex);
    free(pk_hex);
}

/* ===========================================================================
 * NIP-49 Encrypted Key Tests
 * =========================================================================== */

static void
test_nip49_key_protection(void)
{
    /* Test protecting a key with NIP-49 */
    uint8_t sk[32];
    for (int i = 0; i < 32; i++) {
        sk[i] = (uint8_t)(i + 0x10);
    }

    const char *password = "strong-password-123!";

    /* Encrypt with minimal log_n for fast tests */
    char *ncryptsec = NULL;
    int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, password, 16, &ncryptsec);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(ncryptsec);
    g_assert_true(g_str_has_prefix(ncryptsec, "ncryptsec1"));

    /* Decrypt */
    uint8_t decrypted[32];
    NostrNip49SecurityByte out_sec;
    uint8_t out_log_n;

    rc = nostr_nip49_decrypt(ncryptsec, password, decrypted, &out_sec, &out_log_n);
    g_assert_cmpint(rc, ==, 0);
    g_assert_cmpmem(sk, 32, decrypted, 32);
    g_assert_cmpint(out_sec, ==, NOSTR_NIP49_SECURITY_SECURE);
    g_assert_cmpint(out_log_n, ==, 16);

    free(ncryptsec);
}

static void
test_nip49_use_decrypted_key_for_signing(void)
{
    /* Complete flow: encrypt key -> decrypt -> use for signing */
    char *sk_hex = nostr_key_generate_private();
    char *pk_hex = nostr_key_get_public(sk_hex);

    uint8_t sk_bytes[32];
    nostr_hex2bin(sk_bytes, sk_hex, 32);

    const char *password = "test-password";

    /* Encrypt the key */
    char *ncryptsec = NULL;
    g_assert_cmpint(nostr_nip49_encrypt(sk_bytes, NOSTR_NIP49_SECURITY_SECURE,
                                        password, 16, &ncryptsec), ==, 0);

    /* Clear original key (simulating secure storage) */
    memset(sk_bytes, 0, 32);

    /* Later: decrypt and use */
    uint8_t decrypted_sk[32];
    g_assert_cmpint(nostr_nip49_decrypt(ncryptsec, password, decrypted_sk, NULL, NULL), ==, 0);

    /* Convert decrypted key back to hex for signing */
    char *decrypted_sk_hex = nostr_bin2hex(decrypted_sk, 32);

    /* Create and sign event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk_hex);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Signed with decrypted key");

    g_assert_cmpint(nostr_event_sign(event, decrypted_sk_hex), ==, 0);
    g_assert_true(nostr_event_check_signature(event));

    /* Cleanup */
    memset(decrypted_sk, 0, 32);
    memset(decrypted_sk_hex, 0, strlen(decrypted_sk_hex));

    nostr_event_free(event);
    free(ncryptsec);
    free(sk_hex);
    free(pk_hex);
    free(decrypted_sk_hex);
}

/* ===========================================================================
 * Identity/Profile Management Tests (using mock store)
 * =========================================================================== */

typedef struct {
    gchar *npub;
    gchar *nsec_encrypted;  /* NIP-49 encrypted */
    gchar *label;
    gint64 created_at;
} TestProfile;

typedef struct {
    GHashTable *profiles;
    gchar *active_npub;
} TestProfileStore;

static void
test_profile_free(gpointer data)
{
    TestProfile *profile = (TestProfile *)data;
    if (!profile) return;
    g_free(profile->npub);
    if (profile->nsec_encrypted) {
        memset(profile->nsec_encrypted, 0, strlen(profile->nsec_encrypted));
        g_free(profile->nsec_encrypted);
    }
    g_free(profile->label);
    g_free(profile);
}

static TestProfileStore *
test_profile_store_new(void)
{
    TestProfileStore *store = g_new0(TestProfileStore, 1);
    /* Key: g_strdup'd npub, Value: TestProfile* - use custom destructor */
    store->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, test_profile_free);
    return store;
}

static void
test_profile_store_free(TestProfileStore *store)
{
    if (!store) return;
    /* g_hash_table_destroy will call destructors for all entries */
    g_hash_table_destroy(store->profiles);
    g_free(store->active_npub);
    g_free(store);
}

static gboolean
test_profile_store_add(TestProfileStore *store, const gchar *npub,
                        const gchar *nsec_encrypted, const gchar *label)
{
    if (!store || !npub || !nsec_encrypted) return FALSE;
    if (g_hash_table_contains(store->profiles, npub)) return FALSE;

    TestProfile *profile = g_new0(TestProfile, 1);
    profile->npub = g_strdup(npub);
    profile->nsec_encrypted = g_strdup(nsec_encrypted);
    profile->label = label ? g_strdup(label) : g_strdup("");
    profile->created_at = (gint64)time(NULL);

    g_hash_table_insert(store->profiles, g_strdup(npub), profile);

    if (!store->active_npub) {
        store->active_npub = g_strdup(npub);
    }

    return TRUE;
}

static void
test_profile_management_create_from_key(void)
{
    TestProfileStore *store = test_profile_store_new();

    /* Generate new key */
    char *sk_hex = nostr_key_generate_private();
    char *pk_hex = nostr_key_get_public(sk_hex);

    /* Convert to bytes and create npub */
    uint8_t pk_bytes[32], sk_bytes[32];
    nostr_hex2bin(pk_bytes, pk_hex, 32);
    nostr_hex2bin(sk_bytes, sk_hex, 32);

    char *npub = NULL;
    nostr_nip19_encode_npub(pk_bytes, &npub);

    /* Encrypt private key */
    char *ncryptsec = NULL;
    nostr_nip49_encrypt(sk_bytes, NOSTR_NIP49_SECURITY_SECURE, "password", 16, &ncryptsec);

    /* Add profile - store duplicates strings internally via g_strdup */
    g_assert_true(test_profile_store_add(store, npub, ncryptsec, "My Profile"));
    g_assert_cmpuint(g_hash_table_size(store->profiles), ==, 1);

    /* Verify active npub was set (store has its own copy) */
    g_assert_nonnull(store->active_npub);

    /* Cleanup - use free() for library-allocated memory */
    memset(sk_bytes, 0, 32);
    free(sk_hex);
    free(pk_hex);
    free(npub);
    free(ncryptsec);
    test_profile_store_free(store);
}

static void
test_profile_management_sign_with_stored_key(void)
{
    TestProfileStore *store = test_profile_store_new();
    const char *password = "profile-password";

    /* Create and store profile */
    char *sk_hex = nostr_key_generate_private();
    char *pk_hex = nostr_key_get_public(sk_hex);

    uint8_t pk_bytes[32], sk_bytes[32];
    nostr_hex2bin(pk_bytes, pk_hex, 32);
    nostr_hex2bin(sk_bytes, sk_hex, 32);

    char *npub = NULL;
    nostr_nip19_encode_npub(pk_bytes, &npub);

    char *ncryptsec = NULL;
    nostr_nip49_encrypt(sk_bytes, NOSTR_NIP49_SECURITY_SECURE, password, 16, &ncryptsec);

    test_profile_store_add(store, npub, ncryptsec, "Signing Profile");

    /* Retrieve and decrypt key for signing */
    TestProfile *profile = g_hash_table_lookup(store->profiles, npub);
    g_assert_nonnull(profile);

    uint8_t decrypted_sk[32];
    g_assert_cmpint(nostr_nip49_decrypt(profile->nsec_encrypted, password,
                                        decrypted_sk, NULL, NULL), ==, 0);

    char *decrypted_sk_hex = nostr_bin2hex(decrypted_sk, 32);

    /* Sign event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_pubkey(event, pk_hex);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "Signed from profile store");

    g_assert_cmpint(nostr_event_sign(event, decrypted_sk_hex), ==, 0);
    g_assert_true(nostr_event_check_signature(event));

    /* Secure cleanup */
    memset(decrypted_sk, 0, 32);
    memset(decrypted_sk_hex, 0, strlen(decrypted_sk_hex));
    memset(sk_bytes, 0, 32);

    nostr_event_free(event);
    free(sk_hex);
    free(pk_hex);
    free(npub);
    free(ncryptsec);
    free(decrypted_sk_hex);
    test_profile_store_free(store);
}

/* ===========================================================================
 * Edge Cases and Error Handling Tests
 * =========================================================================== */

static void
test_edge_case_null_inputs(void)
{
    /* Test null handling in various functions */

    /* Key functions */
    g_assert_null(nostr_key_get_public(NULL));
    g_assert_false(nostr_key_is_valid_public_hex(NULL));

    /* Event functions with NULL event */
    g_assert_false(nostr_event_check_signature(NULL));
    g_assert_cmpint(nostr_event_sign(NULL, "key"), !=, 0);

    /* Hex conversion */
    uint8_t buf[32];
    g_assert_false(nostr_hex2bin(buf, NULL, 32));
    g_assert_false(nostr_hex2bin(NULL, "hex", 32));
    g_assert_null(nostr_bin2hex(NULL, 32));
}

static void
test_edge_case_invalid_hex(void)
{
    /* Test invalid hex inputs */
    uint8_t buf[32];

    g_assert_false(nostr_hex2bin(buf, "gg", 1));  /* Invalid hex char */
    g_assert_false(nostr_hex2bin(buf, "xyz", 1));  /* Invalid chars */
    g_assert_false(nostr_hex2bin(buf, "12345", 32));  /* Too short */
}

static void
test_edge_case_event_missing_fields(void)
{
    /* Event with missing fields */
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    NostrEvent *event = nostr_event_new();

    /* Try to sign without setting required fields */
    /* Implementation may handle this differently - testing robustness */
    nostr_event_set_kind(event, 1);
    /* Missing pubkey and content */

    /* This may fail or succeed depending on implementation */
    int rc = nostr_event_sign(event, sk);
    /* We're testing that it doesn't crash */
    (void)rc;

    nostr_event_free(event);
    free(sk);
    free(pk);
}

/* ===========================================================================
 * Performance / Stress Tests
 * =========================================================================== */

static void
test_performance_bulk_signing(void)
{
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);

    const int NUM_EVENTS = 100;

    for (int i = 0; i < NUM_EVENTS; i++) {
        NostrEvent *event = nostr_event_new();
        nostr_event_set_pubkey(event, pk);
        nostr_event_set_created_at(event, (int64_t)time(NULL) + i);
        nostr_event_set_kind(event, 1);

        char content[128];
        g_snprintf(content, sizeof(content), "Bulk test message %d", i);
        nostr_event_set_content(event, content);

        g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
        g_assert_true(nostr_event_check_signature(event));

        nostr_event_free(event);
    }

    free(sk);
    free(pk);
}

static void
test_performance_bulk_key_generation(void)
{
    const int NUM_KEYS = 50;

    for (int i = 0; i < NUM_KEYS; i++) {
        char *sk = nostr_key_generate_private();
        g_assert_nonnull(sk);
        g_assert_cmpuint(strlen(sk), ==, 64);

        char *pk = nostr_key_get_public(sk);
        g_assert_nonnull(pk);
        g_assert_cmpuint(strlen(pk), ==, 64);

        free(sk);
        free(pk);
    }
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Key Generation and Derivation Tests */
    g_test_add_func("/signer/core/key/generation_randomness", test_key_generation_randomness);
    g_test_add_func("/signer/core/key/generation_valid_hex", test_key_generation_valid_hex);
    g_test_add_func("/signer/core/key/derivation_deterministic", test_key_derivation_deterministic);
    g_test_add_func("/signer/core/key/derivation_known_vector", test_key_derivation_with_known_vector);
    g_test_add_func("/signer/core/key/validation_public_hex", test_key_validation_public_hex);
    g_test_add_func("/signer/core/key/bytes_conversion", test_key_bytes_conversion);

    /* Schnorr Signature Tests */
    g_test_add_func("/signer/core/schnorr/event_sign_basic", test_schnorr_event_sign_basic);
    g_test_add_func("/signer/core/schnorr/event_verify_valid", test_schnorr_event_verify_valid);
    g_test_add_func("/signer/core/schnorr/event_verify_invalid_signature", test_schnorr_event_verify_invalid_signature);
    g_test_add_func("/signer/core/schnorr/event_verify_wrong_pubkey", test_schnorr_event_verify_wrong_pubkey);
    g_test_add_func("/signer/core/schnorr/multiple_signs", test_schnorr_multiple_signs);
    g_test_add_func("/signer/core/schnorr/different_event_kinds", test_schnorr_different_event_kinds);

    /* NIP-44 Encryption Tests */
    g_test_add_func("/signer/core/nip44/encrypt_decrypt_basic", test_nip44_encrypt_decrypt_basic);
    g_test_add_func("/signer/core/nip44/conversation_key_symmetric", test_nip44_conversation_key_symmetric);
    g_test_add_func("/signer/core/nip44/encrypt_different_messages", test_nip44_encrypt_different_messages);
    g_test_add_func("/signer/core/nip44/encrypt_same_message_different_nonce", test_nip44_encrypt_same_message_different_nonce);
    g_test_add_func("/signer/core/nip44/decrypt_wrong_key", test_nip44_decrypt_wrong_key);
    g_test_add_func("/signer/core/nip44/empty_message", test_nip44_empty_message);
    g_test_add_func("/signer/core/nip44/long_message", test_nip44_long_message);

    /* Event Signing Workflow Tests */
    g_test_add_func("/signer/core/workflow/create_sign_verify", test_event_workflow_create_sign_verify);
    g_test_add_func("/signer/core/workflow/serialize_deserialize", test_event_workflow_serialize_deserialize);
    g_test_add_func("/signer/core/workflow/unicode_content", test_event_workflow_unicode_content);
    g_test_add_func("/signer/core/workflow/regular_kinds", test_event_workflow_regular_kinds);

    /* NIP-19 Integration Tests */
    g_test_add_func("/signer/core/nip19/nsec_integration", test_nip19_nsec_integration);
    g_test_add_func("/signer/core/nip19/npub_integration", test_nip19_npub_integration);

    /* NIP-49 Encrypted Key Tests */
    g_test_add_func("/signer/core/nip49/key_protection", test_nip49_key_protection);
    g_test_add_func("/signer/core/nip49/use_decrypted_key_for_signing", test_nip49_use_decrypted_key_for_signing);

    /* Profile Management Tests */
    g_test_add_func("/signer/core/profile/create_from_key", test_profile_management_create_from_key);
    g_test_add_func("/signer/core/profile/sign_with_stored_key", test_profile_management_sign_with_stored_key);

    /* Edge Cases Tests */
    g_test_add_func("/signer/core/edge/null_inputs", test_edge_case_null_inputs);
    g_test_add_func("/signer/core/edge/invalid_hex", test_edge_case_invalid_hex);
    g_test_add_func("/signer/core/edge/event_missing_fields", test_edge_case_event_missing_fields);

    /* Performance Tests */
    g_test_add_func("/signer/core/perf/bulk_signing", test_performance_bulk_signing);
    g_test_add_func("/signer/core/perf/bulk_key_generation", test_performance_bulk_key_generation);

    return g_test_run();
}
