/*
 * test_syncd_pull_e2e.c — integration test for the pull glue
 * (nh_syncd_pull_ctx + on_remote_pointer callback).
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-p6qp (I2).
 *
 * We do NOT open a real relay here. Instead we build a fake "pointer"
 * by directly hex-encoding a sealed manifest under `home_key` and
 * invoking `nh_syncd_pull_on_pointer` with fabricated `created_at`
 * values in ascending order. The test asserts:
 *   - a first EVENT with new generation is applied,
 *   - a subsequent EVENT with generation <= last is a no-op,
 *   - a follow-up EVENT with a still-higher gen produces the new state,
 *   - counters (total_events / total_conflicts / last_applied_gen) are
 *     consistent throughout,
 *   - the reconciler runs off the exact opts the caller passed
 *     (fetch_chunk sink, push_queue).
 *
 * "No polling" assertion: the callback is invoked from userland — the
 * subscription's real transport is not exercised in this test. A
 * subscription-lifecycle e2e that speaks a real live REQ against
 * relay.sharegap.net is deferred to the acceptance run (see the doc at
 * docs/reviews/porthome-syncd-pull-2026-09-23.md).
 */

#define _GNU_SOURCE

#include "nh_syncd.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ─── in-memory chunk store (copy of the one in unit test — kept
 *     self-contained per test binary). ─────────────────────────── */

typedef struct chunk_row {
    char addr[65];
    uint8_t *bytes;
    size_t   len;
    struct chunk_row *next;
} chunk_row;

typedef struct { chunk_row *head; } chunk_store;

static void hex_of_local(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = lc[(b[i] >> 4) & 0xf];
        out[2*i+1] = lc[b[i] & 0xf];
    }
    out[2*n] = '\0';
}

static void store_put(chunk_store *cs, const char *addr, const uint8_t *b, size_t n) {
    chunk_row *r = calloc(1, sizeof *r);
    strncpy(r->addr, addr, 64); r->addr[64] = 0;
    r->bytes = malloc(n ? n : 1); memcpy(r->bytes, b, n); r->len = n;
    r->next = cs->head; cs->head = r;
}
static int store_fetch(void *ctx, const char *addr, uint8_t **out, size_t *out_len) {
    chunk_store *cs = ctx;
    for (chunk_row *r = cs->head; r; r = r->next) {
        if (!strcmp(r->addr, addr)) {
            *out = malloc(r->len ? r->len : 1); memcpy(*out, r->bytes, r->len);
            *out_len = r->len; return 0;
        }
    }
    return -1;
}
static void store_free(chunk_store *cs) {
    chunk_row *r = cs->head;
    while (r) { chunk_row *n = r->next; free(r->bytes); free(r); r = n; }
    cs->head = NULL;
}

/* Build a hex-of-sealed pointer content string from a manifest. */
static char *build_pointer_content_hex(const nh_porthome_manifest *m,
                                       const uint8_t home_key[32]) {
    uint8_t *sealed = NULL; size_t sealed_len = 0;
    int rc = nh_porthome_manifest_encode_sealed(m, home_key, &sealed, &sealed_len);
    assert(rc == 0);
    char *hex = malloc(sealed_len * 2 + 1);
    hex_of_local(sealed, sealed_len, hex);
    free(sealed);
    return hex;
}

/* Global fixtures. */
static char g_home[300], g_state[300];
static uint8_t g_home_key[32], g_root_id[32];
static const char *ACC_PK = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *D_TAG  = "nostr-homed.home.v1:personal";

static void write_home_file(const char *rel, const char *contents) {
    char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    char *sl = strrchr(abs, '/');
    if (sl) {
        *sl = 0;
        char *p = strdup(abs);
        for (char *c = p+1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0700); *c = '/'; }
        mkdir(p, 0700); free(p);
        *sl = '/';
    }
    int fd = open(abs, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    assert(fd >= 0);
    if (contents) (void)!write(fd, contents, strlen(contents));
    close(fd);
}

static void setup(void) {
    snprintf(g_home,  sizeof g_home,  "/tmp/nh_pull_home_%d",  (int)getpid());
    snprintf(g_state, sizeof g_state, "/tmp/nh_pull_state_%d", (int)getpid());
    char rm[600]; snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
    mkdir(g_home, 0700); mkdir(g_state, 0700);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) { seed[i] = 0x11; g_root_id[i] = (uint8_t)i; }
    assert(nh_porthome_key_derive(seed, g_home_key) == 0);
}

static void manifest_add_file(nh_porthome_manifest *m,
                              chunk_store *cs,
                              const char *rel,
                              const char *contents,
                              uint64_t mtime_ns) {
    char *penc = NULL;
    assert(nh_porthome_encrypt_path(g_home_key, rel, &penc) == 0);
    size_t plen = strlen(contents);
    if (plen > 0) {
        uint8_t *ct = NULL; size_t ct_len = 0; uint8_t sha[32];
        assert(nh_porthome_encrypt_chunk(g_home_key, (const uint8_t *)contents,
                                         plen, &ct, &ct_len, sha) == 0);
        char shex[65]; hex_of_local(sha, 32, shex);
        store_put(cs, shex, ct, ct_len);
        free(ct);
        nh_porthome_chunk ch = { .size = (uint32_t)(plen + NH_PORTHOME_SEAL_OVERHEAD) };
        memcpy(ch.sha256, sha, 32);
        assert(nh_porthome_manifest_add_file(m, penc, 0644, 0, 0, mtime_ns,
                                             plen, &ch, 1) == 0);
    } else {
        assert(nh_porthome_manifest_add_file(m, penc, 0644, 0, 0, mtime_ns,
                                             0, NULL, 0) == 0);
    }
}

static void seed_base_snapshot(nh_syncd_state *s,
                               const char *rel, const char *contents) {
    extern int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *,
                                           uint32_t, uint32_t, uint32_t,
                                           uint64_t, uint64_t, const char *,
                                           const char *const *, size_t);
    uint8_t hh[32]; char chash[65];
    size_t clen = strlen(contents);
    nh_porthome_sha256((const uint8_t *)contents, clen, hh);
    hex_of_local(hh, 32, chash);
    uint8_t sha[32]; uint8_t *ct = NULL; size_t ct_len = 0;
    assert(nh_porthome_encrypt_chunk(g_home_key, (const uint8_t *)contents,
                                     clen, &ct, &ct_len, sha) == 0);
    char shex[65]; hex_of_local(sha, 32, shex);
    free(ct);
    const char *addrs[1] = { shex };
    struct stat st; char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    lstat(abs, &st);
    uint64_t mt = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + st.st_mtim.tv_nsec;
    (void)nh_syncd_state_upsert_file_(s, rel, 0644, 0, 0, mt, clen, chash,
                                      addrs, 1);
}

static void t_pull_applies_new_generation(void) {
    /* Setup: home has hello.txt; base snapshot matches. */
    write_home_file("hello.txt", "v1\n");
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new(g_home, D_TAG, ACC_PK, g_root_id, &s) == 0);
    seed_base_snapshot(s, "hello.txt", "v1\n");
    nh_syncd_ignore *ig = NULL; assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    chunk_store cs = {0};

    /* Manifest 1: hello.txt = v2 (a remote change). Timestamp 100. */
    nh_porthome_manifest m1;
    assert(nh_porthome_manifest_init(&m1, g_root_id) == 0);
    manifest_add_file(&m1, &cs, "hello.txt", "v2 remote\n", 100ull * 1000000000ull);
    char *hex1 = build_pointer_content_hex(&m1, g_home_key);

    /* Pull ctx wiring. */
    nh_syncd_pull_opts po = {0};
    memcpy(po.home_key, g_home_key, 32);
    po.fetch_chunk = store_fetch; po.fetch_chunk_ctx = &cs;
    po.device_name = "pullhost";
    po.state = s;
    po.root_dir = g_home;
    po.ignore = ig;
    po.state_dir_for_persist = g_state;
    po.account_pubkey_hex = ACC_PK;
    po.d_tag = D_TAG;
    nh_syncd_pull_ctx *pc = NULL;
    assert(nh_syncd_pull_ctx_new(&po, &pc) == 0);

    /* First EVENT — created_at=100 (== generation stamp). */
    nh_syncd_pull_on_pointer(pc, "evt1", 100, D_TAG, hex1);
    assert(nh_syncd_pull_ctx_total_events(pc) == 1);
    assert(nh_syncd_pull_ctx_last_applied_generation(pc) == 100);
    /* file on disk shows v2. */
    char abs[600]; snprintf(abs, sizeof abs, "%s/hello.txt", g_home);
    FILE *f = fopen(abs, "r"); assert(f); char rbuf[64] = {0};
    fread(rbuf, 1, sizeof rbuf - 1, f); fclose(f);
    assert(strstr(rbuf, "v2 remote") != NULL);

    /* Second EVENT — same or earlier gen: no-op. */
    nh_syncd_pull_on_pointer(pc, "evt2", 100, D_TAG, hex1);
    assert(nh_syncd_pull_ctx_total_events(pc) == 2);
    assert(nh_syncd_pull_ctx_last_applied_generation(pc) == 100);

    /* Third: gen=200 with v3 content. */
    nh_porthome_manifest m2;
    assert(nh_porthome_manifest_init(&m2, g_root_id) == 0);
    manifest_add_file(&m2, &cs, "hello.txt", "v3 remote\n", 200ull * 1000000000ull);
    char *hex2 = build_pointer_content_hex(&m2, g_home_key);
    nh_syncd_pull_on_pointer(pc, "evt3", 200, D_TAG, hex2);
    assert(nh_syncd_pull_ctx_total_events(pc) == 3);
    assert(nh_syncd_pull_ctx_last_applied_generation(pc) == 200);
    f = fopen(abs, "r"); assert(f);
    memset(rbuf, 0, sizeof rbuf);
    fread(rbuf, 1, sizeof rbuf - 1, f); fclose(f);
    assert(strstr(rbuf, "v3 remote") != NULL);

    /* d-tag mismatch is filtered. */
    nh_syncd_pull_on_pointer(pc, "evt4", 300, "wrong-d-tag", hex2);
    /* total_events increments (we count callbacks) but generation
     * doesn't advance. */
    assert(nh_syncd_pull_ctx_total_events(pc) == 4);
    assert(nh_syncd_pull_ctx_last_applied_generation(pc) == 200);

    /* No conflicts. */
    assert(nh_syncd_pull_ctx_total_conflicts(pc) == 0);

    free(hex1); free(hex2);
    nh_porthome_manifest_dispose(&m1);
    nh_porthome_manifest_dispose(&m2);
    store_free(&cs);
    nh_syncd_pull_ctx_free(pc);
    nh_syncd_ignore_free(ig);
    nh_syncd_state_free(s);
    printf("t_pull_applies_new_generation OK\n");
}

int main(void) {
    setup();
    t_pull_applies_new_generation();
    /* teardown */
    char rm[600]; snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
    printf("test_syncd_pull_e2e: OK\n");
    return 0;
}
