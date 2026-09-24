/*
 * test_syncd_lazy_reconcile.c — additive-only suppression regression
 *                               for the lazy-subtree skip.
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-1u55 (Phase 4 P4-I).
 *
 * Proves:
 *  (a) With cfg->lazy = <matches Portable/>, a remote manifest that
 *      contains "Portable/foo.bin" DOES NOT materialize the file to
 *      $HOME but DOES record it in snapshot.json (with chunk_addrs
 *      preserved so the FUSE overlay can serve it).
 *  (b) With the same knob ON, a subsequent reconcile whose manifest
 *      OMITS Portable/foo.bin does NOT delete a pre-existing local
 *      file at $HOME/Portable/foo.bin — additive suppression only.
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
#include <unistd.h>

/* ── fake chunk store ─────────────────────────────────────────── */

typedef struct row { char addr[65]; uint8_t *b; size_t n; struct row *nx; } row;
static row *g_store = NULL;

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2] = lc[(b[i] >> 4) & 0xf];
        out[i*2 + 1] = lc[b[i] & 0xf];
    }
    out[n*2] = '\0';
}

static void store_put(const char *addr, const uint8_t *b, size_t n) {
    row *r = calloc(1, sizeof *r);
    strncpy(r->addr, addr, 64);
    r->b = malloc(n ? n : 1);
    memcpy(r->b, b, n);
    r->n = n;
    r->nx = g_store;
    g_store = r;
}

static int fetch_cb(void *ctx, const char *addr, uint8_t **out, size_t *out_len) {
    (void)ctx;
    for (row *r = g_store; r; r = r->nx) {
        if (!strcmp(r->addr, addr)) {
            *out = malloc(r->n ? r->n : 1);
            memcpy(*out, r->b, r->n);
            *out_len = r->n;
            return 0;
        }
    }
    return -1;
}

static void store_free(void) {
    row *r = g_store;
    while (r) { row *n = r->nx; free(r->b); free(r); r = n; }
    g_store = NULL;
}

/* ── helpers ─────────────────────────────────────────────────── */

static char g_home[300];
static char g_state[300];
static uint8_t g_hkey[32];
static uint8_t g_root_id[32];

static void mkdirp_absolute(const char *abs) {
    char *tmp = strdup(abs);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0700); *p = '/'; }
    }
    mkdir(tmp, 0700);
    free(tmp);
}

static void write_file(const char *rel, const char *contents) {
    char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    char *last = strrchr(abs, '/');
    if (last) { *last = '\0'; mkdirp_absolute(abs); *last = '/'; }
    int fd = open(abs, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    if (contents) (void)!write(fd, contents, strlen(contents));
    close(fd);
}

static bool file_exists(const char *rel) {
    char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    struct stat s; return lstat(abs, &s) == 0;
}

static void add_pt_file(nh_porthome_manifest *m, const char *rel,
                        const char *contents) {
    char *penc = NULL;
    assert(nh_porthome_encrypt_path(g_hkey, rel, &penc) == 0);
    size_t clen = strlen(contents);
    nh_porthome_chunk ch = {0};
    if (clen > 0) {
        uint8_t *ct = NULL; size_t ct_len = 0;
        assert(nh_porthome_encrypt_chunk(g_hkey, (const uint8_t *)contents,
                                         clen, &ct, &ct_len, ch.sha256) == 0);
        char addr[65]; hex_of(ch.sha256, 32, addr);
        store_put(addr, ct, ct_len);
        free(ct);
        ch.size = (uint32_t)ct_len;
        /* add_file takes ownership of penc. */
        assert(nh_porthome_manifest_add_file(m, penc, 0644, 0, 0,
                                             (uint64_t)time(NULL) * 1000000000ull,
                                             clen, &ch, 1) == 0);
    } else {
        assert(nh_porthome_manifest_add_file(m, penc, 0644, 0, 0,
                                             (uint64_t)time(NULL) * 1000000000ull,
                                             0, NULL, 0) == 0);
    }
}

int main(void) {
    /* Isolated tmpdir. */
    char tmpl[] = "/tmp/nhfuse_lazy_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    snprintf(g_home, sizeof g_home, "%s/home", tmpl);
    snprintf(g_state, sizeof g_state, "%s/state", tmpl);
    mkdirp_absolute(g_home);
    mkdirp_absolute(g_state);

    for (int i = 0; i < 32; i++) g_hkey[i] = (uint8_t)(i * 3 + 1);
    memset(g_root_id, 0xaa, 32);

    /* Empty base snapshot. */
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "nostr-homed.home.v1:personal",
                              "0000000000000000000000000000000000000000000000000000000000000001",
                              g_root_id, &state) == 0);

    /* Ignore matcher (accept everything in $HOME). */
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* Lazy matcher with Portable/. */
    nh_syncd_lazy *lz = NULL;
    assert(nh_syncd_lazy_new_from_string("Portable/", &lz) == 0);

    /* Remote manifest with Portable/foo.bin. */
    nh_porthome_manifest m1_storage;
    assert(nh_porthome_manifest_init(&m1_storage, g_root_id) == 0);
    nh_porthome_manifest *m1 = &m1_storage;
    add_pt_file(m1, "Portable/foo.bin", "cold-tier payload");

    /* But snapshot MUST include the base row for the reconciler to
     * even enter the per-entry loop for that path. Use rescan_home_additive
     * on an empty $HOME (creates no entries). Instead we insert a base
     * DIR entry Portable/ so the ignore matcher does not immediately skip.
     *
     * In practice the reconciler's `remote-only paths not in base` loop
     * would also try to apply the entry — but there the path_enc is
     * an opaque hex hash, not the plaintext prefix, so we need the
     * lazy skip in the main loop. Add Portable/foo.bin as a fresh
     * base row with the correct chunk address so it appears in bc.rows.
     */
    /* We simulate a base entry that has the plaintext rel and points
     * at the manifest's addr. That mimics the state left by a prior
     * push (or by nh_syncd_state_load from disk). */
    extern int nh_syncd_state_upsert_file_(nh_syncd_state *,
                                           const char *,
                                           uint32_t, uint32_t, uint32_t,
                                           uint64_t, uint64_t,
                                           const char *,
                                           const char *const *, size_t);
    /* Base with zero chunks + zero size so local_changed=true (local absent). */
    (void)nh_syncd_state_upsert_file_(state, "Portable/foo.bin",
                                      0644, 0, 0, 1u * 1000000000ull, 0,
                                      "", NULL, 0);

    nh_syncd_reconcile_cfg cfg1 = {
        .manifest = m1,
        .remote_generation = 1,
        .fetch_chunk = fetch_cb,
        .fetch_chunk_ctx = NULL,
        .device_name = "test",
        .push_queue = NULL,
        .notify = NULL,
        .notify_ud = NULL,
        .clock_epoch_secs_override = 1700000000,
        .lazy = lz,
    };
    memcpy(cfg1.home_key, g_hkey, 32);

    nh_syncd_reconcile_result *rr = NULL;
    char *emsg = NULL;
    int rc = nh_syncd_reconcile_from_manifest(&cfg1, state, g_home, ig, g_state,
                                              &rr, &emsg);
    assert(rc == 0);

    /* (a) Assert file NOT materialized. */
    assert(!file_exists("Portable/foo.bin"));

    /* Assert snapshot DOES record the entry (via find + chunk count). */
    const nh_syncd_entry *e = nh_syncd_state_find(state, "Portable/foo.bin");
    assert(e != NULL);
    /* Chunk count > 0 — the lazy path recorded the manifest's chunks. */
    size_t nc = nh_syncd_entry_chunk_count(e);
    if (nc == 0) {
        fprintf(stderr, "expected lazy path to record chunk addrs; got 0\n");
        assert(0);
    }
    nh_porthome_manifest_dispose(m1);

    /* Now — the additive-only test. Simulate the file existing from a
     * PRIOR reconcile (predating the lazy knob). */
    write_file("Portable/foo.bin", "materialized long ago");
    assert(file_exists("Portable/foo.bin"));

    /* Second reconcile: manifest OMITS the file. Without the lazy
     * skip this would delete it. With lazy ON, deletion is suppressed. */
    nh_porthome_manifest m2_storage;
    assert(nh_porthome_manifest_init(&m2_storage, g_root_id) == 0);
    nh_porthome_manifest *m2 = &m2_storage;
    /* Include a different entry so the manifest is non-trivial. */
    add_pt_file(m2, "Documents/other.txt", "keep");

    nh_syncd_reconcile_cfg cfg2 = cfg1;
    cfg2.manifest = m2;
    cfg2.remote_generation = 2;
    nh_syncd_reconcile_result *rr2 = NULL;
    rc = nh_syncd_reconcile_from_manifest(&cfg2, state, g_home, ig, g_state,
                                          &rr2, &emsg);
    assert(rc == 0);

    /* File must still exist — deletion must have been suppressed. */
    if (!file_exists("Portable/foo.bin")) {
        fprintf(stderr,
                "FAIL: lazy skip did NOT preserve pre-existing Portable/foo.bin\n");
        assert(0);
    }

    nh_syncd_reconcile_result_free(rr);
    nh_syncd_reconcile_result_free(rr2);
    nh_porthome_manifest_dispose(m2);
    nh_syncd_lazy_free(lz);
    nh_syncd_ignore_free(ig);
    nh_syncd_state_free(state);
    store_free();

    printf("test_syncd_lazy_reconcile OK\n");
    return 0;
}
