/*
 * test_fuse_snapshot_reload.c — bead nostrc-plo4.
 *
 * SPDX-License-Identifier: MIT
 *
 * Exercises the inotify-driven snapshot-reload seam without spawning
 * a real FUSE mount. The scenarios cover:
 *
 *   (1) happy path — a NEW snapshot.json rendered by save + rename lands
 *       in the watched directory. The reload watcher's swap fires; the
 *       "current" table pointer is atomically flipped; the new
 *       generation is visible to fresh lookups. An OLD fh (captured
 *       gen-1 rel + chunks) can still resolve its recorded fields
 *       because they are owned by the fh, not the table.
 *
 *   (2) parse-failure path — a corrupt snapshot.json is renamed in.
 *       load_fn returns non-zero; fail_fn fires; the swap is NOT
 *       performed; the old table stays live.
 *
 *   (3) escape-hatch — NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY=1 disables
 *       the watcher (fd == -1) but the module still constructs and the
 *       test-seam manual tick still fires the swap.
 *
 * Threading model: the module owns a background watcher thread; the
 * synchronous test seam bypasses it (calls into the same code path
 * via nh_fuse_reload_tick_now).
 */
#define _GNU_SOURCE
#include "nh_fuse_reload.h"
#include "nh_fuse_table.h"
#include "nh_syncd.h"

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

extern int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *,
                                       uint32_t, uint32_t, uint32_t,
                                       uint64_t, uint64_t,
                                       const char *,
                                       const char *const *, size_t);
extern int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *,
                                      uint32_t, uint32_t, uint32_t, uint64_t);
extern void nh_syncd_state_bump_generation_(nh_syncd_state *);

static void rm_rf(const char *p) {
    char c[512]; snprintf(c, sizeof c, "rm -rf '%s'", p); (void)system(c);
}

/* Test clock — fixed at 0, we advance manually. */
static int64_t g_now_ms = 0;
static int64_t clock_fn(void *ud) { (void)ud; return g_now_ms; }

/* Swap plumbing. Wrapped in a mutex-free single-thread test — the
 * background watcher thread does exist but nh_fuse_reload_tick_now
 * lets us drive things synchronously. Even so, the swap callback
 * may run from either thread, so use a plain _Atomic pointer. */
#include <stdatomic.h>
static _Atomic(nh_fuse_table *) g_table = NULL;

static int load_fn(void *ud, const char *state_dir, nh_fuse_table **out) {
    (void)ud;
    return nh_fuse_table_load(state_dir, 4242, 4242, out);
}
static int g_swap_count = 0;
static void swap_fn(void *ud, nh_fuse_table *nt) {
    (void)ud;
    g_swap_count++;
    nh_fuse_table *old = atomic_exchange(&g_table, nt);
    if (old) nh_fuse_table_free(old);
}
static int g_fail_count = 0;
static int g_last_fail_rc = 0;
static void fail_fn(void *ud, int rc) {
    (void)ud;
    g_fail_count++;
    g_last_fail_rc = rc;
}

/* Write snapshot.json for a fresh state with N files under Documents/.
 * Uses atomic rename via nh_syncd_state_save. */
static void write_snapshot(const char *state_dir,
                           uint64_t generation,
                           const char *file_rel,
                           const char *chunk_hex) {
    /* Nuke previous state; write fresh. */
    char rmpath[512];
    snprintf(rmpath, sizeof rmpath, "%s/snapshot.json", state_dir);
    (void)unlink(rmpath);
    snprintf(rmpath, sizeof rmpath, "%s/generation", state_dir);
    (void)unlink(rmpath);

    uint8_t root_id[32]; memset(root_id, 0xab, 32);
    nh_syncd_state *st = NULL;
    assert(nh_syncd_state_new("/home/testuser", "d-tag", "01",
                              root_id, &st) == 0);
    /* Advance generation to the requested value. */
    for (uint64_t i = 0; i < generation; ++i) nh_syncd_state_bump_generation_(st);
    const char *chunks[] = { chunk_hex };
    assert(nh_syncd_state_upsert_file_(st, file_rel,
                                       0644, 1000, 1000, 1u*1000000000ull,
                                       17, "01", chunks, 1) == 0);
    assert(nh_syncd_state_upsert_dir_(st, "Documents", 0755, 1000, 1000,
                                      1u*1000000000ull) == 0);
    assert(nh_syncd_state_save(st, state_dir) == 0);
    nh_syncd_state_free(st);
}

static void write_corrupt_snapshot(const char *state_dir) {
    char path[512];
    char tmp[512];
    snprintf(path, sizeof path, "%s/snapshot.json", state_dir);
    snprintf(tmp,  sizeof tmp,  "%s/snapshot.json.tmp.%d", state_dir, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const char *garbage = "{ this is not valid json &&& \n";
    ssize_t n = write(fd, garbage, strlen(garbage));
    assert(n == (ssize_t)strlen(garbage));
    close(fd);
    assert(rename(tmp, path) == 0);
}

/* Snapshot the fields of an fh at open() time. Independent of the
 * table — mirrors the fh struct used by nostr-home-fuse.c. */
typedef struct {
    uint64_t generation;
    char     rel[4097];
    char     chunk0_hex[65];
    uint64_t size;
} fake_fh;

static void open_fh_from(nh_fuse_table *t, const char *rel, fake_fh *out) {
    const nh_fuse_entry_t *e = nh_fuse_table_find(t, rel);
    assert(e);
    assert(e->kind == NH_FUSE_KIND_FILE);
    out->generation = nh_fuse_table_generation(t);
    snprintf(out->rel, sizeof out->rel, "%s", e->rel);
    assert(e->n_chunks == 1);
    memcpy(out->chunk0_hex, e->chunks_hex[0], 64);
    out->chunk0_hex[64] = '\0';
    out->size = e->size;
}

static void test_happy_path(const char *rootdir) {
    char sdir[512]; snprintf(sdir, sizeof sdir, "%s/happy/state", rootdir);
    (void)mkdir(sdir, 0755);
    /* Need every parent — mkdir -p. */
    char parent[512]; snprintf(parent, sizeof parent, "%s/happy", rootdir);
    (void)mkdir(parent, 0755);
    (void)mkdir(sdir, 0755);

    /* Initial snapshot with generation=1 and a single file whose sole
     * chunk hash is 64 'a' chars. */
    write_snapshot(sdir, 1u,
                   "Documents/notes.txt",
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    nh_fuse_table *initial = NULL;
    assert(nh_fuse_table_load(sdir, 4242, 4242, &initial) == 0);
    atomic_store(&g_table, initial);
    assert(nh_fuse_table_generation(initial) == 1u);

    /* Simulate an open() capturing gen-1 fields. */
    fake_fh old_fh = {0};
    open_fh_from(initial, "Documents/notes.txt", &old_fh);
    assert(old_fh.generation == 1u);
    assert(!strncmp(old_fh.chunk0_hex,
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    64));

    /* Bring up the reload watcher in inotify-DISABLED mode so the test
     * runs deterministically on any FS. We call tick_now(state_dir)
     * with an explicit mark_event to drive one cycle. */
    setenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY", "1", 1);
    g_swap_count = 0;
    g_fail_count = 0;
    nh_fuse_reload_t *r = NULL;
    nh_fuse_reload_cfg cfg = {
        .state_dir       = sdir,
        .watch_basename  = "snapshot.json",
        .debounce_ms     = 250,
        .load_fn = load_fn, .load_ud = NULL,
        .swap_fn = swap_fn, .swap_ud = NULL,
        .fail_fn = fail_fn, .fail_ud = NULL,
        .clock_fn = clock_fn, .clock_ud = NULL,
    };
    assert(nh_fuse_reload_new(&cfg, &r) == 0);
    /* Escape hatch honoured — no inotify fd. */
    assert(nh_fuse_reload_fd(r) == -1);

    /* Publish generation 2 atomically. */
    write_snapshot(sdir, 2u,
                   "Documents/notes.txt",
                   "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    /* Mark event, advance beyond debounce, tick. */
    g_now_ms = 1000;
    nh_fuse_reload_mark_event(r);
    /* Before debounce: swap must NOT fire. */
    int rc = nh_fuse_reload_tick_now(r);
    assert(rc == 0);
    assert(g_swap_count == 0);
    /* After debounce: swap fires. */
    g_now_ms = 1000 + 300;
    rc = nh_fuse_reload_tick_now(r);
    assert(rc == 1);
    assert(g_swap_count == 1);
    assert(g_fail_count == 0);

    /* g_table now holds the new table with generation=2. */
    nh_fuse_table *now_table = atomic_load(&g_table);
    assert(nh_fuse_table_generation(now_table) == 2u);
    const nh_fuse_entry_t *new_e =
        nh_fuse_table_find(now_table, "Documents/notes.txt");
    assert(new_e && new_e->kind == NH_FUSE_KIND_FILE);
    assert(new_e->n_chunks == 1);
    assert(!strncmp(new_e->chunks_hex[0],
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    64));

    /* OLD fh untouched — its captured generation and chunk hash still
     * reflect the original snapshot. This is the per-open GENERATION
     * binding invariant the design mandates. */
    assert(old_fh.generation == 1u);
    assert(!strncmp(old_fh.chunk0_hex,
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    64));

    /* A fresh open sees the NEW generation and NEW chunk hash. */
    fake_fh new_fh = {0};
    open_fh_from(now_table, "Documents/notes.txt", &new_fh);
    assert(new_fh.generation == 2u);
    assert(!strncmp(new_fh.chunk0_hex,
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    64));

    /* Multiple events within one debounce → one swap (bursts collapse). */
    write_snapshot(sdir, 3u,
                   "Documents/notes.txt",
                   "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    g_now_ms += 1000;
    nh_fuse_reload_mark_event(r);
    g_now_ms += 100;
    nh_fuse_reload_mark_event(r); /* re-arms debounce */
    g_now_ms += 100;
    nh_fuse_reload_mark_event(r);
    g_now_ms += 100;
    rc = nh_fuse_reload_tick_now(r);
    /* Only 100 ms since last event — below 250 ms debounce → no swap. */
    assert(rc == 0);
    assert(g_swap_count == 1);
    g_now_ms += 300;
    rc = nh_fuse_reload_tick_now(r);
    assert(rc == 1);
    assert(g_swap_count == 2);

    nh_fuse_table *g3 = atomic_load(&g_table);
    assert(nh_fuse_table_generation(g3) == 3u);

    nh_fuse_reload_free(r);
    unsetenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY");

    /* Cleanup shared table. */
    nh_fuse_table *tail = atomic_exchange(&g_table, NULL);
    if (tail) nh_fuse_table_free(tail);
    printf("happy_path OK (swap_count=%d)\n", g_swap_count);
}

static void test_parse_failure(const char *rootdir) {
    char parent[512]; snprintf(parent, sizeof parent, "%s/fail", rootdir);
    (void)mkdir(parent, 0755);
    char sdir[512]; snprintf(sdir, sizeof sdir, "%s/fail/state", rootdir);
    (void)mkdir(sdir, 0755);

    write_snapshot(sdir, 5u,
                   "Documents/notes.txt",
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    nh_fuse_table *initial = NULL;
    assert(nh_fuse_table_load(sdir, 4242, 4242, &initial) == 0);
    atomic_store(&g_table, initial);
    assert(nh_fuse_table_generation(initial) == 5u);

    setenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY", "1", 1);
    g_swap_count = 0;
    g_fail_count = 0;
    g_last_fail_rc = 0;
    nh_fuse_reload_t *r = NULL;
    nh_fuse_reload_cfg cfg = {
        .state_dir      = sdir,
        .watch_basename = "snapshot.json",
        .debounce_ms    = 250,
        .load_fn = load_fn, .swap_fn = swap_fn, .fail_fn = fail_fn,
        .clock_fn = clock_fn,
    };
    assert(nh_fuse_reload_new(&cfg, &r) == 0);

    /* Corrupt the snapshot. */
    write_corrupt_snapshot(sdir);

    g_now_ms = 10000;
    nh_fuse_reload_mark_event(r);
    g_now_ms += 300;
    int rc = nh_fuse_reload_tick_now(r);
    assert(rc < 0);
    assert(g_swap_count == 0);
    assert(g_fail_count == 1);
    /* OLD table still live and still at generation 5. */
    nh_fuse_table *still = atomic_load(&g_table);
    assert(nh_fuse_table_generation(still) == 5u);
    const nh_fuse_entry_t *e =
        nh_fuse_table_find(still, "Documents/notes.txt");
    assert(e && !strncmp(e->chunks_hex[0],
                         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                         64));

    /* Recovery: a valid new snapshot arrives; swap fires normally. */
    write_snapshot(sdir, 6u,
                   "Documents/notes.txt",
                   "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    g_now_ms += 1000;
    nh_fuse_reload_mark_event(r);
    g_now_ms += 300;
    rc = nh_fuse_reload_tick_now(r);
    assert(rc == 1);
    assert(g_swap_count == 1);
    nh_fuse_table *g6 = atomic_load(&g_table);
    assert(nh_fuse_table_generation(g6) == 6u);

    nh_fuse_reload_free(r);
    unsetenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY");
    nh_fuse_table *tail = atomic_exchange(&g_table, NULL);
    if (tail) nh_fuse_table_free(tail);
    printf("parse_failure OK (fail_count=%d rc=%d)\n",
           g_fail_count, g_last_fail_rc);
}

static void test_escape_hatch(const char *rootdir) {
    /* Also runs disabled — same as happy path really, but assert
     * nh_fuse_reload_fd == -1 to lock the observable behaviour. */
    char parent[512]; snprintf(parent, sizeof parent, "%s/esc", rootdir);
    (void)mkdir(parent, 0755);
    char sdir[512]; snprintf(sdir, sizeof sdir, "%s/esc/state", rootdir);
    (void)mkdir(sdir, 0755);

    write_snapshot(sdir, 1u, "Documents/notes.txt",
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    nh_fuse_table *initial = NULL;
    assert(nh_fuse_table_load(sdir, 4242, 4242, &initial) == 0);
    atomic_store(&g_table, initial);

    setenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY", "1", 1);
    nh_fuse_reload_t *r = NULL;
    nh_fuse_reload_cfg cfg = {
        .state_dir = sdir,
        .watch_basename = "snapshot.json",
        .debounce_ms = 250,
        .load_fn = load_fn, .swap_fn = swap_fn, .fail_fn = fail_fn,
        .clock_fn = clock_fn,
    };
    assert(nh_fuse_reload_new(&cfg, &r) == 0);
    assert(nh_fuse_reload_fd(r) == -1);
    nh_fuse_reload_free(r);
    unsetenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY");

    nh_fuse_table *tail = atomic_exchange(&g_table, NULL);
    if (tail) nh_fuse_table_free(tail);
    printf("escape_hatch OK\n");
}

int main(void) {
    char rootdir[128];
    snprintf(rootdir, sizeof rootdir, "/tmp/nh_fuse_reload_%d", (int)getpid());
    rm_rf(rootdir);
    assert(mkdir(rootdir, 0755) == 0);

    test_happy_path(rootdir);
    test_parse_failure(rootdir);
    test_escape_hatch(rootdir);

    rm_rf(rootdir);
    printf("test_fuse_snapshot_reload: all tests passed\n");
    return 0;
}
