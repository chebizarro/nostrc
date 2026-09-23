/*
 * nh_porthome_provision.c — Phase 2 materializer. Writes into an
 * openat-relative staging descriptor. See nh_porthome_provision.h.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "nh_porthome_provision.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#define DEFAULT_MAX_BYTES_PER_LOAD (2ull * 1024ull * 1024ull * 1024ull)
#define DEFAULT_LOAD_TIMEOUT_SEC   120u
#define DEFAULT_MAX_TOTAL_BYTES    (20ull * 1024ull * 1024ull * 1024ull)

/* Path-component validation — mirrors identity_home.c's beneath() rules
 * plus the encrypted-name shape (48-hex-char components produced by
 * nh_porthome_encrypt_path). We do NOT require the 48-hex shape here —
 * a caller may build a manifest with plaintext components (the smallhome
 * driver does, for round-trip validation) — but we DO enforce the
 * traversal-safety subset. */
static int comp_ok(const char *s, size_t n) {
    if (n == 0 || n > 255) return 0;
    if (n == 1 && s[0] == '.') return 0;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0' || c == '/' || c == '\\') return 0;
    }
    return 1;
}

/* mkdirat/openat every intermediate component of `path` under `base_fd`,
 * finally returning a dirfd for the LAST component if `is_dir`, or a
 * dirfd for the parent (with `*out_leaf` copied to the last component)
 * if !is_dir. */
static int walk_and_mkdirs(int base_fd, const char *path, int is_dir,
                           uid_t uid, gid_t gid,
                           char *out_leaf, size_t out_leaf_cap,
                           int *out_dirfd) {
    if (!path || !*path) return NH_PORTHOME_PROV_INVARIANT;
    if (path[0] == '/') return NH_PORTHOME_PROV_INVARIANT;
    if (path[strlen(path) - 1] == '/') return NH_PORTHOME_PROV_INVARIANT;

    /* Duplicate base_fd so caller-owned fd is not consumed. */
    int cur = dup(base_fd);
    if (cur < 0) return NH_PORTHOME_PROV_IO;

    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
        if (!comp_ok(p, seg_len)) { close(cur); return NH_PORTHOME_PROV_INVARIANT; }

        int is_last = !slash;
        char name[256];
        memcpy(name, p, seg_len);
        name[seg_len] = '\0';

        if (is_last && !is_dir) {
            /* Return the parent dirfd + leaf name to the caller. */
            if (out_leaf_cap <= seg_len) { close(cur); return NH_PORTHOME_PROV_INVARIANT; }
            memcpy(out_leaf, name, seg_len + 1);
            *out_dirfd = cur;
            return NH_PORTHOME_PROV_OK;
        }

        /* Directory segment: mkdirat 0700 (idempotent, EEXIST tolerated
         * only if the existing entry is a real directory). */
        if (mkdirat(cur, name, 0700) != 0) {
            if (errno != EEXIST) { close(cur); return NH_PORTHOME_PROV_IO; }
        } else {
            /* Chown fresh dirs to the target uid/gid. */
            (void)fchownat(cur, name, uid, gid, AT_SYMLINK_NOFOLLOW);
        }
        int next = openat(cur, name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(cur);
        if (next < 0) return NH_PORTHOME_PROV_IO;
        cur = next;
        struct stat st;
        if (fstat(cur, &st) != 0 || !S_ISDIR(st.st_mode)) {
            close(cur);
            return NH_PORTHOME_PROV_INVARIANT;
        }
        if (is_last && is_dir) {
            *out_dirfd = cur;
            return NH_PORTHOME_PROV_OK;
        }
        p = slash + 1;
    }
    close(cur);
    return NH_PORTHOME_PROV_INVARIANT;
}

static uint64_t now_monotonic_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec;
}

/* Best-effort progress write (0640, atomic via tmp+rename). Silently
 * ignored on failure — progress is a UX hint, not a correctness gate. */
static void progress_write(const char *path, const char *tx,
                           const char *state,
                           uint64_t bytes, uint64_t total_bytes,
                           size_t files, size_t total_files) {
    if (!path || !*path) return;
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0);
    if (fd < 0) return;
    char body[512];
    n = snprintf(body, sizeof body,
                 "{\"tx\":\"%s\",\"state\":\"%s\",\"bytes\":%llu,"
                 "\"total_bytes\":%llu,\"files\":%zu,\"total_files\":%zu}\n",
                 tx ? tx : "",
                 state ? state : "in_progress",
                 (unsigned long long)bytes,
                 (unsigned long long)total_bytes,
                 files, total_files);
    if (n > 0 && (size_t)n < sizeof body) {
        ssize_t w = write(fd, body, (size_t)n);
        (void)w;
    }
    (void)fchmod(fd, 0640);
    close(fd);
    (void)rename(tmp, path);
}

static int apply_symlink(int base_fd, const nh_porthome_entry *e,
                         uid_t uid, gid_t gid) {
    char leaf[256];
    int parent_fd = -1;
    int r = walk_and_mkdirs(base_fd, e->path_enc, 0, uid, gid,
                            leaf, sizeof leaf, &parent_fd);
    if (r != NH_PORTHOME_PROV_OK) return r;
    if (!e->symlink_target || !e->symlink_target[0]) {
        close(parent_fd);
        return NH_PORTHOME_PROV_INVARIANT;
    }
    /* Refuse absolute symlink targets — they can point outside the
     * home once the home is renamed into place. */
    if (e->symlink_target[0] == '/') {
        close(parent_fd);
        return NH_PORTHOME_PROV_INVARIANT;
    }
    /* Length cap (matches manifest cap). */
    size_t tlen = strnlen(e->symlink_target, NH_PORTHOME_MAX_SYMLINK_LEN + 1);
    if (tlen > NH_PORTHOME_MAX_SYMLINK_LEN) {
        close(parent_fd);
        return NH_PORTHOME_PROV_INVARIANT;
    }
    if (symlinkat(e->symlink_target, parent_fd, leaf) != 0) {
        int e2 = errno;
        close(parent_fd);
        return e2 == EEXIST ? NH_PORTHOME_PROV_OK : NH_PORTHOME_PROV_IO;
    }
    (void)fchownat(parent_fd, leaf, uid, gid, AT_SYMLINK_NOFOLLOW);
    close(parent_fd);
    return NH_PORTHOME_PROV_OK;
}

static int apply_dir(int base_fd, const nh_porthome_entry *e,
                     uid_t uid, gid_t gid) {
    int dirfd = -1;
    int r = walk_and_mkdirs(base_fd, e->path_enc, 1, uid, gid, NULL, 0, &dirfd);
    if (r != NH_PORTHOME_PROV_OK) return r;
    /* Apply masked mode. */
    uint32_t mode = e->mode & 0777;
    (void)fchmod(dirfd, (mode_t)mode);
    (void)fchown(dirfd, uid, gid);
    close(dirfd);
    return NH_PORTHOME_PROV_OK;
}

static int apply_file(int base_fd, const nh_porthome_entry *e,
                      const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                      nh_porthome_prov_fetch_fn fetch, void *fetch_ctx,
                      uid_t uid, gid_t gid,
                      size_t cap_bytes, unsigned cap_seconds,
                      size_t *total_bytes_running,
                      size_t total_bytes_cap) {
    char leaf[256];
    int parent_fd = -1;
    int r = walk_and_mkdirs(base_fd, e->path_enc, 0, uid, gid,
                            leaf, sizeof leaf, &parent_fd);
    if (r != NH_PORTHOME_PROV_OK) return r;

    if (e->size > cap_bytes) {
        close(parent_fd);
        return NH_PORTHOME_PROV_BUDGET;
    }
    if (e->size && !e->chunks_len) {
        close(parent_fd);
        return NH_PORTHOME_PROV_INVARIANT;
    }

    /* Write to a .part sibling then rename atomically. */
    char part[280];
    int n = snprintf(part, sizeof part, "%s.part", leaf);
    if (n <= 0 || (size_t)n >= sizeof part) {
        close(parent_fd);
        return NH_PORTHOME_PROV_INVARIANT;
    }
    /* O_CREAT|O_EXCL — a stale .part from a crashed prior attempt is
     * unlinked first (bounded). */
    (void)unlinkat(parent_fd, part, 0);
    int out_fd = openat(parent_fd, part,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
    if (out_fd < 0) { close(parent_fd); return NH_PORTHOME_PROV_IO; }

    uint64_t deadline = 0;
    if (cap_seconds) deadline = now_monotonic_sec() + cap_seconds;

    uint64_t wrote = 0;
    int rc = NH_PORTHOME_PROV_OK;
    for (size_t i = 0; i < e->chunks_len; i++) {
        if (deadline && now_monotonic_sec() > deadline) { rc = NH_PORTHOME_PROV_LIMITED; break; }
        char hex[65];
        nh_porthome_hex64(e->chunks[i].sha256, hex);
        uint8_t *ct = NULL; size_t ct_len = 0;
        if (fetch(fetch_ctx, hex, &ct, &ct_len) != 0 || !ct) {
            rc = NH_PORTHOME_PROV_LIMITED; break;
        }
        uint8_t *pt = NULL; size_t pt_len = 0;
        int drc = nh_porthome_decrypt_chunk(home_key, ct, ct_len, &pt, &pt_len);
        OPENSSL_cleanse(ct, ct_len);
        free(ct);
        if (drc != 0) { rc = NH_PORTHOME_PROV_INVARIANT; break; }

        if (wrote + pt_len > cap_bytes) { free(pt); rc = NH_PORTHOME_PROV_BUDGET; break; }
        if (total_bytes_cap &&
            (*total_bytes_running + pt_len > total_bytes_cap)) {
            free(pt);
            rc = NH_PORTHOME_PROV_BUDGET;
            break;
        }

        size_t off = 0;
        while (off < pt_len) {
            ssize_t w = write(out_fd, pt + off, pt_len - off);
            if (w < 0) { if (errno == EINTR) continue; rc = NH_PORTHOME_PROV_IO; break; }
            off += (size_t)w;
        }
        wrote += pt_len;
        *total_bytes_running += pt_len;
        OPENSSL_cleanse(pt, pt_len);
        free(pt);
        if (rc != NH_PORTHOME_PROV_OK) break;
    }

    if (rc == NH_PORTHOME_PROV_OK) {
        /* size check: manifest advertised e->size; we wrote `wrote`. */
        if (wrote != e->size) rc = NH_PORTHOME_PROV_INVARIANT;
    }

    if (rc == NH_PORTHOME_PROV_OK) {
        uint32_t mode = e->mode & 0777;
        if (fchmod(out_fd, (mode_t)mode) != 0) rc = NH_PORTHOME_PROV_IO;
        if (rc == NH_PORTHOME_PROV_OK && fchown(out_fd, uid, gid) != 0)
            rc = NH_PORTHOME_PROV_IO;
        if (rc == NH_PORTHOME_PROV_OK && fsync(out_fd) != 0)
            rc = NH_PORTHOME_PROV_IO;
    }
    close(out_fd);

    if (rc == NH_PORTHOME_PROV_OK) {
        /* Atomic rename to leaf. Manifest's e->path_enc unique per entry
         * per design; a duplicate is a hostile-manifest INVARIANT. */
        if (renameat(parent_fd, part, parent_fd, leaf) != 0) {
            /* Renaming over an existing entry is an invariant break
             * (uniqueness). Fall back to unlink + rename would mask
             * the bug — refuse. */
            (void)unlinkat(parent_fd, part, 0);
            rc = NH_PORTHOME_PROV_INVARIANT;
        }
    } else {
        (void)unlinkat(parent_fd, part, 0);
    }
    close(parent_fd);
    return rc;
}

nh_porthome_prov_status nh_porthome_materialize_into_fd(
    int staging_fd,
    const nh_porthome_manifest *m,
    const uint8_t home_key[NH_PORTHOME_KEY_LEN],
    nh_porthome_prov_fetch_fn fetch, void *fetch_ctx,
    const nh_porthome_prov_opts *opts,
    const char *tx_id_or_null) {
    if (staging_fd < 0 || !m || !home_key || !fetch || !opts)
        return NH_PORTHOME_PROV_ARG;

    size_t cap_bytes = opts->max_bytes_per_load ?
                       opts->max_bytes_per_load : DEFAULT_MAX_BYTES_PER_LOAD;
    unsigned cap_seconds = opts->load_timeout_sec ?
                           opts->load_timeout_sec : DEFAULT_LOAD_TIMEOUT_SEC;
    size_t total_cap = opts->max_total_bytes ?
                       opts->max_total_bytes : DEFAULT_MAX_TOTAL_BYTES;
    size_t entries_cap = opts->max_entries ?
                         opts->max_entries : NH_PORTHOME_MAX_ENTRIES;
    if (m->entries_len > entries_cap)
        return NH_PORTHOME_PROV_INVARIANT;

    /* Two passes: dirs first (any order but longest-prefix stability
     * is easier if the manifest was built dfs-first, which it is here),
     * then files, then symlinks. */
    size_t total_files = 0;
    for (size_t i = 0; i < m->entries_len; i++)
        if (m->entries[i].kind == NH_PORTHOME_KIND_FILE) total_files++;

    size_t running_bytes = 0;
    size_t applied_files = 0;
    nh_porthome_prov_status final_rc = NH_PORTHOME_PROV_OK;

    progress_write(opts->progress_path, tx_id_or_null, "in_progress",
                   0, 0, 0, total_files);

    /* Pass 1: directories. */
    for (size_t i = 0; i < m->entries_len; i++) {
        const nh_porthome_entry *e = &m->entries[i];
        if (e->kind != NH_PORTHOME_KIND_DIR) continue;
        int r = apply_dir(staging_fd, e, opts->local_uid, opts->local_gid);
        if (r != NH_PORTHOME_PROV_OK) { final_rc = r; goto done; }
    }

    /* Pass 2: files. */
    for (size_t i = 0; i < m->entries_len; i++) {
        const nh_porthome_entry *e = &m->entries[i];
        if (e->kind != NH_PORTHOME_KIND_FILE) continue;
        int r = apply_file(staging_fd, e, home_key, fetch, fetch_ctx,
                           opts->local_uid, opts->local_gid,
                           cap_bytes, cap_seconds,
                           &running_bytes, total_cap);
        if (r != NH_PORTHOME_PROV_OK) { final_rc = r; goto done; }
        applied_files++;
        progress_write(opts->progress_path, tx_id_or_null, "in_progress",
                       running_bytes, 0, applied_files, total_files);
    }

    /* Pass 3: symlinks. */
    for (size_t i = 0; i < m->entries_len; i++) {
        const nh_porthome_entry *e = &m->entries[i];
        if (e->kind != NH_PORTHOME_KIND_SYMLINK) continue;
        int r = apply_symlink(staging_fd, e, opts->local_uid, opts->local_gid);
        if (r != NH_PORTHOME_PROV_OK) { final_rc = r; goto done; }
    }

done:
    /* Sync the staging dir so subsequent renameat2 install is durable. */
    (void)fsync(staging_fd);
    progress_write(opts->progress_path, tx_id_or_null,
                   final_rc == NH_PORTHOME_PROV_OK ? "ok" :
                   final_rc == NH_PORTHOME_PROV_LIMITED ? "limited" :
                   final_rc == NH_PORTHOME_PROV_BUDGET ? "limited" : "failed",
                   running_bytes, 0, applied_files, total_files);
    return final_rc;
}

nh_porthome_prov_status nh_porthome_materialize_sealed_into_fd(
    int staging_fd,
    const uint8_t *sealed_manifest, size_t sealed_len,
    const uint8_t home_key[NH_PORTHOME_KEY_LEN],
    nh_porthome_prov_fetch_fn fetch, void *fetch_ctx,
    const nh_porthome_prov_opts *opts,
    const char *tx_id_or_null) {
    if (!sealed_manifest || sealed_len == 0 || !home_key || !opts)
        return NH_PORTHOME_PROV_ARG;
    nh_porthome_manifest *m = NULL;
    if (nh_porthome_manifest_decode_sealed(sealed_manifest, sealed_len,
                                           home_key, &m) != 0 || !m) {
        /* No manifest → LIMITED (never touch the existing home). */
        progress_write(opts->progress_path, tx_id_or_null, "limited", 0, 0, 0, 0);
        return NH_PORTHOME_PROV_LIMITED;
    }
    nh_porthome_prov_status r = nh_porthome_materialize_into_fd(
        staging_fd, m, home_key, fetch, fetch_ctx, opts, tx_id_or_null);
    nh_porthome_manifest_dispose(m);
    free(m);
    return r;
}
