/*
 * test_hanami_blossom_batch.c
 *
 * Unit tests for hanami_blossom_upload_batch() + the session-scoped
 * capability cache (nostrc-xeby) and hanami_server_probe_capabilities()
 * (nostrc-ypn2).
 *
 * Uses a tiny in-process HTTP/1.1 stub bound to 127.0.0.1:<ephemeral>
 * that can be programmed to return different responses per request.
 * The stub speaks only what the tests exercise — enough to satisfy
 * libcurl's parser (Status-Line + a Content-Length: 0 body).
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-server-capability.h"
#include "hanami/hanami-types.h"
#include "hanami/hanami-bud02-auth.h"

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

/* ------------------------------------------------------------------ *
 * Tiny programmable HTTP stub                                        *
 * ------------------------------------------------------------------ */

typedef struct {
    /* Programmed response sequence: for the Nth PUT (0-indexed), return
     * status_codes[N]. If N >= n_codes, use last code repeatedly. */
    int    status_codes[16];
    size_t n_codes;

    int    listen_fd;
    int    port;
    pthread_t thr;
    atomic_int stop;
    atomic_int put_count;
    atomic_int head_count;
    atomic_int delete_count;

    /* Recorded PUT auth headers (first N=8 requests). */
    char *put_auth[8];
    /* Recorded PUT path (URL after the "PUT "). */
    char *put_path[8];
} stub_t;

static void stub_init(stub_t *s)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->stop, 0);
    atomic_init(&s->put_count, 0);
    atomic_init(&s->head_count, 0);
    atomic_init(&s->delete_count, 0);
    s->listen_fd = -1;
    s->port = 0;
}

static void stub_free_records(stub_t *s)
{
    for (int i = 0; i < 8; i++) {
        free(s->put_auth[i]); s->put_auth[i] = NULL;
        free(s->put_path[i]); s->put_path[i] = NULL;
    }
}

/* Read a full HTTP request off `fd` (headers + body). Best-effort; the
 * stub is small and only handles requests small enough to fit into 32
 * KiB in one recv burst. */
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
                /* Look for Content-Length. */
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
            /* count body bytes; break if we have them all */
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
    /* Ignore SIGPIPE so a short-lived client doesn't kill the stub. */
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

        /* Parse method + path. */
        char method[16] = {0}, path[256] = {0};
        sscanf(buf, "%15s %255s", method, path);

        int status = 200;
        int idx = -1;
        if (strcmp(method, "PUT") == 0) {
            idx = atomic_fetch_add(&s->put_count, 1);
            if (idx < 8) {
                /* Record auth header. */
                char *ah = strcasestr(buf, "\r\nauthorization:");
                if (ah) {
                    ah += strlen("\r\nauthorization:");
                    while (*ah == ' ') ah++;
                    char *eol = strstr(ah, "\r\n");
                    if (eol) {
                        size_t l = (size_t)(eol - ah);
                        s->put_auth[idx] = malloc(l + 1);
                        memcpy(s->put_auth[idx], ah, l);
                        s->put_auth[idx][l] = '\0';
                    }
                }
                s->put_path[idx] = strdup(path);
            }
            if (s->n_codes > 0) {
                size_t which = (size_t)idx;
                if (which >= s->n_codes) which = s->n_codes - 1;
                status = s->status_codes[which];
            }
        } else if (strcmp(method, "HEAD") == 0) {
            (void)atomic_fetch_add(&s->head_count, 1);
            status = 404; /* live but no such blob */
        } else if (strcmp(method, "DELETE") == 0) {
            (void)atomic_fetch_add(&s->delete_count, 1);
            status = 200;
        }

        const char *reason = "OK";
        if (status == 201) reason = "Created";
        else if (status == 401) reason = "Unauthorized";
        else if (status == 403) reason = "Forbidden";
        else if (status == 404) reason = "Not Found";
        else if (status == 415) reason = "Unsupported Media Type";
        else if (status == 500) reason = "Internal Server Error";

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
    /* Set a short accept timeout so we can shut the thread down. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    assert(pthread_create(&s->thr, NULL, stub_thread, s) == 0);
}

static void stub_stop(stub_t *s)
{
    atomic_store(&s->stop, 1);
    if (s->listen_fd >= 0) { shutdown(s->listen_fd, SHUT_RDWR); close(s->listen_fd); s->listen_fd = -1; }
    pthread_join(s->thr, NULL);
    stub_free_records(s);
}

/* ------------------------------------------------------------------ *
 * Test signer — mints a syntactically-valid "signature" by leaving
 * the event's fields intact and adding the id + sig we compute here.
 * The stub does not verify signatures, so a trivial passthrough sign
 * is enough. We reuse hanami_bud02_create_auth_header on the returned
 * event, so the JSON MUST be a legal event with id + sig fields.     *
 * ------------------------------------------------------------------ */

#include <nostr-event.h>
#include <nostr-tag.h>
#include <nostr-kinds.h>

static const char *TEST_PRIVATE_KEY =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

/* Signer that parses the unsigned JSON into a NostrEvent, calls
 * nostr_event_sign() with the fixed test key, and re-serialises. */
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

/* Derive our test pubkey once so we can pass it in the signer struct. */
#include <nostr-keys.h>

/* ------------------------------------------------------------------ *
 * Tests                                                              *
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

/* Bypass the auto-probe — every test wants deterministic PUT counts. */
static void skip_probe_env(void)
{
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
}

/* Fill `hashes` with N deterministic 64-hex sha256 strings and give
 * back 1 KiB blobs (in caller-supplied storage). */
static void fill_blobs(hanami_blossom_blob_t *blobs, char (*hashes)[65],
                       uint8_t (*bodies)[1024], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        memset(bodies[i], (int)('A' + (int)i), 1024);
        /* Hash pattern: 64 hex chars — just repeat digit(i) */
        char c = (char)('0' + ((int)i % 10));
        memset(hashes[i], c, 64);
        hashes[i][64] = '\0';
        blobs[i].sha256_hex = hashes[i];
        blobs[i].bytes = bodies[i];
        blobs[i].len = 1024;
    }
}

/* --- 1. Batch happy path: all N PUTs succeed with the shared header. */
static void test_batch_all_succeed(void)
{
    skip_probe_env();
    stub_t s; stub_init(&s);
    s.n_codes = 1; s.status_codes[0] = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    assert(pk != NULL);

    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/1" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blobs[3];
    char hashes[3][65];
    uint8_t bodies[3][1024];
    fill_blobs(blobs, hashes, bodies, 3);

    hanami_blossom_batch_result_t results[3] = {{0}};
    hanami_error_t err = hanami_blossom_upload_batch(cli, blobs, 3, results);
    assert(err == HANAMI_OK);
    for (int i = 0; i < 3; i++) {
        assert(results[i].error == HANAMI_OK);
        assert(results[i].http_status == 201);
        assert(results[i].error_class == NULL);
    }
    /* All three PUTs used the SAME auth header (batch shape). */
    assert(atomic_load(&s.put_count) == 3);
    assert(s.put_auth[0] && s.put_auth[1] && s.put_auth[2]);
    assert(strcmp(s.put_auth[0], s.put_auth[1]) == 0);
    assert(strcmp(s.put_auth[1], s.put_auth[2]) == 0);
    /* Capability cache flipped to YES on success. */
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->batch_ok == HANAMI_CAP_YES);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* --- 2. Fallback: server 201s the first PUT, 401s the second, 201s
 *      the third. The batch header should be dropped after the 401 and
 *      per-blob auth used for blob 2 (retry) and blob 3. Result classes
 *      "batch-fell-back" on blobs 2 and 3. Cache flipped to batch_ok=NO. */
static void test_batch_fallback_on_401(void)
{
    skip_probe_env();
    stub_t s; stub_init(&s);
    /* Sequence:
     *   PUT #0 (batch header)      -> 201
     *   PUT #1 (batch header)      -> 401  (triggers fallback)
     *   PUT #2 (per-blob retry #1) -> 201
     *   PUT #3 (per-blob blob #2)  -> 201
     */
    s.n_codes = 4;
    s.status_codes[0] = 201;
    s.status_codes[1] = 401;
    s.status_codes[2] = 201;
    s.status_codes[3] = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/2" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blobs[3];
    char hashes[3][65];
    uint8_t bodies[3][1024];
    fill_blobs(blobs, hashes, bodies, 3);

    hanami_blossom_batch_result_t results[3] = {{0}};
    hanami_error_t err = hanami_blossom_upload_batch(cli, blobs, 3, results);
    assert(err == HANAMI_OK); /* every blob ultimately succeeded */

    /* Blob 0: succeeded under batch header, no fell-back tag. */
    assert(results[0].error == HANAMI_OK);
    assert(results[0].http_status == 201);
    assert(results[0].error_class == NULL);
    /* Blob 1: 401 on batch, retried with per-blob header — succeeded. */
    assert(results[1].error == HANAMI_OK);
    assert(results[1].http_status == 201);
    assert(results[1].error_class != NULL &&
           strcmp(results[1].error_class, "batch-fell-back") == 0);
    /* Blob 2: went direct to per-blob path after fallback. */
    assert(results[2].error == HANAMI_OK);
    assert(results[2].http_status == 201);
    assert(results[2].error_class != NULL &&
           strcmp(results[2].error_class, "batch-fell-back") == 0);

    /* Exactly 4 PUTs total: 2 attempts on blob 1 (batch + per-blob retry). */
    assert(atomic_load(&s.put_count) == 4);
    /* PUTs 0 and 1 share the same batch header. */
    assert(strcmp(s.put_auth[0], s.put_auth[1]) == 0);
    /* PUT 2 (the retry on blob 1) uses a DIFFERENT header. */
    assert(strcmp(s.put_auth[1], s.put_auth[2]) != 0);
    /* PUT 3 (blob 2) also uses a per-blob header, DIFFERENT from batch. */
    assert(strcmp(s.put_auth[0], s.put_auth[3]) != 0);

    /* Capability cache: batch_ok flipped to NO; last_401_ts non-zero. */
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->batch_ok == HANAMI_CAP_NO);
    assert(caps->last_401_ts != 0);

    /* --- Subsequent batch call MUST bypass the batch header. --- */
    hanami_blossom_batch_result_t results2[2] = {{0}};
    hanami_blossom_blob_t blobs2[2];
    char hashes2[2][65];
    uint8_t bodies2[2][1024];
    fill_blobs(blobs2, hashes2, bodies2, 2);
    /* Reset stub codes: all 201. */
    atomic_store(&s.put_count, 0);
    stub_free_records(&s);
    s.n_codes = 1; s.status_codes[0] = 201;

    err = hanami_blossom_upload_batch(cli, blobs2, 2, results2);
    assert(err == HANAMI_OK);
    assert(atomic_load(&s.put_count) == 2);
    /* Two PUTs, each with a DIFFERENT per-blob header (no batch header). */
    assert(strcmp(s.put_auth[0], s.put_auth[1]) != 0);
    /* No blob should still be flagged "batch-fell-back" — that class is
     * only used when we ACTUALLY fell back within a call. Direct-per-blob
     * calls after the cache flipped return NULL error_class on success. */
    assert(results2[0].error_class == NULL);
    assert(results2[1].error_class == NULL);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* --- 3. Env kill-switch skips probe: with a fresh client and env=1,
 *      the FIRST batch call must not trigger any HEAD / PUT prior to
 *      the actual batch PUTs. */
static void test_probe_kill_switch(void)
{
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
    stub_t s; stub_init(&s);
    s.n_codes = 1; s.status_codes[0] = 201;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/3" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_blossom_blob_t blobs[2];
    char hashes[2][65];
    uint8_t bodies[2][1024];
    fill_blobs(blobs, hashes, bodies, 2);

    hanami_blossom_batch_result_t results[2] = {{0}};
    hanami_error_t err = hanami_blossom_upload_batch(cli, blobs, 2, results);
    assert(err == HANAMI_OK);

    /* With kill-switch: exactly 2 PUTs (no probe HEAD, no probe PUTs). */
    assert(atomic_load(&s.head_count) == 0);
    assert(atomic_load(&s.put_count) == 2);
    /* last_probe_ts stays 0 — probe skipped. */
    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->last_probe_ts == 0);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* --- 4. Explicit probe RUNS and records capabilities. Stub programmed:
 *      HEAD (reach)                          -> 404 (reachable=true)
 *      PUT probe(b): server-tag h1 (1 x-tag) -> 201 (server_tag_ok=YES)
 *      PUT probe(c): batch h1 (of x=[h1,h2]) -> 201
 *      PUT probe(c): batch h2                -> 201 (batch_ok=YES)
 *      PUT probe(d): mismatch (x=[h1], PUT h2) -> 401 (strict_x=YES)
 *      then optional DELETEs.
 */
static void test_probe_records_capabilities(void)
{
    /* Kill-switch: keep test focused on the explicit probe call, not
     * the auto-probe path in upload_batch. */
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
    stub_t s; stub_init(&s);
    /* PUT sequence for the probe:
     *   [0]=201 (server-tag), [1]=201 (batch h1), [2]=201 (batch h2), [3]=401 (mismatch) */
    s.n_codes = 4;
    s.status_codes[0] = 201;
    s.status_codes[1] = 201;
    s.status_codes[2] = 201;
    s.status_codes[3] = 401;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/4" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_error_t err = hanami_server_probe_capabilities(cli);
    assert(err == HANAMI_OK);

    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->last_probe_ts != 0);
    assert(caps->reachable == true);
    assert(caps->server_tag_ok == HANAMI_CAP_YES);
    assert(caps->batch_ok == HANAMI_CAP_YES);
    assert(caps->strict_x_binding == HANAMI_CAP_YES);
    /* Reachability HEAD (1) + 4 probe PUTs. Deletes are best-effort. */
    assert(atomic_load(&s.head_count) == 1);
    assert(atomic_load(&s.put_count) == 4);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* --- 5. Probe recovers a "batch rejected + permissive x-binding" server
 *      (matches the blossom.band-like matrix line: server_tag NO,
 *       batch NO — 401, strict_x NO — 415 permissive). */
static void test_probe_records_permissive_shape(void)
{
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
    stub_t s; stub_init(&s);
    /* PUT sequence:
     *   [0]=401 server-tag rejected  (server_tag_ok=NO)
     *   [1]=401 batch (first blob)   (batch_ok=NO, second blob skipped in effect since we always PUT — still 401 or whatever)
     *   [2]=401 batch (second blob)
     *   [3]=415 mismatch permissive  (strict_x=NO)
     */
    s.n_codes = 4;
    s.status_codes[0] = 401;
    s.status_codes[1] = 401;
    s.status_codes[2] = 401;
    s.status_codes[3] = 415;
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    char *pk = nostr_key_get_public(TEST_PRIVATE_KEY);
    hanami_signer_t signer = { .pubkey = pk, .sign = test_sign, .user_data = NULL };
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/5" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &cli) == HANAMI_OK);

    hanami_error_t err = hanami_server_probe_capabilities(cli);
    assert(err == HANAMI_OK);

    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->server_tag_ok == HANAMI_CAP_NO);
    assert(caps->batch_ok == HANAMI_CAP_NO);
    assert(caps->strict_x_binding == HANAMI_CAP_NO);

    hanami_blossom_client_free(cli);
    free(pk);
    free(endpoint);
    stub_stop(&s);
}

/* --- 6. Capability init sanity. */
static void test_capability_init(void)
{
    hanami_server_capabilities_t caps;
    memset(&caps, 0xFF, sizeof caps);
    hanami_server_capabilities_init(&caps);
    assert(caps.batch_ok == HANAMI_CAP_UNKNOWN);
    assert(caps.server_tag_ok == HANAMI_CAP_UNKNOWN);
    assert(caps.strict_x_binding == HANAMI_CAP_UNKNOWN);
    assert(caps.last_probe_ts == 0);
    assert(caps.last_401_ts == 0);
    assert(caps.reachable == false);
    assert(strcmp(hanami_capability_state_str(HANAMI_CAP_YES), "yes") == 0);
    assert(strcmp(hanami_capability_state_str(HANAMI_CAP_NO), "no") == 0);
    assert(strcmp(hanami_capability_state_str(HANAMI_CAP_UNKNOWN), "unknown") == 0);
}

/* --- 7. Invalid args are refused up-front. */
static void test_upload_batch_arg_validation(void)
{
    hanami_blossom_batch_result_t r[1] = {{0}};
    hanami_blossom_blob_t b = { .sha256_hex = "short", .bytes = (const uint8_t *)"x", .len = 1 };
    /* No client */
    assert(hanami_blossom_upload_batch(NULL, &b, 1, r) == HANAMI_ERR_INVALID_ARG);
    /* No blobs */
    hanami_blossom_client_opts_t opts = { .endpoint = "http://127.0.0.1:1" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);
    /* No signer -> AUTH */
    assert(hanami_blossom_upload_batch(cli, &b, 1, r) == HANAMI_ERR_AUTH);
    hanami_blossom_client_free(cli);
}

int main(void)
{
    printf("libhanami Blossom batch + probe tests\n");
    printf("=====================================\n");

    TEST(capability_init);
    TEST(upload_batch_arg_validation);
    TEST(batch_all_succeed);
    TEST(batch_fallback_on_401);
    TEST(probe_kill_switch);
    TEST(probe_records_capabilities);
    TEST(probe_records_permissive_shape);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
