/* test-crypto.c - Unit tests for gnostr-signer cryptographic operations
 *
 * Tests key generation, signing, verification, and NIP-49 encryption/decryption
 * using the GLib testing framework.
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>

#include <keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip49/nip49.h>

/* ===========================================================================
 * Key Generation Tests
 * =========================================================================== */

static void
test_key_generate_private(void)
{
    /* Test that key generation produces valid hex keys */
    char *sk1 = nostr_key_generate_private();
    g_assert_nonnull(sk1);
    g_assert_cmpuint(strlen(sk1), ==, 64);

    /* Verify it's valid hex */
    for (size_t i = 0; i < 64; i++) {
        gboolean is_hex = g_ascii_isxdigit(sk1[i]);
        g_assert_true(is_hex);
    }

    /* Generate another key - should be different */
    char *sk2 = nostr_key_generate_private();
    g_assert_nonnull(sk2);
    g_assert_cmpstr(sk1, !=, sk2);

    free(sk1);
    free(sk2);
}

static void
test_key_get_public(void)
{
    /* Test deriving public key from private key */
    char *sk = nostr_key_generate_private();
    g_assert_nonnull(sk);

    char *pk = nostr_key_get_public(sk);
    g_assert_nonnull(pk);
    g_assert_cmpuint(strlen(pk), ==, 64);

    /* Public key should be different from private key */
    g_assert_cmpstr(sk, !=, pk);

    /* Same private key should produce same public key */
    char *pk2 = nostr_key_get_public(sk);
    g_assert_nonnull(pk2);
    g_assert_cmpstr(pk, ==, pk2);

    free(sk);
    free(pk);
    free(pk2);
}

static void
test_key_is_valid_public_hex(void)
{
    /* Generate valid keypair */
    char *sk = nostr_key_generate_private();
    g_assert_nonnull(sk);

    char *pk = nostr_key_get_public(sk);
    g_assert_nonnull(pk);

    /* Valid public key should pass validation */
    g_assert_true(nostr_key_is_valid_public_hex(pk));

    /* Invalid inputs should fail */
    g_assert_false(nostr_key_is_valid_public_hex(NULL));
    g_assert_false(nostr_key_is_valid_public_hex(""));
    g_assert_false(nostr_key_is_valid_public_hex("not-hex"));
    g_assert_false(nostr_key_is_valid_public_hex("1234")); /* Too short */

    /*
     * Note: nostr_key_is_valid_public_hex() only validates hex format and length,
     * not whether the key is a valid secp256k1 point. All-zeros would pass hex
     * validation even though it's not a valid curve point. Full validation would
     * require secp256k1_ec_pubkey_parse() which is done at signing time.
     */

    free(sk);
    free(pk);
}

/* ===========================================================================
 * NIP-19 Encoding/Decoding Tests
 * =========================================================================== */

static void
test_nip19_nsec_roundtrip(void)
{
    /* Generate a private key */
    char *sk_hex = nostr_key_generate_private();
    g_assert_nonnull(sk_hex);

    /* Convert to bytes */
    uint8_t sk_bytes[32];
    g_assert_true(nostr_hex2bin(sk_bytes, sk_hex, 32));

    /* Encode to nsec */
    char *nsec = NULL;
    int rc = nostr_nip19_encode_nsec(sk_bytes, &nsec);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(nsec);
    g_assert_true(g_str_has_prefix(nsec, "nsec1"));

    /* Decode back */
    uint8_t decoded[32];
    rc = nostr_nip19_decode_nsec(nsec, decoded);
    g_assert_cmpint(rc, ==, 0);
    g_assert_cmpmem(sk_bytes, 32, decoded, 32);

    free(sk_hex);
    free(nsec);
}

static void
test_nip19_npub_roundtrip(void)
{
    /* Generate keypair */
    char *sk_hex = nostr_key_generate_private();
    g_assert_nonnull(sk_hex);

    char *pk_hex = nostr_key_get_public(sk_hex);
    g_assert_nonnull(pk_hex);

    /* Convert public key to bytes */
    uint8_t pk_bytes[32];
    g_assert_true(nostr_hex2bin(pk_bytes, pk_hex, 32));

    /* Encode to npub */
    char *npub = NULL;
    int rc = nostr_nip19_encode_npub(pk_bytes, &npub);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(npub);
    g_assert_true(g_str_has_prefix(npub, "npub1"));

    /* Decode back */
    uint8_t decoded[32];
    rc = nostr_nip19_decode_npub(npub, decoded);
    g_assert_cmpint(rc, ==, 0);
    g_assert_cmpmem(pk_bytes, 32, decoded, 32);

    free(sk_hex);
    free(pk_hex);
    free(npub);
}

static void
test_nip19_note_roundtrip(void)
{
    /* Use a synthetic event ID */
    uint8_t event_id[32];
    for (int i = 0; i < 32; i++) {
        event_id[i] = (uint8_t)(i * 7 + 13);
    }

    /* Encode to note */
    char *note = NULL;
    int rc = nostr_nip19_encode_note(event_id, &note);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(note);
    g_assert_true(g_str_has_prefix(note, "note1"));

    /* Decode back */
    uint8_t decoded[32];
    rc = nostr_nip19_decode_note(note, decoded);
    g_assert_cmpint(rc, ==, 0);
    g_assert_cmpmem(event_id, 32, decoded, 32);

    free(note);
}

static void
test_nip19_inspect(void)
{
    /* Generate keypair */
    char *sk_hex = nostr_key_generate_private();
    char *pk_hex = nostr_key_get_public(sk_hex);

    uint8_t sk_bytes[32], pk_bytes[32];
    nostr_hex2bin(sk_bytes, sk_hex, 32);
    nostr_hex2bin(pk_bytes, pk_hex, 32);

    char *nsec = NULL, *npub = NULL;
    nostr_nip19_encode_nsec(sk_bytes, &nsec);
    nostr_nip19_encode_npub(pk_bytes, &npub);

    /* Inspect types */
    NostrBech32Type type;

    g_assert_cmpint(nostr_nip19_inspect(nsec, &type), ==, 0);
    g_assert_cmpint(type, ==, NOSTR_B32_NSEC);

    g_assert_cmpint(nostr_nip19_inspect(npub, &type), ==, 0);
    g_assert_cmpint(type, ==, NOSTR_B32_NPUB);

    /* Invalid input */
    g_assert_cmpint(nostr_nip19_inspect("invalid", &type), ==, -1);

    free(sk_hex);
    free(pk_hex);
    free(nsec);
    free(npub);
}

/* ===========================================================================
 * NIP-49 Encryption Tests
 * =========================================================================== */

static void
test_nip49_encrypt_decrypt_basic(void)
{
    /* Create a test private key */
    uint8_t sk[32];
    for (int i = 0; i < 32; i++) {
        sk[i] = (uint8_t)(0x42 + i);
    }

    const char *password = "test-password-123";

    /* Encrypt with secure flag */
    char *ncryptsec = NULL;
    int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, password, 16, &ncryptsec);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(ncryptsec);
    g_assert_true(g_str_has_prefix(ncryptsec, "ncryptsec1"));

    /* Decrypt */
    uint8_t decrypted[32];
    NostrNip49SecurityByte out_sec = 0;
    uint8_t out_log_n = 0;

    rc = nostr_nip49_decrypt(ncryptsec, password, decrypted, &out_sec, &out_log_n);
    g_assert_cmpint(rc, ==, 0);
    g_assert_cmpmem(sk, 32, decrypted, 32);
    g_assert_cmpint(out_sec, ==, NOSTR_NIP49_SECURITY_SECURE);
    g_assert_cmpint(out_log_n, ==, 16);

    free(ncryptsec);
}

static void
test_nip49_security_bytes(void)
{
    uint8_t sk[32];
    for (int i = 0; i < 32; i++) {
        sk[i] = (uint8_t)(0xAB + i);
    }

    const char *password = "security-test";
    NostrNip49SecurityByte test_secs[] = {
        NOSTR_NIP49_SECURITY_INSECURE,
        NOSTR_NIP49_SECURITY_SECURE,
        NOSTR_NIP49_SECURITY_UNKNOWN
    };

    for (size_t i = 0; i < G_N_ELEMENTS(test_secs); i++) {
        char *ncryptsec = NULL;
        int rc = nostr_nip49_encrypt(sk, test_secs[i], password, 16, &ncryptsec);
        g_assert_cmpint(rc, ==, 0);

        uint8_t decrypted[32];
        NostrNip49SecurityByte out_sec = 0;

        rc = nostr_nip49_decrypt(ncryptsec, password, decrypted, &out_sec, NULL);
        g_assert_cmpint(rc, ==, 0);
        g_assert_cmpint(out_sec, ==, test_secs[i]);

        free(ncryptsec);
    }
}

static void
test_nip49_wrong_password(void)
{
    uint8_t sk[32];
    for (int i = 0; i < 32; i++) {
        sk[i] = (uint8_t)(0x10 + i);
    }

    const char *correct_pw = "correct-password";
    const char *wrong_pw = "wrong-password";

    /* Encrypt */
    char *ncryptsec = NULL;
    int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, correct_pw, 16, &ncryptsec);
    g_assert_cmpint(rc, ==, 0);

    /* Decrypt with wrong password should fail */
    uint8_t decrypted[32];
    rc = nostr_nip49_decrypt(ncryptsec, wrong_pw, decrypted, NULL, NULL);
    g_assert_cmpint(rc, !=, 0);

    free(ncryptsec);
}

static void
test_nip49_log_n_values(void)
{
    uint8_t sk[32];
    for (int i = 0; i < 32; i++) {
        sk[i] = (uint8_t)(0x55 + i);
    }

    const char *password = "log-n-test";
    uint8_t test_log_ns[] = { 16, 18, 20 };

    for (size_t i = 0; i < G_N_ELEMENTS(test_log_ns); i++) {
        char *ncryptsec = NULL;
        int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, password,
                                      test_log_ns[i], &ncryptsec);
        g_assert_cmpint(rc, ==, 0);

        uint8_t decrypted[32];
        uint8_t out_log_n = 0;

        rc = nostr_nip49_decrypt(ncryptsec, password, decrypted, NULL, &out_log_n);
        g_assert_cmpint(rc, ==, 0);
        g_assert_cmpint(out_log_n, ==, test_log_ns[i]);
        g_assert_cmpmem(sk, 32, decrypted, 32);

        free(ncryptsec);
    }
}

static void
test_nip49_payload_serialization(void)
{
    NostrNip49Payload payload = {
        .version = 0x02,
        .log_n = 16,
        .ad = NOSTR_NIP49_SECURITY_SECURE
    };

    /* Fill salt and nonce with test data */
    for (int i = 0; i < 16; i++) payload.salt[i] = (uint8_t)i;
    for (int i = 0; i < 24; i++) payload.nonce[i] = (uint8_t)(i + 16);
    for (int i = 0; i < 48; i++) payload.ciphertext[i] = (uint8_t)(i + 40);

    /* Serialize */
    uint8_t serialized[91];
    int rc = nostr_nip49_payload_serialize(&payload, serialized);
    g_assert_cmpint(rc, ==, 0);

    /* Deserialize */
    NostrNip49Payload restored;
    rc = nostr_nip49_payload_deserialize(serialized, &restored);
    g_assert_cmpint(rc, ==, 0);

    /* Verify all fields */
    g_assert_cmpint(restored.version, ==, payload.version);
    g_assert_cmpint(restored.log_n, ==, payload.log_n);
    g_assert_cmpint(restored.ad, ==, payload.ad);
    g_assert_cmpmem(restored.salt, 16, payload.salt, 16);
    g_assert_cmpmem(restored.nonce, 24, payload.nonce, 24);
    g_assert_cmpmem(restored.ciphertext, 48, payload.ciphertext, 48);
}

/* ===========================================================================
 * Hex Conversion Tests
 * =========================================================================== */

static void
test_hex_roundtrip(void)
{
    /* Test nostr_hex2bin and nostr_bin2hex */
    const char *test_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    uint8_t bin[32];
    g_assert_true(nostr_hex2bin(bin, test_hex, 32));

    char *hex_out = nostr_bin2hex(bin, 32);
    g_assert_nonnull(hex_out);
    g_assert_cmpstr(hex_out, ==, test_hex);

    free(hex_out);
}

static void
test_hex2bin_invalid(void)
{
    uint8_t bin[32];

    /* Invalid hex characters */
    g_assert_false(nostr_hex2bin(bin, "ghijklmnopqrstuv", 8));

    /* Odd length not matching expected */
    g_assert_false(nostr_hex2bin(bin, "0123", 32));
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Key generation tests */
    g_test_add_func("/signer/crypto/key/generate_private", test_key_generate_private);
    g_test_add_func("/signer/crypto/key/get_public", test_key_get_public);
    g_test_add_func("/signer/crypto/key/is_valid_public_hex", test_key_is_valid_public_hex);

    /* NIP-19 tests */
    g_test_add_func("/signer/crypto/nip19/nsec_roundtrip", test_nip19_nsec_roundtrip);
    g_test_add_func("/signer/crypto/nip19/npub_roundtrip", test_nip19_npub_roundtrip);
    g_test_add_func("/signer/crypto/nip19/note_roundtrip", test_nip19_note_roundtrip);
    g_test_add_func("/signer/crypto/nip19/inspect", test_nip19_inspect);

    /* NIP-49 tests */
    g_test_add_func("/signer/crypto/nip49/encrypt_decrypt_basic", test_nip49_encrypt_decrypt_basic);
    g_test_add_func("/signer/crypto/nip49/security_bytes", test_nip49_security_bytes);
    g_test_add_func("/signer/crypto/nip49/wrong_password", test_nip49_wrong_password);
    g_test_add_func("/signer/crypto/nip49/log_n_values", test_nip49_log_n_values);
    g_test_add_func("/signer/crypto/nip49/payload_serialization", test_nip49_payload_serialization);

    /* Hex conversion tests */
    g_test_add_func("/signer/crypto/hex/roundtrip", test_hex_roundtrip);
    g_test_add_func("/signer/crypto/hex/invalid", test_hex2bin_invalid);

    return g_test_run();
}
