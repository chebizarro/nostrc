/*
 * nh_syncd_lock.c — single-instance flock on ${state}/sync.lock (§6.4).
 *
 * SPDX-License-Identifier: MIT
 *
 * flock(2) is used advisory + LOCK_NB. It survives the daemon's own
 * fork/exec (kept via the fd) and releases when the fd closes.
 */

#include "nh_syncd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct nh_syncd_lock { int fd; };

static int mkdirp_local(const char *path) {
    if (!path || !*path) return NH_SYNCD_ERR_ARG;
    char *tmp = strdup(path);
    if (!tmp) return NH_SYNCD_ERR_OOM;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST) { free(tmp); return NH_SYNCD_ERR_IO; }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) { free(tmp); return NH_SYNCD_ERR_IO; }
    free(tmp);
    return NH_SYNCD_OK;
}

int nh_syncd_lock_acquire(const char *state_dir, nh_syncd_lock **out) {
    if (!state_dir || !out) return NH_SYNCD_ERR_ARG;
    int rc = mkdirp_local(state_dir);
    if (rc < 0) return rc;
    size_t n = strlen(state_dir) + strlen("/sync.lock") + 1;
    char *path = malloc(n);
    if (!path) return NH_SYNCD_ERR_OOM;
    snprintf(path, n, "%s/sync.lock", state_dir);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(path);
    if (fd < 0) return NH_SYNCD_ERR_IO;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int held = (errno == EWOULDBLOCK);
        close(fd);
        return held ? NH_SYNCD_ERR_LOCKED : NH_SYNCD_ERR_IO;
    }
    nh_syncd_lock *l = calloc(1, sizeof *l);
    if (!l) { close(fd); return NH_SYNCD_ERR_OOM; }
    l->fd = fd;
    *out = l;
    return NH_SYNCD_OK;
}

void nh_syncd_lock_release(nh_syncd_lock *l) {
    if (!l) return;
    if (l->fd >= 0) {
        flock(l->fd, LOCK_UN);
        close(l->fd);
    }
    free(l);
}
