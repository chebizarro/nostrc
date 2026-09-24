/*
 * test_syncd_cache.c — content-addressed cache basics.
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers: put/get/has round-trip; sharded layout; atomic write
 * (crash-safe stale-tmp cleanup); file mode 0600; put with mismatched
 * hash refused; open on an existing dir preserves entries; forbidden
 * DELETE guard fires.
 */

#include "nh_syncd_cache.h"
#include "nh_porthome_crypto.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void hex_of(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    assert(nh_porthome_sha256(buf, len, h) == 0);
    nh_porthome_hex64(h, out);
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    (void)system(cmd);
}

static char *tmp_dir(const char *tag) {
    char *p = malloc(256);
    snprintf(p, 256, "/tmp/nh_syncd_cache_%s_%d", tag, (int)getpid());
    rm_rf(p);
    return p;
}

int main(void) {
    char *dir = tmp_dir("basic");

    /* Open with a large explicit quota so LRU never fires here. */
    nh_syncd_cache *c = NULL;
    assert(nh_syncd_cache_open(dir, 1ull << 30, &c) == NH_SYNCD_CACHE_OK);
    assert(c != NULL);
    assert(nh_syncd_cache_quota_bytes(c) == (1ull << 30));

    /* Directory exists and is 0700. */
    struct stat st;
    assert(stat(dir, &st) == 0);
    assert(S_ISDIR(st.st_mode));
    assert((st.st_mode & 0777) == 0700);

    /* Put a small blob. */
    const uint8_t body[] = "hello portable-home cache";
    char hex[65]; hex_of(body, sizeof body - 1, hex);

    assert(!nh_syncd_cache_has(c, hex));
    assert(nh_syncd_cache_put(c, hex, body, sizeof body - 1) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_has(c, hex));

    /* Sharded path: dir/aa/bb/aabb… with mode 0600. */
    char path[1024];
    snprintf(path, sizeof path, "%s/%c%c/%c%c/%s",
             dir, hex[0], hex[1], hex[2], hex[3], hex);
    assert(stat(path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    /* Read back via get_path. */
    char *got = nh_syncd_cache_get_path(c, hex);
    assert(got != NULL);
    assert(strcmp(got, path) == 0);

    FILE *f = fopen(got, "rb");
    assert(f);
    uint8_t rd[128] = {0};
    size_t rn = fread(rd, 1, sizeof rd, f);
    fclose(f);
    assert(rn == sizeof body - 1);
    assert(memcmp(rd, body, rn) == 0);
    free(got);

    /* Idempotent second put returns OK. */
    assert(nh_syncd_cache_put(c, hex, body, sizeof body - 1) == NH_SYNCD_CACHE_OK);

    /* Mismatched hash refused. */
    char wrong[65]; hex_of((const uint8_t *)"different", 9, wrong);
    assert(nh_syncd_cache_put(c, wrong, body, sizeof body - 1)
           == NH_SYNCD_CACHE_ERR_HASH);
    assert(!nh_syncd_cache_has(c, wrong));

    /* Argument validation. */
    assert(nh_syncd_cache_put(c, "not hex", body, 1) == NH_SYNCD_CACHE_ERR_ARG);
    assert(nh_syncd_cache_has(c, "not hex") == false);

    /* Used bytes accounts for the single stored blob. */
    assert(nh_syncd_cache_used_bytes(c) == (uint64_t)(sizeof body - 1));

    /* Crash simulation: drop a stale .tmp. file older than 1h into
     * the shard dir, run cleanup, assert it is gone. */
    char shard[1024];
    snprintf(shard, sizeof shard, "%s/%c%c/%c%c",
             dir, hex[0], hex[1], hex[2], hex[3]);
    char stale[1024];
    snprintf(stale, sizeof stale, "%s/%s.tmp.999.deadbeef", shard, hex);
    int sfd = open(stale, O_CREAT | O_WRONLY, 0600);
    assert(sfd >= 0);
    (void)write(sfd, "garbage", 7);
    close(sfd);
    struct timespec back[2] = {
        { .tv_sec = time(NULL) - 7200, .tv_nsec = 0 },
        { .tv_sec = time(NULL) - 7200, .tv_nsec = 0 },
    };
    assert(utimensat(AT_FDCWD, stale, back, 0) == 0);
    assert(nh_syncd_cache_cleanup_stale_tmps(c) == NH_SYNCD_CACHE_OK);
    assert(access(stale, F_OK) != 0);
    /* The blob file is untouched. */
    assert(nh_syncd_cache_has(c, hex));

    /* Recent .tmp. (< 1h) is kept — a concurrent writer must not have
     * its scratch stolen. */
    snprintf(stale, sizeof stale, "%s/%s.tmp.998.cafebabe", shard, hex);
    sfd = open(stale, O_CREAT | O_WRONLY, 0600);
    assert(sfd >= 0);
    (void)write(sfd, "in-flight", 9);
    close(sfd);
    assert(nh_syncd_cache_cleanup_stale_tmps(c) == NH_SYNCD_CACHE_OK);
    assert(access(stale, F_OK) == 0);
    /* Clean it up so used-bytes accounting isn't confused later. */
    unlink(stale);

    /* Re-open the same dir: entries persist. */
    nh_syncd_cache_close(c);
    c = NULL;
    assert(nh_syncd_cache_open(dir, 1ull << 30, &c) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_has(c, hex));
    nh_syncd_cache_close(c);

    /* Forbidden remote DELETE guard. */
    assert(nh_syncd_cache_forbid_remote_delete("test") == NH_SYNCD_CACHE_ERR_ARG);

    /* Default paths honour XDG_*_HOME. */
    setenv("XDG_CACHE_HOME", "/tmp/xdg-cache-test", 1);
    char *d = nh_syncd_cache_default_dir();
    assert(d && strcmp(d, "/tmp/xdg-cache-test/nostr-homed/blobs") == 0);
    free(d);
    unsetenv("XDG_CACHE_HOME");

    setenv("XDG_STATE_HOME", "/tmp/xdg-state-test", 1);
    char *p = nh_syncd_cache_default_pin_path();
    assert(p && strcmp(p, "/tmp/xdg-state-test/nostr-homed/pinned.json") == 0);
    free(p);
    unsetenv("XDG_STATE_HOME");

    rm_rf(dir);
    free(dir);
    printf("ok\n");
    return 0;
}
