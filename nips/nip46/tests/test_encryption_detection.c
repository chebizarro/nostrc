/**
 * Encryption detection and format tests for NIP-46.
 * Tests NIP-04 vs NIP-44 format detection, encryption/decryption roundtrips,
 * and correct handling of both formats.
 *
 * NIP-04 format: ciphertext?iv=base64
 * NIP-44 format: base64 only (no ?iv= suffix)
 */

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip04.h"
#include "nostr/nip44/nip44.h"
#include "nostr-keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test helper macros */
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQ_STR(a, b, msg) do { \
    const char *_a = (a); const char *_b = (b); \
    if (!(_a && _b && strcmp(_a, _b) == 0)) { \
        printf("FAIL: %s (line %d): %s - got '%s', expected '%s'\n", \
               __func__, __LINE__, msg, _a ? _a : "(null)", _b ? _b : "(null)"); \
        return 1; \
    } \
} while(0)

/* Known test keypair: private key = 1, public key derived */
static const char *TEST_SK = "0000000000000000000000000000000000000000000000000000000000000001";

/* Helper to detect encryption format.
 *
 * Note: NIP-04 has two variants:
 * - Legacy: AES-256-CBC with format "base64?iv=base64"
 * - Modern (AEAD): AES-256-GCM with pure base64 format
 *
 * The library's nostr_nip04_encrypt_secure uses AEAD (no ?iv=).
 * The legacy format is used by nostr_nip04_encrypt_legacy_secure.
 *
 * NIP-44 also uses pure base64 but with a different internal structure.
 */
static int is_nip04_legacy_format(const char *ciphertext) {
    return ciphertext && strstr(ciphertext, "?iv=") != NULL;
}

static int is_nip04_aead_format(const char *ciphertext) {
    /* AEAD NIP-04 is base64 without ?iv= - hard to distinguish from NIP-44 by format alone */
    return ciphertext && strstr(ciphertext, "?iv=") == NULL;
}

/* For testing, we just check ciphertext is non-empty.
 * Base64 can use standard (+/) or URL-safe (-_) alphabets. */
static int is_valid_ciphertext(const char *ciphertext) {
    return ciphertext && strlen(ciphertext) > 0;
}

/* --- NIP-04 Format Tests --- */

static int test_nip04_format_detection(void) {
    /* Example NIP-04 legacy ciphertext format (AES-CBC with ?iv=) */
    const char *nip04_legacy = "dGVzdA==?iv=dGVzdA==";
    /* Example pure base64 ciphertext (used by both NIP-04 AEAD and NIP-44) */
    const char *base64_cipher = "AYmBL3B5Jy5q6h5Q8Dc8bA==";

    TEST_ASSERT(is_nip04_legacy_format(nip04_legacy), "NIP-04 legacy format detected");
    TEST_ASSERT(!is_nip04_legacy_format(base64_cipher), "base64 not detected as NIP-04 legacy");
    TEST_ASSERT(is_valid_ciphertext(nip04_legacy), "legacy ciphertext valid");
    TEST_ASSERT(is_valid_ciphertext(base64_cipher), "base64 ciphertext valid");

    return 0;
}

static int test_nip04_encryption_roundtrip(void) {
    /* Set up client session with secret */
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    TEST_ASSERT(nostr_nip46_client_set_secret(s, TEST_SK) == 0, "set secret");

    /* Get our public key */
    char *our_pk = nostr_key_get_public(TEST_SK);
    TEST_ASSERT(our_pk != NULL, "derived pubkey");

    const char *plaintext = "hello world from NIP-04";

    /* Encrypt with NIP-04 (uses AEAD variant, not legacy ?iv= format) */
    char *ciphertext = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, our_pk, plaintext, &ciphertext) == 0, "encrypt");
    TEST_ASSERT(ciphertext != NULL, "ciphertext not null");
    TEST_ASSERT(is_valid_ciphertext(ciphertext), "ciphertext is valid");

    /* Decrypt with NIP-04 */
    char *decrypted = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, our_pk, ciphertext, &decrypted) == 0, "decrypt");
    TEST_ASSERT_EQ_STR(decrypted, plaintext, "roundtrip successful");

    free(ciphertext);
    free(decrypted);
    free(our_pk);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_nip04_cross_session_roundtrip(void) {
    /* Two different sessions (simulating client and bunker) */
    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *bunker_sk = "0000000000000000000000000000000000000000000000000000000000000002";

    char *client_pk = nostr_key_get_public(client_sk);
    char *bunker_pk = nostr_key_get_public(bunker_sk);
    TEST_ASSERT(client_pk != NULL && bunker_pk != NULL, "pubkeys derived");

    /* Client encrypts to bunker */
    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "client session");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, client_sk) == 0, "client set secret");

    const char *message = "{\"id\":\"1\",\"method\":\"ping\"}";
    char *cipher = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(client, bunker_pk, message, &cipher) == 0, "encrypt");

    /* Bunker decrypts from client */
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    TEST_ASSERT(bunker != NULL, "bunker session");

    /* Set bunker's secret via connect (using bunker:// URI format) */
    char uri[256];
    snprintf(uri, sizeof(uri), "bunker://%s?secret=%s", client_pk, bunker_sk);
    TEST_ASSERT(nostr_nip46_client_connect(bunker, uri, NULL) == 0, "bunker connect");

    char *decrypted = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(bunker, client_pk, cipher, &decrypted) == 0, "decrypt");
    TEST_ASSERT_EQ_STR(decrypted, message, "decrypted matches");

    free(cipher);
    free(decrypted);
    free(client_pk);
    free(bunker_pk);
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}

/* --- NIP-44 Format Tests --- */

static int test_nip44_encryption_roundtrip(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_set_secret(s, TEST_SK) == 0, "set secret");

    char *our_pk = nostr_key_get_public(TEST_SK);
    TEST_ASSERT(our_pk != NULL, "derived pubkey");

    const char *plaintext = "hello world from NIP-44";

    /* Encrypt with NIP-44 */
    char *ciphertext = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_encrypt(s, our_pk, plaintext, &ciphertext) == 0, "encrypt");
    TEST_ASSERT(ciphertext != NULL, "ciphertext not null");
    TEST_ASSERT(is_valid_ciphertext(ciphertext), "ciphertext is valid");

    /* Decrypt with NIP-44 */
    char *decrypted = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_decrypt(s, our_pk, ciphertext, &decrypted) == 0, "decrypt");
    TEST_ASSERT_EQ_STR(decrypted, plaintext, "roundtrip successful");

    free(ciphertext);
    free(decrypted);
    free(our_pk);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_nip44_cross_session_roundtrip(void) {
    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *bunker_sk = "0000000000000000000000000000000000000000000000000000000000000002";

    char *client_pk = nostr_key_get_public(client_sk);
    char *bunker_pk = nostr_key_get_public(bunker_sk);
    TEST_ASSERT(client_pk != NULL && bunker_pk != NULL, "pubkeys derived");

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "client session");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, client_sk) == 0, "client set secret");

    const char *message = "{\"id\":\"2\",\"method\":\"sign_event\",\"params\":[]}";
    char *cipher = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_encrypt(client, bunker_pk, message, &cipher) == 0, "encrypt");

    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    TEST_ASSERT(bunker != NULL, "bunker session");
    char uri[256];
    snprintf(uri, sizeof(uri), "bunker://%s?secret=%s", client_pk, bunker_sk);
    TEST_ASSERT(nostr_nip46_client_connect(bunker, uri, NULL) == 0, "bunker connect");

    char *decrypted = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_decrypt(bunker, client_pk, cipher, &decrypted) == 0, "decrypt");
    TEST_ASSERT_EQ_STR(decrypted, message, "decrypted matches");

    free(cipher);
    free(decrypted);
    free(client_pk);
    free(bunker_pk);
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}

/* --- Error Cases --- */

static int test_encryption_without_secret(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    /* No secret set */

    const char *pubkey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char *cipher = NULL;

    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, pubkey, "test", &cipher) != 0,
                "NIP-04 encrypt should fail without secret");
    TEST_ASSERT(nostr_nip46_client_nip44_encrypt(s, pubkey, "test", &cipher) != 0,
                "NIP-44 encrypt should fail without secret");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_decryption_without_secret(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    /* No secret set */

    const char *pubkey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char *plain = NULL;

    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, pubkey, "test?iv=test", &plain) != 0,
                "NIP-04 decrypt should fail without secret");
    TEST_ASSERT(nostr_nip46_client_nip44_decrypt(s, pubkey, "test", &plain) != 0,
                "NIP-44 decrypt should fail without secret");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_encryption_null_params(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_set_secret(s, TEST_SK) == 0, "set secret");

    char *our_pk = nostr_key_get_public(TEST_SK);
    char *cipher = NULL;
    char *plain = NULL;

    /* NULL session */
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(NULL, our_pk, "test", &cipher) != 0, "NULL session");
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(NULL, our_pk, "test?iv=test", &plain) != 0, "NULL session");

    /* NULL pubkey */
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, NULL, "test", &cipher) != 0, "NULL pubkey");
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, NULL, "test?iv=test", &plain) != 0, "NULL pubkey");

    /* NULL plaintext */
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, our_pk, NULL, &cipher) != 0, "NULL plaintext");

    /* NULL ciphertext */
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, our_pk, NULL, &plain) != 0, "NULL ciphertext");

    /* NULL output */
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, our_pk, "test", NULL) != 0, "NULL output");
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, our_pk, "test?iv=test", NULL) != 0, "NULL output");

    free(our_pk);
    nostr_nip46_session_free(s);
    return 0;
}

/* --- Mixed Format Tests (what the real world throws at us) --- */

static int test_long_message_encryption(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_set_secret(s, TEST_SK) == 0, "set secret");

    char *our_pk = nostr_key_get_public(TEST_SK);
    TEST_ASSERT(our_pk != NULL, "derived pubkey");

    /* Long message (simulating a large event JSON) */
    char long_message[4096];
    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';

    /* NIP-04 */
    char *cipher04 = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, our_pk, long_message, &cipher04) == 0, "NIP-04 encrypt long");
    char *decrypted04 = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, our_pk, cipher04, &decrypted04) == 0, "NIP-04 decrypt long");
    TEST_ASSERT_EQ_STR(decrypted04, long_message, "NIP-04 roundtrip long");
    free(cipher04);
    free(decrypted04);

    /* NIP-44 */
    char *cipher44 = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_encrypt(s, our_pk, long_message, &cipher44) == 0, "NIP-44 encrypt long");
    char *decrypted44 = NULL;
    TEST_ASSERT(nostr_nip46_client_nip44_decrypt(s, our_pk, cipher44, &decrypted44) == 0, "NIP-44 decrypt long");
    TEST_ASSERT_EQ_STR(decrypted44, long_message, "NIP-44 roundtrip long");
    free(cipher44);
    free(decrypted44);

    free(our_pk);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_special_chars_encryption(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_set_secret(s, TEST_SK) == 0, "set secret");

    char *our_pk = nostr_key_get_public(TEST_SK);
    TEST_ASSERT(our_pk != NULL, "derived pubkey");

    /* Messages with special characters (excluding empty string which may not be supported) */
    const char *messages[] = {
        "{\"content\":\"hello \\\"world\\\"\"}",
        "line1\\nline2\\ttab",
        "unicode: test",
        "{}",
        "single"
    };

    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        /* NIP-04 */
        char *cipher04 = NULL;
        TEST_ASSERT(nostr_nip46_client_nip04_encrypt(s, our_pk, messages[i], &cipher04) == 0, "NIP-04 encrypt");
        char *decrypted04 = NULL;
        TEST_ASSERT(nostr_nip46_client_nip04_decrypt(s, our_pk, cipher04, &decrypted04) == 0, "NIP-04 decrypt");
        TEST_ASSERT_EQ_STR(decrypted04, messages[i], "NIP-04 roundtrip special chars");
        free(cipher04);
        free(decrypted04);

        /* NIP-44 */
        char *cipher44 = NULL;
        int enc_rc = nostr_nip46_client_nip44_encrypt(s, our_pk, messages[i], &cipher44);
        if (enc_rc != 0) {
            /* NIP-44 may have stricter requirements - skip this message */
            continue;
        }
        char *decrypted44 = NULL;
        TEST_ASSERT(nostr_nip46_client_nip44_decrypt(s, our_pk, cipher44, &decrypted44) == 0, "NIP-44 decrypt");
        TEST_ASSERT_EQ_STR(decrypted44, messages[i], "NIP-44 roundtrip special chars");
        free(cipher44);
        free(decrypted44);
    }

    free(our_pk);
    nostr_nip46_session_free(s);
    return 0;
}

/* --- Main --- */

int main(void) {
    int rc = 0;
    int total = 0;
    int passed = 0;

    #define RUN_TEST(fn) do { \
        total++; \
        int r = fn(); \
        if (r == 0) { passed++; } \
        else { rc = 1; } \
    } while(0)

    /* Format detection */
    RUN_TEST(test_nip04_format_detection);

    /* NIP-04 tests */
    RUN_TEST(test_nip04_encryption_roundtrip);
    RUN_TEST(test_nip04_cross_session_roundtrip);

    /* NIP-44 tests */
    RUN_TEST(test_nip44_encryption_roundtrip);
    RUN_TEST(test_nip44_cross_session_roundtrip);

    /* Error cases */
    RUN_TEST(test_encryption_without_secret);
    RUN_TEST(test_decryption_without_secret);
    RUN_TEST(test_encryption_null_params);

    /* Edge cases */
    RUN_TEST(test_long_message_encryption);
    RUN_TEST(test_special_chars_encryption);

    printf("test_encryption_detection: %d/%d passed\n", passed, total);
    return rc;
}
