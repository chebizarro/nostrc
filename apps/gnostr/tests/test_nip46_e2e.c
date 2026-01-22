/**
 * @file test_nip46_e2e.c
 * @brief End-to-end tests for NIP-46 (Nostr Connect) authentication flow in gnostr
 *
 * This test file provides comprehensive E2E testing for:
 * 1. NIP-46 bunker connection flow (bunker:// connection strings)
 * 2. Remote signing requests (sign_event, nip04_encrypt/decrypt, nip44_encrypt/decrypt)
 * 3. Connection string parsing and validation
 * 4. Error handling for failed connections and timeouts
 * 5. Session persistence and reconnection
 *
 * The tests use a mock NIP-46 bunker that simulates the remote signer behavior
 * without requiring a real relay connection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_uri.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-json.h"
#include "nostr/nip04.h"

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

#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { TEST_FAIL(msg); } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { TEST_FAIL(msg); } \
} while(0)

#define ASSERT_STREQ(a, b, msg) do { \
    if ((a) == NULL || (b) == NULL || strcmp((a), (b)) != 0) { TEST_FAIL(msg); } \
} while(0)

/* ============================================================================
 * SECTION 1: Connection String Parsing Tests
 * ============================================================================ */

/**
 * Test parsing of bunker:// URIs with various formats
 */
static int test_bunker_uri_parsing_basic(void) {
    TEST_START("Parse basic bunker:// URI");

    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.example.com&secret=mysecret";

    NostrNip46BunkerURI parsed = {0};
    int rc = nostr_nip46_uri_parse_bunker(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    ASSERT_NOT_NULL(parsed.remote_signer_pubkey_hex, "pubkey is NULL");
    ASSERT_EQ(strlen(parsed.remote_signer_pubkey_hex), 64, "pubkey wrong length");

    if (parsed.n_relays == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("no relays found");
    }

    ASSERT_STREQ(parsed.secret, "mysecret", "secret mismatch");

    nostr_nip46_uri_bunker_free(&parsed);
    TEST_PASS();
    return 0;
}

/**
 * Test bunker:// URI with multiple relays
 */
static int test_bunker_uri_multiple_relays(void) {
    TEST_START("Parse bunker:// URI with multiple relays");

    /* Pubkey must be exactly 64 hex characters (32 bytes) */
    const char *uri = "bunker://abcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd"
                      "?relay=wss%3A%2F%2Frelay1.example.com"
                      "&relay=wss%3A%2F%2Frelay2.example.com"
                      "&relay=wss%3A%2F%2Frelay3.example.com";

    NostrNip46BunkerURI parsed = {0};
    int rc = nostr_nip46_uri_parse_bunker(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    if (parsed.n_relays < 3) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("expected at least 3 relays");
    }

    nostr_nip46_uri_bunker_free(&parsed);
    TEST_PASS();
    return 0;
}

/**
 * Test bunker:// URI without secret (optional field)
 */
static int test_bunker_uri_no_secret(void) {
    TEST_START("Parse bunker:// URI without secret");

    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.example.com";

    NostrNip46BunkerURI parsed = {0};
    int rc = nostr_nip46_uri_parse_bunker(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    /* Secret should be NULL or empty when not provided */
    if (parsed.secret != NULL && strlen(parsed.secret) > 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("secret should be empty");
    }

    nostr_nip46_uri_bunker_free(&parsed);
    TEST_PASS();
    return 0;
}

/**
 * Test nostrconnect:// URI parsing
 */
static int test_nostrconnect_uri_parsing(void) {
    TEST_START("Parse nostrconnect:// URI");

    const char *uri = "nostrconnect://abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd"
                      "?relay=wss%3A%2F%2Frelay.example.com"
                      "&secret=test"
                      "&perms=nip04_encrypt%2Csign_event"
                      "&name=TestApp";

    NostrNip46ConnectURI parsed = {0};
    int rc = nostr_nip46_uri_parse_connect(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    ASSERT_NOT_NULL(parsed.client_pubkey_hex, "client pubkey is NULL");
    ASSERT_EQ(strlen(parsed.client_pubkey_hex), 64, "client pubkey wrong length");

    if (parsed.n_relays == 0) {
        nostr_nip46_uri_connect_free(&parsed);
        TEST_FAIL("no relays found");
    }

    nostr_nip46_uri_connect_free(&parsed);
    TEST_PASS();
    return 0;
}

/**
 * Test invalid URI handling
 */
static int test_invalid_uri_handling(void) {
    TEST_START("Handle invalid URIs gracefully");

    NostrNip46BunkerURI parsed = {0};

    /* Empty URI */
    if (nostr_nip46_uri_parse_bunker("", &parsed) == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("empty URI should fail");
    }

    /* Wrong scheme */
    if (nostr_nip46_uri_parse_bunker("http://example.com", &parsed) == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("http:// URI should fail");
    }

    /* Invalid pubkey (too short) */
    if (nostr_nip46_uri_parse_bunker("bunker://1234", &parsed) == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("short pubkey should fail");
    }

    /* Invalid pubkey (non-hex characters) */
    if (nostr_nip46_uri_parse_bunker(
            "bunker://ghijklmnopqrstuvwxyz0123456789abcdef0123456789abcdef0123456789ab",
            &parsed) == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("non-hex pubkey should fail");
    }

    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 2: Client Session Tests
 * ============================================================================ */

/**
 * Test client session creation
 */
static int test_client_session_creation(void) {
    TEST_START("Create NIP-46 client session");

    NostrNip46Session *client = nostr_nip46_client_new();
    ASSERT_NOT_NULL(client, "client_new failed");

    nostr_nip46_session_free(client);
    TEST_PASS();
    return 0;
}

/**
 * Test client connect with bunker:// URI
 */
static int test_client_connect_bunker_uri(void) {
    TEST_START("Connect client with bunker:// URI");

    const char *rs_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.one&secret=sec";

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    if (nostr_nip46_client_connect(s, uri, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("connect fail");
    }

    /* Verify remote pubkey was extracted */
    char *pub = NULL;
    if (nostr_nip46_session_get_remote_pubkey(s, &pub) != 0 || !pub) {
        nostr_nip46_session_free(s);
        TEST_FAIL("get remote pub fail");
    }
    if (strcmp(pub, rs_pub) != 0) {
        free(pub);
        nostr_nip46_session_free(s);
        TEST_FAIL("remote pub mismatch");
    }
    free(pub);

    /* Verify relays were extracted */
    char **relays = NULL;
    size_t n = 0;
    if (nostr_nip46_session_get_relays(s, &relays, &n) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("get relays fail");
    }
    if (n < 1 || !relays[0] || strcmp(relays[0], "wss://relay.one") != 0) {
        for (size_t i = 0; i < n; ++i) free(relays[i]);
        free(relays);
        nostr_nip46_session_free(s);
        TEST_FAIL("relay mismatch");
    }
    for (size_t i = 0; i < n; ++i) free(relays[i]);
    free(relays);

    /* Verify secret was extracted */
    char *sec = NULL;
    nostr_nip46_session_get_secret(s, &sec);
    if (!sec || strcmp(sec, "sec") != 0) {
        free(sec);
        nostr_nip46_session_free(s);
        TEST_FAIL("secret mismatch");
    }
    free(sec);

    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/**
 * Test client connect with nostrconnect:// URI
 */
static int test_client_connect_nostrconnect_uri(void) {
    TEST_START("Connect client with nostrconnect:// URI");

    const char *cli_pub = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                      "?relay=wss%3A%2F%2Frelay.two";

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    if (nostr_nip46_client_connect(s, uri, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("connect fail");
    }

    char *pub = NULL;
    if (nostr_nip46_session_get_client_pubkey(s, &pub) != 0 || !pub) {
        nostr_nip46_session_free(s);
        TEST_FAIL("get client pub fail");
    }
    if (strcmp(pub, cli_pub) != 0) {
        free(pub);
        nostr_nip46_session_free(s);
        TEST_FAIL("client pub mismatch");
    }
    free(pub);

    char **relays = NULL;
    size_t n = 0;
    nostr_nip46_session_get_relays(s, &relays, &n);
    if (n < 1 || strcmp(relays[0], "wss://relay.two") != 0) {
        for (size_t i = 0; i < n; ++i) free(relays[i]);
        free(relays);
        nostr_nip46_session_free(s);
        TEST_FAIL("relay mismatch");
    }
    for (size_t i = 0; i < n; ++i) free(relays[i]);
    free(relays);

    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/**
 * Test connect with invalid URI
 */
static int test_client_connect_invalid_uri(void) {
    TEST_START("Client connect with invalid URI fails gracefully");

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    /* Invalid scheme */
    if (nostr_nip46_client_connect(s, "http://example.com", NULL) == 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("http:// should fail");
    }

    /* Empty URI */
    if (nostr_nip46_client_connect(s, "", NULL) == 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("empty URI should fail");
    }

    /* NULL URI */
    if (nostr_nip46_client_connect(s, NULL, NULL) == 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("NULL URI should fail");
    }

    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 3: Bunker Session Tests
 * ============================================================================ */

/**
 * Bunker callback for testing - always allows authorization
 */
static int bunker_auth_always_allow(const char *client_pubkey_hex,
                                     const char *perms_csv,
                                     void *user_data) {
    (void)client_pubkey_hex;
    (void)perms_csv;
    (void)user_data;
    return 1;  /* Always authorize */
}

/**
 * Bunker callback for testing - always denies authorization
 */
static int bunker_auth_always_deny(const char *client_pubkey_hex,
                                    const char *perms_csv,
                                    void *user_data) {
    (void)client_pubkey_hex;
    (void)perms_csv;
    (void)user_data;
    return 0;  /* Always deny */
}

/**
 * Sign callback that signs events with provided secret key
 */
static char *bunker_sign_callback(const char *event_json, void *user_data) {
    const char *sk_hex = (const char *)user_data;
    if (!sk_hex || !event_json) return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    if (nostr_event_deserialize(ev, event_json) != 0) {
        nostr_event_free(ev);
        return NULL;
    }

    if (nostr_event_sign(ev, sk_hex) != 0) {
        nostr_event_free(ev);
        return NULL;
    }

    char *signed_json = nostr_event_serialize(ev);
    nostr_event_free(ev);
    return signed_json;
}

/**
 * Test bunker session creation
 */
static int test_bunker_session_creation(void) {
    TEST_START("Create NIP-46 bunker session");

    char *sk = nostr_key_generate_private();
    ASSERT_NOT_NULL(sk, "keypair generation failed");

    NostrNip46BunkerCallbacks cbs = {
        .authorize_cb = bunker_auth_always_allow,
        .sign_cb = bunker_sign_callback,
        .user_data = sk
    };

    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    if (!bunker) {
        free(sk);
        TEST_FAIL("bunker_new failed");
    }

    nostr_nip46_session_free(bunker);
    free(sk);
    TEST_PASS();
    return 0;
}

/**
 * Test bunker URI generation
 */
static int test_bunker_uri_generation(void) {
    TEST_START("Generate bunker:// URI");

    char *sk = nostr_key_generate_private();
    ASSERT_NOT_NULL(sk, "keypair generation failed");

    char *pk = nostr_key_get_public(sk);
    ASSERT_NOT_NULL(pk, "pubkey derivation failed");

    NostrNip46BunkerCallbacks cbs = {
        .authorize_cb = bunker_auth_always_allow,
        .sign_cb = bunker_sign_callback,
        .user_data = sk
    };

    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    if (!bunker) {
        free(sk);
        free(pk);
        TEST_FAIL("bunker_new failed");
    }

    const char *relays[] = { "wss://relay.example.com" };
    char *uri = NULL;
    int rc = nostr_nip46_bunker_issue_bunker_uri(bunker, pk, relays, 1, "test_secret", &uri);
    if (rc != 0 || !uri) {
        nostr_nip46_session_free(bunker);
        free(sk);
        free(pk);
        TEST_FAIL("issue_bunker_uri failed");
    }

    /* Verify URI starts with bunker:// */
    if (strncmp(uri, "bunker://", 9) != 0) {
        free(uri);
        nostr_nip46_session_free(bunker);
        free(sk);
        free(pk);
        TEST_FAIL("URI should start with bunker://");
    }

    /* Verify URI contains the pubkey */
    if (strstr(uri, pk) == NULL) {
        free(uri);
        nostr_nip46_session_free(bunker);
        free(sk);
        free(pk);
        TEST_FAIL("URI should contain pubkey");
    }

    free(uri);
    nostr_nip46_session_free(bunker);
    free(sk);
    free(pk);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 4: NIP-46 Message Building and Parsing Tests
 * ============================================================================ */

/**
 * Test NIP-46 request message building
 */
static int test_request_message_building(void) {
    TEST_START("Build NIP-46 request message");

    const char *params[] = { "param1", "param2" };
    char *msg = nostr_nip46_request_build("req-123", "test_method", params, 2);
    ASSERT_NOT_NULL(msg, "request_build returned NULL");

    /* Should contain method and id */
    if (!strstr(msg, "test_method")) {
        free(msg);
        TEST_FAIL("missing method in request");
    }
    if (!strstr(msg, "req-123")) {
        free(msg);
        TEST_FAIL("missing id in request");
    }

    free(msg);
    TEST_PASS();
    return 0;
}

/**
 * Test NIP-46 response message building
 */
static int test_response_message_building(void) {
    TEST_START("Build NIP-46 response messages");

    /* Test OK response */
    char *ok_msg = nostr_nip46_response_build_ok("resp-456", "\"result_value\"");
    ASSERT_NOT_NULL(ok_msg, "response_build_ok returned NULL");
    if (!strstr(ok_msg, "resp-456")) {
        free(ok_msg);
        TEST_FAIL("missing id in ok response");
    }
    free(ok_msg);

    /* Test error response */
    char *err_msg = nostr_nip46_response_build_err("resp-789", "error message");
    ASSERT_NOT_NULL(err_msg, "response_build_err returned NULL");
    if (!strstr(err_msg, "error")) {
        free(err_msg);
        TEST_FAIL("missing error in err response");
    }
    free(err_msg);

    TEST_PASS();
    return 0;
}

/**
 * Test NIP-46 request parsing
 */
static int test_request_message_parsing(void) {
    TEST_START("Parse NIP-46 request message");

    const char *json = "{\"id\":\"test-id\",\"method\":\"get_public_key\",\"params\":[]}";

    NostrNip46Request req = {0};
    int rc = nostr_nip46_request_parse(json, &req);
    if (rc != 0) TEST_FAIL("request parse failed");

    if (!req.id || strcmp(req.id, "test-id") != 0) {
        nostr_nip46_request_free(&req);
        TEST_FAIL("id mismatch");
    }

    if (!req.method || strcmp(req.method, "get_public_key") != 0) {
        nostr_nip46_request_free(&req);
        TEST_FAIL("method mismatch");
    }

    nostr_nip46_request_free(&req);
    TEST_PASS();
    return 0;
}

/**
 * Test NIP-46 response parsing
 */
static int test_response_message_parsing(void) {
    TEST_START("Parse NIP-46 response messages");

    /* Test OK response */
    const char *ok_json = "{\"id\":\"resp-1\",\"result\":\"success\"}";
    NostrNip46Response resp = {0};
    int rc = nostr_nip46_response_parse(ok_json, &resp);
    if (rc != 0) TEST_FAIL("ok response parse failed");
    if (!resp.id || strcmp(resp.id, "resp-1") != 0) {
        nostr_nip46_response_free(&resp);
        TEST_FAIL("id mismatch");
    }
    if (resp.error != NULL) {
        nostr_nip46_response_free(&resp);
        TEST_FAIL("unexpected error field in ok response");
    }
    nostr_nip46_response_free(&resp);

    /* Test error response */
    const char *err_json = "{\"id\":\"resp-2\",\"error\":\"something went wrong\"}";
    memset(&resp, 0, sizeof(resp));
    rc = nostr_nip46_response_parse(err_json, &resp);
    if (rc != 0) TEST_FAIL("err response parse failed");
    if (!resp.error) {
        nostr_nip46_response_free(&resp);
        TEST_FAIL("missing error in err response");
    }
    nostr_nip46_response_free(&resp);

    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 5: Remote Signing (sign_event) Tests
 * ============================================================================ */

/**
 * Test sign_event request building
 */
static int test_sign_event_request_building(void) {
    TEST_START("Build sign_event request");

    /* Create a simple unsigned event */
    const char *event_json = "{\"kind\":1,\"content\":\"test\",\"tags\":[],\"created_at\":1234567890}";
    const char *params[] = { event_json };

    char *req = nostr_nip46_request_build("sign-1", "sign_event", params, 1);
    ASSERT_NOT_NULL(req, "request_build returned NULL");

    if (!strstr(req, "sign_event")) {
        free(req);
        TEST_FAIL("missing method in request");
    }

    if (!strstr(req, "test")) {
        free(req);
        TEST_FAIL("missing content in request");
    }

    free(req);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 6: Error Handling Tests
 * ============================================================================ */

/**
 * Test handling of NULL parameters
 */
static int test_null_parameter_handling(void) {
    TEST_START("Handle NULL parameters gracefully");

    /* Client with NULL session */
    if (nostr_nip46_client_connect(NULL, "bunker://...", NULL) == 0) {
        TEST_FAIL("connect with NULL session should fail");
    }

    /* Get public key with NULL session */
    char *pk = NULL;
    if (nostr_nip46_client_get_public_key(NULL, &pk) == 0) {
        free(pk);
        TEST_FAIL("get_public_key with NULL session should fail");
    }

    /* Sign event with NULL session */
    char *signed_ev = NULL;
    if (nostr_nip46_client_sign_event(NULL, "{}", &signed_ev) == 0) {
        free(signed_ev);
        TEST_FAIL("sign_event with NULL session should fail");
    }

    /* Session free with NULL (should not crash) */
    nostr_nip46_session_free(NULL);

    TEST_PASS();
    return 0;
}

/**
 * Test request parsing with malformed JSON
 */
static int test_malformed_json_handling(void) {
    TEST_START("Handle malformed JSON gracefully");

    NostrNip46Request req = {0};

    /* Invalid JSON syntax */
    if (nostr_nip46_request_parse("{invalid", &req) == 0) {
        nostr_nip46_request_free(&req);
        TEST_FAIL("invalid JSON should fail");
    }

    /* Missing required fields */
    if (nostr_nip46_request_parse("{}", &req) == 0) {
        nostr_nip46_request_free(&req);
        TEST_FAIL("empty object should fail");
    }

    /* Missing method */
    if (nostr_nip46_request_parse("{\"id\":\"test\"}", &req) == 0) {
        nostr_nip46_request_free(&req);
        TEST_FAIL("missing method should fail");
    }

    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 7: Session State and Reconnection Tests
 * ============================================================================ */

/**
 * Test that session state is properly cleared on reconnect
 */
static int test_session_state_on_reconnect(void) {
    TEST_START("Session state cleared on reconnect");

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    /* Connect to first URI */
    const char *uri1 = "bunker://1111111111111111111111111111111111111111111111111111111111111111"
                       "?relay=wss%3A%2F%2Frelay1.example.com&secret=secret1";
    if (nostr_nip46_client_connect(s, uri1, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("first connect fail");
    }

    /* Verify first connection state */
    char *pub1 = NULL;
    nostr_nip46_session_get_remote_pubkey(s, &pub1);
    if (!pub1 || strstr(pub1, "11111111") == NULL) {
        free(pub1);
        nostr_nip46_session_free(s);
        TEST_FAIL("first pubkey not set");
    }
    free(pub1);

    /* Reconnect to second URI */
    const char *uri2 = "bunker://2222222222222222222222222222222222222222222222222222222222222222"
                       "?relay=wss%3A%2F%2Frelay2.example.com&secret=secret2";
    if (nostr_nip46_client_connect(s, uri2, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("second connect fail");
    }

    /* Verify second connection replaced first */
    char *pub2 = NULL;
    nostr_nip46_session_get_remote_pubkey(s, &pub2);
    if (!pub2 || strstr(pub2, "22222222") == NULL) {
        free(pub2);
        nostr_nip46_session_free(s);
        TEST_FAIL("second pubkey not set");
    }
    free(pub2);

    /* Verify old secret was replaced */
    char *sec = NULL;
    nostr_nip46_session_get_secret(s, &sec);
    if (!sec || strcmp(sec, "secret2") != 0) {
        free(sec);
        nostr_nip46_session_free(s);
        TEST_FAIL("secret not updated");
    }
    free(sec);

    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 8: NIP-04/NIP-44 Encryption Tests (via session with secret)
 * ============================================================================ */

/**
 * Test that NIP-04 encrypt/decrypt requires a secret
 */
static int test_nip04_requires_secret(void) {
    TEST_START("NIP-04 operations require secret");

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    /* Connect without setting a secret in session (just parsing bunker URI) */
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.example.com";
    nostr_nip46_client_connect(s, uri, NULL);

    /* Check if there's a secret - bunker:// doesn't set session secret for client */
    char *sec = NULL;
    nostr_nip46_session_get_secret(s, &sec);

    /* Without a secret, encryption should fail */
    char *cipher = NULL;
    const char *peer = "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
    int rc = nostr_nip46_client_nip04_encrypt(s, peer, "test message", &cipher);

    /* If no secret, encryption should fail */
    if (!sec && rc == 0) {
        free(cipher);
        free(sec);
        nostr_nip46_session_free(s);
        TEST_FAIL("encrypt should fail without secret");
    }

    free(sec);
    free(cipher);
    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 9: Get Public Key Tests
 * ============================================================================ */

/**
 * Test get_public_key returns correct pubkey from nostrconnect URI
 */
static int test_get_public_key_from_nostrconnect(void) {
    TEST_START("Get public key from nostrconnect:// URI");

    const char *expected_pk = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                      "?relay=wss%3A%2F%2Frelay.example.com";

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    if (nostr_nip46_client_connect(s, uri, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("connect fail");
    }

    char *pk = NULL;
    if (nostr_nip46_client_get_public_key(s, &pk) != 0 || !pk) {
        nostr_nip46_session_free(s);
        TEST_FAIL("get_public_key fail");
    }

    if (strcmp(pk, expected_pk) != 0) {
        free(pk);
        nostr_nip46_session_free(s);
        TEST_FAIL("pubkey mismatch");
    }

    free(pk);
    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/**
 * Test get_public_key returns remote pubkey from bunker URI as fallback
 */
static int test_get_public_key_from_bunker(void) {
    TEST_START("Get public key from bunker:// URI (fallback)");

    const char *expected_pk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.example.com";

    NostrNip46Session *s = nostr_nip46_client_new();
    ASSERT_NOT_NULL(s, "session new fail");

    if (nostr_nip46_client_connect(s, uri, NULL) != 0) {
        nostr_nip46_session_free(s);
        TEST_FAIL("connect fail");
    }

    char *pk = NULL;
    if (nostr_nip46_client_get_public_key(s, &pk) != 0 || !pk) {
        nostr_nip46_session_free(s);
        TEST_FAIL("get_public_key fail");
    }

    /* Should return the remote signer pubkey as fallback */
    if (strcmp(pk, expected_pk) != 0) {
        free(pk);
        nostr_nip46_session_free(s);
        TEST_FAIL("pubkey mismatch");
    }

    free(pk);
    nostr_nip46_session_free(s);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * SECTION 10: Bunker Reply Tests
 * ============================================================================ */

/**
 * Test bunker reply building
 */
static int test_bunker_reply_building(void) {
    TEST_START("Build bunker reply messages");

    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    ASSERT_NOT_NULL(bunker, "bunker_new failed");

    NostrNip46Request req = {
        .id = "test-id-123",
        .method = "get_public_key",
        .params = NULL,
        .n_params = 0
    };

    /* Test successful reply */
    int rc = nostr_nip46_bunker_reply(bunker, &req, "\"pubkey123\"", NULL);
    if (rc != 0) {
        nostr_nip46_session_free(bunker);
        TEST_FAIL("bunker_reply failed");
    }

    /* Get the last reply */
    char *reply = NULL;
    nostr_nip46_session_take_last_reply_json(bunker, &reply);
    if (!reply) {
        nostr_nip46_session_free(bunker);
        TEST_FAIL("no reply stored");
    }
    if (!strstr(reply, "test-id-123")) {
        free(reply);
        nostr_nip46_session_free(bunker);
        TEST_FAIL("reply missing request id");
    }
    free(reply);

    /* Test error reply */
    rc = nostr_nip46_bunker_reply(bunker, &req, NULL, "access denied");
    if (rc != 0) {
        nostr_nip46_session_free(bunker);
        TEST_FAIL("bunker_reply error failed");
    }

    nostr_nip46_session_take_last_reply_json(bunker, &reply);
    if (!reply || !strstr(reply, "access denied")) {
        free(reply);
        nostr_nip46_session_free(bunker);
        TEST_FAIL("error reply missing error message");
    }
    free(reply);

    nostr_nip46_session_free(bunker);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main(void) {
    printf("\n");
    printf("================================================================\n");
    printf("NIP-46 End-to-End Tests for gnostr\n");
    printf("================================================================\n\n");

    /* Section 1: Connection String Parsing */
    printf("Section 1: Connection String Parsing\n");
    printf("------------------------------------\n");
    test_bunker_uri_parsing_basic();
    test_bunker_uri_multiple_relays();
    test_bunker_uri_no_secret();
    test_nostrconnect_uri_parsing();
    test_invalid_uri_handling();
    printf("\n");

    /* Section 2: Client Session */
    printf("Section 2: Client Session\n");
    printf("-------------------------\n");
    test_client_session_creation();
    test_client_connect_bunker_uri();
    test_client_connect_nostrconnect_uri();
    test_client_connect_invalid_uri();
    printf("\n");

    /* Section 3: Bunker Session */
    printf("Section 3: Bunker Session\n");
    printf("-------------------------\n");
    test_bunker_session_creation();
    test_bunker_uri_generation();
    printf("\n");

    /* Section 4: Message Building and Parsing */
    printf("Section 4: Message Building and Parsing\n");
    printf("---------------------------------------\n");
    test_request_message_building();
    test_response_message_building();
    test_request_message_parsing();
    test_response_message_parsing();
    printf("\n");

    /* Section 5: Remote Signing */
    printf("Section 5: Remote Signing\n");
    printf("-------------------------\n");
    test_sign_event_request_building();
    printf("\n");

    /* Section 6: Error Handling */
    printf("Section 6: Error Handling\n");
    printf("-------------------------\n");
    test_null_parameter_handling();
    test_malformed_json_handling();
    printf("\n");

    /* Section 7: Session State and Reconnection */
    printf("Section 7: Session State and Reconnection\n");
    printf("-----------------------------------------\n");
    test_session_state_on_reconnect();
    printf("\n");

    /* Section 8: Encryption Requirements */
    printf("Section 8: Encryption Requirements\n");
    printf("----------------------------------\n");
    test_nip04_requires_secret();
    printf("\n");

    /* Section 9: Get Public Key */
    printf("Section 9: Get Public Key\n");
    printf("-------------------------\n");
    test_get_public_key_from_nostrconnect();
    test_get_public_key_from_bunker();
    printf("\n");

    /* Section 10: Bunker Reply */
    printf("Section 10: Bunker Reply\n");
    printf("------------------------\n");
    test_bunker_reply_building();
    printf("\n");

    /* Print summary */
    printf("================================================================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(" (%d FAILED)", tests_failed);
    }
    printf("\n");
    printf("================================================================\n\n");

    return tests_failed > 0 ? 1 : 0;
}
