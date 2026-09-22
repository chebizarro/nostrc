/**
 * @file test_nip46_qr_socket.c
 * @brief NIP-46 nostrconnect:// QR handshake over a real WebSocket relay.
 *
 * Bugfix guard (nostrc-fix-nip46-await-dispatch, references nostrc-z1fb and
 * nostrc-ot2c.7): the Phase-1 QR tests exercised the client-side dispatch via
 * a plaintext-ingest test hook that bypassed the pool/message-loop path
 * entirely. This test drives the real WebSocket read path with the in-repo
 * mock relay so a regression in the pool servicing/dispatch pump — the
 * failure mode that stalled every live QR login attempt in the greeter
 * acceptance run — is caught in CI.
 *
 * Cases:
 *   A) happy path:  client await_connect + bunker connect_to_client over
 *      the socket -> connect accepted, then get_public_key + sign_event.
 *   B) wrong-secret: bunker publishes a `connect` carrying a bogus secret;
 *      client's await_connect must NOT accept it and must time out cleanly.
 *   C) timeout:     no bunker publishes; client's await_connect returns -1
 *      cleanly within the caller-supplied deadline.
 */

#include "nostr/testing/mock_relay_server.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_uri.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

/* Deterministic keys — user's signer identity. */
static const char SIGNER_SK[] =
    "b4b147bc522828731f1a016bfa72c073a012fce3c9debc1896eec0da7a5c7d0c";

#define OK(x)  do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", \
                                 __func__, __LINE__, #x); return 1; } } while (0)
#define OK_EQI(a,b) do { long _a=(long)(a), _b=(long)(b); \
    if (_a != _b) { fprintf(stderr, "FAIL %s:%d %s: got %ld expected %ld\n", \
                            __func__, __LINE__, #a, _a, _b); return 1; } } while (0)
#define OK_STR(a,b) do { const char *_a=(a), *_b=(b); \
    if (!(_a && _b && strcmp(_a,_b)==0)) { \
      fprintf(stderr, "FAIL %s:%d %s: got '%s' expected '%s'\n", \
              __func__, __LINE__, #a, _a?_a:"(null)", _b?_b:"(null)"); \
      return 1; } } while (0)

/* Thread payload for the bunker publish thread. Runs after client_start so
 * the subscription is live on the relay before the bunker's EVENT lands. */
typedef struct {
    NostrNip46Session *bunker;
    const char *uri;
    int delay_ms;
    int rc;
} BunkerJob;

static void *bunker_publish_thread(void *arg) {
    BunkerJob *j = (BunkerJob *)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)j->delay_ms * 1000000L };
    nanosleep(&ts, NULL);
    /* nostr_nip46_bunker_connect_to_client publishes fire-and-forget: the
     * first call also brings the bunker's pool up, so the relay may not be
     * fully connected in time for the immediate publish. Retry with backoff
     * (mirrors the phone-signer stand-in in
     * gnome/nostr-homed/tests/integration/qr_signer_standin.c). */
    j->rc = -1;
    for (int i = 0; i < 20 && j->rc != 0; i++) {
        j->rc = nostr_nip46_bunker_connect_to_client(j->bunker, j->uri);
        if (j->rc != 0) {
            struct timespec s = { .tv_sec = 0, .tv_nsec = 100 * 1000000L };
            nanosleep(&s, NULL);
        }
    }
    return NULL;
}

/* Optional signer callback: honour sign_event with the well-known signer_sk. */
static int bunker_authorize_all(const char *pk, const char *perms, void *ud) {
    (void)pk; (void)perms; (void)ud; return 1;
}
static char *bunker_sign_with_sk(const char *event_json, void *ud) {
    const char *sk = (const char *)ud;
    if (!sk || !event_json) return NULL;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;
    if (nostr_event_deserialize(ev, event_json) != 0) { nostr_event_free(ev); return NULL; }
    if (nostr_event_sign(ev, sk) != 0) { nostr_event_free(ev); return NULL; }
    char *j = nostr_event_serialize(ev);
    nostr_event_free(ev);
    return j;
}

/* Shared mock relay for all cases in this suite: recreating the LWS server
 * across tests exposes flakiness in libnostr's global connection state that
 * is not what this file is testing. */
static NostrMockRelayServer *g_mock = NULL;
static const char *g_mock_url = NULL;

/* Case A: full happy-path QR handshake through the socket. */
static int test_qr_handshake_over_socket(void) {
    OK(g_mock != NULL && g_mock_url != NULL);
    const char *url = g_mock_url;

    /* --- Client side: mint QR + start the persistent pool. ------------- */
    const char *relays[] = { url };
    NostrNip46Session *client = nostr_nip46_client_new();
    OK(client != NULL);
    char *uri = NULL;
    OK_EQI(nostr_nip46_client_new_qr_session(client, relays, 1, "sign_event",
                                             "GNOME (QR socket)", &uri), 0);
    OK(uri != NULL);
    NostrNip46ConnectURI parsed = {0};
    OK_EQI(nostr_nip46_uri_parse_connect(uri, &parsed), 0);

    /* Bring pool + sub up BEFORE the bunker publishes. This is the exact
     * ordering the broker uses (subscription-before-publish). */
    OK_EQI(nostr_nip46_client_start(client), 0);

    /* --- Bunker (phone) side: consume URI + publish `connect`. --------- */
    NostrNip46BunkerCallbacks cbs = {
        .authorize_cb = bunker_authorize_all,
        .sign_cb      = bunker_sign_with_sk,
        .user_data    = (void *)SIGNER_SK,
    };
    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    OK(bunker != NULL);
    OK_EQI(nostr_nip46_client_set_secret(bunker, SIGNER_SK), 0);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    OK(signer_pk != NULL);

    BunkerJob job = { .bunker = bunker, .uri = uri, .delay_ms = 150, .rc = -1 };
    pthread_t th;
    pthread_create(&th, NULL, bunker_publish_thread, &job);

    /* await_connect must dispatch the middleware fired from the pool
     * worker thread — a stall here is the regression we're guarding. */
    char *signer_out = NULL;
    int aw = nostr_nip46_client_await_connect(client, parsed.secret,
                                              5000, &signer_out);
    pthread_join(th, NULL);
    OK_EQI(job.rc, 0);
    OK_EQI(aw, 0);
    OK_STR(signer_out, signer_pk);
    free(signer_out);

    /* Optional: get_public_key over the socket. */
    nostr_nip46_client_set_timeout(client, 5000);
    char *user_pk = NULL;
    int gp = nostr_nip46_client_get_public_key_rpc(client, &user_pk);
    if (gp == 0 && user_pk) {
        OK_STR(user_pk, signer_pk);
    }
    free(user_pk);

    /* Optional: sign_event over the socket. */
    const char *unsigned_evt =
        "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],"
        "\"content\":\"hi from nip46 qr socket test\","
        "\"pubkey\":\"b4b147bc522828731f1a016bfa72c073a012fce3c9debc1896eec0da7a5c7d0c\"}";
    char *signed_json = NULL;
    int sr = nostr_nip46_client_sign_event(client, unsigned_evt, &signed_json);
    if (sr == 0 && signed_json) {
        OK(strstr(signed_json, "\"sig\"") != NULL);
        free(signed_json);
    }

    nostr_nip46_uri_connect_free(&parsed);
    free(uri);
    free(signer_pk);
    nostr_nip46_session_free(bunker);
    nostr_nip46_session_free(client);
    return 0;
}

/* Case B: bunker publishes a `connect` with a WRONG secret. The client
 * must ignore it and time out; the session must NOT rebind to the sender. */
static int test_qr_wrong_secret_over_socket(void) {
    OK(g_mock != NULL && g_mock_url != NULL);
    const char *url = g_mock_url;

    const char *relays[] = { url };
    NostrNip46Session *client = nostr_nip46_client_new();
    OK(client != NULL);
    char *uri = NULL;
    OK_EQI(nostr_nip46_client_new_qr_session(client, relays, 1, NULL,
                                             NULL, &uri), 0);
    NostrNip46ConnectURI parsed = {0};
    OK_EQI(nostr_nip46_uri_parse_connect(uri, &parsed), 0);
    OK_EQI(nostr_nip46_client_start(client), 0);

    /* Handcraft a bunker URI that carries a DIFFERENT secret. Easiest way
     * to drive the wrong path is to build our own URI (client_pk + relay +
     * bad secret) and hand it to connect_to_client. */
    char bad_uri[1024];
    snprintf(bad_uri, sizeof(bad_uri),
             "nostrconnect://%s?relay=ws%%3A%%2F%%2F%s&secret=deadbeefdeadbeefdeadbeefdeadbeef",
             parsed.client_pubkey_hex,
             /* strip "ws://" prefix from url, keep host:port */
             url + 5);

    NostrNip46BunkerCallbacks cbs = {
        .authorize_cb = bunker_authorize_all,
        .sign_cb      = bunker_sign_with_sk,
        .user_data    = (void *)SIGNER_SK,
    };
    NostrNip46Session *bunker = nostr_nip46_bunker_new(&cbs);
    OK(bunker != NULL);
    OK_EQI(nostr_nip46_client_set_secret(bunker, SIGNER_SK), 0);
    BunkerJob job = { .bunker = bunker, .uri = bad_uri, .delay_ms = 100, .rc = -1 };
    pthread_t th;
    pthread_create(&th, NULL, bunker_publish_thread, &job);

    char *signer_out = NULL;
    int aw = nostr_nip46_client_await_connect(client, parsed.secret,
                                              1200, &signer_out);
    pthread_join(th, NULL);
    OK_EQI(aw, -1);
    OK(signer_out == NULL);

    /* The session's remote_pubkey must NOT be pinned to the sender. */
    char *remote_pk = NULL;
    nostr_nip46_session_get_remote_pubkey(client, &remote_pk);
    OK(remote_pk == NULL || remote_pk[0] == '\0');
    free(remote_pk);

    nostr_nip46_uri_connect_free(&parsed);
    free(uri);
    nostr_nip46_session_free(bunker);
    nostr_nip46_session_free(client);
    return 0;
}

/* Case C: no signer publishes anything. await_connect must return -1
 * cleanly within the caller's deadline (no infinite hang, no leaked waiter). */
static int test_qr_timeout_over_socket(void) {
    OK(g_mock != NULL && g_mock_url != NULL);
    const char *url = g_mock_url;

    const char *relays[] = { url };
    NostrNip46Session *client = nostr_nip46_client_new();
    OK(client != NULL);
    char *uri = NULL;
    OK_EQI(nostr_nip46_client_new_qr_session(client, relays, 1, NULL,
                                             NULL, &uri), 0);
    NostrNip46ConnectURI parsed = {0};
    OK_EQI(nostr_nip46_uri_parse_connect(uri, &parsed), 0);
    OK_EQI(nostr_nip46_client_start(client), 0);

    char *signer_out = NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int aw = nostr_nip46_client_await_connect(client, parsed.secret,
                                              600, &signer_out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    OK_EQI(aw, -1);
    OK(signer_out == NULL);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    /* Must respect the caller's deadline; allow generous jitter margin. */
    OK(elapsed_ms >= 500 && elapsed_ms <= 3000);

    /* Second attempt must also succeed cleanly (no leaked waiter). */
    aw = nostr_nip46_client_await_connect(client, parsed.secret, 200, &signer_out);
    OK_EQI(aw, -1);
    OK(signer_out == NULL);

    nostr_nip46_uri_connect_free(&parsed);
    free(uri);
    nostr_nip46_session_free(client);
    return 0;
}

int main(void) {
    /* One mock relay for the whole suite. */
    g_mock = nostr_mock_server_new(NULL);
    if (!g_mock || nostr_mock_server_start(g_mock) != 0) {
        fprintf(stderr, "could not start mock relay\n");
        return 1;
    }
    g_mock_url = nostr_mock_server_get_url(g_mock);
    fprintf(stderr, "test suite: mock relay URL = %s\n", g_mock_url);
    /* Wait until the mock relay's TCP listener is actually accepting: a bare
     * usleep is racy under load and hides real regressions in first-connect
     * timing. */
    {
        for (int i = 0; i < 60; i++) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            char port_buf[8];
            snprintf(port_buf, sizeof(port_buf), "%u",
                     (unsigned)nostr_mock_server_get_port(g_mock));
            if (getaddrinfo("127.0.0.1", port_buf, &hints, &res) == 0 && res) {
                int probe = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                int ok = (probe >= 0 &&
                          connect(probe, res->ai_addr, res->ai_addrlen) == 0);
                if (probe >= 0) close(probe);
                freeaddrinfo(res);
                if (ok) break;
            }
            usleep(50 * 1000);
        }
    }

    int rc = 0, total = 0, passed = 0;
    #define RUN(fn) do { total++; \
        printf("Running %s...\n", #fn); \
        int r = fn(); \
        if (r == 0) { passed++; printf("  PASS\n"); } \
        else { rc = 1; printf("  FAIL\n"); } } while (0)

    printf("\n=== nostrc-fix-nip46-await-dispatch: QR handshake over a real WebSocket ===\n\n");
    RUN(test_qr_handshake_over_socket);
    RUN(test_qr_wrong_secret_over_socket);
    RUN(test_qr_timeout_over_socket);
    printf("\nResults: %d/%d passed\n", passed, total);
    nostr_mock_server_free(g_mock);
    return rc;
}
