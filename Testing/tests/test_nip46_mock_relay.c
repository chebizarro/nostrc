/* Integration tests for NIP-46 remote signing protocol.
 *
 * This test verifies NIP-46 message parsing, building, and protocol handling.
 * It doesn't require a running relay server - just tests the protocol layer.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_uri.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-json.h"

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

/* Test 1: Parse bunker:// URI */
static int test_bunker_uri_parsing(void) {
    TEST_START("Parse bunker:// URI");

    /* Pubkey must be exactly 64 hex chars, relay URL must be percent-encoded */
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?relay=wss%3A%2F%2Frelay.example.com&secret=mysecret";

    NostrNip46BunkerURI parsed = {0};
    int rc = nostr_nip46_uri_parse_bunker(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    if (!parsed.remote_signer_pubkey_hex) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("pubkey is NULL");
    }

    if (parsed.n_relays == 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("no relays found");
    }

    if (!parsed.secret || strcmp(parsed.secret, "mysecret") != 0) {
        nostr_nip46_uri_bunker_free(&parsed);
        TEST_FAIL("secret mismatch");
    }

    nostr_nip46_uri_bunker_free(&parsed);
    TEST_PASS();
    return 0;
}

/* Test 2: Build NIP-46 request message */
static int test_request_build(void) {
    TEST_START("Build NIP-46 request message");

    const char *params[] = { "param1", "param2" };
    char *msg = nostr_nip46_request_build("req-123", "test_method", params, 2);
    if (!msg) TEST_FAIL("request_build returned NULL");

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

/* Test 3: Build NIP-46 response message */
static int test_response_build(void) {
    TEST_START("Build NIP-46 response message");

    char *ok_msg = nostr_nip46_response_build_ok("resp-456", "\"result_value\"");
    if (!ok_msg) TEST_FAIL("response_build_ok returned NULL");
    if (!strstr(ok_msg, "resp-456")) {
        free(ok_msg);
        TEST_FAIL("missing id in ok response");
    }
    free(ok_msg);

    char *err_msg = nostr_nip46_response_build_err("resp-789", "error message");
    if (!err_msg) TEST_FAIL("response_build_err returned NULL");
    if (!strstr(err_msg, "error")) {
        free(err_msg);
        TEST_FAIL("missing error in err response");
    }
    free(err_msg);

    TEST_PASS();
    return 0;
}

/* Test 4: Parse NIP-46 request */
static int test_request_parse(void) {
    TEST_START("Parse NIP-46 request");

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

/* Test 5: Parse NIP-46 response */
static int test_response_parse(void) {
    TEST_START("Parse NIP-46 response");

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

/* Bunker callback for testing */
static int bunker_auth_always_allow(const char *client_pubkey_hex,
                                     const char *perms_csv,
                                     void *user_data) {
    (void)client_pubkey_hex;
    (void)perms_csv;
    (void)user_data;
    return 1;  /* Always authorize */
}

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

/* Test 6: Create bunker session */
static int test_bunker_session(void) {
    TEST_START("Create bunker session");

    char *sk = nostr_key_generate_private();
    if (!sk) TEST_FAIL("keypair generation failed");

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

/* Test 7: Client session creation */
static int test_client_session(void) {
    TEST_START("Create client session");

    NostrNip46Session *client = nostr_nip46_client_new();
    if (!client) TEST_FAIL("client_new failed");

    nostr_nip46_session_free(client);
    TEST_PASS();
    return 0;
}

/* Test 8: Parse nostrconnect:// URI */
static int test_nostrconnect_uri_parsing(void) {
    TEST_START("Parse nostrconnect:// URI");

    /* Pubkey must be exactly 64 hex chars, relay URL must be percent-encoded */
    const char *uri = "nostrconnect://abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd?relay=wss%3A%2F%2Frelay.example.com&secret=test&perms=nip04_encrypt%2Csign_event";

    NostrNip46ConnectURI parsed = {0};
    int rc = nostr_nip46_uri_parse_connect(uri, &parsed);
    if (rc != 0) TEST_FAIL("URI parse failed");

    if (!parsed.client_pubkey_hex) {
        nostr_nip46_uri_connect_free(&parsed);
        TEST_FAIL("client pubkey is NULL");
    }

    if (parsed.n_relays == 0) {
        nostr_nip46_uri_connect_free(&parsed);
        TEST_FAIL("no relays found");
    }

    nostr_nip46_uri_connect_free(&parsed);
    TEST_PASS();
    return 0;
}

int main(void) {
    printf("NIP-46 Remote Signing Protocol Tests\n");
    printf("====================================\n\n");

    /* Run protocol-level tests */
    test_bunker_uri_parsing();
    test_request_build();
    test_response_build();
    test_request_parse();
    test_response_parse();
    test_bunker_session();
    test_client_session();
    test_nostrconnect_uri_parsing();

    /* Print summary */
    printf("\n====================================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(" (%d FAILED)", tests_failed);
    }
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
