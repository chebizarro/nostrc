/*
 * test_syncd_pin_ring_from_snapshot.c — validates the exact code path
 *   nh_syncd_pin_ring_promote_from_snapshot()
 * that the syncd daemon now invokes after every successful push
 * (W(1)(a)) and every successful pull-reconcile (W(1)(b)).
 *
 * We build a synthetic ${state_dir}/snapshot.json with two files, each
 * carrying two distinct 64-hex chunk_addrs entries, then assert that
 * after promote_from_snapshot the effective pin set contains all four
 * hashes and that the daemon's persisted pinned.json reflects them.
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-p6qp.
 */

#include "nh_syncd_cache.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void rm_rf(const char *p) {
    char c[512]; snprintf(c,sizeof c,"rm -rf '%s'",p); (void)system(c);
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, strlen(data));
    close(fd);
    return (w == (ssize_t)strlen(data)) ? 0 : -1;
}

int main(void) {
    char state_dir[128], pin_path[192];
    snprintf(state_dir, sizeof state_dir,
             "/tmp/nh_syncd_pin_snap_%d", (int)getpid());
    snprintf(pin_path,  sizeof pin_path,
             "%s/pinned.json", state_dir);
    rm_rf(state_dir);
    assert(mkdir(state_dir, 0700) == 0);

    /* snapshot.json shape matches nh_syncd_state persistence: object
     * with a numeric "generation" and an object "files" mapping
     * rel-path -> { chunk_addrs_hex: [ "<64-hex>", ... ], ... }. */
    static const char SNAP[] =
      "{\n"
      "  \"schema\": 1,\n"
      "  \"generation\": 42,\n"
      "  \"root\":   \"/home/nobody\",\n"
      "  \"d_tag\":  \"nostr-homed.home.v1:personal\",\n"
      "  \"files\": {\n"
      "    \"docs/a.txt\": { \"kind\": \"file\", \"chunk_addrs_hex\": [\n"
      "        \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
      "        \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
      "    ] },\n"
      "    \"docs/b.txt\": { \"kind\": \"file\", \"chunk_addrs_hex\": [\n"
      "        \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\n"
      "        \"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"\n"
      "    ] }\n"
      "  }\n"
      "}\n";
    char snap_path[192];
    snprintf(snap_path, sizeof snap_path, "%s/snapshot.json", state_dir);
    assert(write_file(snap_path, SNAP) == 0);

    /* Open ring at the daemon's canonical pin_path (co-located with
     * state so the same code path exercised by the daemon runs). */
    nh_syncd_pin_ring *r = NULL;
    assert(nh_syncd_pin_ring_open(pin_path, &r) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 0);

    /* Promote from the synthetic snapshot — exactly what the daemon
     * calls after nh_syncd_push_batch() and nh_syncd_reconcile_from_manifest()
     * return NH_SYNCD_OK. */
    assert(nh_syncd_pin_ring_promote_from_snapshot(r, state_dir)
           == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 1);

    /* Effective set MUST contain all four known hashes. */
    char **eff = NULL; size_t neff = 0;
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    assert(neff == 4);
    const char *expect[4] = {
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
    };
    for (int i = 0; i < 4; i++) {
        int found = 0;
        for (size_t j = 0; j < neff; j++) {
            if (strcmp(eff[j], expect[i]) == 0) { found = 1; break; }
        }
        assert(found);
    }
    for (size_t j = 0; j < neff; j++) free(eff[j]);
    free(eff);

    /* pinned.json is 0600 (matches greeter-artifact hygiene). */
    struct stat st;
    assert(stat(pin_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    /* Re-promote same generation is idempotent (daemon can fire twice
     * on the same batch id if it retries). */
    assert(nh_syncd_pin_ring_promote_from_snapshot(r, state_dir)
           == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 1);

    /* Round-trip through disk — the daemon reopens on next startup and
     * MUST see the same effective set (LRU-eviction survivorship). */
    nh_syncd_pin_ring_close(r);
    r = NULL;
    assert(nh_syncd_pin_ring_open(pin_path, &r) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 1);
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    assert(neff == 4);
    for (size_t j = 0; j < neff; j++) free(eff[j]);
    free(eff);
    nh_syncd_pin_ring_close(r);

    rm_rf(state_dir);
    printf("ok\n");
    return 0;
}
