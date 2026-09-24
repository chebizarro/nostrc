/*
 * test_syncd_state.c — round-trip snapshot.json + generation and
 * validate the SNAPSHOT_UNKNOWN branch. Also exercises the internal
 * upsert/delete helpers via `extern` declarations (they're used by
 * the pusher but not part of the public API).
 */

#include "nh_syncd.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
    char rmcmd[512];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", g_dir);
    (void)system(rmcmd);
    assert(mkdir(g_dir, 0700) == 0);
}
static void teardown(void) {
    char rmcmd[512];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", g_dir);
    (void)system(rmcmd);
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
    fread(buf, 1, sizeof buf - 1, gf);
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
    unlink(snap);
    printf("t_malformed_json_is_unknown OK\n");
}

int main(void) {
    setup();
    t_missing_snapshot_is_unknown();
    t_roundtrip();
    t_malformed_json_is_unknown();
    teardown();
    printf("test_syncd_state: OK\n");
    return 0;
}
