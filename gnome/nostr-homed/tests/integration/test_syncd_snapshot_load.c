/*
 * test_syncd_snapshot_load.c — nostrc-5y2t, daemon level.
 *
 * SPDX-License-Identifier: MIT
 *
 * Runs the real nostr-home-syncd binary against snapshot.json files that
 * the old loader mishandled:
 *   A. a valid snapshot over the old 32 MiB cap loads (--check exit 0,
 *      entry count logged);
 *   B. over the (env-lowered) ceiling: the daemon refuses to start
 *      (exit 7) and the file is left untouched;
 *   C. corrupt + --check: exit 7, no quarantine (dry run is read-only);
 *   D. corrupt + managed run: the file is preserved as
 *      snapshot.json.corrupt.<epoch> (same bytes) and the additive
 *      rescan writes a fresh baseline next to it — never over it.
 */
#define _GNU_SOURCE
#include "nh_syncd.h"
#include "nh_syncd_spawn.h"
#include "../nh_test_fs.h"

#include <assert.h>
#include <glob.h>

extern int nh_syncd_state_upsert_file_(nh_syncd_state *s, const char *rel,
                                       uint32_t mode, uint32_t uid, uint32_t gid,
                                       uint64_t mtime_ns, uint64_t size,
                                       const char *content_hash_hex,
                                       const char *const *chunk_addrs_hex,
                                       size_t chunk_addrs_n);

#define BIG_N 220000u
static const char HEX_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char CORRUPT[] = "{\"schema\": 1, \"generation\": 3, \"files\": {\"a\": ";

static nh_syncd_sandbox g_sb;
static char g_snap[256];

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    close(fd);
}

static void fresh_sandbox(void) {
    nh_test_rm_rf(g_sb.root);
    assert(mkdir(g_sb.root, 0700) == 0);
    assert(mkdir(g_sb.home, 0700) == 0);
    assert(mkdir(g_sb.state, 0700) == 0);
    snprintf(g_snap, sizeof g_snap, "%s/snapshot.json", g_sb.state);
}

static size_t count_quarantined(char *first, size_t n) {
    char pat[320]; snprintf(pat, sizeof pat, "%s/snapshot.json.corrupt.*", g_sb.state);
    glob_t g; memset(&g, 0, sizeof g);
    size_t c = glob(pat, 0, NULL, &g) == 0 ? g.gl_pathc : 0;
    if (c && first) snprintf(first, n, "%s", g.gl_pathv[0]);
    globfree(&g);
    return c;
}

static void write_big_snapshot(void) {
    uint8_t root_id[32] = {7};
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new(g_sb.home, "nostr-homed.home.v1:personal", "",
                              root_id, &s) == 0);
    const char *addrs[1] = { HEX_A };
    char rel[96];
    for (unsigned i = 0; i < BIG_N; i++) {
        snprintf(rel, sizeof rel, "src/p%03u/module/file-%06u.txt", i % 997, i);
        assert(nh_syncd_state_upsert_file_(s, rel, 0644, 1000, 1000,
                                           1700000000000000000ull + i, i, HEX_A,
                                           addrs, 1) == 0);
    }
    assert(nh_syncd_state_save(s, g_sb.state) == 0);
    nh_syncd_state_free(s);
    struct stat st; assert(stat(g_snap, &st) == 0);
    fprintf(stderr, "big snapshot: %lld bytes\n", (long long)st.st_size);
    assert(st.st_size > (off_t)(32u * 1024u * 1024u));
}

static bool same_file(const struct stat *a, const struct stat *b) {
    return a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static bool rebuilt_baseline(void *ud) {
    (void)ud;
    bool unknown = true;
    nh_syncd_state *s = NULL;
    if (nh_syncd_state_load(g_sb.state, &s, &unknown) != NH_SYNCD_OK) return false;
    bool ok = !unknown && nh_syncd_state_find(s, "notes.txt") != NULL;
    nh_syncd_state_free(s);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <nostr-home-syncd>\n", argv[0]); return 64; }
    const char *bin = argv[1];
    nh_syncd_sandbox_init(&g_sb, "syncd_snapload", "drop/home_seed");
    fresh_sandbox();

    /* A — >32 MiB snapshot loads. */
    write_big_snapshot();
    struct stat before; assert(stat(g_snap, &before) == 0);
    int rc = nh_syncd_wait(nh_syncd_spawn(bin, &g_sb, true, NULL), 60.0);
    if (rc != 0 || !nh_syncd_log_has(&g_sb, "loaded snapshot.json: 220000 entries"))
        nh_syncd_dump_log(&g_sb);
    assert(rc == 0);
    assert(nh_syncd_log_has(&g_sb, "loaded snapshot.json: 220000 entries"));
    fprintf(stderr, "A ok\n");

    /* B — over the ceiling: refuse (exit 7), leave the file alone. */
    const char *const tiny[] = { "NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES=1048576", NULL };
    rc = nh_syncd_wait(nh_syncd_spawn(bin, &g_sb, false, tiny), 60.0);
    struct stat after; assert(stat(g_snap, &after) == 0);
    if (rc != 7) nh_syncd_dump_log(&g_sb);
    assert(rc == 7);
    assert(same_file(&before, &after));
    assert(nh_syncd_log_has(&g_sb, "snapshot-too-large"));
    assert(count_quarantined(NULL, 0) == 0);
    fprintf(stderr, "B ok\n");

    /* C — corrupt + --check: report, exit 7, no side effects. */
    fresh_sandbox();
    write_file(g_snap, CORRUPT);
    rc = nh_syncd_wait(nh_syncd_spawn(bin, &g_sb, true, NULL), 30.0);
    if (rc != 7) nh_syncd_dump_log(&g_sb);
    assert(rc == 7);
    assert(count_quarantined(NULL, 0) == 0);
    assert(stat(g_snap, &after) == 0 && after.st_size == (off_t)strlen(CORRUPT));
    fprintf(stderr, "C ok\n");

    /* D — corrupt + managed run: quarantine, then rebuild beside it. */
    write_file(g_snap, CORRUPT);
    char note[256]; snprintf(note, sizeof note, "%s/notes.txt", g_sb.home);
    write_file(note, "hello\n");
    char drop_dir[256]; snprintf(drop_dir, sizeof drop_dir, "%s/drop", g_sb.root);
    assert(mkdir(drop_dir, 0700) == 0);
    write_file(g_sb.seed_path,
               "1122334455667788112233445566778811223344556677881122334455667788");
    pid_t pid = nh_syncd_spawn(bin, &g_sb, false, NULL);
    assert(pid > 0);
    bool rebuilt = nh_syncd_until(rebuilt_baseline, NULL, 30.0);
    rc = nh_syncd_stop(pid);
    if (!rebuilt || rc != 0) nh_syncd_dump_log(&g_sb);
    assert(rebuilt);
    assert(rc == 0);
    char q[320] = {0};
    assert(count_quarantined(q, sizeof q) == 1);
    assert(stat(q, &after) == 0 && after.st_size == (off_t)strlen(CORRUPT));
    assert(nh_syncd_log_has(&g_sb, "preserved as"));
    assert(nh_syncd_partial_state_is_set(g_sb.state));
    fprintf(stderr, "D ok\n");

    nh_test_rm_rf(g_sb.root);
    printf("ok\n");
    return 0;
}
