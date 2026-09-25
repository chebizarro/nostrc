/*
 * test_hanami_server_capability_probe_shim.c
 *
 * Unit tests for the raw-vs-shim body-sniffer probe extension
 * (nostrc-prli). The existing probe (nostrc-ypn2) covers
 * server-tag / batch / strict-x-binding. This test file focuses on the
 * two ADDITIONAL uploads:
 *
 *   (e) raw random 1 KiB, single-x auth, default Content-Type ->
 *       raw_random_ok flip
 *   (f) shim(random 1 KiB), single-x auth, Content-Type: image/png ->
 *       png_shim_ok flip
 *
 * Verifies:
 *   - probe issues exactly 2 additional PUTs per server (E + F)
 *   - PUT (f)'s Content-Type header is "image/png" and its body begins
 *     with the shim's 41-byte PNG prefix
 *   - PUT (e)'s Content-Type header is "application/octet-stream"
 *   - three server response profiles cache correctly:
 *       (A) 201 raw + 201 shim -> raw_ok=YES, shim_ok=YES
 *       (B) 415 raw + 201 shim -> raw_ok=NO,  shim_ok=YES
 *       (C) 201 raw + 500 shim -> raw_ok=YES, shim_ok=NO (band-shape)
 *
 * The stub is a small extension of the batch test stub: it tracks the
 * full request buffer per PUT so the test can pull out the Content-Type
 * header and the body prefix.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-blossom-shim.h"
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

#define STUB_MAX_RECORDS 16

typedef struct {
    int    status_codes[STUB_MAX_RECORDS];
    size_t n_codes;

    int    listen_fd;
    int    port;
    pthread_t thr;
    atomic_int stop;
    atomic_int put_count;
    atomic_int head_count;
    atomic_int delete_count;

    /* Per-PUT recorded fields. */
    char *put_content_type[STUB_MAX_RECORDS];
    /* First 64 body bytes captured so we can memcmp against the shim
     * prefix without racing on a huge alloc. */
    unsigned char put_body_prefix[STUB_MAX_RECORDS][64];
    size_t        put_body_prefix_len[STUB_MAX_RECORDS];
} stub_t;

static void stub_init(stub_t *s)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->stop, 0);
    atomic_init(&s->put_count, 0);
    atomic_init(&s->head_count, 0);
    atomic_init(&s->delete_count, 0);
    s->listen_fd = -1;
}

static void stub_free_records(stub_t *s)
{
    for (int i = 0; i < STUB_MAX_RECORDS; i++) {
        free(s->put_content_type[i]);
        s->put_content_type[i] = NULL;
        s->put_body_prefix_len[i] = 0;
    }
}

/* Read a full HTTP request off `fd` (headers + body). */
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
        char buf[64 * 1024];
        ssize_t n = stub_read_request(fd, buf, sizeof buf);
        if (n <= 0) { close(fd); continue; }
        buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)sizeof(buf) - 1] = '\0';

        char method[16] = {0}, path[256] = {0};
        sscanf(buf, "%15s %255s", method, path);

        int status = 200;
        int idx = -1;
        if (strcmp(method, "PUT") == 0) {
            idx = atomic_fetch_add(&s->put_count, 1);
            if (idx < STUB_MAX_RECORDS) {
                /* Content-Type header. */
                char *ct = strcasestr(buf, "\r\ncontent-type:");
                if (ct) {
                    ct += strlen("\r\ncontent-type:");
                    while (*ct == ' ') ct++;
                    char *eol = strstr(ct, "\r\n");
                    if (eol) {
                        size_t l = (size_t)(eol - ct);
                        s->put_content_type[idx] = malloc(l + 1);
                        memcpy(s->put_content_type[idx], ct, l);
                        s->put_content_type[idx][l] = '\0';
                    }
                }
                /* Body prefix — copy first 64 bytes after the header
                 * terminator. */
                char *hdr_end = strstr(buf, "\r\n\r\n");
                if (hdr_end) {
                    hdr_end += 4;
                    size_t hdr_used = (size_t)(hdr_end - buf);
                    size_t body_avail = (size_t)n - hdr_used;
                    size_t copy = body_avail < 64 ? body_avail : 64;
                    memcpy(s->put_body_prefix[idx], hdr_end, copy);
                    s->put_body_prefix_len[idx] = copy;
                }
            }
            if (s->n_codes > 0) {
                size_t which = (size_t)idx;
                if (which >= s->n_codes) which = s->n_codes - 1;
                status = s->status_codes[which];
            }
        } else if (strcmp(method, "HEAD") == 0) {
            (void)atomic_fetch_add(&s->head_count, 1);
            status = 404;
        } else if (strcmp(method, "DELETE") == 0) {
            (void)atomic_fetch_add(&s->delete_count, 1);
            status = 200;
        }

        const char *reason = "OK";
        if (status == 201) reason = "Created";
        else if (status == 400) reason = "Bad Request";
        else if (status == 401) reason = "Unauthorized";
        else if (status == 415) reason = "Unsupported Media Type";
        else if (status == 500) reason = "Internal Server Error";
        else if (status == 404) reason = "Not Found";

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
    if (s->listen_fd >= 0) { shutdown(s->listen_fd, SHUT_RDWR); close(s->listen_fd); s->listen_fd = -1; }
    pthread_join(s->thr, NULL);
    stub_free_records(s);
}

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

/* Extended probe is at PUT indices 4 and 5:
 *   [0] server-tag h1
 *   [1] batch h1 of x=[h1,h2]
 *   [2] batch h2
 *   [3] strict-x (x=[h1], PUT h2)
 *   [4] RAW (new — nostrc-prli)
 *   [5] SHIM (new — nostrc-prli)
 */
#define STEP_E_INDEX 4
#define STEP_F_INDEX 5

static char *make_endpoint(int port)
{
    char *ep = malloc(64);
    snprintf(ep, 64, "http://127.0.0.1:%d", port);
    return ep;
}

/* Case A: 201 raw + 201 shim. Every existing probe step also 201s. */
static void test_probe_scenario_A_both_ok(void)
{
    hanami_blossom_shim_cache_reset();
    stub_t s; stub_init(&s);
    s.n_codes = 6;
    s.status_codes[0] = 201; /* server-tag */
    s.status_codes[1] = 201; /* batch h1 */
    s.status_codes[2] = 201; /* batch h2 */
    s.status_codes[3] = 415; /* strict-x mismatch — permissive */
    s.status_codes[4] = 201; /* raw   */
    s.status_codes[5] = 201; /* shim  */
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/A" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);

    hanami_error_t err = hanami_server_probe_capabilities(cli);
    assert(err == HANAMI_OK);

    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->raw_random_ok == HANAMI_CAP_YES);
    assert(caps->png_shim_ok   == HANAMI_CAP_YES);

    /* Reachability HEAD (1) + existing 4 PUTs + 2 new PUTs = 6 PUTs. */
    assert(atomic_load(&s.head_count) == 1);
    assert(atomic_load(&s.put_count) == 6);

    /* Shim probe (PUT 5) must have Content-Type: image/png and its body
     * MUST start with the 41-byte PNG shim prefix. */
    assert(s.put_content_type[STEP_F_INDEX] != NULL);
    assert(strcmp(s.put_content_type[STEP_F_INDEX], "image/png") == 0);
    /* Byte 0 == PNG signature start (0x89). */
    assert(s.put_body_prefix_len[STEP_F_INDEX] >= (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN);
    assert(s.put_body_prefix[STEP_F_INDEX][0] == 0x89);
    assert(s.put_body_prefix[STEP_F_INDEX][1] == 'P');
    assert(s.put_body_prefix[STEP_F_INDEX][2] == 'N');
    assert(s.put_body_prefix[STEP_F_INDEX][3] == 'G');

    /* Raw probe (PUT 4) MUST have Content-Type: application/octet-stream. */
    assert(s.put_content_type[STEP_E_INDEX] != NULL);
    assert(strcmp(s.put_content_type[STEP_E_INDEX], "application/octet-stream") == 0);
    /* Body must NOT begin with the PNG signature. */
    assert(s.put_body_prefix_len[STEP_E_INDEX] >= 4);
    assert(!(s.put_body_prefix[STEP_E_INDEX][0] == 0x89 &&
             s.put_body_prefix[STEP_E_INDEX][1] == 'P'));

    hanami_blossom_client_free(cli);
    free(endpoint);
    stub_stop(&s);
}

/* Case B: 415 raw + 201 shim — the primal.net shape from hy3e §2. */
static void test_probe_scenario_B_primal_like(void)
{
    hanami_blossom_shim_cache_reset();
    stub_t s; stub_init(&s);
    s.n_codes = 6;
    s.status_codes[0] = 201;
    s.status_codes[1] = 201;
    s.status_codes[2] = 201;
    s.status_codes[3] = 415;
    s.status_codes[4] = 415; /* raw rejected */
    s.status_codes[5] = 201; /* shim accepted */
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/B" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);
    assert(hanami_server_probe_capabilities(cli) == HANAMI_OK);

    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->raw_random_ok == HANAMI_CAP_NO);
    assert(caps->png_shim_ok   == HANAMI_CAP_YES);

    hanami_blossom_client_free(cli);
    free(endpoint);
    stub_stop(&s);
}

/* Case C: 201 raw + 500 shim — the blossom.band shape (full PNG decoder
 * blows up on the truncated IDAT). raw_ok=YES, png_shim_ok=NO. */
static void test_probe_scenario_C_band_like(void)
{
    hanami_blossom_shim_cache_reset();
    stub_t s; stub_init(&s);
    s.n_codes = 6;
    s.status_codes[0] = 201;
    s.status_codes[1] = 201;
    s.status_codes[2] = 201;
    s.status_codes[3] = 415;
    s.status_codes[4] = 201; /* raw ok — sharegap-shape */
    s.status_codes[5] = 500; /* shim decoded as PNG, blew up */
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/C" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);
    assert(hanami_server_probe_capabilities(cli) == HANAMI_OK);

    const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
    assert(caps->raw_random_ok == HANAMI_CAP_YES);
    assert(caps->png_shim_ok   == HANAMI_CAP_NO);

    hanami_blossom_client_free(cli);
    free(endpoint);
    stub_stop(&s);
}

/* Verify the probe extension respects the 2-additional-PUTs budget per
 * server (nostrc-prli constraint). */
static void test_probe_extension_budget(void)
{
    hanami_blossom_shim_cache_reset();
    stub_t s; stub_init(&s);
    s.n_codes = 6;
    for (int i = 0; i < 6; i++) s.status_codes[i] = 201;
    s.status_codes[3] = 415; /* strict-x check permissive */
    stub_start(&s);

    char *endpoint = make_endpoint(s.port);
    hanami_blossom_client_opts_t opts = { .endpoint = endpoint, .timeout_seconds = 5, .user_agent = "test/budget" };
    hanami_blossom_client_t *cli = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &cli) == HANAMI_OK);
    assert(hanami_server_probe_capabilities(cli) == HANAMI_OK);

    /* Exactly 6 total PUTs: 4 existing + 2 new. Never 7 or more. */
    assert(atomic_load(&s.put_count) == 6);

    hanami_blossom_client_free(cli);
    free(endpoint);
    stub_stop(&s);
}

int main(void)
{
    printf("libhanami server-capability probe (raw + shim) tests\n");
    printf("====================================================\n");

    TEST(probe_scenario_A_both_ok);
    TEST(probe_scenario_B_primal_like);
    TEST(probe_scenario_C_band_like);
    TEST(probe_extension_budget);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
