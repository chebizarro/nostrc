/*
 * test_syncd_pusher_multi_server.c — xnxd part 1 coverage.
 *
 * SPDX-License-Identifier: MIT
 *
 * Stubs three Blossom servers via the pusher's `upload_to_server_fn`
 * seam (see nh_syncd.h). Two accept, one refuses. Asserts:
 *   1. min_replication=2 → push OK, worst.succeeded == 2.
 *   2. min_replication=3 → push returns NH_SYNCD_ERR_INSUFFICIENT_REPLICATION
 *      AND the local generation was NOT advanced.
 *   3. min_replication=1 → push OK with a single accept.
 *   4. All three servers refusing → NH_SYNCD_ERR_UPLOAD, generation stays.
 */

#include "nh_syncd.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *SRV[] = {
    "https://blossom-a.example.org",
    "https://blossom-b.example.org",
    "https://blossom-c.example.org",
};
#define N_SRV 3

/* Per-URL disposition table (1 = accept, 0 = refuse). */
static int g_accept[N_SRV] = { 1, 1, 0 };
static int g_calls[N_SRV] = { 0, 0, 0 };

static int stub_upload(void *ud, const char *url,
                       const uint8_t *ct, size_t ct_len,
                       const char *sha, int *http, uint64_t *ms)
{
    (void)ud; (void)ct; (void)sha;
    if (ms) *ms = 5; /* fake latency */
    for (size_t i = 0; i < N_SRV; i++) {
        if (!strcmp(url, SRV[i])) {
            g_calls[i]++;
            if (g_accept[i]) {
                if (http) *http = 200;
                return 0;
            } else {
                if (http) *http = 502;
                return -1;
            }
        }
    }
    (void)ct_len;
    if (http) *http = 500;
    return -1;
}

static nh_syncd_upload_result g_last_summary;
static void capture_summary(void *ud, const nh_syncd_upload_result *r) {
    (void)ud;
    g_last_summary = *r;
}

static int mock_publish(void *ud, const char *pk, int64_t cat,
                        const char *dtag,
                        const uint8_t *sealed, size_t sealed_len,
                        char out_event_id[65])
{
    (void)ud; (void)pk; (void)cat; (void)dtag; (void)sealed; (void)sealed_len;
    memcpy(out_event_id,
           "deadbeef000000000000000000000000000000000000000000000000000000ab",
           65);
    return 0;
}

static char g_home[300];
static char g_state[300];

static void write_file(const char *rel, const char *body) {
    char abs[600];
    snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    FILE *f = fopen(abs, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void setup(void) {
    snprintf(g_home,  sizeof g_home,  "/tmp/nh_multi_home_%d",  (int)getpid());
    snprintf(g_state, sizeof g_state, "/tmp/nh_multi_state_%d", (int)getpid());
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
    assert(mkdir(g_home, 0700) == 0);
    assert(mkdir(g_state, 0700) == 0);
    write_file("payload.txt", "hello multi-server world\n");
}
static void teardown(void) {
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
}

static uint64_t vnow(void *ud) { (void)ud; static uint64_t t = 0; t += 10ull*1000000000ull; return t; }

static const char *FIXTURE_NSEC = "1111111111111111111111111111111111111111111111111111111111111111";
static const char *FIXTURE_PK   = "0000000000000000000000000000000000000000000000000000000000000001";

static int drive_push(size_t min_repl, int a0, int a1, int a2, uint64_t *out_gen_after) {
    g_accept[0] = a0; g_accept[1] = a1; g_accept[2] = a2;
    g_calls[0] = g_calls[1] = g_calls[2] = 0;
    memset(&g_last_summary, 0, sizeof g_last_summary);

    uint8_t root[32];
    for (int i = 0; i < 32; i++) root[i] = (uint8_t)(i + 1);
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "nostr-homed.home.v1:personal",
                              FIXTURE_PK, root, &state) == 0);
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(vnow, NULL, 1, 1, &b) == 0);
    assert(nh_syncd_batcher_push(b, "payload.txt", NH_SYNCD_CHANGE_CREATE) == 0);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);
    assert(bat != NULL);

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    nh_syncd_push_cfg pc = {0};
    assert(nh_porthome_key_derive(seed, pc.home_key) == 0);
    memcpy(pc.root_id, seed, 32);
    pc.blossom_servers   = SRV;
    pc.n_blossom_servers = N_SRV;
    pc.min_replication   = min_repl;
    pc.upload_to_server_fn  = stub_upload;
    pc.on_upload_summary    = capture_summary;
    pc.event_publish_fn     = mock_publish;
    pc.event_signer_nsec_hex = FIXTURE_NSEC;
    pc.account_pubkey_hex_override = FIXTURE_PK;
    pc.d_tag = "nostr-homed.home.v1:personal";

    nh_syncd_interlocks ilk = { g_home, NULL };
    char *emsg = NULL;
    int rc = nh_syncd_push_batch(&pc, state, bat, g_home, ig, &ilk,
                                 g_state, &emsg);
    if (out_gen_after)
        *out_gen_after = nh_syncd_state_get_local_generation(state);
    free(emsg);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    nh_syncd_ignore_free(ig);
    nh_syncd_state_free(state);
    return rc;
}

static void t_two_of_three_quorum_ok(void) {
    setup();
    uint64_t gen = 0;
    int rc = drive_push(2, 1, 1, 0, &gen);
    assert(rc == NH_SYNCD_OK);
    assert(g_last_summary.total     == N_SRV);
    assert(g_last_summary.succeeded == 2);
    assert(gen == 1);
    /* Per-server outcome: first two OK, third has non-zero error_class. */
    assert(g_last_summary.per_server[0].error_class == 0);
    assert(g_last_summary.per_server[0].http_status == 200);
    assert(g_last_summary.per_server[1].error_class == 0);
    assert(g_last_summary.per_server[2].error_class == NH_SYNCD_ERR_UPLOAD);
    assert(g_last_summary.per_server[2].http_status == 502);
    /* All three attempted. */
    assert(g_calls[0] == 1 && g_calls[1] == 1 && g_calls[2] == 1);
    teardown();
    printf("t_two_of_three_quorum_ok OK\n");
}

static void t_three_needed_but_only_two_ok(void) {
    setup();
    uint64_t gen = 42; /* preseed sentinel */
    int rc = drive_push(3, 1, 1, 0, &gen);
    assert(rc == NH_SYNCD_ERR_INSUFFICIENT_REPLICATION);
    /* Generation MUST NOT advance on quorum miss. */
    assert(gen == 0);
    assert(g_last_summary.succeeded == 2);
    teardown();
    printf("t_three_needed_but_only_two_ok OK\n");
}

static void t_min_repl_one_single_ok(void) {
    setup();
    /* Make ONLY the third accept. */
    uint64_t gen = 0;
    int rc = drive_push(1 /*min_repl*/, 0, 0, 1, &gen);
    assert(rc == NH_SYNCD_OK);
    assert(g_last_summary.succeeded == 1);
    assert(gen == 1);
    teardown();
    printf("t_min_repl_one_single_ok OK\n");
}

static void t_all_refuse_returns_upload_error(void) {
    setup();
    uint64_t gen = 42;
    int rc = drive_push(2, 0, 0, 0, &gen);
    assert(rc == NH_SYNCD_ERR_UPLOAD);
    /* Generation still zero — push refused. */
    assert(gen == 0);
    assert(g_last_summary.succeeded == 0);
    teardown();
    printf("t_all_refuse_returns_upload_error OK\n");
}

int main(void) {
    t_two_of_three_quorum_ok();
    t_three_needed_but_only_two_ok();
    t_min_repl_one_single_ok();
    t_all_refuse_returns_upload_error();
    printf("test_syncd_pusher_multi_server: ALL OK\n");
    return 0;
}
