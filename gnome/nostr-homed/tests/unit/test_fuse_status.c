/*
 * test_fuse_status.c — regression for bead nostrc-h4tv.
 *
 * The fuse-status.json writer used to be inlined in
 * nostr-home-fuse.c and read a global g_session that was never
 * assigned — so the file always reported "mounted":false, even while
 * the mount was serving reads (design §6.2 health seam broken). This
 * test exercises the extracted writer directly with a caller-supplied
 * "mounted" flag, proving both transitions:
 *   1. mounted=true  → JSON contains "mounted":true
 *   2. mounted=false → JSON contains "mounted":false
 * plus schema shape (schema:1, generation, counters).
 *
 * Full-mount coverage — spawning nostr-home-fuse against a live FUSE
 * kernel dispatcher and observing the status file transitioning as
 * the session actually starts / stops — is deliberately deferred to
 * the harness / live-smoke run (bead nostrc-1u55 case (c) already
 * proves the status file is updated on real mount events).
 */

#define _GNU_SOURCE
#include "nh_fuse_status.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    char *buf = malloc((size_t)st.st_size + 1);
    assert(buf);
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        assert(r > 0);
        off += (size_t)r;
    }
    buf[off] = '\0';
    close(fd);
    return buf;
}

int main(void) {
    char tmpl[] = "/tmp/nhfuse_status_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    char path[300]; snprintf(path, sizeof path, "%s/fuse-status.json", tmpl);

    nh_fuse_source_stats_t stats = {
        .hits_local      = 4,
        .hits_chunk_lru  = 7,
        .hits_cache      = 3,
        .fetches         = 5,
        .misses          = 2,
        .last_miss_epoch = 1727170000ULL,
    };

    /* (1) mounted=true — the transition the g_session fix now produces
     *     the first time nhf_init runs. */
    assert(nh_fuse_write_status(path, /*mounted=*/true,
                                /*generation=*/42, &stats) == 0);
    {
        struct stat st; assert(stat(path, &st) == 0);
        assert((st.st_mode & 0777) == 0600);
        char *body = slurp(path);
        assert(strstr(body, "\"schema\":1") != NULL);
        assert(strstr(body, "\"mounted\":true") != NULL);
        assert(strstr(body, "\"generation\":42") != NULL);
        assert(strstr(body, "\"tier0\":true") != NULL);
        assert(strstr(body, "\"hits_local\":4") != NULL);
        assert(strstr(body, "\"hits_cache\":3") != NULL);
        assert(strstr(body, "\"fetches\":5") != NULL);
        assert(strstr(body, "\"misses\":2") != NULL);
        assert(strstr(body, "\"last_miss_epoch\":1727170000") != NULL);
        free(body);
    }

    /* (2) mounted=false — the transition nhf_destroy produces at
     *     unmount time (g_session cleared before final write). */
    assert(nh_fuse_write_status(path, /*mounted=*/false,
                                /*generation=*/42, &stats) == 0);
    {
        char *body = slurp(path);
        assert(strstr(body, "\"mounted\":false") != NULL);
        /* Sanity: writer overwrote atomically, only one JSON object. */
        assert(strstr(body, "\"mounted\":true") == NULL);
        free(body);
    }

    /* Stats-may-be-NULL path — defensive, matches the pre-source
     * call in write_status_json when g_source is not yet open. */
    assert(nh_fuse_write_status(path, /*mounted=*/false,
                                /*generation=*/0, NULL) == 0);
    {
        char *body = slurp(path);
        assert(strstr(body, "\"generation\":0") != NULL);
        assert(strstr(body, "\"hits_local\":0") != NULL);
        assert(strstr(body, "\"fetches\":0") != NULL);
        free(body);
    }

    /* No .tmp lingering from any of the above. */
    char tmp[400]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    struct stat sst;
    assert(stat(tmp, &sst) != 0 && errno == ENOENT);

    (void)unlink(path);
    (void)rmdir(tmpl);
    printf("test_fuse_status OK\n");
    return 0;
}
