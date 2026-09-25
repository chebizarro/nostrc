/*
 * test_syncd_rescan_overflow.c — xnxd part 2 coverage.
 *
 * SPDX-License-Identifier: MIT
 *
 * Exercises nh_syncd_rescan_diff directly (the same primitive the
 * watcher calls on IN_Q_OVERFLOW). Asserts:
 *   1. On a tree that matches state exactly, zero events are pushed and
 *      all four stats-counters are zero except `unchanged`.
 *   2. After adding three files and removing one, the rescan produces
 *      4 events (3 CREATE, 1 DELETE) and a matching stats block.
 *   3. A second immediate rescan against the SAME snapshot is idempotent
 *      (still four events — because state hasn't been updated by the
 *      push path yet). Once the state IS updated, the rescan produces
 *      zero events on the third pass.
 */

#include "nh_syncd.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>

extern int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                       const char *rel,
                                       uint32_t mode, uint32_t uid, uint32_t gid,
                                       uint64_t mtime_ns, uint64_t size,
                                       const char *content_hash_hex,
                                       const char *const *chunk_addrs_hex,
                                       size_t chunk_addrs_n);
extern int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel);

static char g_home[300];
static const char *FIXTURE_PK = "0000000000000000000000000000000000000000000000000000000000000001";

static void write_file(const char *rel, const char *body) {
    char abs[600];
    snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    FILE *f = fopen(abs, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void setup_tree(void) {
    snprintf(g_home, sizeof g_home, "/tmp/nh_rescan_home_%d", (int)getpid());
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s", g_home);
    (void)system(rm);
    assert(mkdir(g_home, 0700) == 0);
    write_file("keep.txt", "keep me\n");
    write_file("also.txt", "still here\n");
}

static void teardown(void) {
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s", g_home);
    (void)system(rm);
}

/* Rebuild `state` from what's on disk RIGHT NOW so its (size, mtime_ns)
 * tuples match. Used to synthesise the "baseline" for idempotency
 * checks. */
static void snapshot_state(nh_syncd_state *state) {
    /* Enumerate existing files. */
    struct dirent *de;
    DIR *d = opendir(g_home);
    assert(d);
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char abs[600];
        snprintf(abs, sizeof abs, "%s/%s", g_home, de->d_name);
        struct stat st;
        if (lstat(abs, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        uint64_t mt = (uint64_t)st.st_mtim.tv_sec * 1000000000ull
                    + (uint64_t)st.st_mtim.tv_nsec;
        (void)nh_syncd_state_upsert_file_(state, de->d_name,
                                          st.st_mode & 07777,
                                          (uint32_t)st.st_uid,
                                          (uint32_t)st.st_gid,
                                          mt, (uint64_t)st.st_size,
                                          "", NULL, 0);
    }
    closedir(d);
}

static uint64_t vnow(void *ud) { (void)ud; static uint64_t t = 0; t += 10ull*1000000000ull; return t; }

static void t_quiescent_zero_events(void) {
    setup_tree();
    uint8_t root[32]; for (int i = 0; i < 32; i++) root[i] = (uint8_t)i;
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "d", FIXTURE_PK, root, &state) == 0);
    snapshot_state(state);

    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(vnow, NULL, 1, 1, &b) == 0);
    nh_syncd_rescan_stats st = {0};
    int rc = nh_syncd_rescan_diff(state, g_home, ig, b, &st);
    assert(rc == NH_SYNCD_OK);
    assert(st.added == 0);
    assert(st.modified == 0);
    assert(st.deleted == 0);
    assert(st.unchanged >= 2);
    /* Batcher should now be IDLE (nothing pushed). */
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);
    assert(bat == NULL || nh_syncd_batch_len(bat) == 0);
    if (bat) nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    nh_syncd_ignore_free(ig);
    nh_syncd_state_free(state);
    teardown();
    printf("t_quiescent_zero_events OK\n");
}

static void t_added_and_removed(void) {
    setup_tree();
    uint8_t root[32]; for (int i = 0; i < 32; i++) root[i] = (uint8_t)i;
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "d", FIXTURE_PK, root, &state) == 0);
    snapshot_state(state);

    /* Diverge: add 3 files, remove 1 (also.txt). */
    char abs_removed[600];
    snprintf(abs_removed, sizeof abs_removed, "%s/also.txt", g_home);
    assert(unlink(abs_removed) == 0);
    write_file("new1.txt", "first new\n");
    write_file("new2.txt", "second new\n");
    write_file("new3.txt", "third new\n");

    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(vnow, NULL, 1, 1, &b) == 0);

    nh_syncd_rescan_stats st = {0};
    int rc = nh_syncd_rescan_diff(state, g_home, ig, b, &st);
    assert(rc == NH_SYNCD_OK);
    assert(st.added == 3);
    assert(st.deleted == 1);
    assert(st.unchanged >= 1); /* keep.txt */
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);
    assert(bat != NULL);
    assert(nh_syncd_batch_len(bat) == 4);
    nh_syncd_batch_free(bat);

    /* Immediate second rescan against the SAME (unchanged) state
     * MUST return the same divergence (idempotent up to state
     * mutation). */
    nh_syncd_rescan_stats st2 = {0};
    (void)nh_syncd_rescan_diff(state, g_home, ig, b, &st2);
    assert(st2.added == 3);
    assert(st2.deleted == 1);
    nh_syncd_batch *bat2 = nh_syncd_batcher_take(b, true);
    assert(bat2 != NULL);
    assert(nh_syncd_batch_len(bat2) == 4);
    nh_syncd_batch_free(bat2);

    /* Now simulate the push loop updating state: promote the new files
     * and drop the removed one. Third rescan should produce ZERO events. */
    for (int i = 0; i < 3; i++) {
        char rel[32]; snprintf(rel, sizeof rel, "new%d.txt", i + 1);
        char abs[600]; snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
        struct stat s;
        assert(lstat(abs, &s) == 0);
        uint64_t mt = (uint64_t)s.st_mtim.tv_sec * 1000000000ull
                    + (uint64_t)s.st_mtim.tv_nsec;
        (void)nh_syncd_state_upsert_file_(state, rel,
                                          s.st_mode & 07777,
                                          (uint32_t)s.st_uid,
                                          (uint32_t)s.st_gid,
                                          mt, (uint64_t)s.st_size,
                                          "", NULL, 0);
    }
    (void)nh_syncd_state_delete_(state, "also.txt");
    nh_syncd_rescan_stats st3 = {0};
    (void)nh_syncd_rescan_diff(state, g_home, ig, b, &st3);
    assert(st3.added == 0);
    assert(st3.deleted == 0);
    assert(st3.modified == 0);
    nh_syncd_batch *bat3 = nh_syncd_batcher_take(b, true);
    assert(bat3 == NULL || nh_syncd_batch_len(bat3) == 0);
    if (bat3) nh_syncd_batch_free(bat3);

    nh_syncd_batcher_free(b);
    nh_syncd_ignore_free(ig);
    nh_syncd_state_free(state);
    teardown();
    printf("t_added_and_removed OK\n");
}

int main(void) {
    t_quiescent_zero_events();
    t_added_and_removed();
    printf("test_syncd_rescan_overflow: ALL OK\n");
    return 0;
}
