/*
 * nh_porthome_rename.c — post-materialise rename walk (nostrc-bms6).
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Contract: see `nh_porthome_rename_walk` in nh_porthome_manifest.h.
 *
 * After `nh_porthome_materialize_into_fd` writes every manifest entry
 * under its `path_enc` name, this walker renames each leaf to its
 * `name_plain` counterpart so the on-disk layout matches the operator-
 * visible tree the source-side push saw.
 *
 * Algorithm (depth-first, deepest first):
 *
 *   1. Sort entries by descending `path_enc` depth (number of '/'+1).
 *   2. For each entry E:
 *        - Split E.path_enc → (parent_enc, leaf_enc).
 *        - Open parent_enc as a dirfd via walk_parent_dirfd(staging_fd).
 *        - `renameat(parent_dirfd, leaf_enc, parent_dirfd, name_plain)`
 *          (RENAME_EXCHANGE when both sides exist).
 *   3. On a collision (leaf_plain already exists under parent_enc) the
 *      walk aborts with NH_PORTHOME_ERR_PATH.
 *
 * By processing DEEPEST entries first, we never rename a directory
 * before all of its children have been renamed; the parent's path_enc
 * form is still intact for the next-shallower rename step, and the
 * next-shallower step then renames it to its plaintext form.
 *
 * For symlinks: because materialize wrote the LINK NODE named
 * <path_enc>-leaf pointing at whatever plaintext target the manifest
 * carried, the rename here operates on the link inode itself, not the
 * target. renameat is symlink-safe (it never chases). Nothing to do
 * for the target string.
 *
 * V1 manifests: `name_plain` is NULL everywhere; we return OK with
 * `renamed_count == 0` and `missed_count == entries_len`. The fetch
 * helper logs this INFO and leaves path_enc names on disk.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nh_porthome_manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>       /* renameat is declared here on some libcs */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

/* Linux-only renameat2 syscall wrapper. The porthome subsystem is
 * Linux-only (nostr-homed is a Linux service — sd_bus, PAM, sandbox),
 * so we can rely on syscall(2). We use it directly rather than the
 * glibc renameat2 wrapper because older glibc (< 2.28) does not
 * provide it. Falls back to plain renameat on any -ENOSYS. */
#if defined(__linux__)
#include <sys/syscall.h>
static int nhp_renameat2(int olddirfd, const char *oldpath,
                         int newdirfd, const char *newpath,
                         unsigned int flags) {
#ifdef __NR_renameat2
    long r = syscall(__NR_renameat2, olddirfd, oldpath,
                     newdirfd, newpath, flags);
    return (int)r;
#else
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}
#else
static int nhp_renameat2(int olddirfd, const char *oldpath,
                         int newdirfd, const char *newpath,
                         unsigned int flags) {
    (void)olddirfd; (void)oldpath; (void)newdirfd; (void)newpath; (void)flags;
    errno = ENOSYS;
    return -1;
}
#endif

/* Depth (component count) of a slash-joined path. */
static size_t path_depth(const char *p) {
    size_t d = 1;
    for (; *p; p++) if (*p == '/') d++;
    return d;
}

/* Open the dirfd for the PARENT of `path_enc` under `base_fd`.
 * Returns fd on success; -1 on any open error (errno set). Caller closes.
 * Empty parent (path with no '/') → dup(base_fd). */
static int open_parent_dirfd(int base_fd, const char *path_enc,
                             const char **out_leaf) {
    const char *slash = strrchr(path_enc, '/');
    if (!slash) {
        *out_leaf = path_enc;
        return dup(base_fd);
    }
    size_t parent_len = (size_t)(slash - path_enc);
    if (parent_len == 0) { errno = EINVAL; return -1; } /* leading '/' */
    char pbuf[4096];
    if (parent_len >= sizeof pbuf) { errno = ENAMETOOLONG; return -1; }
    memcpy(pbuf, path_enc, parent_len);
    pbuf[parent_len] = '\0';
    *out_leaf = slash + 1;

    /* Walk each component with openat + O_NOFOLLOW so we can't be
     * redirected through a symlink. */
    int cur = dup(base_fd);
    if (cur < 0) return -1;
    const char *p = pbuf;
    while (*p) {
        const char *s = strchr(p, '/');
        size_t clen = s ? (size_t)(s - p) : strlen(p);
        char comp[256];
        if (clen == 0 || clen >= sizeof comp) {
            close(cur); errno = ENAMETOOLONG; return -1;
        }
        memcpy(comp, p, clen); comp[clen] = '\0';
        int next = openat(cur, comp,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(cur);
        if (next < 0) return -1;
        cur = next;
        if (!s) break;
        p = s + 1;
    }
    return cur;
}

/* Sort by path_enc depth descending (stable). Straight insertion sort —
 * expected entry counts are small (100..500) and the sort runs once
 * per pull. */
typedef struct { size_t idx; size_t depth; } rw_ord;

static int rw_ord_cmp_desc(const void *a, const void *b) {
    const rw_ord *x = (const rw_ord *)a;
    const rw_ord *y = (const rw_ord *)b;
    if (x->depth < y->depth) return 1;
    if (x->depth > y->depth) return -1;
    /* Stable within depth: original insertion order. */
    if (x->idx < y->idx) return -1;
    if (x->idx > y->idx) return 1;
    return 0;
}

int nh_porthome_rename_walk(int staging_fd,
                            const nh_porthome_manifest *m,
                            size_t *out_renamed_count,
                            size_t *out_missed_count) {
    if (out_renamed_count) *out_renamed_count = 0;
    if (out_missed_count)  *out_missed_count  = 0;
    if (staging_fd < 0 || !m) return NH_PORTHOME_ERR_ARG;
    if (m->entries_len == 0) return NH_PORTHOME_OK;

    /* Fast-exit on v1 manifests: no plaintext names at all → nothing
     * to rename. Log an INFO here so the fetch helper's caller sees it. */
    int any_v2 = 0;
    for (size_t i = 0; i < m->entries_len; i++)
        if (m->entries[i].name_plain) { any_v2 = 1; break; }
    if (!any_v2) {
        if (out_missed_count) *out_missed_count = m->entries_len;
        syslog(LOG_INFO,
               "porthome: manifest v1 — keeping path_enc names (no name_plain fields)");
        return NH_PORTHOME_OK;
    }

    /* Build a descending-depth order. */
    rw_ord *ord = (rw_ord *)calloc(m->entries_len, sizeof(*ord));
    if (!ord) return NH_PORTHOME_ERR_OOM;
    for (size_t i = 0; i < m->entries_len; i++) {
        ord[i].idx   = i;
        ord[i].depth = m->entries[i].path_enc ? path_depth(m->entries[i].path_enc) : 0;
    }
    qsort(ord, m->entries_len, sizeof(*ord), rw_ord_cmp_desc);

    size_t renamed = 0, missed = 0;
    int rc = NH_PORTHOME_OK;
    for (size_t k = 0; k < m->entries_len; k++) {
        const nh_porthome_entry *e = &m->entries[ord[k].idx];
        if (!e->path_enc) { missed++; continue; }
        if (!e->name_plain) { missed++; continue; }

        const char *leaf_enc = NULL;
        int parent_fd = open_parent_dirfd(staging_fd, e->path_enc, &leaf_enc);
        if (parent_fd < 0) {
            syslog(LOG_ERR,
                   "porthome: rename walk: open parent for '%s' failed: %s",
                   e->path_enc, strerror(errno));
            rc = NH_PORTHOME_ERR_ARG;
            break;
        }
        /* Fast path: if leaf_enc already equals name_plain (identity
         * rename), skip. Cheap safety belt for the rare case a caller
         * built the manifest with plaintext leaves already. */
        if (strcmp(leaf_enc, e->name_plain) == 0) {
            close(parent_fd);
            renamed++;
            continue;
        }

        /* Collision guard: refuse to overwrite an existing plaintext
         * entry. That would indicate a manifest with two entries whose
         * plaintext basenames collide inside the same parent — either a
         * hostile pointer or a bug. */
        struct stat st;
        if (fstatat(parent_fd, e->name_plain, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            syslog(LOG_ERR,
                   "porthome: rename collision at %s/%s — aborting materialise",
                   e->path_enc, e->name_plain);
            close(parent_fd);
            rc = NH_PORTHOME_ERR_PATH;
            break;
        }
        if (errno != ENOENT) {
            int se = errno;
            syslog(LOG_ERR,
                   "porthome: fstatat(%s/%s) failed: %s",
                   e->path_enc, e->name_plain, strerror(se));
            close(parent_fd);
            rc = NH_PORTHOME_ERR_PATH;
            break;
        }

        /* Prefer renameat2(RENAME_NOREPLACE) — atomic guard against the
         * collision race between our stat and the rename. Fall back to
         * plain renameat on ENOSYS (kernel < 3.15). */
        int rr = nhp_renameat2(parent_fd, leaf_enc,
                               parent_fd, e->name_plain,
                               RENAME_NOREPLACE);
        if (rr != 0 && errno == ENOSYS) {
            rr = renameat(parent_fd, leaf_enc, parent_fd, e->name_plain);
        }
        if (rr != 0) {
            int se = errno;
            syslog(LOG_ERR,
                   "porthome: renameat(%s → %s under %s) failed: %s",
                   leaf_enc, e->name_plain, e->path_enc, strerror(se));
            close(parent_fd);
            rc = NH_PORTHOME_ERR_PATH;
            break;
        }
        /* fsync the parent so the rename is durable before we move
         * on to the shallower level. */
        (void)fsync(parent_fd);
        close(parent_fd);
        renamed++;
    }

    free(ord);
    if (out_renamed_count) *out_renamed_count = renamed;
    if (out_missed_count)  *out_missed_count  = missed;
    if (rc == NH_PORTHOME_OK) {
        syslog(LOG_INFO,
               "porthome: renamed %zu of %zu entries (missed=%zu)",
               renamed, m->entries_len, missed);
    }
    return rc;
}
