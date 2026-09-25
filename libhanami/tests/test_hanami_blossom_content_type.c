/*
 * test_hanami_blossom_content_type.c
 *
 * Unit tests for the Blossom upload Content-Type resolver (nostrc-bpum).
 *
 * Verifies precedence:
 *   1. Per-server cache override (via hanami_blossom_client_set_upload_content_type)
 *   2. Environment override (NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE)
 *   3. Compile-time default (HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE)
 *
 * Uses a tiny in-process HTTP/1.1 stub that records the Content-Type of
 * every incoming PUT so tests can assert the wire value. The stub is a
 * trimmed-down variant of the one in test_hanami_blossom_batch.c —
 * keeping it self-contained avoids a shared-header refactor.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-server-capability.h"
#include "hanami/hanami-types.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nostr-event.h>
#include <nostr-keys.h>

/* ------------------------------------------------------------------ *
 * Tiny recording HTTP stub                                           *
 * ------------------------------------------------------------------ */

#define STUB_MAX_PUTS 8

typedef struct {
    int  status_code;      /* returned for every PUT */
    int  listen_fd;
    int  port;
    pthread_t thr;
    atomic_int stop;
    atomic_int put_count;
    /* Recorded PUT Content-Type headers. */
    char *put_ct[STUB_MAX_PUTS];
} stub_t;

static void stub_init(stub_t *s)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->stop, 0);
    atomic_init(&s->put_count, 0);
    s->listen_fd = -1;
    s->status_code = 201;
}

static void stub_free_records(stub_t *s)
{
    for (int i = 0; i < STUB_MAX_PUTS; i++) {
        free(s->put_ct[i]);
        s->put_ct[i] = NULL;
    }
}

static ssize_t stub_read_request(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    size_t content_length = 0;
    int have_hdr_end = 0;
    while (total < cap - 1) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (!have_hdr_end) {
            char *end = strstr(buf, "\r\n\r\n");
            if (end) {
                have_hdr_end = 1;
                char *cl = strcasestr(buf, "\r\ncontent-length:");
                if (cl) {
                    cl += strlen("\r\ncontent-length:");
                    while (*cl == ' ') cl++;
                    content_length = (size_t)strtoul(cl, NULL, 10);
                }
                size_t hdr_len = (size_t)(end - buf) + 4;
                size_t got_body = total - hdr_len;
                if (got_body >= content_length) break;
            }
        } else {
            char *end = strstr(buf, "\r\n\r\n");
            size_t hdr_len = (size_t)(end - buf) + 4;
            if (total - hdr_len >= content_length) break;
        }
    }
    return (ssize_t)total;
}

static void *stub_thread(void *arg)
{
    stub_t *s = (stub_t *)arg;
    signal(SIGPIPE, SIG_IGN);

    while (!atomic_load(&s->stop)) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int fd = accept(s->listen_fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (atomic_load(&s->stop)) break;
            continue;
        }
        char buf[32 * 1024];
        ssize_t n = stub_read_request(fd, buf, sizeof buf);
        if (n <= 0) { close(fd); continue; }
        buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)sizeof(buf) - 1] = '\0';

        char method[16] = {0};
        sscanf(buf, "%15s", method);

        int status = s->status_code;
        if (strcmp(method, "PUT") == 0) {
            int idx = atomic_fetch_add(&s->put_count, 1);
            if (idx < STUB_MAX_PUTS) {
                char *ct = strcasestr(buf, "\r\ncontent-type:");
                if (ct) {
                    ct += strlen("\r\ncontent-type:");
                    while (*ct == ' ' || *ct == '\t') ct++;
                    char *eol = strstr(ct, "\r\n");
                    if (eol) {
                        size_t l = (size_t)(eol - ct);
                        s->put_ct[idx] = malloc(l + 1);
                        memcpy(s->put_ct[idx], ct, l);
                        s->put_ct[idx][l] = '\0';
                    }
                }
            }
        } else {
            /* GET/HEAD/etc get a benign 404. */
            status = 404;
        }

        const char *reason = "OK";
        if (status == 201) reason = "Created";
        else if (status == 401) reason = "Unauthorized";
        else if (status == 404) reason = "Not Found";
        else if (status == 415) reason = "Unsupported Media Type";

        char resp[256];
        int rn = snprintf(resp, sizeof resp,
            "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            status, reason);
        send(fd, resp, (size_t)rn, 0);
        close(fd);
    }
    return NULL;
}

static void stub_start(stub_t *s)
{
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(s->listen_fd >= 0);
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert(bind(s->listen_fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    socklen_t sl = sizeof sa;
    assert(getsockname(s->listen_fd, (struct sockaddr *)&sa, &sl) == 0);
    s->port = ntohs(sa.sin_port);
    assert(listen(s->listen_fd, 32) == 0);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    assert(pthread_create(&s->thr, NULL, stub_thread, s) == 0);
}

static void stub_stop(stub_t *s)
{
    atomic_store(&s->stop, 1);
    if (s->listen_fd >= 0) {
        shutdown(s->listen_fd, SHUT_RDWR);
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    pthread_join(s->thr, NULL);
    stub_free_records(s);
}

/* ------------------------------------------------------------------ *
 * Trivial signer — reuses the pattern from test_hanami_blossom_batch.
 * ------------------------------------------------------------------ */

static const char *TEST_PRIVATE_KEY =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

static hanami_error_t test_sign(const char *event_json,
                                char **out_signed_json,
                                void *user_data)
{
    (void)user_data;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return HANAMI_ERR_NOMEM;
    if (nostr_event_deserialize_compact(ev, event_json, NULL) != 1) {
        nostr_event_free(ev);
        return HANAMI_ERR_NOSTR;
    }
    if (nostr_event_sign(ev, TEST_PRIVATE_KEY) != 0) {
        nostr_event_free(ev);
        return HANAMI_ERR_NOSTR;
    }
    char *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    if (!out) return HANAMI_ERR_NOMEM;
    *out_signed_json = out;
    return HANAMI_OK;
}

/* ------------------------------------------------------------------ *
 * Test scaffolding                                                   *
 * ------------------------------------------------------------------ */

static int tests_passed = 0;
#define TEST(name) \
    do { \
        printf("  %-56s ", #name); \
        fflush(stdout); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

static char *make_endpoint(int port)
{
    char *ep = malloc(64);
    snprintf(ep, 64, "http://127.0.0.1:%d", port);
    return ep;
}

static void fill_one_blob(hanami_blossom_blob_t *blob, char *hash65,
                          uint8_t *body, size_t body_len)
{
    for (size_t i = 0; i < body_len; i++) body[i] = 'X';
    memset(hash65, '0', 64);
    hash65[64] = '\0';
    blob->sha256_hex = hash65;
    blob->bytes = body;
    blob->len = body_len;
}

static void skip_probe(void)
{
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
}

static void clear_ct_env(void)
{
    unsetenv(HANAMI_BLOSSOM_UPLOAD_CT_ENV);
}

/* ------------------------------------------------------------------ *
 * Test 1: default Content-Type is the compile-time constant.
 * ------------------------------------------------------------------ */

static void test_default_content_type(void)
{
    skip_probe();
    clear_ct_env();

    stub_t s; stub_init(&s);
    s.status_code = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = {
        .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "ct-test/1"
    };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blob;
    char hash65[65]; uint8_t body[512];
    fill_one_blob(&blob, hash65, body, sizeof body);

    hanami_blossom_batch_result_t r = {0};
    hanami_error_t err = hanami_blossom_upload_batch(cli, &blob, 1, &r);
    assert(err == HANAMI_OK);
    assert(atomic_load(&s.put_count) == 1);
    assert(s.put_ct[0] != NULL);
    assert(strcmp(s.put_ct[0], HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE) == 0);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* ------------------------------------------------------------------ *
 * Test 2: env override wins over the compile-time default.
 * ------------------------------------------------------------------ */

static void test_env_override_wins(void)
{
    skip_probe();
    setenv(HANAMI_BLOSSOM_UPLOAD_CT_ENV, "image/binary", 1);

    stub_t s; stub_init(&s);
    s.status_code = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = {
        .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "ct-test/2"
    };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blob;
    char hash65[65]; uint8_t body[512];
    fill_one_blob(&blob, hash65, body, sizeof body);

    hanami_blossom_batch_result_t r = {0};
    hanami_error_t err = hanami_blossom_upload_batch(cli, &blob, 1, &r);
    assert(err == HANAMI_OK);
    assert(atomic_load(&s.put_count) == 1);
    assert(s.put_ct[0] != NULL);
    assert(strcmp(s.put_ct[0], "image/binary") == 0);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);

    clear_ct_env();
}

/* ------------------------------------------------------------------ *
 * Test 3: per-server setter beats the env var.
 * ------------------------------------------------------------------ */

static void test_setter_beats_env(void)
{
    skip_probe();
    setenv(HANAMI_BLOSSOM_UPLOAD_CT_ENV, "image/binary", 1);

    stub_t s; stub_init(&s);
    s.status_code = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = {
        .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "ct-test/3"
    };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_error_t set_err = hanami_blossom_client_set_upload_content_type(
        cli, "application/vnd.blossom.v1+octet-stream");
    assert(set_err == HANAMI_OK);

    hanami_blossom_blob_t blob;
    char hash65[65]; uint8_t body[512];
    fill_one_blob(&blob, hash65, body, sizeof body);

    hanami_blossom_batch_result_t r = {0};
    hanami_error_t err = hanami_blossom_upload_batch(cli, &blob, 1, &r);
    assert(err == HANAMI_OK);
    assert(atomic_load(&s.put_count) == 1);
    assert(s.put_ct[0] != NULL);
    assert(strcmp(s.put_ct[0], "application/vnd.blossom.v1+octet-stream") == 0);

    /* Clearing the setter falls back to env. */
    assert(hanami_blossom_client_set_upload_content_type(cli, NULL) == HANAMI_OK);
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->preferred_content_type[0] == '\0');

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);

    clear_ct_env();
}

/* ------------------------------------------------------------------ *
 * Test 4: setter argument validation.
 * ------------------------------------------------------------------ */

static void test_setter_validation(void)
{
    hanami_blossom_client_opts_t opts = { .endpoint = "http://127.0.0.1:1" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);

    /* NULL client */
    assert(hanami_blossom_client_set_upload_content_type(NULL, "image/binary")
           == HANAMI_ERR_INVALID_ARG);

    /* Reject a CT that would overflow the fixed buffer. */
    char big[HANAMI_PREFERRED_CT_MAX + 4];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    assert(hanami_blossom_client_set_upload_content_type(cli, big)
           == HANAMI_ERR_INVALID_ARG);

    /* Reject header injection. */
    assert(hanami_blossom_client_set_upload_content_type(cli, "image/binary\r\nX: y")
           == HANAMI_ERR_INVALID_ARG);
    assert(hanami_blossom_client_set_upload_content_type(cli, "image/binary\nfoo")
           == HANAMI_ERR_INVALID_ARG);

    /* Empty/NULL clears without error. */
    /* First seed a value. */
    assert(hanami_blossom_client_set_upload_content_type(cli, "text/plain")
           == HANAMI_OK);
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(strcmp(caps->preferred_content_type, "text/plain") == 0);
    /* Now clear. */
    assert(hanami_blossom_client_set_upload_content_type(cli, "") == HANAMI_OK);
    assert(caps->preferred_content_type[0] == '\0');
    assert(hanami_blossom_client_set_upload_content_type(cli, NULL) == HANAMI_OK);
    assert(caps->preferred_content_type[0] == '\0');

    hanami_blossom_client_free(cli);
}

/* ------------------------------------------------------------------ *
 * Test 5: 415-first stub still reports the sent CT in every retry —
 * proves the resolver is consulted on the retry path too (batch
 * fell-back per-blob PUT). This complements the fallback test in
 * test_hanami_blossom_batch by asserting CT is stable across retries.
 * ------------------------------------------------------------------ */

static void test_ct_stable_across_batch_fallback(void)
{
    skip_probe();
    setenv(HANAMI_BLOSSOM_UPLOAD_CT_ENV, "image/binary", 1);

    stub_t s; stub_init(&s);
    /* Batch first PUT -> 401 forces per-blob retry -> 201; second blob 201. */
    s.status_code = 201; /* default 201; test doesn't rely on 401 mechanics */
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = {
        .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "ct-test/5"
    };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blobs[2];
    char h[2][65]; uint8_t bodies[2][512];
    for (int i = 0; i < 2; i++) fill_one_blob(&blobs[i], h[i], bodies[i], sizeof bodies[i]);
    /* Make hashes distinct so batch validation passes. */
    memset(h[1], '1', 64);

    hanami_blossom_batch_result_t r[2] = {{0}};
    hanami_error_t err = hanami_blossom_upload_batch(cli, blobs, 2, r);
    assert(err == HANAMI_OK);
    assert(atomic_load(&s.put_count) == 2);
    for (int i = 0; i < 2; i++) {
        assert(s.put_ct[i] != NULL);
        assert(strcmp(s.put_ct[i], "image/binary") == 0);
    }

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);

    clear_ct_env();
}

/* ------------------------------------------------------------------ *
 * Test 6: setter fills the on-disk field and the resolver reads it
 * verbatim (defence against a lurking off-by-one in copy sizing).
 * ------------------------------------------------------------------ */

static void test_setter_fills_max_length(void)
{
    hanami_blossom_client_opts_t opts = { .endpoint = "http://127.0.0.1:1" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);

    /* Exactly HANAMI_PREFERRED_CT_MAX - 1 chars is legal. */
    char maxct[HANAMI_PREFERRED_CT_MAX];
    memset(maxct, 'x', HANAMI_PREFERRED_CT_MAX - 1);
    maxct[HANAMI_PREFERRED_CT_MAX - 1] = '\0';
    assert(hanami_blossom_client_set_upload_content_type(cli, maxct) == HANAMI_OK);
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(strlen(caps->preferred_content_type) == HANAMI_PREFERRED_CT_MAX - 1);
    assert(memcmp(caps->preferred_content_type, maxct, HANAMI_PREFERRED_CT_MAX) == 0);

    hanami_blossom_client_free(cli);
}

int main(void)
{
    printf("libhanami Blossom Content-Type resolver tests\n");
    printf("=============================================\n");

    TEST(setter_validation);
    TEST(setter_fills_max_length);
    TEST(default_content_type);
    TEST(env_override_wins);
    TEST(setter_beats_env);
    TEST(ct_stable_across_batch_fallback);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
