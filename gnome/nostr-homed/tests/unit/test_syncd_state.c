/*
 * test_syncd_state.c — round-trip snapshot.json + generation and
 * validate the SNAPSHOT_UNKNOWN branch. Also exercises the internal
 * upsert/delete helpers via `extern` declarations (they're used by
 * the pusher but not part of the public API).
 */

#define _GNU_SOURCE
#include "nh_syncd.h"

#include <assert.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../nh_test_fs.h"

/* Not-in-header helpers from nh_syncd_state.c we exercise here. */
extern int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                       const char *rel,
                                       uint32_t mode, uint32_t uid, uint32_t gid,
                                       uint64_t mtime_ns, uint64_t size,
                                       const char *content_hash_hex,
                                       const char *const *chunk_addrs_hex,
                                       size_t chunk_addrs_n);
extern int nh_syncd_state_upsert_dir_(nh_syncd_state *s,
                                      const char *rel,
                                      uint32_t mode, uint32_t uid, uint32_t gid,
                                      uint64_t mtime_ns);
extern int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel);
extern void nh_syncd_state_bump_generation_(nh_syncd_state *s);

static char g_dir[256];

static void setup(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/nh_syncd_state_%d", (int)getpid());
    nh_test_rm_rf(g_dir);
    assert(mkdir(g_dir, 0700) == 0);
}
static void teardown(void) {
    nh_test_rm_rf(g_dir);
}

static void t_missing_snapshot_is_unknown(void) {
    bool unknown = false;
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_load(g_dir, &s, &unknown) == 0);
    assert(unknown == true);
    assert(s != NULL);
    /* Fresh state has no root/d_tag — the pusher uses that to refuse. */
    assert(nh_syncd_state_get_root(s)[0] == '\0');
    assert(nh_syncd_state_get_d_tag(s)[0] == '\0');
    nh_syncd_state_free(s);
    printf("t_missing_snapshot_is_unknown OK\n");
}

static void t_roundtrip(void) {
    uint8_t root_id[32];
    for (int i = 0; i < 32; i++) root_id[i] = (uint8_t)(i * 7 + 3);
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new("/tmp/home", "nostr-homed.home.v1:personal",
                              "0000000000000000000000000000000000000000000000000000000000000000",
                              root_id, &s) == 0);
    const char *addrs[2] = {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};
    assert(nh_syncd_state_upsert_file_(s, "notes/hello.txt",
                                       0644, 1000, 1000, 1700000000ull * 1000000000ull, 13,
                                       "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                                       addrs, 2) == 0);
    assert(nh_syncd_state_upsert_dir_(s, "notes",
                                      0755, 1000, 1000, 1700000000ull * 1000000000ull) == 0);
    nh_syncd_state_bump_generation_(s);
    nh_syncd_state_bump_generation_(s);
    assert(nh_syncd_state_get_local_generation(s) == 2);
    assert(nh_syncd_state_save(s, g_dir) == 0);

    /* Verify the standalone `generation` file has the ASCII decimal. */
    char gpath[300]; snprintf(gpath, sizeof gpath, "%s/generation", g_dir);
    FILE *gf = fopen(gpath, "r");
    assert(gf);
    char buf[16] = {0};
    size_t nr = fread(buf, 1, sizeof buf - 1, gf);
    assert(nr > 0);
    fclose(gf);
    assert(buf[0] == '2');

    /* Reload and verify shape survives. */
    bool unknown = true;
    nh_syncd_state *r = NULL;
    assert(nh_syncd_state_load(g_dir, &r, &unknown) == 0);
    assert(unknown == false);
    assert(nh_syncd_state_get_local_generation(r) == 2);
    assert(nh_syncd_state_file_count(r) == 2);
    const nh_syncd_entry *e = nh_syncd_state_find(r, "notes/hello.txt");
    assert(e != NULL);
    assert(nh_syncd_entry_kind(e) == NH_SYNCD_KIND_FILE);
    assert(nh_syncd_entry_size(e) == 13);

    /* Delete + resave. */
    assert(nh_syncd_state_delete_(r, "notes/hello.txt") == 0);
    assert(nh_syncd_state_file_count(r) == 1);

    nh_syncd_state_free(s);
    nh_syncd_state_free(r);
    printf("t_roundtrip OK\n");
}

static void t_malformed_json_is_unknown(void) {
    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    FILE *f = fopen(snap, "w");
    assert(f);
    fprintf(f, "not-json-at-all\n");
    fclose(f);
    bool unknown = false;
    nh_syncd_state *s = NULL;
    int rc = nh_syncd_state_load(g_dir, &s, &unknown);
    assert(rc == NH_SYNCD_ERR_JSON);
    assert(unknown == true);
    /* nostrc-5y2t: a failed load must not hand back an empty state the
     * caller could save over the unreadable file. */
    assert(s == NULL);
    assert(strcmp(nh_syncd_state_load_error_class(rc), "snapshot-corrupt") == 0);
    unlink(snap);
    printf("t_malformed_json_is_unknown OK\n");
}

/* ── nostrc-5y2t ───────────────────────────────────────────────────── */

static const char HEX_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char HEX_C[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

static off_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_size : -1;
}

static void write_str(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* The lab home had 203,561 entries -> a 64 MB snapshot, which the old
 * 32 MiB read cap rejected. Build one comfortably past 32 MiB and make
 * sure it round-trips. */
#define BIG_N 220000u
static void t_large_snapshot_roundtrip(void) {
    uint8_t root_id[32] = {1};
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new("/tmp/home", "nostr-homed.home.v1:personal",
                              "", root_id, &s) == 0);
    const char *addrs[1] = { HEX_A };
    char rel[96];
    for (unsigned i = 0; i < BIG_N; i++) {
        snprintf(rel, sizeof rel, "src/project-%03u/module/file-%06u.txt", i % 997, i);
        assert(nh_syncd_state_upsert_file_(s, rel, 0644, 1000, 1000,
                                           1700000000ull * 1000000000ull + i, i,
                                           HEX_C, addrs, 1) == 0);
    }
    nh_syncd_state_bump_generation_(s);
    assert(nh_syncd_state_save(s, g_dir) == 0);
    nh_syncd_state_free(s);

    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    off_t sz = file_size(snap);
    fprintf(stderr, "large snapshot: %u entries, %lld bytes\n", BIG_N, (long long)sz);
    assert(sz > (off_t)(32u * 1024u * 1024u));

    bool unknown = true;
    nh_syncd_state *r = NULL;
    assert(nh_syncd_state_load(g_dir, &r, &unknown) == NH_SYNCD_OK);
    assert(r && !unknown);
    assert(nh_syncd_state_file_count(r) == BIG_N);
    assert(nh_syncd_state_get_local_generation(r) == 1);
    snprintf(rel, sizeof rel, "src/project-%03u/module/file-%06u.txt",
             (BIG_N - 1) % 997, BIG_N - 1);
    const nh_syncd_entry *e = nh_syncd_state_find(r, rel);
    assert(e && nh_syncd_entry_size(e) == BIG_N - 1);
    assert(nh_syncd_entry_chunk_count(e) == 1);
    nh_syncd_state_free(r);
    printf("t_large_snapshot_roundtrip OK\n");
}

/* Over the ceiling: refused loudly, and the file is left byte-for-byte
 * alone (runs right after the large round-trip, reusing its file). */
static void t_ceiling_refuses_and_preserves(void) {
    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    struct stat before; assert(stat(snap, &before) == 0);
    setenv("NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES", "4096", 1);
    assert(nh_syncd_snapshot_max_bytes() == 4096);
    bool unknown = false;
    nh_syncd_state *s = NULL;
    int rc = nh_syncd_state_load(g_dir, &s, &unknown);
    unsetenv("NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES");
    assert(rc == NH_SYNCD_ERR_SNAPSHOT_TOO_LARGE);
    assert(s == NULL && unknown);
    assert(strcmp(nh_syncd_state_load_error_class(rc), "snapshot-too-large") == 0);
    struct stat after; assert(stat(snap, &after) == 0);
    assert(after.st_size == before.st_size && after.st_ino == before.st_ino &&
           after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
           after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
    assert(nh_syncd_snapshot_max_bytes() == NH_SYNCD_SNAPSHOT_MAX_BYTES_DEFAULT);
    unlink(snap);
    printf("t_ceiling_refuses_and_preserves OK\n");
}

static void t_future_schema_is_refused_not_corrupt(void) {
    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    write_str(snap,
        "{\"schema\": 2, \"generation\": 9, \"root\": \"/h\", \"d_tag\": \"x\","
        " \"account_pubkey_hex\": \"\", \"root_id_hex\": \"" /* 64 hex */
        "0000000000000000000000000000000000000000000000000000000000000000"
        "\", \"files\": {}}\n");
    bool unknown = false;
    nh_syncd_state *s = NULL;
    int rc = nh_syncd_state_load(g_dir, &s, &unknown);
    assert(rc == NH_SYNCD_ERR_SNAPSHOT_SCHEMA);
    assert(s == NULL && unknown);
    unlink(snap);
    printf("t_future_schema_is_refused_not_corrupt OK\n");
}

static void t_non_regular_snapshot_is_refused(void) {
    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    assert(mkdir(snap, 0700) == 0);
    bool unknown = false;
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_load(g_dir, &s, &unknown) == NH_SYNCD_ERR_IO);
    assert(s == NULL && unknown);
    rmdir(snap);
    printf("t_non_regular_snapshot_is_refused OK\n");
}

static size_t count_quarantined(void) {
    char pat[320]; snprintf(pat, sizeof pat, "%s/snapshot.json.corrupt.*", g_dir);
    glob_t g; memset(&g, 0, sizeof g);
    size_t n = glob(pat, 0, NULL, &g) == 0 ? g.gl_pathc : 0;
    globfree(&g);
    return n;
}

static void t_quarantine_preserves_and_never_clobbers(void) {
    char snap[300]; snprintf(snap, sizeof snap, "%s/snapshot.json", g_dir);
    char *q1 = NULL, *q2 = NULL;
    write_str(snap, "{\"schema\": 1, truncated");
    assert(nh_syncd_state_quarantine_snapshot(g_dir, &q1) == NH_SYNCD_OK);
    assert(q1 && file_size(snap) < 0 && file_size(q1) > 0);
    /* Same second -> the second quarantine must pick a fresh name. */
    write_str(snap, "garbage #2");
    assert(nh_syncd_state_quarantine_snapshot(g_dir, &q2) == NH_SYNCD_OK);
    assert(q2 && strcmp(q1, q2) != 0);
    assert(file_size(q1) == (off_t)strlen("{\"schema\": 1, truncated"));
    assert(file_size(q2) == (off_t)strlen("garbage #2"));
    assert(count_quarantined() == 2);
    /* Nothing to move -> error, not a silent success. */
    assert(nh_syncd_state_quarantine_snapshot(g_dir, NULL) == NH_SYNCD_ERR_IO);
    /* After quarantine the loader sees "missing" again. */
    bool unknown = false;
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_load(g_dir, &s, &unknown) == NH_SYNCD_OK && s && unknown);
    nh_syncd_state_free(s);
    unlink(q1); unlink(q2); free(q1); free(q2);
    printf("t_quarantine_preserves_and_never_clobbers OK\n");
}

int main(void) {
    setup();
    t_missing_snapshot_is_unknown();
    t_roundtrip();
    t_malformed_json_is_unknown();
    t_large_snapshot_roundtrip();
    t_ceiling_refuses_and_preserves();
    t_future_schema_is_refused_not_corrupt();
    t_non_regular_snapshot_is_refused();
    t_quarantine_preserves_and_never_clobbers();
    teardown();
    printf("test_syncd_state: OK\n");
    return 0;
}
