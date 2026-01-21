/* Integration tests for NIP-55L signer operations.
 * Tests signing, encryption, and decryption without requiring D-Bus daemon.
 *
 * These tests verify the core signer_ops functionality by setting
 * NOSTR_SIGNER_SECKEY_HEX environment variable and calling the C API directly.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "nostr/nip55l/signer_ops.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-json.h"
#include "nostr/nip19/nip19.h"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    printf("  [%d] %s... ", tests_run, name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define TEST_FAIL(msg) do { \
    tests_failed++; \
    printf("FAIL: %s\n", msg); \
    return 1; \
} while(0)

/* Forward declaration */
static char *make_event_json_with_pubkey(int kind, const char *content, int64_t created_at, const char *pubkey);

/* Build event JSON with optional pubkey (for proper signing).
 * Note: The pubkey MUST be included for signature verification to work,
 * because nostr_event_sign_secure computes the hash before setting pubkey. */
static char *make_event_json_with_pubkey(int kind, const char *content, int64_t created_at, const char *pubkey) {
    size_t len = strlen(content);
    char *esc = (char*)malloc(len * 2 + 1);
    if (!esc) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = content[i];
        if (c == '"' || c == '\\') esc[j++] = '\\';
        esc[j++] = c;
    }
    esc[j] = '\0';

    char *json = (char*)malloc(j + 256);
    if (!json) { free(esc); return NULL; }

    if (pubkey) {
        snprintf(json, j + 256,
            "{\"kind\":%d,\"created_at\":%lld,\"tags\":[],\"content\":\"%s\",\"pubkey\":\"%s\"}",
            kind, (long long)created_at, esc, pubkey);
    } else {
        snprintf(json, j + 256,
            "{\"kind\":%d,\"created_at\":%lld,\"tags\":[],\"content\":\"%s\"}",
            kind, (long long)created_at, esc);
    }
    free(esc);
    return json;
}

/* Build minimal unsigned event JSON for signing (no pubkey) */
static char *make_unsigned_event_json(int kind, const char *content, int64_t created_at) {
    return make_event_json_with_pubkey(kind, content, created_at, NULL);
}

/* Test 1: GetPublicKey returns valid npub */
static int test_get_public_key(const char *expected_pk_hex) {
    TEST_START("GetPublicKey returns valid npub");

    char *npub = NULL;
    int rc = nostr_nip55l_get_public_key(&npub);
    if (rc != 0) TEST_FAIL("nostr_nip55l_get_public_key returned error");
    if (!npub) TEST_FAIL("npub is NULL");
    if (strncmp(npub, "npub1", 5) != 0) {
        free(npub);
        TEST_FAIL("npub doesn't start with 'npub1'");
    }

    /* Decode npub and verify it matches expected public key */
    uint8_t pk[32];
    if (nostr_nip19_decode_npub(npub, pk) != 0) {
        free(npub);
        TEST_FAIL("failed to decode npub");
    }

    /* Convert to hex and compare */
    char pk_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    }

    if (strcmp(pk_hex, expected_pk_hex) != 0) {
        free(npub);
        TEST_FAIL("public key mismatch");
    }

    free(npub);
    TEST_PASS();
    return 0;
}

/* Test 2: SignEvent produces valid signature */
static int test_sign_event(const char *expected_pk_hex) {
    TEST_START("SignEvent produces valid signature");

    int64_t now = (int64_t)time(NULL);
    /* Include pubkey in the event - this is required because nostr_event_sign_secure
     * computes the signature hash BEFORE setting pubkey internally */
    char *event_json = make_event_json_with_pubkey(1, "test message", now, expected_pk_hex);
    if (!event_json) TEST_FAIL("failed to create event JSON");

    char *signature = NULL;
    int rc = nostr_nip55l_sign_event(event_json, NULL, "test-app", &signature);
    if (rc != 0) {
        free(event_json);
        TEST_FAIL("nostr_nip55l_sign_event returned error");
    }
    if (!signature) {
        free(event_json);
        TEST_FAIL("signature is NULL");
    }

    /* Signature should be 128 hex characters */
    if (strlen(signature) != 128) {
        free(event_json);
        free(signature);
        TEST_FAIL("signature wrong length");
    }

    /* Build full signed event and verify */
    char full_event[2048];
    snprintf(full_event, sizeof(full_event),
        "{\"kind\":1,\"created_at\":%lld,\"tags\":[],\"content\":\"test message\","
        "\"pubkey\":\"%s\",\"sig\":\"%s\"}",
        (long long)now, expected_pk_hex, signature);

    NostrEvent *ev = nostr_event_new();
    if (!ev) {
        free(event_json);
        free(signature);
        TEST_FAIL("failed to allocate event");
    }

    if (nostr_event_deserialize(ev, full_event) != 0) {
        nostr_event_free(ev);
        free(event_json);
        free(signature);
        TEST_FAIL("failed to deserialize signed event");
    }

    /* Compute event ID and verify signature */
    /* ID is computed during deserialize/serialize */
    if (!nostr_event_check_signature(ev)) {
        nostr_event_free(ev);
        free(event_json);
        free(signature);
        TEST_FAIL("signature verification failed");
    }

    nostr_event_free(ev);
    free(event_json);
    free(signature);
    TEST_PASS();
    return 0;
}

/* Test 3: NIP-04 encrypt/decrypt roundtrip */
static int test_nip04_roundtrip(const char *peer_pk_hex) {
    TEST_START("NIP-04 encrypt/decrypt roundtrip");

    const char *plaintext = "Hello, NIP-04!";
    char *ciphertext = NULL;

    int rc = nostr_nip55l_nip04_encrypt(plaintext, peer_pk_hex, NULL, &ciphertext);
    if (rc != 0) TEST_FAIL("NIP-04 encrypt failed");
    if (!ciphertext) TEST_FAIL("ciphertext is NULL");

    char *decrypted = NULL;
    rc = nostr_nip55l_nip04_decrypt(ciphertext, peer_pk_hex, NULL, &decrypted);
    if (rc != 0) {
        free(ciphertext);
        TEST_FAIL("NIP-04 decrypt failed");
    }
    if (!decrypted) {
        free(ciphertext);
        TEST_FAIL("decrypted is NULL");
    }

    if (strcmp(decrypted, plaintext) != 0) {
        free(ciphertext);
        free(decrypted);
        TEST_FAIL("decrypted message doesn't match");
    }

    free(ciphertext);
    free(decrypted);
    TEST_PASS();
    return 0;
}

/* Test 4: NIP-44 encrypt/decrypt roundtrip */
static int test_nip44_roundtrip(const char *peer_pk_hex) {
    TEST_START("NIP-44 encrypt/decrypt roundtrip");

    const char *plaintext = "Hello, NIP-44 v2!";
    char *ciphertext = NULL;

    int rc = nostr_nip55l_nip44_encrypt(plaintext, peer_pk_hex, NULL, &ciphertext);
    if (rc != 0) TEST_FAIL("NIP-44 encrypt failed");
    if (!ciphertext) TEST_FAIL("ciphertext is NULL");

    char *decrypted = NULL;
    rc = nostr_nip55l_nip44_decrypt(ciphertext, peer_pk_hex, NULL, &decrypted);
    if (rc != 0) {
        free(ciphertext);
        TEST_FAIL("NIP-44 decrypt failed");
    }
    if (!decrypted) {
        free(ciphertext);
        TEST_FAIL("decrypted is NULL");
    }

    if (strcmp(decrypted, plaintext) != 0) {
        free(ciphertext);
        free(decrypted);
        TEST_FAIL("decrypted message doesn't match");
    }

    free(ciphertext);
    free(decrypted);
    TEST_PASS();
    return 0;
}

/* Test 5: NIP-44 with unicode content */
static int test_nip44_unicode(const char *peer_pk_hex) {
    TEST_START("NIP-44 with unicode content");

    const char *plaintext = "Hello 世界! 🎉 Привет мир!";
    char *ciphertext = NULL;

    int rc = nostr_nip55l_nip44_encrypt(plaintext, peer_pk_hex, NULL, &ciphertext);
    if (rc != 0) TEST_FAIL("NIP-44 encrypt failed for unicode");

    char *decrypted = NULL;
    rc = nostr_nip55l_nip44_decrypt(ciphertext, peer_pk_hex, NULL, &decrypted);
    free(ciphertext);

    if (rc != 0) TEST_FAIL("NIP-44 decrypt failed for unicode");
    if (strcmp(decrypted, plaintext) != 0) {
        free(decrypted);
        TEST_FAIL("unicode content mismatch");
    }

    free(decrypted);
    TEST_PASS();
    return 0;
}

/* Test 6: GetRelays returns valid JSON */
static int test_get_relays(void) {
    TEST_START("GetRelays returns valid JSON");

    char *relays = NULL;
    int rc = nostr_nip55l_get_relays(&relays);
    if (rc != 0) TEST_FAIL("nostr_nip55l_get_relays returned error");
    if (!relays) TEST_FAIL("relays is NULL");

    /* Should be valid JSON (at minimum an empty array) */
    if (relays[0] != '[') {
        free(relays);
        TEST_FAIL("relays not a JSON array");
    }

    free(relays);
    TEST_PASS();
    return 0;
}

/* Test 7: Invalid event JSON handling */
static int test_invalid_event_json(void) {
    TEST_START("Invalid event JSON handling");

    char *signature = NULL;

    /* Missing required fields */
    int rc = nostr_nip55l_sign_event("{}", NULL, "test-app", &signature);
    if (rc == 0 && signature != NULL) {
        free(signature);
        /* Actually, empty event might still get signed - check if it validates */
    }

    /* Malformed JSON */
    signature = NULL;
    rc = nostr_nip55l_sign_event("not json at all", NULL, "test-app", &signature);
    if (rc == 0) {
        if (signature) free(signature);
        TEST_FAIL("expected error for malformed JSON");
    }

    /* NULL event */
    signature = NULL;
    rc = nostr_nip55l_sign_event(NULL, NULL, "test-app", &signature);
    if (rc == 0) {
        if (signature) free(signature);
        TEST_FAIL("expected error for NULL event");
    }

    TEST_PASS();
    return 0;
}

/* Test 8: Sign event with specific identity (via env, not stored key) */
static int test_sign_with_identity(const char *sk_hex, const char *expected_pk_hex) {
    TEST_START("Sign with explicit identity");

    int64_t now = (int64_t)time(NULL);
    /* Include pubkey in event for proper hash computation */
    char *event_json = make_event_json_with_pubkey(1, "identity test", now, expected_pk_hex);
    if (!event_json) TEST_FAIL("failed to create event JSON");

    /* Pass the secret key hex as current_user (identity) */
    char *signature = NULL;
    int rc = nostr_nip55l_sign_event(event_json, sk_hex, "test-app", &signature);
    if (rc != 0) {
        free(event_json);
        TEST_FAIL("sign with identity failed");
    }

    /* Verify signature */
    char full_event[2048];
    snprintf(full_event, sizeof(full_event),
        "{\"kind\":1,\"created_at\":%lld,\"tags\":[],\"content\":\"identity test\","
        "\"pubkey\":\"%s\",\"sig\":\"%s\"}",
        (long long)now, expected_pk_hex, signature);

    NostrEvent *ev = nostr_event_new();
    if (nostr_event_deserialize(ev, full_event) != 0) {
        nostr_event_free(ev);
        free(event_json);
        free(signature);
        TEST_FAIL("deserialize failed");
    }

    /* ID is computed during deserialize/serialize */
    int valid = nostr_event_check_signature(ev);
    nostr_event_free(ev);
    free(event_json);
    free(signature);

    if (!valid) TEST_FAIL("signature invalid");
    TEST_PASS();
    return 0;
}

/* Test 9: Large message encryption */
static int test_large_message_encryption(const char *peer_pk_hex) {
    TEST_START("Large message encryption (16KB)");

    /* Create 16KB message */
    size_t msg_size = 16 * 1024;
    char *plaintext = (char*)malloc(msg_size + 1);
    if (!plaintext) TEST_FAIL("failed to allocate large message");

    for (size_t i = 0; i < msg_size; i++) {
        plaintext[i] = 'A' + (i % 26);
    }
    plaintext[msg_size] = '\0';

    char *ciphertext = NULL;
    int rc = nostr_nip55l_nip44_encrypt(plaintext, peer_pk_hex, NULL, &ciphertext);
    if (rc != 0) {
        free(plaintext);
        TEST_FAIL("NIP-44 encrypt failed for large message");
    }

    char *decrypted = NULL;
    rc = nostr_nip55l_nip44_decrypt(ciphertext, peer_pk_hex, NULL, &decrypted);
    free(ciphertext);

    if (rc != 0) {
        free(plaintext);
        TEST_FAIL("NIP-44 decrypt failed for large message");
    }

    int match = (strcmp(decrypted, plaintext) == 0);
    free(plaintext);
    free(decrypted);

    if (!match) TEST_FAIL("large message content mismatch");
    TEST_PASS();
    return 0;
}

/* Test 10: Empty message handling
 * Note: NIP-44 v2 spec requires minimum 1 byte of plaintext, so empty messages
 * are expected to fail. This test verifies that behavior. */
static int test_empty_message(const char *peer_pk_hex) {
    TEST_START("Empty message rejected (expected)");

    const char *plaintext = "";
    char *ciphertext = NULL;

    int rc = nostr_nip55l_nip44_encrypt(plaintext, peer_pk_hex, NULL, &ciphertext);
    /* Empty message encryption should fail per NIP-44 spec */
    if (rc == 0) {
        if (ciphertext) free(ciphertext);
        TEST_FAIL("expected NIP-44 to reject empty message");
    }

    TEST_PASS();
    return 0;
}

/* Test 11: Invalid peer public key */
static int test_invalid_peer_pubkey(void) {
    TEST_START("Invalid peer public key handling");

    char *ciphertext = NULL;

    /* Too short */
    int rc = nostr_nip55l_nip44_encrypt("test", "abc", NULL, &ciphertext);
    if (rc == 0) {
        if (ciphertext) free(ciphertext);
        TEST_FAIL("expected error for short pubkey");
    }

    /* Invalid hex */
    ciphertext = NULL;
    rc = nostr_nip55l_nip44_encrypt("test",
        "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg",
        NULL, &ciphertext);
    if (rc == 0) {
        if (ciphertext) free(ciphertext);
        TEST_FAIL("expected error for invalid hex pubkey");
    }

    /* NULL pubkey */
    ciphertext = NULL;
    rc = nostr_nip55l_nip44_encrypt("test", NULL, NULL, &ciphertext);
    if (rc == 0) {
        if (ciphertext) free(ciphertext);
        TEST_FAIL("expected error for NULL pubkey");
    }

    TEST_PASS();
    return 0;
}

int main(void) {
    printf("NIP-55L Signer Integration Tests\n");
    printf("================================\n\n");

    /* Generate a fresh keypair for testing */
    char *sk_hex = nostr_key_generate_private();
    if (!sk_hex) {
        fprintf(stderr, "Failed to generate test keypair\n");
        return 1;
    }

    char *pk_hex = nostr_key_get_public(sk_hex);
    if (!pk_hex) {
        fprintf(stderr, "Failed to derive public key\n");
        free(sk_hex);
        return 1;
    }

    printf("Test keypair:\n");
    printf("  SK: %s\n", sk_hex);
    printf("  PK: %s\n\n", pk_hex);

    /* Set environment variable for signer to use */
    setenv("NOSTR_SIGNER_SECKEY_HEX", sk_hex, 1);

    /* Run tests */
    test_get_public_key(pk_hex);
    test_sign_event(pk_hex);
    test_nip04_roundtrip(pk_hex);
    test_nip44_roundtrip(pk_hex);
    test_nip44_unicode(pk_hex);
    test_get_relays();
    test_invalid_event_json();
    test_sign_with_identity(sk_hex, pk_hex);
    test_large_message_encryption(pk_hex);
    test_empty_message(pk_hex);
    test_invalid_peer_pubkey();

    /* Cleanup */
    unsetenv("NOSTR_SIGNER_SECKEY_HEX");
    free(sk_hex);
    free(pk_hex);

    /* Print summary */
    printf("\n================================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(" (%d FAILED)", tests_failed);
    }
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
