/**
 * Mock RPC flow tests for NIP-46.
 * Tests the complete request/response flow without actual relay connections.
 * Simulates signer responses with various scenarios.
 *
 * This tests the LOCAL bunker-side handling, not actual network RPC.
 * For full relay-based tests, you'd need a mock relay server.
 */

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip04.h"
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

/* Test keys (secp256k1 sk=1 and sk=2) */
static const char *CLIENT_SK = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *BUNKER_SK = "0000000000000000000000000000000000000000000000000000000000000002";

/* --- Helper: simulate full request/response cycle locally --- */

typedef struct {
    NostrNip46Session *client;
    NostrNip46Session *bunker;
    char *client_pk;
    char *bunker_pk;
} MockContext;

static int mock_context_init(MockContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->client_pk = nostr_key_get_public(CLIENT_SK);
    ctx->bunker_pk = nostr_key_get_public(BUNKER_SK);
    if (!ctx->client_pk || !ctx->bunker_pk) return -1;

    /* Create client session */
    ctx->client = nostr_nip46_client_new();
    if (!ctx->client) return -1;
    char client_uri[256];
    snprintf(client_uri, sizeof(client_uri), "bunker://%s?secret=%s", ctx->bunker_pk, CLIENT_SK);
    if (nostr_nip46_client_connect(ctx->client, client_uri, NULL) != 0) return -1;

    /* Create bunker session */
    ctx->bunker = nostr_nip46_bunker_new(NULL);
    if (!ctx->bunker) return -1;
    char bunker_uri[256];
    snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", ctx->client_pk, BUNKER_SK);
    if (nostr_nip46_client_connect(ctx->bunker, bunker_uri, NULL) != 0) return -1;

    return 0;
}

static void mock_context_cleanup(MockContext *ctx) {
    if (ctx->client) nostr_nip46_session_free(ctx->client);
    if (ctx->bunker) nostr_nip46_session_free(ctx->bunker);
    free(ctx->client_pk);
    free(ctx->bunker_pk);
}

/* Simulates: client encrypts request -> bunker decrypts/handles/encrypts response -> client decrypts */
static int mock_rpc_call(MockContext *ctx, const char *request_json, char **out_response_json) {
    *out_response_json = NULL;

    /* Client encrypts request to bunker */
    char *cipher_req = NULL;
    if (nostr_nip46_client_nip04_encrypt(ctx->client, ctx->bunker_pk, request_json, &cipher_req) != 0) {
        return -1;
    }

    /* Bunker handles and produces encrypted response */
    char *cipher_resp = NULL;
    if (nostr_nip46_bunker_handle_cipher(ctx->bunker, ctx->client_pk, cipher_req, &cipher_resp) != 0) {
        free(cipher_req);
        return -1;
    }
    free(cipher_req);

    /* Client decrypts response */
    char *plain_resp = NULL;
    if (nostr_nip46_client_nip04_decrypt(ctx->client, ctx->bunker_pk, cipher_resp, &plain_resp) != 0) {
        free(cipher_resp);
        return -1;
    }
    free(cipher_resp);

    *out_response_json = plain_resp;
    return 0;
}

/* --- Test: get_public_key RPC --- */

static int test_rpc_get_public_key(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    /* Build get_public_key request */
    char *req_json = nostr_nip46_request_build("gpk-1", "get_public_key", NULL, 0);
    TEST_ASSERT(req_json != NULL, "build request");

    /* Execute mock RPC */
    char *resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, req_json, &resp_json) == 0, "mock RPC");
    free(req_json);

    /* Parse response */
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(resp_json, &resp) == 0, "parse response");
    free(resp_json);

    /* Verify response */
    TEST_ASSERT_EQ_STR(resp.id, "gpk-1", "response id matches request");
    TEST_ASSERT(resp.error == NULL, "no error");
    TEST_ASSERT(resp.result != NULL, "has result");

    /* Result should be the bunker's pubkey */
    char *expected_pk = nostr_key_get_public(BUNKER_SK);
    TEST_ASSERT_EQ_STR(resp.result, expected_pk, "result is bunker pubkey");
    free(expected_pk);

    nostr_nip46_response_free(&resp);
    mock_context_cleanup(&ctx);
    return 0;
}

/* --- Test: connect RPC with permissions --- */

static int test_rpc_connect(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    /* Build connect request with permissions */
    const char *params[] = {ctx.client_pk, "sign_event,nip04_encrypt"};
    char *req_json = nostr_nip46_request_build("conn-1", "connect", params, 2);
    TEST_ASSERT(req_json != NULL, "build request");

    char *resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, req_json, &resp_json) == 0, "mock RPC");
    free(req_json);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(resp_json, &resp) == 0, "parse response");
    free(resp_json);

    TEST_ASSERT_EQ_STR(resp.id, "conn-1", "response id matches");
    TEST_ASSERT(resp.error == NULL, "no error");
    TEST_ASSERT(resp.result != NULL, "has result");
    /* Result should be "ack" */
    TEST_ASSERT(strstr(resp.result, "ack") != NULL, "result contains ack");

    nostr_nip46_response_free(&resp);
    mock_context_cleanup(&ctx);
    return 0;
}

/* --- Test: sign_event RPC with ACL enforcement --- */

static int test_rpc_sign_event_after_connect(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    /* First: connect with sign_event permission */
    const char *conn_params[] = {ctx.client_pk, "sign_event"};
    char *conn_req = nostr_nip46_request_build("c1", "connect", conn_params, 2);
    char *conn_resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, conn_req, &conn_resp_json) == 0, "connect RPC");
    free(conn_req);
    free(conn_resp_json);

    /* Then: sign_event request */
    const char *event_json = "{\"kind\":1,\"content\":\"test\",\"tags\":[]}";
    const char *sign_params[] = {event_json};
    char *sign_req = nostr_nip46_request_build("s1", "sign_event", sign_params, 1);
    TEST_ASSERT(sign_req != NULL, "build sign request");

    char *sign_resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, sign_req, &sign_resp_json) == 0, "sign RPC");
    free(sign_req);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(sign_resp_json, &resp) == 0, "parse response");
    free(sign_resp_json);

    TEST_ASSERT_EQ_STR(resp.id, "s1", "response id matches");
    TEST_ASSERT(resp.error == NULL, "no error");
    TEST_ASSERT(resp.result != NULL, "has result");
    /* Result should be a signed event JSON with signature */
    TEST_ASSERT(strstr(resp.result, "\"sig\":") != NULL, "result has signature");
    TEST_ASSERT(strstr(resp.result, "\"pubkey\":") != NULL, "result has pubkey");

    nostr_nip46_response_free(&resp);
    mock_context_cleanup(&ctx);
    return 0;
}

static int test_rpc_sign_event_denied_without_permission(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    /* Connect WITHOUT sign_event permission.
     * NIP-46 connect params: [remote_signer_pubkey, secret, permissions] */
    const char *conn_params[] = {ctx.client_pk, "", "nip04_encrypt"};  /* No sign_event */
    char *conn_req = nostr_nip46_request_build("c1", "connect", conn_params, 3);
    char *conn_resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, conn_req, &conn_resp_json) == 0, "connect RPC");
    free(conn_req);
    free(conn_resp_json);

    /* Try sign_event - should be forbidden */
    const char *event_json = "{\"kind\":1,\"content\":\"test\"}";
    const char *sign_params[] = {event_json};
    char *sign_req = nostr_nip46_request_build("s1", "sign_event", sign_params, 1);
    char *sign_resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, sign_req, &sign_resp_json) == 0, "sign RPC (should get error response)");
    free(sign_req);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(sign_resp_json, &resp) == 0, "parse response");
    free(sign_resp_json);

    TEST_ASSERT_EQ_STR(resp.id, "s1", "response id matches");
    TEST_ASSERT(resp.error != NULL, "has error");
    TEST_ASSERT(strstr(resp.error, "forbidden") != NULL, "error is forbidden");

    nostr_nip46_response_free(&resp);
    mock_context_cleanup(&ctx);
    return 0;
}

/* --- Test: unknown method --- */

static int test_rpc_unknown_method(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    char *req_json = nostr_nip46_request_build("u1", "unknown_method", NULL, 0);
    TEST_ASSERT(req_json != NULL, "build request");

    char *resp_json = NULL;
    TEST_ASSERT(mock_rpc_call(&ctx, req_json, &resp_json) == 0, "mock RPC");
    free(req_json);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(resp_json, &resp) == 0, "parse response");
    free(resp_json);

    TEST_ASSERT_EQ_STR(resp.id, "u1", "response id matches");
    TEST_ASSERT(resp.error != NULL, "has error");
    TEST_ASSERT(strstr(resp.error, "not_supported") != NULL, "error mentions not supported");

    nostr_nip46_response_free(&resp);
    mock_context_cleanup(&ctx);
    return 0;
}

/* --- Test: Request ID matching --- */

static int test_rpc_request_id_matching(void) {
    MockContext ctx;
    TEST_ASSERT(mock_context_init(&ctx) == 0, "context init");

    /* Multiple requests with different IDs */
    const char *ids[] = {"id-1", "id-2", "id-unique-12345", "1704067200_1"};

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char *req_json = nostr_nip46_request_build(ids[i], "get_public_key", NULL, 0);
        TEST_ASSERT(req_json != NULL, "build request");

        char *resp_json = NULL;
        TEST_ASSERT(mock_rpc_call(&ctx, req_json, &resp_json) == 0, "mock RPC");
        free(req_json);

        NostrNip46Response resp = {0};
        TEST_ASSERT(nostr_nip46_response_parse(resp_json, &resp) == 0, "parse response");
        free(resp_json);

        TEST_ASSERT_EQ_STR(resp.id, ids[i], "response id matches request id");

        nostr_nip46_response_free(&resp);
    }

    mock_context_cleanup(&ctx);
    return 0;
}

/* --- Test: Bunker callback integration --- */

static int s_auth_called = 0;
static int s_auth_allow = 1;
static char s_auth_perms[256] = {0};

static int test_authorize_callback(const char *client_pubkey_hex, const char *perms_csv, void *user_data) {
    (void)client_pubkey_hex;
    (void)user_data;
    s_auth_called = 1;
    if (perms_csv) {
        strncpy(s_auth_perms, perms_csv, sizeof(s_auth_perms) - 1);
    }
    return s_auth_allow;
}

static int test_rpc_connect_with_authorize_callback(void) {
    s_auth_called = 0;
    s_auth_allow = 1;
    s_auth_perms[0] = '\0';

    NostrNip46BunkerCallbacks cbs = {0};
    cbs.authorize_cb = test_authorize_callback;

    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *bunker_pk = nostr_key_get_public(BUNKER_SK);
    TEST_ASSERT(client_pk && bunker_pk, "pubkeys");

    /* Create client */
    NostrNip46Session *client = nostr_nip46_client_new();
    char client_uri[256];
    snprintf(client_uri, sizeof(client_uri), "bunker://%s?secret=%s", bunker_pk, CLIENT_SK);
    TEST_ASSERT(nostr_nip46_client_connect(client, client_uri, NULL) == 0, "client connect");

    /* Create bunker with callback */
    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    char bunker_uri[256];
    snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", client_pk, BUNKER_SK);
    TEST_ASSERT(nostr_nip46_client_connect(bunker, bunker_uri, NULL) == 0, "bunker connect");

    /* Send connect request.
     * NIP-46 connect params: [remote_signer_pubkey, secret, permissions] */
    const char *params[] = {client_pk, "", "sign_event,nip04_encrypt"};
    char *req_json = nostr_nip46_request_build("c1", "connect", params, 3);

    char *cipher_req = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(client, bunker_pk, req_json, &cipher_req) == 0, "encrypt");
    free(req_json);

    char *cipher_resp = NULL;
    TEST_ASSERT(nostr_nip46_bunker_handle_cipher(bunker, client_pk, cipher_req, &cipher_resp) == 0, "handle");
    free(cipher_req);
    free(cipher_resp);

    /* Verify callback was invoked */
    TEST_ASSERT(s_auth_called == 1, "authorize callback was called");
    TEST_ASSERT(strstr(s_auth_perms, "sign_event") != NULL, "perms passed to callback");

    free(client_pk);
    free(bunker_pk);
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}

static int test_rpc_connect_denied_by_callback(void) {
    s_auth_called = 0;
    s_auth_allow = 0;  /* Deny */

    NostrNip46BunkerCallbacks cbs = {0};
    cbs.authorize_cb = test_authorize_callback;

    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *bunker_pk = nostr_key_get_public(BUNKER_SK);

    NostrNip46Session *client = nostr_nip46_client_new();
    char client_uri[256];
    snprintf(client_uri, sizeof(client_uri), "bunker://%s?secret=%s", bunker_pk, CLIENT_SK);
    nostr_nip46_client_connect(client, client_uri, NULL);

    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    char bunker_uri[256];
    snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", client_pk, BUNKER_SK);
    nostr_nip46_client_connect(bunker, bunker_uri, NULL);

    /* NIP-46 connect params: [remote_signer_pubkey, secret, permissions] */
    const char *params[] = {client_pk, "", "sign_event"};
    char *req_json = nostr_nip46_request_build("c1", "connect", params, 3);

    char *cipher_req = NULL;
    nostr_nip46_client_nip04_encrypt(client, bunker_pk, req_json, &cipher_req);
    free(req_json);

    char *cipher_resp = NULL;
    TEST_ASSERT(nostr_nip46_bunker_handle_cipher(bunker, client_pk, cipher_req, &cipher_resp) == 0, "handle");
    free(cipher_req);

    char *plain_resp = NULL;
    nostr_nip46_client_nip04_decrypt(client, bunker_pk, cipher_resp, &plain_resp);
    free(cipher_resp);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(plain_resp, &resp) == 0, "parse");
    free(plain_resp);

    TEST_ASSERT(s_auth_called == 1, "callback called");
    TEST_ASSERT(resp.error != NULL, "has error");
    TEST_ASSERT(strstr(resp.error, "denied") != NULL, "error is denied");

    nostr_nip46_response_free(&resp);
    free(client_pk);
    free(bunker_pk);
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}

/* --- Test: Custom sign callback --- */

static char *test_sign_callback(const char *event_json, void *user_data) {
    (void)user_data;
    /* Return a fake signed event */
    size_t len = strlen(event_json);
    char *result = (char *)malloc(len + 50);
    if (!result) return NULL;
    snprintf(result, len + 50, "{\"signed_by_callback\":%s}", event_json);
    return result;
}

static int test_rpc_sign_event_custom_callback(void) {
    NostrNip46BunkerCallbacks cbs = {0};
    cbs.sign_cb = test_sign_callback;

    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *bunker_pk = nostr_key_get_public(BUNKER_SK);

    NostrNip46Session *client = nostr_nip46_client_new();
    char client_uri[256];
    snprintf(client_uri, sizeof(client_uri), "bunker://%s?secret=%s", bunker_pk, CLIENT_SK);
    nostr_nip46_client_connect(client, client_uri, NULL);

    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    char bunker_uri[256];
    snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", client_pk, BUNKER_SK);
    nostr_nip46_client_connect(bunker, bunker_uri, NULL);

    /* Connect first to grant permission */
    const char *conn_params[] = {client_pk, "sign_event"};
    char *conn_req = nostr_nip46_request_build("c1", "connect", conn_params, 2);
    char *cipher_conn = NULL;
    nostr_nip46_client_nip04_encrypt(client, bunker_pk, conn_req, &cipher_conn);
    free(conn_req);
    char *cipher_conn_resp = NULL;
    nostr_nip46_bunker_handle_cipher(bunker, client_pk, cipher_conn, &cipher_conn_resp);
    free(cipher_conn);
    free(cipher_conn_resp);

    /* Sign event */
    const char *event_json = "{\"kind\":1,\"content\":\"custom\"}";
    const char *sign_params[] = {event_json};
    char *sign_req = nostr_nip46_request_build("s1", "sign_event", sign_params, 1);

    char *cipher_sign = NULL;
    nostr_nip46_client_nip04_encrypt(client, bunker_pk, sign_req, &cipher_sign);
    free(sign_req);

    char *cipher_sign_resp = NULL;
    TEST_ASSERT(nostr_nip46_bunker_handle_cipher(bunker, client_pk, cipher_sign, &cipher_sign_resp) == 0, "handle");
    free(cipher_sign);

    char *plain_resp = NULL;
    nostr_nip46_client_nip04_decrypt(client, bunker_pk, cipher_sign_resp, &plain_resp);
    free(cipher_sign_resp);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(plain_resp, &resp) == 0, "parse");
    free(plain_resp);

    TEST_ASSERT(resp.error == NULL, "no error");
    TEST_ASSERT(resp.result != NULL, "has result");
    TEST_ASSERT(strstr(resp.result, "signed_by_callback") != NULL, "custom callback was used");

    nostr_nip46_response_free(&resp);
    free(client_pk);
    free(bunker_pk);
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
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

    /* Basic RPC tests */
    RUN_TEST(test_rpc_get_public_key);
    RUN_TEST(test_rpc_connect);
    RUN_TEST(test_rpc_sign_event_after_connect);
    RUN_TEST(test_rpc_sign_event_denied_without_permission);
    RUN_TEST(test_rpc_unknown_method);
    RUN_TEST(test_rpc_request_id_matching);

    /* Callback tests */
    RUN_TEST(test_rpc_connect_with_authorize_callback);
    RUN_TEST(test_rpc_connect_denied_by_callback);
    RUN_TEST(test_rpc_sign_event_custom_callback);

    printf("test_rpc_flow_mock: %d/%d passed\n", passed, total);
    return rc;
}
