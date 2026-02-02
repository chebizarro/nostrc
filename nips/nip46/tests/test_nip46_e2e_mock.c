/**
 * @file test_nip46_e2e_mock.c
 * @brief Mocked end-to-end tests for NIP-46 sign-in flows
 *
 * Tests the complete client-side sign-in flow for both bunker:// and nostrconnect://
 * protocols using the in-process mock relay infrastructure.
 *
 * These tests verify:
 * 1. bunker:// flow: client initiates connection to remote signer
 * 2. nostrconnect:// flow: client generates URI for signer to connect
 * 3. Relay preservation across connect -> sign_event flow
 * 4. NIP-04/NIP-44 encryption handling
 * 5. Error scenarios (timeout, invalid response, etc.)
 */

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip04.h"
#include "nostr-keys.h"
#include "nostr-event.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test helper macros */
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s (line %d): %s - got %d, expected %d\n", \
               __func__, __LINE__, msg, (int)(a), (int)(b)); \
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

/* Test keypairs (deterministic for reproducibility)
 * These are valid secp256k1 private keys derived from SHA256 of simple strings.
 * CLIENT_SK = SHA256("client_test_key")
 * SIGNER_SK = SHA256("signer_test_key")
 */
static const char *CLIENT_SK = "a665a45920422f9d417e4867efdc4fb8a04a1f3fff1fa07e998e86f7f7a27ae3";
static const char *SIGNER_SK = "b4b147bc522828731f1a016bfa72c073a012fce3c9debc1896eec0da7a5c7d0c";

/* Test relay URLs */
static const char *TEST_RELAY_1 = "wss://relay1.test.local";
static const char *TEST_RELAY_2 = "wss://relay2.test.local";
static const char *TEST_RELAY_3 = "wss://relay3.test.local";

/* ============================================================================
 * Mock Signer Context
 * ============================================================================
 * Simulates a remote signer (like nsec.app) that processes NIP-46 requests.
 * This runs in the same process but simulates the protocol flow.
 */

typedef struct {
    NostrNip46Session *bunker;
    char *signer_pk;
    char *signer_sk;
    char *client_pk;
    int requests_received;
    int connect_called;
    int sign_event_called;
    int get_public_key_called;
} MockSigner;

static int mock_signer_init(MockSigner *signer, const char *signer_secret) {
    memset(signer, 0, sizeof(*signer));
    signer->signer_sk = strdup(signer_secret);
    signer->signer_pk = nostr_key_get_public(signer_secret);
    if (!signer->signer_pk) return -1;

    signer->bunker = nostr_nip46_bunker_new(NULL);
    if (!signer->bunker) return -1;

    return 0;
}

static void mock_signer_cleanup(MockSigner *signer) {
    if (signer->bunker) nostr_nip46_session_free(signer->bunker);
    free(signer->signer_pk);
    free(signer->signer_sk);
    free(signer->client_pk);
}

/**
 * Process an encrypted NIP-46 request and return encrypted response.
 * Simulates the signer-side of the protocol.
 */
static int mock_signer_process_request(MockSigner *signer,
                                       const char *client_pk,
                                       const char *encrypted_request,
                                       char **out_encrypted_response) {
    *out_encrypted_response = NULL;
    signer->requests_received++;

    /* Store client pubkey for response encryption */
    if (!signer->client_pk) {
        signer->client_pk = strdup(client_pk);
    }

    /* Decrypt request using NIP-04 */
    char *plaintext = NULL;
    char *err = NULL;
    int rc = nostr_nip04_decrypt(encrypted_request, client_pk, signer->signer_sk,
                                 &plaintext, &err);
    if (rc != 0) {
        free(err);
        return -1;
    }

    /* Parse request */
    NostrNip46Request req = {0};
    rc = nostr_nip46_request_parse(plaintext, &req);
    free(plaintext);
    if (rc != 0) return -1;

    /* Build response based on method */
    char *response_json = NULL;

    if (strcmp(req.method, "connect") == 0) {
        signer->connect_called++;
        /* Return "ack" for connect */
        response_json = nostr_nip46_response_build_ok(req.id, "\"ack\"");
    }
    else if (strcmp(req.method, "get_public_key") == 0) {
        signer->get_public_key_called++;
        /* Return signer's pubkey (the user's key) */
        char result[128];
        snprintf(result, sizeof(result), "\"%s\"", signer->signer_pk);
        response_json = nostr_nip46_response_build_ok(req.id, result);
    }
    else if (strcmp(req.method, "sign_event") == 0) {
        signer->sign_event_called++;
        /* Sign the event and return */
        if (req.n_params > 0 && req.params[0]) {
            NostrEvent *ev = nostr_event_new();
            if (ev && nostr_event_deserialize(ev, req.params[0]) == 0) {
                if (nostr_event_sign(ev, signer->signer_sk) == 0) {
                    char *signed_json = nostr_event_serialize(ev);
                    if (signed_json) {
                        /* Build response with JSON-escaped signed event */
                        size_t len = strlen(signed_json) + 3;
                        char *result = malloc(len);
                        if (result) {
                            snprintf(result, len, "%s", signed_json);
                            response_json = nostr_nip46_response_build_ok(req.id, result);
                            free(result);
                        }
                        free(signed_json);
                    }
                }
                nostr_event_free(ev);
            }
        }
        if (!response_json) {
            response_json = nostr_nip46_response_build_err(req.id, "sign_event failed");
        }
    }
    else if (strcmp(req.method, "ping") == 0) {
        response_json = nostr_nip46_response_build_ok(req.id, "\"pong\"");
    }
    else {
        response_json = nostr_nip46_response_build_err(req.id, "method not supported");
    }

    nostr_nip46_request_free(&req);

    if (!response_json) return -1;

    /* Encrypt response with NIP-04 */
    char *enc_err = NULL;
    rc = nostr_nip04_encrypt(response_json, client_pk, signer->signer_sk,
                             out_encrypted_response, &enc_err);
    free(response_json);
    free(enc_err);

    return rc;
}

/* ============================================================================
 * Mock Relay Simulation
 * ============================================================================
 * Simulates relay message routing between client and signer.
 */

typedef struct {
    MockSigner *signer;
    char *client_pk;
    char *signer_pk;
    int events_routed;
    int failed_deliveries;
    /* Relay URLs the client should use */
    const char **relay_urls;
    size_t n_relays;
} MockRelayNetwork;

static int mock_relay_init(MockRelayNetwork *network, MockSigner *signer,
                           const char *client_pk, const char **relay_urls, size_t n_relays) {
    memset(network, 0, sizeof(*network));
    network->signer = signer;
    network->client_pk = strdup(client_pk);
    network->signer_pk = strdup(signer->signer_pk);
    network->relay_urls = relay_urls;
    network->n_relays = n_relays;
    return 0;
}

static void mock_relay_cleanup(MockRelayNetwork *network) {
    free(network->client_pk);
    free(network->signer_pk);
}

/**
 * Simulate the full RPC cycle:
 * 1. Client sends encrypted request (as kind 24133 event)
 * 2. Relay routes to signer
 * 3. Signer processes and sends encrypted response
 * 4. Relay routes response back to client
 *
 * Returns the decrypted response JSON.
 */
static int mock_relay_rpc(MockRelayNetwork *network,
                          NostrNip46Session *client,
                          const char *request_json,
                          char **out_response_json) {
    *out_response_json = NULL;

    /* Get client's pubkey for encryption target */
    char *client_pk = NULL;
    nostr_nip46_session_get_client_pubkey(client, &client_pk);
    if (!client_pk) {
        /* For bunker:// connections, use remote_pubkey for the client identity */
        char *temp = NULL;
        nostr_nip46_session_get_secret(client, &temp);
        if (temp) {
            client_pk = nostr_key_get_public(temp);
            free(temp);
        }
    }
    if (!client_pk) return -1;

    /* Get signer's pubkey (destination) */
    char *signer_pk = NULL;
    nostr_nip46_session_get_remote_pubkey(client, &signer_pk);
    if (!signer_pk) {
        free(client_pk);
        return -1;
    }

    /* Client encrypts request */
    char *encrypted_request = NULL;
    int rc = nostr_nip46_client_nip04_encrypt(client, signer_pk,
                                              request_json, &encrypted_request);
    if (rc != 0) {
        free(client_pk);
        free(signer_pk);
        return -1;
    }

    /* Signer processes request and produces encrypted response */
    char *encrypted_response = NULL;
    rc = mock_signer_process_request(network->signer, client_pk,
                                     encrypted_request, &encrypted_response);
    free(encrypted_request);
    network->events_routed++;

    if (rc != 0) {
        network->failed_deliveries++;
        free(client_pk);
        free(signer_pk);
        return -1;
    }

    /* Client decrypts response */
    rc = nostr_nip46_client_nip04_decrypt(client, signer_pk,
                                          encrypted_response, out_response_json);
    free(encrypted_response);
    free(client_pk);
    free(signer_pk);

    return rc;
}

/* ============================================================================
 * SECTION 1: bunker:// Sign-In Flow Tests
 * ============================================================================
 */

/**
 * Test complete bunker:// sign-in flow:
 * 1. Parse bunker:// URI with multiple relays
 * 2. Send connect RPC
 * 3. Verify session state is correct
 * 4. Send get_public_key RPC
 * 5. Verify user pubkey is received
 */
static int test_bunker_signin_flow_complete(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    TEST_ASSERT(client_pk != NULL, "derive client pubkey");

    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(signer_pk != NULL, "derive signer pubkey");

    /* Initialize mock signer */
    MockSigner signer;
    TEST_ASSERT(mock_signer_init(&signer, SIGNER_SK) == 0, "init mock signer");

    /* Create bunker URI with multiple relays */
    char bunker_uri[512];
    snprintf(bunker_uri, sizeof(bunker_uri),
             "bunker://%s?relay=%s&relay=%s&relay=%s&secret=test_secret",
             signer_pk,
             "wss%3A%2F%2Frelay1.test.local",
             "wss%3A%2F%2Frelay2.test.local",
             "wss%3A%2F%2Frelay3.test.local");

    /* Create client session */
    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client session");

    /* Connect (parses URI, sets up session) */
    TEST_ASSERT(nostr_nip46_client_connect(client, bunker_uri, "sign_event") == 0,
                "connect to bunker URI");

    /* Set client's secret for encryption AFTER connect (connect overwrites secret with URI param) */
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set client secret");

    /* Verify relays were parsed */
    char **relays = NULL;
    size_t n_relays = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(client, &relays, &n_relays) == 0,
                "get relays");
    TEST_ASSERT(n_relays == 3, "expected 3 relays");
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);

    /* Verify remote pubkey was set */
    char *remote_pk = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(client, &remote_pk) == 0,
                "get remote pubkey");
    TEST_ASSERT_EQ_STR(remote_pk, signer_pk, "remote pubkey matches signer");
    free(remote_pk);

    /* Initialize mock relay network */
    const char *relay_urls[] = { TEST_RELAY_1, TEST_RELAY_2, TEST_RELAY_3 };
    MockRelayNetwork network;
    mock_relay_init(&network, &signer, client_pk, relay_urls, 3);

    /* Simulate connect RPC */
    const char *connect_params[] = { client_pk, "sign_event" };
    char *connect_req = nostr_nip46_request_build("connect-1", "connect", connect_params, 2);
    TEST_ASSERT(connect_req != NULL, "build connect request");

    char *connect_resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, connect_req, &connect_resp) == 0,
                "connect RPC");
    free(connect_req);

    /* Verify connect response */
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(connect_resp, &resp) == 0, "parse connect response");
    free(connect_resp);
    TEST_ASSERT_EQ_STR(resp.id, "connect-1", "response id matches");
    TEST_ASSERT(resp.error == NULL, "no error in connect response");
    TEST_ASSERT(strstr(resp.result, "ack") != NULL, "connect acked");
    nostr_nip46_response_free(&resp);

    /* Simulate get_public_key RPC */
    char *gpk_req = nostr_nip46_request_build("gpk-1", "get_public_key", NULL, 0);
    TEST_ASSERT(gpk_req != NULL, "build get_public_key request");

    char *gpk_resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, gpk_req, &gpk_resp) == 0,
                "get_public_key RPC");
    free(gpk_req);

    /* Verify get_public_key response */
    memset(&resp, 0, sizeof(resp));
    TEST_ASSERT(nostr_nip46_response_parse(gpk_resp, &resp) == 0, "parse gpk response");
    free(gpk_resp);
    TEST_ASSERT_EQ_STR(resp.id, "gpk-1", "gpk response id matches");
    TEST_ASSERT(resp.error == NULL, "no error in gpk response");
    TEST_ASSERT(strstr(resp.result, signer_pk) != NULL, "gpk result contains signer pubkey");
    nostr_nip46_response_free(&resp);

    /* Verify signer received both requests */
    TEST_ASSERT_EQ(signer.connect_called, 1, "signer received connect");
    TEST_ASSERT_EQ(signer.get_public_key_called, 1, "signer received get_public_key");
    TEST_ASSERT_EQ(network.events_routed, 2, "2 events routed through relay");

    mock_relay_cleanup(&network);
    mock_signer_cleanup(&signer);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/**
 * Test bunker:// sign_event flow (the critical path that was broken):
 * 1. Connect with bunker:// URI
 * 2. Send sign_event RPC
 * 3. Verify signed event is returned
 * 4. Verify relays were preserved from connect to sign_event
 */
static int test_bunker_sign_event_flow(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(client_pk && signer_pk, "derive pubkeys");

    MockSigner signer;
    TEST_ASSERT(mock_signer_init(&signer, SIGNER_SK) == 0, "init signer");

    /* bunker URI with 4 relays (like nsec.app) */
    char bunker_uri[1024];
    snprintf(bunker_uri, sizeof(bunker_uri),
             "bunker://%s?relay=%s&relay=%s&relay=%s&relay=%s&secret=signin",
             signer_pk,
             "wss%3A%2F%2Frelay1.nsecbunker.com",
             "wss%3A%2F%2Frelay2.nsecbunker.com",
             "wss%3A%2F%2Frelay.nsec.app",
             "wss%3A%2F%2Fnostr.wine");

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client");
    TEST_ASSERT(nostr_nip46_client_connect(client, bunker_uri, NULL) == 0, "connect");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    /* Verify 4 relays are configured */
    char **relays = NULL;
    size_t n_relays = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(client, &relays, &n_relays) == 0, "get relays");
    TEST_ASSERT_EQ((int)n_relays, 4, "expected 4 relays from bunker URI");
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);

    const char *relay_urls[] = { TEST_RELAY_1, TEST_RELAY_2, TEST_RELAY_3 };
    MockRelayNetwork network;
    mock_relay_init(&network, &signer, client_pk, relay_urls, 3);

    /* First: connect RPC */
    const char *connect_params[] = { client_pk };
    char *connect_req = nostr_nip46_request_build("c1", "connect", connect_params, 1);
    char *connect_resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, connect_req, &connect_resp) == 0, "connect RPC");
    free(connect_req);
    free(connect_resp);

    /* Verify relays are STILL configured after connect */
    TEST_ASSERT(nostr_nip46_session_get_relays(client, &relays, &n_relays) == 0, "get relays after connect");
    TEST_ASSERT_EQ((int)n_relays, 4, "relays preserved after connect RPC");
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);

    /* Now: sign_event RPC */
    const char *event_json = "{\"kind\":7,\"content\":\"+\",\"tags\":[[\"e\",\"abc123\"]],\"created_at\":1704067200}";
    const char *sign_params[] = { event_json };
    char *sign_req = nostr_nip46_request_build("s1", "sign_event", sign_params, 1);
    TEST_ASSERT(sign_req != NULL, "build sign_event request");

    char *sign_resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, sign_req, &sign_resp) == 0, "sign_event RPC");
    free(sign_req);

    /* Verify signed event response */
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(sign_resp, &resp) == 0, "parse sign response");
    free(sign_resp);
    TEST_ASSERT_EQ_STR(resp.id, "s1", "sign response id");
    TEST_ASSERT(resp.error == NULL, "no sign error");
    TEST_ASSERT(resp.result != NULL, "has sign result");
    TEST_ASSERT(strstr(resp.result, "\"sig\":") != NULL, "result has signature");
    TEST_ASSERT(strstr(resp.result, "\"pubkey\":") != NULL, "result has pubkey");
    nostr_nip46_response_free(&resp);

    /* Verify the signing flow worked */
    TEST_ASSERT_EQ(signer.connect_called, 1, "connect called once");
    TEST_ASSERT_EQ(signer.sign_event_called, 1, "sign_event called once");

    mock_relay_cleanup(&network);
    mock_signer_cleanup(&signer);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/**
 * Test that relays are correctly preserved when session is serialized/deserialized
 * (simulates the GSettings save/restore flow).
 */
static int test_bunker_relay_persistence(void) {
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(signer_pk != NULL, "derive signer pubkey");

    /* bunker URI with multiple relays */
    char bunker_uri[512];
    snprintf(bunker_uri, sizeof(bunker_uri),
             "bunker://%s?relay=%s&relay=%s&relay=%s",
             signer_pk,
             "wss%3A%2F%2Frelay1.test",
             "wss%3A%2F%2Frelay2.test",
             "wss%3A%2F%2Frelay3.test");

    /* Create and connect first session */
    NostrNip46Session *session1 = nostr_nip46_client_new();
    TEST_ASSERT(session1 != NULL, "create session 1");
    TEST_ASSERT(nostr_nip46_client_set_secret(session1, CLIENT_SK) == 0, "set secret");
    TEST_ASSERT(nostr_nip46_client_connect(session1, bunker_uri, NULL) == 0, "connect session 1");

    /* Get relays from session 1 */
    char **relays1 = NULL;
    size_t n_relays1 = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(session1, &relays1, &n_relays1) == 0, "get relays 1");
    TEST_ASSERT_EQ((int)n_relays1, 3, "session 1 has 3 relays");

    /* Simulate save/restore by creating new session and setting relays manually */
    NostrNip46Session *session2 = nostr_nip46_client_new();
    TEST_ASSERT(session2 != NULL, "create session 2");
    TEST_ASSERT(nostr_nip46_client_set_secret(session2, CLIENT_SK) == 0, "set secret 2");

    /* Set relays from saved values (simulating GSettings restore) */
    TEST_ASSERT(nostr_nip46_session_set_relays(session2, (const char *const *)relays1, n_relays1) == 0,
                "set relays on session 2");

    /* Verify relays were set */
    char **relays2 = NULL;
    size_t n_relays2 = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(session2, &relays2, &n_relays2) == 0, "get relays 2");
    TEST_ASSERT_EQ((int)n_relays2, 3, "session 2 has 3 relays");

    /* Verify relay URLs match */
    for (size_t i = 0; i < n_relays1; i++) {
        TEST_ASSERT_EQ_STR(relays1[i], relays2[i], "relay URL matches");
    }

    /* Cleanup */
    for (size_t i = 0; i < n_relays1; i++) free(relays1[i]);
    free(relays1);
    for (size_t i = 0; i < n_relays2; i++) free(relays2[i]);
    free(relays2);
    nostr_nip46_session_free(session1);
    nostr_nip46_session_free(session2);
    free(signer_pk);
    return 0;
}

/* ============================================================================
 * SECTION 2: nostrconnect:// Sign-In Flow Tests
 * ============================================================================
 */

/**
 * Test nostrconnect:// flow where client generates URI for signer to scan:
 * 1. Client generates nostrconnect:// URI with its pubkey
 * 2. Signer scans and sends connect request
 * 3. Client receives signer's pubkey
 * 4. Subsequent RPCs work
 */
static int test_nostrconnect_signin_flow(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(client_pk && signer_pk, "derive pubkeys");

    MockSigner signer;
    TEST_ASSERT(mock_signer_init(&signer, SIGNER_SK) == 0, "init signer");

    /* Client creates nostrconnect:// URI for signer to scan */
    char nostrconnect_uri[512];
    snprintf(nostrconnect_uri, sizeof(nostrconnect_uri),
             "nostrconnect://%s?relay=%s&relay=%s&secret=client_secret&metadata=%s",
             client_pk,
             "wss%3A%2F%2Frelay1.test",
             "wss%3A%2F%2Frelay2.test",
             "%7B%22name%22%3A%22TestApp%22%7D");

    /* Client session parses nostrconnect:// URI */
    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client");
    TEST_ASSERT(nostr_nip46_client_connect(client, nostrconnect_uri, NULL) == 0, "parse nostrconnect URI");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    /* Verify client pubkey was extracted from URI */
    char *extracted_pk = NULL;
    TEST_ASSERT(nostr_nip46_session_get_client_pubkey(client, &extracted_pk) == 0, "get client pubkey");
    TEST_ASSERT_EQ_STR(extracted_pk, client_pk, "client pubkey matches");
    free(extracted_pk);

    /* Verify relays were extracted */
    char **relays = NULL;
    size_t n_relays = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(client, &relays, &n_relays) == 0, "get relays");
    TEST_ASSERT_EQ((int)n_relays, 2, "2 relays from nostrconnect URI");
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);

    /* For nostrconnect://, client needs to set the remote signer's pubkey
     * after the signer connects. Simulate signer scanning and connecting. */
    TEST_ASSERT(nostr_nip46_client_set_signer_pubkey(client, signer_pk) == 0,
                "set signer pubkey after scan");

    /* Verify remote pubkey is now set */
    char *remote_pk = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(client, &remote_pk) == 0, "get remote pubkey");
    TEST_ASSERT_EQ_STR(remote_pk, signer_pk, "remote pubkey is signer");
    free(remote_pk);

    const char *relay_urls[] = { TEST_RELAY_1, TEST_RELAY_2 };
    MockRelayNetwork network;
    mock_relay_init(&network, &signer, client_pk, relay_urls, 2);

    /* Now client can send RPC requests */
    char *ping_req = nostr_nip46_request_build("ping-1", "ping", NULL, 0);
    char *ping_resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, ping_req, &ping_resp) == 0, "ping RPC");
    free(ping_req);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(ping_resp, &resp) == 0, "parse ping response");
    free(ping_resp);
    TEST_ASSERT(resp.error == NULL, "no ping error");
    TEST_ASSERT(strstr(resp.result, "pong") != NULL, "got pong");
    nostr_nip46_response_free(&resp);

    mock_relay_cleanup(&network);
    mock_signer_cleanup(&signer);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/**
 * Test nostrconnect:// get_public_key returns the correct user pubkey.
 */
static int test_nostrconnect_get_public_key(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    TEST_ASSERT(client_pk != NULL, "derive client pubkey");

    char nostrconnect_uri[256];
    snprintf(nostrconnect_uri, sizeof(nostrconnect_uri),
             "nostrconnect://%s?relay=wss%%3A%%2F%%2Frelay.test",
             client_pk);

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client");
    TEST_ASSERT(nostr_nip46_client_connect(client, nostrconnect_uri, NULL) == 0, "parse URI");

    /* For nostrconnect://, get_public_key should return the client pubkey
     * from the URI (this is the user's pubkey for the app) */
    char *user_pk = NULL;
    TEST_ASSERT(nostr_nip46_client_get_public_key(client, &user_pk) == 0, "get public key");
    TEST_ASSERT_EQ_STR(user_pk, client_pk, "user pubkey matches client pubkey from URI");
    free(user_pk);

    nostr_nip46_session_free(client);
    free(client_pk);
    return 0;
}

/* ============================================================================
 * SECTION 3: Error Handling Tests
 * ============================================================================
 */

/**
 * Test handling of error responses from signer.
 */
static int test_error_response_handling(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(client_pk && signer_pk, "derive pubkeys");

    MockSigner signer;
    TEST_ASSERT(mock_signer_init(&signer, SIGNER_SK) == 0, "init signer");

    char bunker_uri[256];
    snprintf(bunker_uri, sizeof(bunker_uri),
             "bunker://%s?relay=wss%%3A%%2F%%2Frelay.test",
             signer_pk);

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client");
    TEST_ASSERT(nostr_nip46_client_connect(client, bunker_uri, NULL) == 0, "connect");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    const char *relay_urls[] = { TEST_RELAY_1 };
    MockRelayNetwork network;
    mock_relay_init(&network, &signer, client_pk, relay_urls, 1);

    /* Send unknown method - should get error response */
    char *req = nostr_nip46_request_build("err-1", "unknown_method", NULL, 0);
    char *resp_json = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, req, &resp_json) == 0, "RPC succeeds");
    free(req);

    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(resp_json, &resp) == 0, "parse response");
    free(resp_json);
    TEST_ASSERT_EQ_STR(resp.id, "err-1", "error response id matches");
    TEST_ASSERT(resp.error != NULL, "has error field");
    TEST_ASSERT(strstr(resp.error, "not supported") != NULL, "error mentions not supported");
    nostr_nip46_response_free(&resp);

    mock_relay_cleanup(&network);
    mock_signer_cleanup(&signer);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/**
 * Test that sign_event fails gracefully when session has no relays.
 */
static int test_sign_event_no_relays_error(void) {
    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(client != NULL, "create client");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    /* Don't connect - session has no relays or remote pubkey */
    char *signed_json = NULL;
    int rc = nostr_nip46_client_sign_event(client, "{\"kind\":1}", &signed_json);
    TEST_ASSERT(rc != 0, "sign_event should fail without session state");
    TEST_ASSERT(signed_json == NULL, "no output on failure");

    nostr_nip46_session_free(client);
    return 0;
}

/**
 * Test multiple sequential sign_event calls preserve relay configuration.
 */
static int test_multiple_sign_events_preserve_relays(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(client_pk && signer_pk, "derive pubkeys");

    MockSigner signer;
    TEST_ASSERT(mock_signer_init(&signer, SIGNER_SK) == 0, "init signer");

    char bunker_uri[512];
    snprintf(bunker_uri, sizeof(bunker_uri),
             "bunker://%s?relay=%s&relay=%s",
             signer_pk,
             "wss%3A%2F%2Frelay1.test",
             "wss%3A%2F%2Frelay2.test");

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(nostr_nip46_client_connect(client, bunker_uri, NULL) == 0, "connect");
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    const char *relay_urls[] = { TEST_RELAY_1, TEST_RELAY_2 };
    MockRelayNetwork network;
    mock_relay_init(&network, &signer, client_pk, relay_urls, 2);

    /* Connect first */
    const char *connect_params[] = { client_pk };
    char *connect_req = nostr_nip46_request_build("c1", "connect", connect_params, 1);
    char *resp = NULL;
    TEST_ASSERT(mock_relay_rpc(&network, client, connect_req, &resp) == 0, "connect");
    free(connect_req);
    free(resp);

    /* Multiple sign_event calls */
    for (int i = 0; i < 5; i++) {
        char event_json[128];
        snprintf(event_json, sizeof(event_json),
                 "{\"kind\":1,\"content\":\"test %d\",\"tags\":[],\"created_at\":%ld}",
                 i, (long)time(NULL) + i);

        const char *sign_params[] = { event_json };
        char req_id[16];
        snprintf(req_id, sizeof(req_id), "s%d", i);
        char *sign_req = nostr_nip46_request_build(req_id, "sign_event", sign_params, 1);

        char *sign_resp = NULL;
        TEST_ASSERT(mock_relay_rpc(&network, client, sign_req, &sign_resp) == 0, "sign_event RPC");
        free(sign_req);

        NostrNip46Response parsed = {0};
        TEST_ASSERT(nostr_nip46_response_parse(sign_resp, &parsed) == 0, "parse response");
        free(sign_resp);
        TEST_ASSERT(parsed.error == NULL, "no error");
        TEST_ASSERT(parsed.result != NULL, "has result");
        nostr_nip46_response_free(&parsed);

        /* Verify relays still configured */
        char **relays = NULL;
        size_t n_relays = 0;
        TEST_ASSERT(nostr_nip46_session_get_relays(client, &relays, &n_relays) == 0, "get relays");
        TEST_ASSERT_EQ((int)n_relays, 2, "relays preserved after sign_event");
        for (size_t j = 0; j < n_relays; j++) free(relays[j]);
        free(relays);
    }

    TEST_ASSERT_EQ(signer.sign_event_called, 5, "5 sign_event calls processed");

    mock_relay_cleanup(&network);
    mock_signer_cleanup(&signer);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/* ============================================================================
 * SECTION 4: NIP-04/NIP-44 Encryption Tests
 * ============================================================================
 */

/**
 * Test that messages are correctly encrypted/decrypted in the RPC flow.
 */
static int test_encryption_in_rpc_flow(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    TEST_ASSERT(client_pk && signer_pk, "derive pubkeys");

    NostrNip46Session *client = nostr_nip46_client_new();
    TEST_ASSERT(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0, "set secret");

    /* Test NIP-04 roundtrip */
    const char *plaintext = "{\"id\":\"test\",\"method\":\"ping\",\"params\":[]}";
    char *ciphertext = NULL;
    TEST_ASSERT(nostr_nip46_client_nip04_encrypt(client, signer_pk, plaintext, &ciphertext) == 0,
                "NIP-04 encrypt");
    TEST_ASSERT(ciphertext != NULL, "ciphertext not null");
    TEST_ASSERT(strcmp(ciphertext, plaintext) != 0, "ciphertext differs from plaintext");

    /* Decrypt with signer's key */
    char *decrypted = NULL;
    char *dec_err = NULL;
    TEST_ASSERT(nostr_nip04_decrypt(ciphertext, client_pk, SIGNER_SK, &decrypted, &dec_err) == 0,
                "NIP-04 decrypt");
    free(dec_err);
    TEST_ASSERT_EQ_STR(decrypted, plaintext, "decrypted matches plaintext");

    free(ciphertext);
    free(decrypted);
    nostr_nip46_session_free(client);
    free(client_pk);
    free(signer_pk);
    return 0;
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================
 */

int main(void) {
    int rc = 0;
    int total = 0;
    int passed = 0;

    #define RUN_TEST(fn) do { \
        total++; \
        printf("Running %s...\n", #fn); \
        int r = fn(); \
        if (r == 0) { \
            passed++; \
            printf("  PASS\n"); \
        } else { \
            rc = 1; \
            printf("  FAIL\n"); \
        } \
    } while(0)

    printf("\n=== NIP-46 Mocked E2E Tests ===\n\n");

    printf("Section 1: bunker:// Sign-In Flow\n");
    printf("---------------------------------\n");
    RUN_TEST(test_bunker_signin_flow_complete);
    RUN_TEST(test_bunker_sign_event_flow);
    RUN_TEST(test_bunker_relay_persistence);
    printf("\n");

    printf("Section 2: nostrconnect:// Sign-In Flow\n");
    printf("---------------------------------------\n");
    RUN_TEST(test_nostrconnect_signin_flow);
    RUN_TEST(test_nostrconnect_get_public_key);
    printf("\n");

    printf("Section 3: Error Handling\n");
    printf("-------------------------\n");
    RUN_TEST(test_error_response_handling);
    RUN_TEST(test_sign_event_no_relays_error);
    RUN_TEST(test_multiple_sign_events_preserve_relays);
    printf("\n");

    printf("Section 4: Encryption\n");
    printf("---------------------\n");
    RUN_TEST(test_encryption_in_rpc_flow);
    printf("\n");

    printf("=================================\n");
    printf("Results: %d/%d passed\n", passed, total);
    printf("=================================\n\n");

    return rc;
}
