/*
 * nostr-home-fuse — read-only FUSE 3 overlay for the portable-home
 * snapshot (Phase 4, bead nostrc-1u55).
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED. Gated behind
 * NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL (default OFF).
 *
 * Design: docs/designs/nostrfs-porthome-overlay.md. Namespace is
 * loaded from $XDG_STATE_HOME/nostr-homed/snapshot.json; content
 * flows through the 4-tier ladder (nh_fuse_source). Read-only —
 * every mutator returns EROFS. Type=notify systemd unit; no
 * allow_other, no allow_root, no CAP_SYS_ADMIN.
 *
 * Exit codes:
 *   0 clean shutdown
 *   3 no / bad snapshot
 *   4 no seed
 *   5 already mounted (fuse.lock held or mountpoint busy)
 *   6 mount failed
 *
 * Test env overrides:
 *   NH_FUSE_STATE_DIR     — override snapshot state dir
 *   NH_FUSE_MOUNTPOINT    — mountpoint (default $HOME/Portable)
 *   NH_FUSE_SEED_HEX      — inline seed (headless tests)
 *   NH_FUSE_SEED_FILE     — path override for the drop
 *   NH_FUSE_HOME          — override $HOME (for tier 0 pread)
 *   NH_FUSE_STATUS_DIR    — override $XDG_RUNTIME_DIR/nostr-homed
 *   NH_FUSE_NO_LOOP       — 1 = skip fuse_loop, exit after mount+notify
 *                            (used by tests to prove clean setup without a
 *                            running kernel FUSE dispatcher).
 *   NH_FUSE_TEST_HARNESS  — 1 = do not require /dev/fuse; skip fuse_mount.
 *                            Loads the snapshot + source seam and prints
 *                            "READY\n" then exits 0 (used by the source
 *                            unit tests to exercise wiring without root).
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include "nh_fuse_key.h"
#include "nh_fuse_source.h"
#include "nh_fuse_status.h"
#include "nh_fuse_table.h"
#include "nh_porthome_blossom.h"
#include "nh_porthome_crypto.h"
#include "nh_syncd_cache.h"
#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Fallback for older systemd headers. When systemd is unavailable we
 * fall back to writing READY to stdout so the unit's Type=notify
 * fires via sd_notify's SO_DGRAM socket if present, else a no-op. */
#if __has_include(<systemd/sd-daemon.h>)
#  include <systemd/sd-daemon.h>
#  define NH_HAVE_SD_NOTIFY 1
#else
#  define NH_HAVE_SD_NOTIFY 0
static int sd_notify(int u, const char *s) { (void)u; (void)s; return 0; }
#endif

/* Globals — single-threaded FUSE loop; simple state. */
static nh_fuse_table    *g_table         = NULL;
static nh_fuse_source   *g_source        = NULL;
static nh_syncd_cache   *g_cache         = NULL;
static nh_porthome_blossom_t *g_blossom  = NULL;
static char             *g_state_dir     = NULL;
static char             *g_status_path   = NULL;
static char             *g_mountpoint    = NULL;
static uint32_t          g_uid_us        = 0;
static uint32_t          g_gid_us        = 0;
static _Atomic uint64_t  g_open_seq      = 0;
static int               g_fuse_lock_fd  = -1;
static struct fuse_session *g_session    = NULL;

typedef struct {
    /* Immutable for the life of the fh: entry snapshot at open() time. */
    uint64_t         generation;
    char             rel[NH_FUSE_MAX_PATH_BYTES + 1];
    nh_fuse_kind_t   kind;
    uint64_t         size;
    char             content_hash_hex[65];
    char           **chunks_hex; /* deep-owned copies */
    size_t           n_chunks;
    /* Set if this fh is stale (removed since open — read returns EIO). */
    bool             stale;
} nh_fuse_fh;

static void fh_free(nh_fuse_fh *fh) {
    if (!fh) return;
    if (fh->chunks_hex) {
        for (size_t i = 0; i < fh->n_chunks; i++) free(fh->chunks_hex[i]);
        free(fh->chunks_hex);
    }
    free(fh);
}

/* ─── status file + notification throttle ────────────────────────── */

static uint64_t g_last_notify_epoch = 0;
static const uint64_t NOTIFY_INTERVAL_SECS = 10 * 60;

/* Phase 5 I3 / h10m.1.1 — last-error snapshot mirrored into
 * porthome-status.json under the "fuse" key. */
static char    g_last_err_class[64] = "";
static int64_t g_last_err_ts        = 0;

static void set_last_err(const char *class_slug) {
    snprintf(g_last_err_class, sizeof g_last_err_class,
             "%s", class_slug ? class_slug : "");
    g_last_err_ts = (int64_t)time(NULL);
}

static void write_unified_status(void) {
    nh_fuse_source_stats_t st = {0};
    if (g_source) nh_fuse_source_stats(g_source, &st);
    uint64_t gen = g_table ? nh_fuse_table_generation(g_table) : 0ull;
    uint64_t cbytes = g_cache ? nh_syncd_cache_used_bytes(g_cache) : 0ull;
    (void)nh_fuse_write_status_unified(NULL,
                                       g_session != NULL,
                                       g_mountpoint,
                                       gen,
                                       cbytes,
                                       &st,
                                       g_last_err_class,
                                       g_last_err_ts);
}

static void write_status_json(void) {
    if (!g_status_path) return;
    nh_fuse_source_stats_t st = {0};
    if (g_source) nh_fuse_source_stats(g_source, &st);
    uint64_t gen = g_table ? nh_fuse_table_generation(g_table) : 0ull;
    (void)nh_fuse_write_status(g_status_path, g_session != NULL, gen, &st);
    /* Mirror into the unified porthome-status.json under "fuse". */
    write_unified_status();
}

static void maybe_notify_miss(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now = (uint64_t)ts.tv_sec;
    if (now - g_last_notify_epoch < NOTIFY_INTERVAL_SECS) return;
    g_last_notify_epoch = now;
    const char *ne = getenv("NOSTR_HOMED_SYNCD_NOTIFY");
    if (ne && !strcmp(ne, "0")) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (dn >= 0) { dup2(dn, 2); close(dn); }
        execlp("notify-send", "notify-send",
               "--app-name=nostr-home-fuse",
               "--icon=folder-remote",
               "portable home",
               "One or more files under Portable/ could not be fetched — the Blossom server is unreachable.",
               (char *)NULL);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, WNOHANG);
}

static void on_source_miss(void *ud, const char *rel, int errcode) {
    (void)ud; (void)rel;
    /* Bucket the errno into a small stable slug so operators
     * can grep status output. */
    const char *cls = "unknown";
    switch (-errcode) {
        case 0:               cls = "none"; break;
        case 5:  /* EIO */    cls = "tier3-eio"; break;
        case 12: /* ENOMEM */ cls = "oom"; break;
        case 61: /* ENODATA on macOS - just fallback */
        case 74: /* EBADMSG on Linux */ cls = "decrypt-failed"; break;
        case 110: /* ETIMEDOUT */ cls = "timeout"; break;
        case 101: /* ENETUNREACH */ cls = "offline"; break;
        default: cls = "tier3-error"; break;
    }
    set_last_err(cls);
    maybe_notify_miss();
    write_status_json();
}

/* ─── FUSE operations ────────────────────────────────────────────── */

static int nhf_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof *st);
    const char *rel = (path && path[0] == '/') ? path + 1 : path;
    const nh_fuse_entry_t *e = nh_fuse_table_find(g_table, rel);
    if (!e) return -ENOENT;
    st->st_uid = g_uid_us;
    st->st_gid = g_gid_us;
    st->st_nlink = 1;
    /* content is immutable within a generation → kernel_cache=1 in init.
     * Force fields from the snapshot. */
    if (e->kind == NH_FUSE_KIND_DIR) {
        st->st_mode = S_IFDIR | (e->mode ? e->mode : 0700);
        st->st_nlink = 2;
    } else if (e->kind == NH_FUSE_KIND_SYMLINK) {
        st->st_mode = S_IFLNK | 0777;
        st->st_size = e->symlink_target ? (off_t)strlen(e->symlink_target) : 0;
    } else {
        st->st_mode = S_IFREG | (e->mode ? e->mode : 0444);
        st->st_size = (off_t)e->size;
    }
    st->st_mtim.tv_sec  = (time_t)(e->mtime_ns / 1000000000ull);
    st->st_mtim.tv_nsec = (long)(e->mtime_ns % 1000000000ull);
    st->st_atim = st->st_mtim;
    st->st_ctim = st->st_mtim;
    return 0;
}

typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
} readdir_ud;

static int readdir_cb(void *ud, const char *name, const nh_fuse_entry_t *e) {
    (void)e;
    readdir_ud *r = ud;
    r->filler(r->buf, name, NULL, 0, 0);
    return 0;
}

static int nhf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t off, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;
    const char *rel = (path && path[0] == '/') ? path + 1 : path;
    const nh_fuse_entry_t *e = nh_fuse_table_find(g_table, rel);
    if (!e) return -ENOENT;
    if (e->kind != NH_FUSE_KIND_DIR) return -ENOTDIR;
    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    readdir_ud u = { buf, filler };
    return nh_fuse_table_readdir(g_table, rel, readdir_cb, &u);
}

static int nhf_readlink(const char *path, char *buf, size_t len) {
    const char *rel = (path && path[0] == '/') ? path + 1 : path;
    const nh_fuse_entry_t *e = nh_fuse_table_find(g_table, rel);
    if (!e) return -ENOENT;
    if (e->kind != NH_FUSE_KIND_SYMLINK) return -EINVAL;
    const char *t = e->symlink_target ? e->symlink_target : "";
    size_t tn = strlen(t);
    if (tn + 1 > len) tn = len - 1;
    memcpy(buf, t, tn);
    buf[tn] = '\0';
    return 0;
}

static int nhf_open(const char *path, struct fuse_file_info *fi) {
    /* Refuse any write intent. Read-only overlay. */
    int acc = fi->flags & O_ACCMODE;
    if (acc != O_RDONLY) return -EROFS;
    if (fi->flags & (O_CREAT | O_TRUNC | O_APPEND)) return -EROFS;
    const char *rel = (path && path[0] == '/') ? path + 1 : path;
    const nh_fuse_entry_t *e = nh_fuse_table_find(g_table, rel);
    if (!e) return -ENOENT;
    if (e->kind == NH_FUSE_KIND_DIR) return -EISDIR;
    if (e->kind == NH_FUSE_KIND_SYMLINK) return -ELOOP;

    nh_fuse_fh *fh = calloc(1, sizeof *fh);
    if (!fh) return -ENOMEM;
    fh->generation = nh_fuse_table_generation(g_table);
    strncpy(fh->rel, e->rel ? e->rel : rel, NH_FUSE_MAX_PATH_BYTES);
    fh->rel[NH_FUSE_MAX_PATH_BYTES] = '\0';
    fh->kind = e->kind;
    fh->size = e->size;
    memcpy(fh->content_hash_hex, e->content_hash_hex, sizeof fh->content_hash_hex);
    if (e->n_chunks > 0) {
        fh->chunks_hex = calloc(e->n_chunks, sizeof *fh->chunks_hex);
        if (!fh->chunks_hex) { fh_free(fh); return -ENOMEM; }
        for (size_t i = 0; i < e->n_chunks; i++) {
            fh->chunks_hex[i] = strdup(e->chunks_hex[i]);
            if (!fh->chunks_hex[i]) { fh_free(fh); return -ENOMEM; }
        }
        fh->n_chunks = e->n_chunks;
    }
    fi->fh = (uint64_t)(uintptr_t)fh;
    /* Content immutable within a generation. */
    fi->keep_cache = 1;
    return 0;
}

static int nhf_read(const char *path, char *buf, size_t len, off_t off,
                    struct fuse_file_info *fi) {
    (void)path;
    nh_fuse_fh *fh = (nh_fuse_fh *)(uintptr_t)fi->fh;
    if (!fh) return -EBADF;
    if (fh->stale) return -EIO;
    /* Detect stale-open: generation changed AND our entry no longer
     * exists at the same path in the new table. */
    if (fh->generation != nh_fuse_table_generation(g_table)) {
        const nh_fuse_entry_t *e = nh_fuse_table_find(g_table, fh->rel);
        if (!e || e->kind != NH_FUSE_KIND_FILE) {
            fh->stale = true;
            return -EIO;
        }
    }
    /* Chunk size: defaults to design's 4 MiB. Overridable via
     * NH_FUSE_CHUNK_SIZE for live-acceptance rigs whose Blossom
     * server enforces a smaller PUT ceiling than the design chunk
     * size (bead nostrc-1u55 live-smoke). */
    size_t chunk_size = 4u * 1024u * 1024u;
    const char *cs_env = getenv("NH_FUSE_CHUNK_SIZE");
    if (cs_env && *cs_env) {
        unsigned long long v = strtoull(cs_env, NULL, 10);
        if (v >= 4096ull && v <= 4194304ull) chunk_size = (size_t)v;
    }
    ssize_t r = nh_fuse_source_pread(g_source, fh->rel, fh->content_hash_hex,
                                     fh->size,
                                     (const char * const *)fh->chunks_hex,
                                     fh->n_chunks,
                                     chunk_size,
                                     buf, len, off);
    if (r < 0) {
        write_status_json();
        return (int)r;
    }
    return (int)r;
}

static int nhf_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    nh_fuse_fh *fh = (nh_fuse_fh *)(uintptr_t)fi->fh;
    fh_free(fh);
    fi->fh = 0;
    return 0;
}

static int nhf_statfs(const char *path, struct statvfs *st) {
    (void)path;
    memset(st, 0, sizeof *st);
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    /* Report the cache filesystem's capacity — best-effort. */
    if (g_cache) {
        const char *dir = nh_syncd_cache_dir(g_cache);
        if (dir) {
            struct statvfs cs;
            if (statvfs(dir, &cs) == 0) {
                *st = cs;
            }
        }
    }
    st->f_flag |= ST_RDONLY;
    return 0;
}

/* Every mutator: EROFS. */
static int rofs(void) { return -EROFS; }
static int nhf_mknod(const char *p, mode_t m, dev_t d) { (void)p;(void)m;(void)d; return rofs(); }
static int nhf_mkdir(const char *p, mode_t m) { (void)p;(void)m; return rofs(); }
static int nhf_unlink(const char *p) { (void)p; return rofs(); }
static int nhf_rmdir(const char *p) { (void)p; return rofs(); }
static int nhf_symlink(const char *t, const char *p) { (void)t;(void)p; return rofs(); }
static int nhf_rename(const char *a, const char *b, unsigned f) { (void)a;(void)b;(void)f; return rofs(); }
static int nhf_link(const char *a, const char *b) { (void)a;(void)b; return rofs(); }
static int nhf_chmod(const char *p, mode_t m, struct fuse_file_info *fi) { (void)p;(void)m;(void)fi; return rofs(); }
static int nhf_chown(const char *p, uid_t u, gid_t g, struct fuse_file_info *fi) { (void)p;(void)u;(void)g;(void)fi; return rofs(); }
static int nhf_truncate(const char *p, off_t o, struct fuse_file_info *fi) { (void)p;(void)o;(void)fi; return rofs(); }
static int nhf_write(const char *p, const char *b, size_t l, off_t o, struct fuse_file_info *fi) {
    (void)p;(void)b;(void)l;(void)o;(void)fi; return rofs();
}
static int nhf_create(const char *p, mode_t m, struct fuse_file_info *fi) {
    (void)p;(void)m;(void)fi; return rofs();
}
static int nhf_setxattr(const char *p, const char *n, const char *v, size_t s, int f) {
    (void)p;(void)n;(void)v;(void)s;(void)f; return -ENOTSUP;
}
static int nhf_getxattr(const char *p, const char *n, char *v, size_t s) {
    (void)p;(void)n;(void)v;(void)s; return -ENOTSUP;
}
static int nhf_listxattr(const char *p, char *v, size_t s) {
    (void)p;(void)v;(void)s; return -ENOTSUP;
}
static int nhf_removexattr(const char *p, const char *n) {
    (void)p;(void)n; return -ENOTSUP;
}

static void *nhf_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1;
    cfg->entry_timeout = 1.0;
    cfg->attr_timeout = 1.0;
    cfg->negative_timeout = 0.0;
    /* Capture the live session so write_status_json() can report
     * mounted:true. The high-level API sets fuse_get_context()->fuse
     * before init fires; fuse_get_session(fuse) yields the low-level
     * session pointer that fuse_main assembled through session_new +
     * session_mount. Bead nostrc-h4tv. */
    struct fuse_context *fctx = fuse_get_context();
    if (fctx && fctx->fuse) {
        g_session = fuse_get_session(fctx->fuse);
    }
    write_status_json();
    /* Notify systemd after mount is live. */
    (void)sd_notify(0, "READY=1");
    return NULL;
}

static void nhf_destroy(void *ud) {
    (void)ud;
    /* mount teardown — clear the last-error slot so a subsequent
     * clean unmount doesn't leave a stale class in the status. */
    set_last_err("");
    if (g_source) { nh_fuse_source_close(g_source); g_source = NULL; }
    if (g_table)  { nh_fuse_table_free(g_table); g_table = NULL; }
    /* Session is being torn down by fuse_main's unmount + destroy path;
     * clear our reference so the final status write reports mounted:false. */
    g_session = NULL;
    write_status_json();
}

static const struct fuse_operations g_ops = {
    .init     = nhf_init,
    .destroy  = nhf_destroy,
    .getattr  = nhf_getattr,
    .readdir  = nhf_readdir,
    .readlink = nhf_readlink,
    .open     = nhf_open,
    .read     = nhf_read,
    .release  = nhf_release,
    .statfs   = nhf_statfs,

    .mknod       = nhf_mknod,
    .mkdir       = nhf_mkdir,
    .unlink      = nhf_unlink,
    .rmdir       = nhf_rmdir,
    .symlink     = nhf_symlink,
    .rename      = nhf_rename,
    .link        = nhf_link,
    .chmod       = nhf_chmod,
    .chown       = nhf_chown,
    .truncate    = nhf_truncate,
    .write       = nhf_write,
    .create      = nhf_create,
    .setxattr    = nhf_setxattr,
    .getxattr    = nhf_getxattr,
    .listxattr   = nhf_listxattr,
    .removexattr = nhf_removexattr,
};

/* ─── env helpers ───────────────────────────────────────────────── */

static char *default_home(void) {
    const char *h = getenv("NH_FUSE_HOME");
    if (!h) h = getenv("HOME");
    if (!h) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) h = pw->pw_dir;
    }
    return h ? strdup(h) : NULL;
}

static char *default_state_dir(const char *home) {
    const char *ovr = getenv("NH_FUSE_STATE_DIR");
    if (ovr && *ovr) return strdup(ovr);
    const char *xdg = getenv("XDG_STATE_HOME");
    char *p = NULL;
    if (xdg && xdg[0] == '/') {
        if (asprintf(&p, "%s/nostr-homed", xdg) < 0) return NULL;
    } else {
        if (asprintf(&p, "%s/.local/state/nostr-homed", home) < 0) return NULL;
    }
    return p;
}

static char *default_status_path(void) {
    const char *ovr = getenv("NH_FUSE_STATUS_DIR");
    const char *rd = getenv("XDG_RUNTIME_DIR");
    char *dir = NULL;
    if (ovr && *ovr) dir = strdup(ovr);
    else if (rd && rd[0] == '/') {
        if (asprintf(&dir, "%s/nostr-homed", rd) < 0) return NULL;
    } else {
        /* Fall back to /tmp/nostr-homed-<uid>. */
        if (asprintf(&dir, "/tmp/nostr-homed-%u", (unsigned)getuid()) < 0) return NULL;
    }
    (void)mkdir(dir, 0700);
    char *out = NULL;
    if (asprintf(&out, "%s/fuse-status.json", dir) < 0) { free(dir); return NULL; }
    free(dir);
    return out;
}

static int acquire_fuse_lock(const char *state_dir) {
    char path[512];
    int n = snprintf(path, sizeof path, "%s/fuse.lock", state_dir);
    if (n <= 0 || n >= (int)sizeof path) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;
}

/* ─── main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setlinebuf(stdout);
    setlinebuf(stderr);

    /* Zero core-file rlimit before we ever touch the seed. */
    struct rlimit rc = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rc);

    g_uid_us = (uint32_t)getuid();
    g_gid_us = (uint32_t)getgid();

    char *home = default_home();
    if (!home) { fprintf(stderr, "fuse: cannot determine $HOME\n"); return 3; }
    g_state_dir = default_state_dir(home);
    if (!g_state_dir) { free(home); return 3; }
    g_status_path = default_status_path();

    /* Mountpoint. */
    const char *mp_env = getenv("NH_FUSE_MOUNTPOINT");
    if (mp_env && *mp_env) g_mountpoint = strdup(mp_env);
    else                    (void)asprintf(&g_mountpoint, "%s/Portable", home);
    if (!g_mountpoint) { free(home); free(g_state_dir); return 3; }

    /* Ensure mountpoint dir exists and is a plain empty dir. */
    struct stat mps;
    if (stat(g_mountpoint, &mps) != 0) {
        if (mkdir(g_mountpoint, 0700) != 0) {
            fprintf(stderr, "fuse: cannot create mountpoint %s errno=%d\n",
                    g_mountpoint, errno);
            free(home); free(g_state_dir); free(g_mountpoint); return 6;
        }
    } else if (!S_ISDIR(mps.st_mode)) {
        fprintf(stderr, "fuse: mountpoint %s is not a directory\n", g_mountpoint);
        free(home); free(g_state_dir); free(g_mountpoint); return 6;
    }
    struct stat lps;
    if (lstat(g_mountpoint, &lps) == 0 && S_ISLNK(lps.st_mode)) {
        fprintf(stderr, "fuse: mountpoint %s is a symlink; refusing\n", g_mountpoint);
        free(home); free(g_state_dir); free(g_mountpoint); return 6;
    }

    /* fuse.lock */
    g_fuse_lock_fd = acquire_fuse_lock(g_state_dir);
    if (g_fuse_lock_fd < 0) {
        fprintf(stderr, "fuse: another nostr-home-fuse holds fuse.lock; exiting\n");
        free(home); free(g_state_dir); free(g_mountpoint); return 5;
    }

    /* Load snapshot. */
    int tr = nh_fuse_table_load(g_state_dir, g_uid_us, g_gid_us, &g_table);
    if (tr != 0 || !g_table) {
        fprintf(stderr, "fuse: snapshot load failed rc=%d — exiting cleanly\n", tr);
        free(home); free(g_state_dir); free(g_mountpoint);
        if (g_fuse_lock_fd >= 0) close(g_fuse_lock_fd);
        return 3;
    }

    /* Load seed → home_key. */
    uint8_t home_key[NH_PORTHOME_KEY_LEN];
    if (nh_fuse_key_load(home_key) != 0) {
        fprintf(stderr, "fuse: no seed available (home_seed.fuse missing and no env fallback)\n");
        nh_fuse_table_free(g_table); g_table = NULL;
        free(home); free(g_state_dir); free(g_mountpoint);
        if (g_fuse_lock_fd >= 0) close(g_fuse_lock_fd);
        return 4;
    }

    /* Open local blob cache. */
    char *cdir = nh_syncd_cache_default_dir();
    if (cdir) {
        (void)nh_syncd_cache_open(cdir, 0, &g_cache);
        if (g_cache) nh_syncd_cache_set_auto_evict(g_cache, true);
        free(cdir);
    }

    /* Open Blossom client (fetch only — no signer). Server list from
     * $NH_FUSE_BLOSSOM (comma-separated https URLs). If unset the
     * client is not created; tier 3 will EIO — the mount still
     * serves tier 0/1/2. */
    const char *bl = getenv("NH_FUSE_BLOSSOM");
    if (bl && *bl) {
        size_t n = 1;
        for (const char *p = bl; *p; p++) if (*p == ',') n++;
        char **arr = calloc(n + 1, sizeof *arr);
        const char *start = bl;
        size_t i = 0;
        for (const char *p = bl; ; p++) {
            if (*p == ',' || *p == '\0') {
                size_t l = (size_t)(p - start);
                char *item = malloc(l + 1);
                if (item) { memcpy(item, start, l); item[l] = '\0'; arr[i++] = item; }
                if (*p == '\0') break;
                start = p + 1;
            }
        }
        nh_porthome_blossom_opts_t opts = {
            .servers = (const char *const *)arr,
            .n_servers = i,
            .timeout_seconds = 15,
            .max_retries = 1,
            .max_blob_bytes = 0,
        };
        (void)nh_porthome_blossom_new(&opts, /*signer*/ NULL, &g_blossom);
        for (size_t k = 0; k < i; k++) free(arr[k]);
        free(arr);
    }

    /* Source. */
    nh_fuse_source_cfg scfg = {
        .home_dir = home,
        .tier0_enabled = (getenv("NOSTR_HOME_STATE") == NULL ||
                          strcmp(getenv("NOSTR_HOME_STATE"), "ready") == 0),
        .cache = g_cache,
        .blossom = g_blossom,
        .chunk_cache_bytes = 0,
        .offline_budget_ms = 0,
        .on_miss = on_source_miss,
        .on_miss_ud = NULL,
    };
    memcpy(scfg.home_key, home_key, NH_PORTHOME_KEY_LEN);
    int sr = nh_fuse_source_open(&scfg, &g_source);
    /* Wipe local copy immediately. */
    memset(home_key, 0, sizeof home_key);
    memset(scfg.home_key, 0, sizeof scfg.home_key);
    if (sr != 0 || !g_source) {
        fprintf(stderr, "fuse: source open failed rc=%d\n", sr);
        nh_fuse_table_free(g_table); g_table = NULL;
        free(home); free(g_state_dir); free(g_mountpoint);
        if (g_fuse_lock_fd >= 0) close(g_fuse_lock_fd);
        return 6;
    }

    /* Test-harness path: exercise wiring without a real FUSE mount. */
    if (getenv("NH_FUSE_TEST_HARNESS")) {
        fprintf(stdout, "READY generation=%" PRIu64 " entries=%zu mountpoint=%s\n",
                nh_fuse_table_generation(g_table),
                nh_fuse_table_entry_count(g_table),
                g_mountpoint);
        write_status_json();
        /* Even in harness mode, land a `fuse` key so operators
         * can eyeball porthome-status.json without a real mount. */
        write_unified_status();
        nh_fuse_source_close(g_source); g_source = NULL;
        nh_fuse_table_free(g_table); g_table = NULL;
        if (g_blossom) nh_porthome_blossom_free(g_blossom);
        if (g_cache) nh_syncd_cache_close(g_cache);
        free(home); free(g_state_dir); free(g_mountpoint); free(g_status_path);
        if (g_fuse_lock_fd >= 0) close(g_fuse_lock_fd);
        return 0;
    }

    /* Real FUSE mount. Assemble argv for fuse_main(). */
    char *fargv[16];
    int fargc = 0;
    fargv[fargc++] = "nostr-home-fuse";
    fargv[fargc++] = g_mountpoint;
    fargv[fargc++] = "-s"; /* single-threaded v1 (§D13) */
    fargv[fargc++] = "-f"; /* stay in foreground; systemd manages the process */
    fargv[fargc++] = "-o";
    fargv[fargc++] = "ro,nosuid,nodev,noatime,default_permissions,auto_unmount,"
                     "fsname=nostr-home,subtype=porthome";
    fargv[fargc] = NULL;

    /* Post-mount self-check must happen from a helper — fuse_main is
     * blocking. Rely on the init callback to sd_notify READY=1; the
     * mountpoint st_dev comparison is a manual verification we do
     * in the unit's ExecStartPost path (see nostr-home-fuse.service).
     */
    int rrc = fuse_main(fargc, fargv, &g_ops, NULL);

    /* Cleanup. */
    if (g_blossom) nh_porthome_blossom_free(g_blossom);
    if (g_cache) nh_syncd_cache_close(g_cache);
    free(home); free(g_state_dir); free(g_mountpoint); free(g_status_path);
    if (g_fuse_lock_fd >= 0) close(g_fuse_lock_fd);
    return rrc == 0 ? 0 : 6;
}
