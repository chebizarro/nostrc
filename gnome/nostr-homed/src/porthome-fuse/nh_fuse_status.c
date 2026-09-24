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


/* ─────────── Phase 5 I3 / h10m.1.1: mirror into porthome-status.json ─── */

#include "nh_porthome_status.h"

int nh_fuse_write_status_unified(const char *unified_path,
                                 bool mounted,
                                 const char *mountpoint,
                                 uint64_t generation,
                                 uint64_t cache_bytes,
                                 const nh_fuse_source_stats_t *stats,
                                 const char *last_error_class,
                                 int64_t last_error_ts) {
    nh_fuse_source_stats_t z = {0};
    const nh_fuse_source_stats_t *st = stats ? stats : &z;

    char mp_esc[512]  = {0};
    char err_esc[128] = {0};
    (void)nh_porthome_json_escape(mountpoint ? mountpoint : "",
                                  mp_esc, sizeof mp_esc);
    (void)nh_porthome_json_escape(last_error_class ? last_error_class : "",
                                  err_esc, sizeof err_esc);

    char body[1024];
    int m = snprintf(body, sizeof body,
        "{\"mounted\":%s,"
         "\"mountpoint\":\"%s\","
         "\"generation\":%" PRIu64 ","
         "\"cache_bytes\":%" PRIu64 ","
         "\"hits_local\":%" PRIu64 ","
         "\"hits_cache\":%" PRIu64 ","
         "\"fetches\":%" PRIu64 ","
         "\"misses\":%" PRIu64 ","
         "\"last_miss_epoch\":%" PRIu64 ","
         "\"last_error_class\":\"%s\","
         "\"last_error_ts\":%" PRId64 "}",
        mounted ? "true" : "false",
        mp_esc,
        generation,
        cache_bytes,
        st->hits_local, st->hits_cache, st->fetches, st->misses,
        st->last_miss_epoch,
        err_esc,
        last_error_ts);
    if (m <= 0 || m >= (int)sizeof body) return -EOVERFLOW;

    if (unified_path)
        return nh_porthome_status_write_key(unified_path, "fuse", body);
    return nh_porthome_status_write_key_default("fuse", body);
}
