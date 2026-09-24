/*
 * test_syncd_reconcile.c — unit tests for I2 three-way reconciler.
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-p6qp (I2).
 *
 * Fake fetch sink: an in-process dict from sha256_hex → sealed bytes.
 * Every "remote fixture" first encrypts the intended plaintext under
 * home_key using nh_porthome_encrypt_chunk, capturing the sealed bytes
 * and the sha256 address. Then the manifest is constructed pointing at
 * those addresses. When the reconciler calls fetch_chunk we return the
 * stored bytes.
 *
 * These tests exercise the four scenarios in the plan:
 *   1. remote-only change             → applied
 *   2. local-only change              → I1 batcher receives it
 *   3. changed-both                   → LWW conflict + notification
 *   4. remote-delete of local-modified → local preserved + marker
 *   5. unknown base (empty snapshot) → additive rescan + partial marker
 */

#define _GNU_SOURCE

#include "nh_syncd.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ────────── in-memory chunk store (fake Blossom) ─────────────────── */

typedef struct chunk_row {
    char addr_hex[65];
    uint8_t *bytes;
    size_t   len;
    struct chunk_row *next;
} chunk_row;

typedef struct {
    chunk_row *head;
} chunk_store;

static void store_put(chunk_store *cs, const char *addr, const uint8_t *b, size_t n) {
    chunk_row *r = calloc(1, sizeof *r);
    strncpy(r->addr_hex, addr, 64);
    r->addr_hex[64] = '\0';
    r->bytes = malloc(n ? n : 1);
    memcpy(r->bytes, b, n);
    r->len = n;
    r->next = cs->head;
    cs->head = r;
}

static int store_fetch_cb(void *ctx, const char *addr,
                          uint8_t **out, size_t *out_len) {
    chunk_store *cs = (chunk_store *)ctx;
    for (chunk_row *r = cs->head; r; r = r->next) {
        if (!strcmp(r->addr_hex, addr)) {
            *out = malloc(r->len ? r->len : 1);
            memcpy(*out, r->bytes, r->len);
            *out_len = r->len;
            return 0;
        }
    }
    return -1;
}

static void store_free(chunk_store *cs) {
    chunk_row *r = cs->head;
    while (r) { chunk_row *n = r->next; free(r->bytes); free(r); r = n; }
    cs->head = NULL;
}

/* ────────── shared fixtures ───────────────────────────────────── */

static char g_home[300];
static char g_state[300];
static uint8_t g_home_key[32];
static uint8_t g_root_id[32];

static void hex_of_local(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = lc[(b[i] >> 4) & 0xf];
        out[i * 2 + 1] = lc[b[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static void write_home_file(const char *rel, const char *contents, mode_t mode) {
    char abs[600];
    snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    char *slash = strrchr(abs, '/');
    if (slash) {
        *slash = '\0';
        char *p = strdup(abs);
        for (char *c = p + 1; *c; c++) {
            if (*c == '/') { *c = '\0'; mkdir(p, 0700); *c = '/'; }
        }
        mkdir(p, 0700);
        free(p);
        *slash = '/';
    }
    int fd = open(abs, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    if (contents) (void)!write(fd, contents, strlen(contents));
    close(fd);
}

static int slurp_home_file(const char *rel, char **out, size_t *out_len) {
    char abs[600];
    snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    FILE *f = fopen(abs, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    fread(buf, 1, (size_t)n, f); buf[n] = '\0';
    fclose(f);
    *out = buf; *out_len = (size_t)n;
    return 0;
}

static uint64_t stat_mtime_ns(const char *rel) {
    char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    struct stat s; if (lstat(abs, &s) != 0) return 0;
    return (uint64_t)s.st_mtim.tv_sec * 1000000000ull + (uint64_t)s.st_mtim.tv_nsec;
}

static void set_home_mtime_ns(const char *rel, uint64_t ns) {
    char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    struct timespec ts[2];
    ts[0].tv_sec = ns / 1000000000ull;
    ts[0].tv_nsec = ns % 1000000000ull;
    ts[1] = ts[0];
    (void)utimensat(AT_FDCWD, abs, ts, AT_SYMLINK_NOFOLLOW);
}

/* Encrypt `pt` bytes under home_key and put into store; return the
 * sha256 as 32-byte + fill hex64. */
static void seal_and_store(chunk_store *cs, const uint8_t *pt, size_t pt_len,
                           uint8_t sha[32], char sha_hex[65]) {
    uint8_t *ct = NULL; size_t ct_len = 0;
    int rc = nh_porthome_encrypt_chunk(g_home_key, pt, pt_len, &ct, &ct_len, sha);
    assert(rc == 0);
    hex_of_local(sha, 32, sha_hex);
    store_put(cs, sha_hex, ct, ct_len);
    free(ct);
}

/* Add a file entry to a manifest describing plaintext `contents` under
 * plaintext `rel` (auto-encrypts path). */
static void manifest_add_pt_file(nh_porthome_manifest *m,
                                 chunk_store *cs,
                                 const char *rel,
                                 const char *contents,
                                 uint32_t mode,
                                 uint64_t mtime_ns) {
    char *penc = NULL;
    assert(nh_porthome_encrypt_path(g_home_key, rel, &penc) == 0);
    size_t plen = strlen(contents);
    nh_porthome_chunk ch = {0};
    if (plen > 0) {
        char sha_hex[65];
        seal_and_store(cs, (const uint8_t *)contents, plen, ch.sha256, sha_hex);
        ch.size = (uint32_t)(plen + NH_PORTHOME_SEAL_OVERHEAD);
    }
    if (plen > 0)
        assert(nh_porthome_manifest_add_file(m, penc, mode, 0, 0, mtime_ns,
                                             plen, &ch, 1) == 0);
    else
        assert(nh_porthome_manifest_add_file(m, penc, mode, 0, 0, mtime_ns,
                                             0, NULL, 0) == 0);
}

/* Base state: preload snapshot with the current $HOME layout. */
static nh_syncd_state *make_base_state(const char *const *rels, size_t n,
                                       const char *const *contents) {
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new(g_home, "nostr-homed.home.v1:personal",
                              "0000000000000000000000000000000000000000000000000000000000000001",
                              g_root_id, &s) == 0);
    /* We use the private upsert seams via a mini-helper: build fake
     * chunk addresses by calling nh_porthome_encrypt_chunk to get the
     * exact addr the pusher would have recorded. This makes the base
     * "byte-identical" to what a real push would have saved. */
    extern int nh_syncd_state_upsert_file_(nh_syncd_state *,
                                           const char *,
                                           uint32_t, uint32_t, uint32_t,
                                           uint64_t, uint64_t,
                                           const char *,
                                           const char *const *, size_t);
    for (size_t i = 0; i < n; i++) {
        uint8_t hh[32]; char chash[65];
        size_t clen = strlen(contents[i]);
        assert(nh_porthome_sha256((const uint8_t *)contents[i], clen, hh) == 0);
        hex_of_local(hh, 32, chash);
        uint8_t sha[32]; char sha_hex[65];
        chunk_store tmp = {0};
        seal_and_store(&tmp, (const uint8_t *)contents[i], clen, sha, sha_hex);
        store_free(&tmp);
        const char *addrs[1] = { sha_hex };
        uint64_t mt = stat_mtime_ns(rels[i]);
        (void)nh_syncd_state_upsert_file_(s, rels[i], 0644, 0, 0, mt, clen,
                                          chash, addrs, 1);
    }
    return s;
}

/* Test-only notification recorder. */
typedef struct {
    int calls;
    char *last_body;
} notify_rec;
static void rec_notify(void *ud, const char *sum, const char *body) {
    notify_rec *r = (notify_rec *)ud; r->calls++;
    (void)sum;
    free(r->last_body);
    r->last_body = strdup(body ? body : "");
}

static void setup(void) {
    snprintf(g_home,  sizeof g_home,  "/tmp/nh_reconcile_home_%d",  (int)getpid());
    snprintf(g_state, sizeof g_state, "/tmp/nh_reconcile_state_%d", (int)getpid());
    char rm[600];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
    mkdir(g_home, 0700);
    mkdir(g_state, 0700);
    /* home_key = derive(seed=all 0x11). */
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) { seed[i] = 0x11; g_root_id[i] = (uint8_t)i; }
    assert(nh_porthome_key_derive(seed, g_home_key) == 0);
}
static void teardown(void) {
    char rm[600];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
}

/* ────────────────────────── tests ────────────────────────────── */

/* Test 1: remote-only change → applied, snapshot advanced. */
static void t_remote_only_change(void) {
    /* Initial fixture: hello.txt = "hello v1". */
    write_home_file("hello.txt", "hello v1\n", 0644);
    const char *rels[] = { "hello.txt" };
    const char *contents[] = { "hello v1\n" };
    nh_syncd_state *st = make_base_state(rels, 1, contents);
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* Remote manifest: hello.txt = "hello v2 remote". */
    chunk_store cs = {0};
    nh_porthome_manifest m; assert(nh_porthome_manifest_init(&m, g_root_id) == 0);
    manifest_add_pt_file(&m, &cs, "hello.txt", "hello v2 remote\n", 0644,
                         stat_mtime_ns("hello.txt") + 1000000000ull);

    nh_syncd_reconcile_cfg rc = {0};
    rc.manifest = &m;
    rc.remote_generation = 42;
    memcpy(rc.home_key, g_home_key, 32);
    rc.fetch_chunk = store_fetch_cb;
    rc.fetch_chunk_ctx = &cs;
    rc.device_name = "testdev";
    rc.clock_epoch_secs_override = 1700000000;
    notify_rec nr = {0};
    rc.notify = rec_notify; rc.notify_ud = &nr;

    nh_syncd_reconcile_result *res = NULL;
    char *emsg = NULL;
    int r = nh_syncd_reconcile_from_manifest(&rc, st, g_home, ig, g_state,
                                             &res, &emsg);
    if (r != 0) fprintf(stderr, "reconcile err: %s\n", emsg ? emsg : "");
    assert(r == NH_SYNCD_OK);
    assert(res != NULL);
    assert(nh_syncd_reconcile_result_applied(res) == 1);
    assert(nh_syncd_reconcile_result_conflicts(res) == 0);
    assert(nr.calls == 0);
    /* File on disk now says v2 remote. */
    char *rb = NULL; size_t rlen = 0;
    assert(slurp_home_file("hello.txt", &rb, &rlen) == 0);
    assert(strstr(rb, "hello v2 remote") != NULL);
    free(rb);
    /* State bumped to 42. */
    assert(nh_syncd_state_get_local_generation(st) >= 42);

    nh_syncd_reconcile_result_free(res);
    free(emsg);
    nh_porthome_manifest_dispose(&m);
    store_free(&cs);
    nh_syncd_state_free(st);
    nh_syncd_ignore_free(ig);
    free(nr.last_body);
    printf("t_remote_only_change OK\n");
}

/* Test 2: local-only change → I1 batcher receives it. */
static uint64_t nowfn_fixed(void *ud) { (void)ud; return 1000000000ull; }
static void t_local_only_change(void) {
    /* Fixture: doc.md exists on disk AND base. Modify it after base. */
    write_home_file("doc.md", "before\n", 0644);
    const char *rels[] = { "doc.md" };
    const char *cts[]  = { "before\n" };
    nh_syncd_state *st = make_base_state(rels, 1, cts);
    nh_syncd_ignore *ig = NULL; assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* User modifies file locally. */
    write_home_file("doc.md", "after (local)\n", 0644);

    /* Remote manifest: doc.md unchanged. Same chunk address as base. */
    chunk_store cs = {0};
    nh_porthome_manifest m; assert(nh_porthome_manifest_init(&m, g_root_id) == 0);
    manifest_add_pt_file(&m, &cs, "doc.md", "before\n", 0644,
                         stat_mtime_ns("doc.md") - 1000000000ull);

    nh_syncd_batcher *ba = NULL;
    assert(nh_syncd_batcher_new(nowfn_fixed, NULL, 1, 1, &ba) == 0);

    nh_syncd_reconcile_cfg rc = {0};
    rc.manifest = &m; rc.remote_generation = 10;
    memcpy(rc.home_key, g_home_key, 32);
    rc.fetch_chunk = store_fetch_cb; rc.fetch_chunk_ctx = &cs;
    rc.device_name = "testdev";
    rc.push_queue = ba;
    rc.clock_epoch_secs_override = 1700000000;

    nh_syncd_reconcile_result *res = NULL;
    char *emsg = NULL;
    int r = nh_syncd_reconcile_from_manifest(&rc, st, g_home, ig, g_state,
                                             &res, &emsg);
    assert(r == NH_SYNCD_OK);
    assert(nh_syncd_reconcile_result_push_queued(res) == 1);
    assert(nh_syncd_reconcile_result_conflicts(res) == 0);
    /* Batcher received the change. */
    nh_syncd_batch *bat = nh_syncd_batcher_take(ba, true);
    assert(bat != NULL);
    assert(nh_syncd_batch_len(bat) == 1);
    const char *rel = NULL; nh_syncd_change_kind k = 0;
    assert(nh_syncd_batch_at(bat, 0, &rel, &k) == 0);
    assert(!strcmp(rel, "doc.md"));
    assert(k == NH_SYNCD_CHANGE_MODIFY);
    nh_syncd_batch_free(bat);

    nh_syncd_reconcile_result_free(res);
    free(emsg);
    nh_porthome_manifest_dispose(&m);
    store_free(&cs);
    nh_syncd_batcher_free(ba);
    nh_syncd_state_free(st);
    nh_syncd_ignore_free(ig);
    printf("t_local_only_change OK\n");
}

/* Test 3: changed-both → LWW conflict + notification. */
static void t_changed_both_conflict(void) {
    write_home_file("shared.txt", "base v1\n", 0644);
    const char *rels[] = { "shared.txt" };
    const char *cts[]  = { "base v1\n" };
    nh_syncd_state *st = make_base_state(rels, 1, cts);
    nh_syncd_ignore *ig = NULL; assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* Local edit. */
    write_home_file("shared.txt", "local v2\n", 0644);
    /* Force a specific local mtime older than remote so remote wins. */
    set_home_mtime_ns("shared.txt", 1000000000ull * 1000ull);

    /* Remote: shared.txt = "remote v2" with higher mtime. */
    chunk_store cs = {0};
    nh_porthome_manifest m; assert(nh_porthome_manifest_init(&m, g_root_id) == 0);
    uint64_t remote_mt = 1000000000ull * 2000ull;
    manifest_add_pt_file(&m, &cs, "shared.txt", "remote v2\n", 0644, remote_mt);

    notify_rec nr = {0};
    nh_syncd_reconcile_cfg rc = {0};
    rc.manifest = &m; rc.remote_generation = 55;
    memcpy(rc.home_key, g_home_key, 32);
    rc.fetch_chunk = store_fetch_cb; rc.fetch_chunk_ctx = &cs;
    rc.device_name = "hostA";
    rc.notify = rec_notify; rc.notify_ud = &nr;
    rc.clock_epoch_secs_override = 1700000000;

    nh_syncd_reconcile_result *res = NULL;
    char *emsg = NULL;
    int r = nh_syncd_reconcile_from_manifest(&rc, st, g_home, ig, g_state,
                                             &res, &emsg);
    assert(r == NH_SYNCD_OK);
    assert(nh_syncd_reconcile_result_conflicts(res) == 1);
    /* Winning file is remote. */
    char *rb = NULL; size_t rlen = 0;
    assert(slurp_home_file("shared.txt", &rb, &rlen) == 0);
    assert(strstr(rb, "remote v2") != NULL);
    free(rb);
    /* Loser preserved under conflict path. */
    const char *rel = NULL, *cp = NULL;
    assert(nh_syncd_reconcile_result_conflict_at(res, 0, &rel, &cp) == 0);
    assert(!strcmp(rel, "shared.txt"));
    assert(strstr(cp, "shared.txt.conflict-hostA-") != NULL);
    /* Loser file exists and contains local content. */
    char *lb = NULL; size_t llen = 0;
    assert(slurp_home_file(cp, &lb, &llen) == 0);
    assert(strstr(lb, "local v2") != NULL);
    free(lb);
    /* Notification fired. */
    assert(nr.calls == 1);
    assert(nr.last_body && strstr(nr.last_body, "shared.txt") != NULL);

    nh_syncd_reconcile_result_free(res);
    free(emsg);
    nh_porthome_manifest_dispose(&m);
    store_free(&cs);
    nh_syncd_state_free(st);
    nh_syncd_ignore_free(ig);
    free(nr.last_body);
    printf("t_changed_both_conflict OK\n");
}

/* Test 4: remote-delete of local-modified → local preserved + .conflict-deleted marker. */
static void t_remote_delete_of_local_modified(void) {
    write_home_file("keeper.txt", "orig\n", 0644);
    const char *rels[] = { "keeper.txt" };
    const char *cts[]  = { "orig\n" };
    nh_syncd_state *st = make_base_state(rels, 1, cts);
    nh_syncd_ignore *ig = NULL; assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* Local modification. */
    write_home_file("keeper.txt", "local edits\n", 0644);

    /* Remote: manifest is EMPTY (file was deleted on remote). */
    chunk_store cs = {0};
    nh_porthome_manifest m; assert(nh_porthome_manifest_init(&m, g_root_id) == 0);
    /* no entries */

    notify_rec nr = {0};
    nh_syncd_reconcile_cfg rc = {0};
    rc.manifest = &m; rc.remote_generation = 88;
    memcpy(rc.home_key, g_home_key, 32);
    rc.fetch_chunk = store_fetch_cb; rc.fetch_chunk_ctx = &cs;
    rc.device_name = "hostB";
    rc.notify = rec_notify; rc.notify_ud = &nr;
    rc.clock_epoch_secs_override = 1700000000;

    nh_syncd_reconcile_result *res = NULL;
    char *emsg = NULL;
    int r = nh_syncd_reconcile_from_manifest(&rc, st, g_home, ig, g_state,
                                             &res, &emsg);
    assert(r == NH_SYNCD_OK);
    /* Local file is still there. */
    struct stat sk;
    char abs[600]; snprintf(abs, sizeof abs, "%s/keeper.txt", g_home);
    assert(lstat(abs, &sk) == 0);
    /* Conflict recorded. */
    assert(nh_syncd_reconcile_result_conflicts(res) == 1);
    const char *rel = NULL, *cp = NULL;
    assert(nh_syncd_reconcile_result_conflict_at(res, 0, &rel, &cp) == 0);
    assert(!strcmp(rel, "keeper.txt"));
    assert(strstr(cp, ".conflict-deleted") != NULL);
    /* Marker exists (empty). */
    char abs2[600]; snprintf(abs2, sizeof abs2, "%s/%s", g_home, cp);
    struct stat sm;
    assert(lstat(abs2, &sm) == 0);
    /* Notification fired. */
    assert(nr.calls == 1);

    nh_syncd_reconcile_result_free(res);
    free(emsg);
    nh_porthome_manifest_dispose(&m);
    store_free(&cs);
    nh_syncd_state_free(st);
    nh_syncd_ignore_free(ig);
    free(nr.last_body);
    printf("t_remote_delete_of_local_modified OK\n");
}

/* Test 5: unknown base (missing snapshot.json) → additive rescan sets
 * NOSTR_HOME_STATE=partial marker. */
static void t_unknown_base_additive(void) {
    /* $HOME has a couple of files. State dir is empty (no snapshot.json). */
    write_home_file("a.txt", "aaa\n", 0644);
    write_home_file("dir1/b.txt", "bbb\n", 0644);

    /* Fresh EMPTY state (matches nh_syncd_state_load "missing" branch). */
    nh_syncd_state *s = NULL;
    bool unknown = false;
    assert(nh_syncd_state_load(g_state, &s, &unknown) == 0);
    assert(unknown == true);
    nh_syncd_ignore *ig = NULL; assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    int r = nh_syncd_rescan_home_additive(s, g_home, ig, g_state);
    assert(r == NH_SYNCD_OK);
    /* State now has at least the 2 files + 1 dir. */
    assert(nh_syncd_state_file_count(s) >= 2);
    /* Partial marker set. */
    assert(nh_syncd_partial_state_is_set(g_state) == 1);

    /* Interlock trips. */
    nh_syncd_interlocks ilk = { g_home, "partial" };
    assert(nh_syncd_interlocks_check(&ilk) == NH_SYNCD_ERR_PARTIAL_STATE);

    nh_syncd_state_free(s);
    nh_syncd_ignore_free(ig);
    printf("t_unknown_base_additive OK\n");
}

int main(void) {
    setup();
    t_remote_only_change();
    setup(); /* fresh $HOME per test */
    t_local_only_change();
    setup();
    t_changed_both_conflict();
    setup();
    t_remote_delete_of_local_modified();
    setup();
    t_unknown_base_additive();
    teardown();
    printf("test_syncd_reconcile: OK\n");
    return 0;
}
