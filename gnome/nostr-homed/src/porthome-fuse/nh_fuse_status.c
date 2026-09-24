/*
 * nh_fuse_status.c — fuse-status.json writer.
 *
 * SPDX-License-Identifier: MIT
 *
 * See nh_fuse_status.h for the schema and rationale. Bead nostrc-h4tv.
 */

#define _GNU_SOURCE
#include "nh_fuse_status.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int nh_fuse_write_status(const char *path,
                         bool mounted,
                         uint64_t generation,
                         const nh_fuse_source_stats_t *stats) {
    if (!path || !*path) return -EINVAL;
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof tmp) return -ENAMETOOLONG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;

    nh_fuse_source_stats_t z = {0};
    const nh_fuse_source_stats_t *st = stats ? stats : &z;

    char body[512];
    int m = snprintf(body, sizeof body,
        "{\"schema\":1,\"mounted\":%s,\"generation\":%" PRIu64
        ",\"tier0\":true,\"hits_local\":%" PRIu64
        ",\"hits_cache\":%" PRIu64
        ",\"fetches\":%" PRIu64
        ",\"misses\":%" PRIu64
        ",\"last_miss_epoch\":%" PRIu64 "}\n",
        mounted ? "true" : "false",
        generation,
        st->hits_local, st->hits_cache, st->fetches, st->misses,
        st->last_miss_epoch);
    if (m <= 0 || m >= (int)sizeof body) {
        close(fd);
        (void)unlink(tmp);
        return -EOVERFLOW;
    }
    ssize_t off = 0;
    while (off < m) {
        ssize_t w = write(fd, body + off, (size_t)(m - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = -errno;
            close(fd);
            (void)unlink(tmp);
            return e;
        }
        off += w;
    }
    if (close(fd) != 0) {
        int e = -errno;
        (void)unlink(tmp);
        return e;
    }
    if (rename(tmp, path) != 0) {
        int e = -errno;
        (void)unlink(tmp);
        return e;
    }
    return 0;
}
