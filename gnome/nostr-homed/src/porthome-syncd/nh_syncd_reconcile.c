/*
 * nh_syncd_reconcile.c — three-way reconciler + conflict policy (§6.3).
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED — gated by
 * NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL.
 *
 * Beads: nostrc-p6qp (I2), nostrc-h10m (parent).
 *
 * Behaviour (design §6.3):
 *   Base   = snapshot.json at start of pass (last committed sync state).
 *   Local  = current $HOME (rescanned per-entry as we go).
 *   Remote = decoded nh_porthome_manifest.
 *
 *   For each rel_path in base:
 *     path_enc = encrypt_path(home_key, rel)
 *     remote   = manifest_lookup(path_enc)  // may be missing
 *     current_st = lstat($HOME/rel)         // may be missing
 *     local_change  = base disagrees with the on-disk stat (mtime/size/hash)
 *     remote_change = base disagrees with remote (present/absent/hash)
 *     both  → CONFLICT (last-writer-wins by mtime; loser preserved)
 *     local → push_queue.push(rel)          // I1 batch API
 *     remote-only:
 *       missing on remote AND local unchanged since base → apply delete
 *       missing on remote AND local modified            → keep + .conflict-deleted
 *       present on remote                                → materialize + snapshot
 *     neither → noop
 *
 *   For each path_enc in remote that no base entry mapped to:
 *     v1 punt: we can't recover plaintext from a keyed HMAC. The
 *     reconciler notes it (result->unmatched_remote) and leaves the
 *     file un-materialized; the fetch helper is the authoritative path
 *     that materializes such names into a staging tree with opaque
 *     filenames (design §2.3). Tests exercise the base-known path.
 *
 * Materialization is via openat*() under a fresh dirfd rooted at
 * $HOME with O_DIRECTORY; every path is refused if it is absolute or
 * contains "..". Sticky/setuid/setgid bits are stripped. uid/gid
 * from the manifest are treated as hints only (the local uid is used
 * — this is a user-session daemon).
 */

#define _GNU_SOURCE

#include "nh_syncd.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_notify.h"

#include <jansson.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* Symbols from nh_syncd_state.c (see nh_syncd_pusher.c for the same
 * extern trick). */
extern int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                       const char *rel,
                                       uint32_t mode, uint32_t uid, uint32_t gid,
                                       uint64_t mtime_ns, uint64_t size,
                                       const char *content_hash_hex,
                                       const char *const *chunk_addrs_hex,
                                       size_t chunk_addrs_n);
extern int nh_syncd_state_upsert_dir_(nh_syncd_state *s,
                                      const char *rel,
                                      uint32_t mode, uint32_t uid, uint32_t gid,
                                      uint64_t mtime_ns);
extern int nh_syncd_state_upsert_symlink_(nh_syncd_state *s,
                                          const char *rel,
                                          uint32_t mode, uint32_t uid, uint32_t gid,
                                          uint64_t mtime_ns,
                                          const char *target);
extern int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel);

typedef int (*nh_syncd_state_iter_cb)(void *ud, const char *rel, json_t *row);
extern int nh_syncd_state_iter_(const nh_syncd_state *s,
                                nh_syncd_state_iter_cb cb, void *ud);
extern int nh_syncd_state_save(const nh_syncd_state *s, const char *state_dir);
extern void nh_syncd_state_set_remote_generation(nh_syncd_state *s, uint64_t g);
extern uint64_t nh_syncd_state_get_local_generation(const nh_syncd_state *s);

/* Also declared/defined in nh_syncd_state.c: bump local generation. We
 * use a direct set instead so the reconciler can pull local up to the
 * remote value in one shot without inventing gen numbers. */
extern void nh_syncd_state_bump_generation_(nh_syncd_state *s);
/* Not exposed elsewhere: we splice into the underlying json object by
 * calling the upsert_* helpers with the target generation instead of
 * introducing a set_local_generation helper (which would need I1
 * seam changes and is outside I2's scope). Instead, the reconciler
 * uses bump_generation_ once per applied EVENT — one EVENT advances
 * the local counter by exactly one gen, which matches the semantics
 * "one remote pointer observed" (we do NOT try to catch up multiple
 * missed generations in a single pass — future EVENTs will fire the
 * callback again). */

/* ────────────────────────── error helper ────────────────────────── */

static void set_err(char **out, const char *fmt, ...) {
    if (!out) return;
    if (*out) { free(*out); *out = NULL; }
    va_list ap; va_start(ap, fmt);
    char *buf = NULL;
    if (vasprintf(&buf, fmt, ap) < 0) buf = NULL;
    va_end(ap);
    *out = buf;
}

/* ────────────────── partial-state marker file ────────────────────── */

static int state_marker_write(const char *state_dir, const char *val) {
    if (!state_dir || !val) return NH_SYNCD_ERR_ARG;
    char *tmp = NULL, *fin = NULL;
    if (asprintf(&tmp, "%s/%s.tmp", state_dir, NH_SYNCD_STATE_MARKER_FILE) < 0)
        return NH_SYNCD_ERR_OOM;
    if (asprintf(&fin, "%s/%s", state_dir, NH_SYNCD_STATE_MARKER_FILE) < 0) {
        free(tmp); return NH_SYNCD_ERR_OOM;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    size_t vlen = strlen(val);
    const char *nl = "\n";
    if (write(fd, val, vlen) != (ssize_t)vlen ||
        write(fd, nl, 1)    != 1) {
        close(fd); unlink(tmp); free(tmp); free(fin);
        return NH_SYNCD_ERR_IO;
    }
    (void)fsync(fd);
    close(fd);
    if (rename(tmp, fin) < 0) { unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    free(tmp); free(fin);
    return NH_SYNCD_OK;
}

int nh_syncd_partial_state_set(const char *state_dir) {
    return state_marker_write(state_dir, NH_SYNCD_STATE_MARKER_PARTIAL);
}
int nh_syncd_partial_state_clear(const char *state_dir) {
    return state_marker_write(state_dir, NH_SYNCD_STATE_MARKER_OK);
}
int nh_syncd_partial_state_is_set(const char *state_dir) {
    if (!state_dir) return 0;
    char *p = NULL;
    if (asprintf(&p, "%s/%s", state_dir, NH_SYNCD_STATE_MARKER_FILE) < 0) return 0;
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return 0;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return 0;
    /* Trim trailing whitespace. */
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return strcmp(buf, NH_SYNCD_STATE_MARKER_PARTIAL) == 0;
}

/* ─────────────────── reconcile result (opaque) ───────────────────── */

typedef struct {
    char *rel;
    char *conflict_path;
} conflict_row;

struct nh_syncd_reconcile_result {
    size_t applied;
    size_t deleted;

    char **push;
    size_t push_n;
    size_t push_cap;

    conflict_row *conflicts;
    size_t conflicts_n;
    size_t conflicts_cap;
};

static nh_syncd_reconcile_result *result_new(void) {
    return calloc(1, sizeof(nh_syncd_reconcile_result));
}

static int result_push(nh_syncd_reconcile_result *r, const char *rel) {
    if (r->push_n == r->push_cap) {
        size_t nc = r->push_cap ? r->push_cap * 2 : 8;
        char **np = realloc(r->push, nc * sizeof *np);
        if (!np) return NH_SYNCD_ERR_OOM;
        r->push = np; r->push_cap = nc;
    }
    r->push[r->push_n] = strdup(rel);
    if (!r->push[r->push_n]) return NH_SYNCD_ERR_OOM;
    r->push_n++;
    return 0;
}

static int result_conflict(nh_syncd_reconcile_result *r,
                           const char *rel, const char *conflict_path) {
    if (r->conflicts_n == r->conflicts_cap) {
        size_t nc = r->conflicts_cap ? r->conflicts_cap * 2 : 8;
        conflict_row *nr = realloc(r->conflicts, nc * sizeof *nr);
        if (!nr) return NH_SYNCD_ERR_OOM;
        r->conflicts = nr; r->conflicts_cap = nc;
    }
    r->conflicts[r->conflicts_n].rel = strdup(rel);
    r->conflicts[r->conflicts_n].conflict_path = strdup(conflict_path);
    if (!r->conflicts[r->conflicts_n].rel || !r->conflicts[r->conflicts_n].conflict_path) {
        free(r->conflicts[r->conflicts_n].rel);
        free(r->conflicts[r->conflicts_n].conflict_path);
        return NH_SYNCD_ERR_OOM;
    }
    r->conflicts_n++;
    return 0;
}

size_t nh_syncd_reconcile_result_applied(const nh_syncd_reconcile_result *r) {
    return r ? r->applied : 0;
}
size_t nh_syncd_reconcile_result_deleted(const nh_syncd_reconcile_result *r) {
    return r ? r->deleted : 0;
}
size_t nh_syncd_reconcile_result_push_queued(const nh_syncd_reconcile_result *r) {
    return r ? r->push_n : 0;
}
size_t nh_syncd_reconcile_result_conflicts(const nh_syncd_reconcile_result *r) {
    return r ? r->conflicts_n : 0;
}
const char *nh_syncd_reconcile_result_push_at(const nh_syncd_reconcile_result *r, size_t i) {
    if (!r || i >= r->push_n) return NULL;
    return r->push[i];
}
int nh_syncd_reconcile_result_conflict_at(const nh_syncd_reconcile_result *r,
                                          size_t i,
                                          const char **out_rel,
                                          const char **out_conflict_path)
{
    if (!r || i >= r->conflicts_n) return NH_SYNCD_ERR_ARG;
    if (out_rel) *out_rel = r->conflicts[i].rel;
    if (out_conflict_path) *out_conflict_path = r->conflicts[i].conflict_path;
    return 0;
}
void nh_syncd_reconcile_result_free(nh_syncd_reconcile_result *r) {
    if (!r) return;
    for (size_t i = 0; i < r->push_n; i++) free(r->push[i]);
    free(r->push);
    for (size_t i = 0; i < r->conflicts_n; i++) {
        free(r->conflicts[i].rel);
        free(r->conflicts[i].conflict_path);
    }
    free(r->conflicts);
    free(r);
}

/* ─────────────────── hex helpers + digests ───────────────────────── */

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = lc[(b[i] >> 4) & 0xf];
        out[i * 2 + 1] = lc[b[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static int file_content_hash_hex(const char *abs, char out[65], uint64_t *out_size) {
    int fd = open(abs, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NH_SYNCD_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NH_SYNCD_ERR_IO; }
    if (out_size) *out_size = (uint64_t)st.st_size;
    /* Slurp — bounded by manifest cap (files this big are rejected by
     * the pusher too). */
    if ((uint64_t)st.st_size >
        (uint64_t)NH_SYNCD_CHUNK_SIZE * NH_PORTHOME_MAX_CHUNKS_PER_ENTRY) {
        close(fd); return NH_SYNCD_ERR_PATH;
    }
    uint8_t *buf = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (!buf) { close(fd); return NH_SYNCD_ERR_OOM; }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NH_SYNCD_ERR_IO; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    uint8_t h[32];
    if (nh_porthome_sha256(buf, off, h) != 0) { free(buf); return NH_SYNCD_ERR_CRYPTO; }
    free(buf);
    hex_of(h, 32, out);
    return 0;
}

/* ───────────────── path safety (openat rooted) ───────────────────── */

/* Refuse any rel that is absolute, empty, ".", "..", contains "//",
 * a "/./" or "/../" component, or a NUL. */
static int rel_ok(const char *rel) {
    if (!rel || !*rel) return 0;
    if (rel[0] == '/') return 0;
    size_t n = strlen(rel);
    const char *s = rel;
    /* iterate components */
    for (size_t i = 0; i <= n; i++) {
        if (i == n || rel[i] == '/') {
            size_t clen = (size_t)(rel + i - s);
            if (clen == 0) return 0; /* empty component */
            if (clen == 1 && s[0] == '.') return 0;
            if (clen == 2 && s[0] == '.' && s[1] == '.') return 0;
            for (size_t k = 0; k < clen; k++) {
                if (s[k] == '\0' || s[k] == '\\') return 0;
            }
            s = rel + i + 1;
        }
    }
    return 1;
}

/* mkdirat -p relative to dirfd. Splits `rel` on '/' and creates every
 * intermediate directory with mode 0700, ignoring EEXIST. */
static int mkdirat_p(int dirfd, const char *rel) {
    if (!rel_ok(rel)) return NH_SYNCD_ERR_PATH;
    char *tmp = strdup(rel);
    if (!tmp) return NH_SYNCD_ERR_OOM;
    for (char *p = tmp; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdirat(dirfd, tmp, 0700) < 0 && errno != EEXIST) {
                free(tmp); return NH_SYNCD_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdirat(dirfd, tmp, 0700) < 0 && errno != EEXIST) {
        free(tmp); return NH_SYNCD_ERR_IO;
    }
    free(tmp);
    return 0;
}

/* Ensure parent directories of rel exist under dirfd (mkdir -p on
 * dirname(rel)). */
static int ensure_parent_dirs(int dirfd, const char *rel) {
    const char *last = strrchr(rel, '/');
    if (!last) return 0; /* no parent */
    size_t plen = (size_t)(last - rel);
    char *parent = malloc(plen + 1);
    if (!parent) return NH_SYNCD_ERR_OOM;
    memcpy(parent, rel, plen);
    parent[plen] = '\0';
    int rc = mkdirat_p(dirfd, parent);
    free(parent);
    return rc;
}

/* Write `data` (len bytes) to a temp sibling of `rel` under dirfd,
 * then renameat to `rel`. Returns 0 on success. */
static int write_file_atomic_at(int dirfd, const char *rel,
                                const uint8_t *data, size_t len,
                                uint32_t mode) {
    if (!rel_ok(rel)) return NH_SYNCD_ERR_PATH;
    int pr = ensure_parent_dirs(dirfd, rel);
    if (pr != 0) return pr;
    /* Temp sibling: `<rel>.nhsyncd.tmp.<pid>`. */
    char *tmp = NULL;
    if (asprintf(&tmp, "%s.nhsyncd.tmp.%d", rel, (int)getpid()) < 0) return NH_SYNCD_ERR_OOM;
    int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                    (mode_t)(mode & 0777));
    if (fd < 0) { free(tmp); return NH_SYNCD_ERR_IO; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue;
            close(fd); unlinkat(dirfd, tmp, 0); free(tmp); return NH_SYNCD_ERR_IO; }
        off += (size_t)w;
    }
    if (fsync(fd) < 0) { close(fd); unlinkat(dirfd, tmp, 0); free(tmp); return NH_SYNCD_ERR_IO; }
    close(fd);
    if (renameat(dirfd, tmp, dirfd, rel) < 0) {
        unlinkat(dirfd, tmp, 0); free(tmp); return NH_SYNCD_ERR_IO;
    }
    free(tmp);
    /* fchmod post-rename to force the mode (openat's mode arg is
     * subject to umask). */
    int ffd = openat(dirfd, rel, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (ffd >= 0) { (void)fchmod(ffd, (mode_t)(mode & 0777)); close(ffd); }
    return 0;
}

static int write_symlink_at(int dirfd, const char *rel, const char *target) {
    if (!rel_ok(rel)) return NH_SYNCD_ERR_PATH;
    int pr = ensure_parent_dirs(dirfd, rel);
    if (pr != 0) return pr;
    /* Unlink existing (best effort). */
    (void)unlinkat(dirfd, rel, 0);
    if (symlinkat(target, dirfd, rel) < 0) return NH_SYNCD_ERR_IO;
    return 0;
}

/* Delete regular file (or symlink) at rel under dirfd. */
static int delete_file_at(int dirfd, const char *rel) {
    if (!rel_ok(rel)) return NH_SYNCD_ERR_PATH;
    if (unlinkat(dirfd, rel, 0) < 0 && errno != ENOENT) return NH_SYNCD_ERR_IO;
    return 0;
}

/* Rename file at rel to `new_rel` (both under dirfd). Best-effort:
 * ensures parent of new_rel exists. */
static int rename_at(int dirfd, const char *rel, const char *new_rel) {
    int pr = ensure_parent_dirs(dirfd, new_rel);
    if (pr != 0) return pr;
    if (renameat(dirfd, rel, dirfd, new_rel) < 0) return NH_SYNCD_ERR_IO;
    return 0;
}

/* ───────────── ISO-8601 stamp for conflict file names ────────────── */

static void format_iso8601_utc(int64_t epoch_secs, char *out, size_t cap) {
    struct tm tm;
    time_t t = (time_t)epoch_secs;
    gmtime_r(&t, &tm);
    /* Filename-safe: no ':' — use compact form. */
    strftime(out, cap, "%Y%m%dT%H%M%SZ", &tm);
}

/* Build "<rel>.conflict-<device>-<stamp>" — a rel-path sibling. If rel
 * contains '/', place the marker BESIDE the file (same dirname). */
static int build_conflict_path(const char *rel, const char *device,
                               int64_t epoch, const char *suffix,
                               char **out) {
    char stamp[32];
    format_iso8601_utc(epoch, stamp, sizeof stamp);
    /* Sanitize device: fnmatch/rename doesn't like '/' in device names. */
    char dev[64];
    size_t di = 0;
    for (size_t i = 0; device && device[i] && di + 1 < sizeof dev; i++) {
        char c = device[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            dev[di++] = c;
    }
    if (di == 0) { strcpy(dev, "device"); di = 6; }
    dev[di] = '\0';
    if (asprintf(out, "%s.conflict-%s-%s%s", rel, dev, stamp,
                 suffix ? suffix : "") < 0)
        return NH_SYNCD_ERR_OOM;
    return 0;
}

/* ────────────────── notify-send fallback ───────────────────────── */
/* Prior to Phase 5 I1 this open-coded fork()/execlp("notify-send", …).
 * That path now lives inside nh_porthome_notify(), which adds
 * per-(category,key) throttling and quiet-hours honouring while
 * keeping the fork+exec semantics + NOSTR_HOMED_SYNCD_NOTIFY kill
 * switch intact. See gnome/nostr-homed/include/nh_porthome_notify.h. */
static void default_notify(const char *summary, const char *body) {
    static nh_porthome_notifier *n = NULL;
    if (!n) n = nh_porthome_notifier_new();
    /* Reconcile emits a single accumulated notification per pass;
     * throttle on the summary string (dropped_count-sensitive), so
     * repeated identical passes collapse but a "count changed" pass
     * still surfaces. */
    (void)nh_porthome_notify(n,
        "nostr-home-syncd",
        "folder-remote",
        summary,
        body,
        NH_NOTIFY_CAT_CONFLICT,
        summary ? summary : "");
}

/* Called at end of pass with the accumulated conflict list. `paths` is
 * space-separated. */
static void emit_conflict_notification(const nh_syncd_reconcile_cfg *cfg,
                                       size_t count, const char *paths_joined) {
    char summary[128];
    snprintf(summary, sizeof summary,
             "%zu portable-home conflict%s", count, count == 1 ? "" : "s");
    if (cfg->notify) {
        cfg->notify(cfg->notify_ud, summary, paths_joined);
    } else {
        default_notify(summary, paths_joined);
    }
}

/* ───────────────────── base-index snapshot ───────────────────────── */

/* Row snapshot we take from the state at pass start — we mutate the
 * state during the pass, so we cannot iterate it live. */
typedef struct {
    char    *rel;             /* plaintext */
    char    *path_enc;        /* encrypted (heap) */
    nh_syncd_kind kind;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime_ns;
    uint64_t size;
    char     content_hash_hex[65];
    char    *symlink_target;  /* NULL for non-symlinks */
    /* Note: chunk_addrs_hex are held on the state itself; we don't
     * duplicate them because we don't re-upload during pull. */
} base_row;

typedef struct {
    const uint8_t *home_key;
    base_row *rows;
    size_t    rows_n;
    size_t    rows_cap;
    int       err;
} base_ctx;

static int base_collect_cb(void *ud, const char *rel, json_t *row) {
    base_ctx *bc = (base_ctx *)ud;
    if (bc->rows_n == bc->rows_cap) {
        size_t nc = bc->rows_cap ? bc->rows_cap * 2 : 32;
        base_row *nr = realloc(bc->rows, nc * sizeof *nr);
        if (!nr) { bc->err = NH_SYNCD_ERR_OOM; return -1; }
        bc->rows = nr; bc->rows_cap = nc;
    }
    base_row *r = &bc->rows[bc->rows_n];
    memset(r, 0, sizeof *r);
    r->rel = strdup(rel);
    if (!r->rel) { bc->err = NH_SYNCD_ERR_OOM; return -1; }
    const char *kind = json_string_value(json_object_get(row, "kind"));
    if (kind && !strcmp(kind, "dir")) r->kind = NH_SYNCD_KIND_DIR;
    else if (kind && !strcmp(kind, "symlink")) r->kind = NH_SYNCD_KIND_SYMLINK;
    else r->kind = NH_SYNCD_KIND_FILE;
    r->mode = (uint32_t)json_integer_value(json_object_get(row, "mode"));
    r->uid  = (uint32_t)json_integer_value(json_object_get(row, "uid"));
    r->gid  = (uint32_t)json_integer_value(json_object_get(row, "gid"));
    r->mtime_ns = (uint64_t)json_integer_value(json_object_get(row, "mtime_ns"));
    r->size     = (uint64_t)json_integer_value(json_object_get(row, "size"));
    const char *ch = json_string_value(json_object_get(row, "content_hash_hex"));
    if (ch) { strncpy(r->content_hash_hex, ch, 64); r->content_hash_hex[64] = '\0'; }
    const char *sl = json_string_value(json_object_get(row, "symlink_target"));
    if (sl) r->symlink_target = strdup(sl);

    /* Encrypted-path form (matches manifest key). */
    char *penc = NULL;
    if (nh_porthome_encrypt_path(bc->home_key, rel, &penc) != 0) {
        bc->err = NH_SYNCD_ERR_CRYPTO; return -1;
    }
    r->path_enc = penc;
    bc->rows_n++;
    return 0;
}

static void base_ctx_free(base_ctx *bc) {
    for (size_t i = 0; i < bc->rows_n; i++) {
        free(bc->rows[i].rel);
        free(bc->rows[i].path_enc);
        free(bc->rows[i].symlink_target);
    }
    free(bc->rows);
}

/* ────────── remote manifest → path_enc → entry map ────────────────── */

typedef struct {
    const nh_porthome_entry **arr;    /* by index, points into cfg->manifest->entries */
    size_t n;
} remote_index;

static const nh_porthome_entry *remote_lookup(const nh_porthome_manifest *m,
                                              const char *path_enc) {
    if (!m || !path_enc) return NULL;
    for (size_t i = 0; i < m->entries_len; i++) {
        if (m->entries[i].path_enc && !strcmp(m->entries[i].path_enc, path_enc))
            return &m->entries[i];
    }
    return NULL;
}

/* ────────────────────── materialization ──────────────────────────── */

/* Fetch + decrypt chunks for a file entry, compose into memory, return
 * heap buffer. Caller frees. */
static int materialize_file_bytes(const nh_syncd_reconcile_cfg *cfg,
                                  const nh_porthome_entry *e,
                                  uint8_t **out, size_t *out_len,
                                  char **err) {
    *out = NULL; *out_len = 0;
    if (!cfg->fetch_chunk) { set_err(err, "no fetch_chunk"); return NH_SYNCD_ERR_ARG; }
    size_t total = 0;
    uint8_t *buf = NULL;
    for (size_t i = 0; i < e->chunks_len; i++) {
        char addr[65];
        hex_of(e->chunks[i].sha256, 32, addr);
        uint8_t *ct = NULL; size_t ct_len = 0;
        int rc = cfg->fetch_chunk(cfg->fetch_chunk_ctx, addr, &ct, &ct_len);
        if (rc != 0) { free(buf); set_err(err, "fetch_chunk[%s] rc=%d", addr, rc); return NH_SYNCD_ERR_IO; }
        uint8_t *pt = NULL; size_t pt_len = 0;
        rc = nh_porthome_decrypt_chunk(cfg->home_key, ct, ct_len, &pt, &pt_len);
        free(ct);
        if (rc != 0) { free(buf); set_err(err, "decrypt_chunk rc=%d", rc); return NH_SYNCD_ERR_CRYPTO; }
        size_t need = total + pt_len; if (need == 0) need = 1;
        uint8_t *nb = realloc(buf, need);
        if (!nb) { free(pt); free(buf); return NH_SYNCD_ERR_OOM; }
        buf = nb;
        memcpy(buf + total, pt, pt_len);
        total += pt_len;
        free(pt);
    }
    *out = buf; *out_len = total;
    return 0;
}

/* Apply a single remote entry to $HOME. Updates state.files. */
static int apply_remote_entry(const nh_syncd_reconcile_cfg *cfg,
                              nh_syncd_state *state,
                              int home_fd,
                              const char *rel,
                              const nh_porthome_entry *e,
                              char **err) {
    /* Refuse fishy paths at defence-in-depth even though the manifest
     * decoder already checks. */
    if (!rel_ok(rel)) { set_err(err, "unsafe rel: %s", rel); return NH_SYNCD_ERR_PATH; }
    if (e->kind == NH_PORTHOME_KIND_DIR) {
        int rc = mkdirat_p(home_fd, rel);
        if (rc != 0) { set_err(err, "mkdirat %s rc=%d", rel, rc); return rc; }
        /* Open + fchmod. */
        int ffd = openat(home_fd, rel, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (ffd >= 0) { (void)fchmod(ffd, (mode_t)(e->mode & 0777)); close(ffd); }
        return nh_syncd_state_upsert_dir_(state, rel,
                                          e->mode & 0777, (uint32_t)getuid(),
                                          (uint32_t)getgid(), e->mtime_ns);
    }
    if (e->kind == NH_PORTHOME_KIND_SYMLINK) {
        const char *tgt = e->symlink_target ? e->symlink_target : "";
        int rc = write_symlink_at(home_fd, rel, tgt);
        if (rc != 0) { set_err(err, "symlinkat %s rc=%d", rel, rc); return rc; }
        return nh_syncd_state_upsert_symlink_(state, rel,
                                              e->mode & 0777, (uint32_t)getuid(),
                                              (uint32_t)getgid(), e->mtime_ns,
                                              tgt);
    }
    /* file */
    uint8_t *bytes = NULL; size_t bytes_len = 0;
    int rc = materialize_file_bytes(cfg, e, &bytes, &bytes_len, err);
    if (rc != 0) return rc;
    /* Enforce declared size, if given. */
    if (e->size != 0 && bytes_len != e->size) {
        free(bytes); set_err(err, "size mismatch %s: manifest=%llu bytes=%zu",
                             rel, (unsigned long long)e->size, bytes_len);
        return NH_SYNCD_ERR_CRYPTO;
    }
    int wr = write_file_atomic_at(home_fd, rel, bytes, bytes_len,
                                  e->mode & 0777);
    if (wr != 0) { free(bytes); set_err(err, "write %s rc=%d", rel, wr); return wr; }
    /* Compute content_hash_hex for snapshot fidelity. */
    char chash[65]; uint8_t hh[32];
    if (nh_porthome_sha256(bytes, bytes_len, hh) == 0) hex_of(hh, 32, chash);
    else chash[0] = '\0';
    free(bytes);
    /* We store chunk addresses from the manifest so a subsequent push
     * doesn't need to re-derive them (though push actually recomputes
     * from local content — this is defensive). */
    char **addrs = NULL;
    if (e->chunks_len) {
        addrs = calloc(e->chunks_len, sizeof *addrs);
        if (!addrs) return NH_SYNCD_ERR_OOM;
        for (size_t i = 0; i < e->chunks_len; i++) {
            addrs[i] = malloc(65);
            if (!addrs[i]) { for (size_t k = 0; k < i; k++) free(addrs[k]); free(addrs); return NH_SYNCD_ERR_OOM; }
            hex_of(e->chunks[i].sha256, 32, addrs[i]);
        }
    }
    rc = nh_syncd_state_upsert_file_(state, rel,
                                     e->mode & 0777, (uint32_t)getuid(),
                                     (uint32_t)getgid(), e->mtime_ns,
                                     bytes_len, chash,
                                     (const char *const *)addrs, e->chunks_len);
    if (addrs) {
        for (size_t i = 0; i < e->chunks_len; i++) free(addrs[i]);
        free(addrs);
    }
    return rc;
}

/* ───────────────── conflict-file writer helpers ───────────────────── */

/* On changed-both: keep the remote version at `rel` (last-writer-wins
 * by mtime), and rename the local loser aside. */
static int handle_content_conflict(const nh_syncd_reconcile_cfg *cfg,
                                   nh_syncd_state *state,
                                   int home_fd,
                                   const char *rel,
                                   const nh_porthome_entry *remote,
                                   const base_row *base,
                                   nh_syncd_reconcile_result *res,
                                   char **err,
                                   int64_t clock_now)
{
    /* Compare mtimes. Remote wins iff remote_mtime > local_mtime.
     * "Local mtime" for us is the current on-disk mtime. */
    struct stat st;
    char abs[PATH_MAX];
    snprintf(abs, sizeof abs, "%s/%s",
             /* home_fd is a fd; we still need root_dir on the cfg only for
              * abs paths in log lines. Use "." with fstatat instead. */
             "", "");
    (void)abs;
    (void)base;
    int have_st = (fstatat(home_fd, rel, &st, AT_SYMLINK_NOFOLLOW) == 0);
    uint64_t local_mt = have_st ? ((uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                                   (uint64_t)st.st_mtim.tv_nsec) : 0;

    /* Winner: side with strictly greater mtime; on equal → REMOTE wins
     * (deterministic, matches "last writer" tie-broken by the network
     * source per §6.3 caveat about clock skew — remote-wins is the
     * defence for clock-lying local). */
    bool remote_wins = (remote->mtime_ns >= local_mt);

    char *loser_path = NULL;
    int rc = build_conflict_path(rel, cfg->device_name, clock_now,
                                 "", &loser_path);
    if (rc != 0) return rc;

    if (remote_wins) {
        /* Preserve local as loser copy first. */
        if (have_st) {
            int r = rename_at(home_fd, rel, loser_path);
            if (r != 0) { free(loser_path); set_err(err, "conflict rename %s -> %s rc=%d", rel, loser_path, r); return r; }
        }
        /* Then apply remote in the true slot. */
        int ar = apply_remote_entry(cfg, state, home_fd, rel, remote, err);
        if (ar != 0) { free(loser_path); return ar; }
    } else {
        /* Local wins. Write the remote-would-be file to loser_path. */
        /* Reuse materialize_file_bytes for FILE remotes; for DIR/SYMLINK
         * remotes it is degenerate — just write a small marker. */
        if (remote->kind == NH_PORTHOME_KIND_FILE) {
            uint8_t *bytes = NULL; size_t blen = 0;
            int mr = materialize_file_bytes(cfg, remote, &bytes, &blen, err);
            if (mr != 0) { free(loser_path); return mr; }
            int wr = write_file_atomic_at(home_fd, loser_path, bytes, blen,
                                          remote->mode & 0777);
            free(bytes);
            if (wr != 0) { free(loser_path); return wr; }
        } else if (remote->kind == NH_PORTHOME_KIND_SYMLINK) {
            (void)write_symlink_at(home_fd, loser_path,
                                   remote->symlink_target ? remote->symlink_target : "");
        }
        /* State: still reflects the LOCAL winner. We enqueue the local
         * onto push_queue so the local mtime propagates. */
        if (cfg->push_queue)
            (void)nh_syncd_batcher_push(cfg->push_queue, rel, NH_SYNCD_CHANGE_MODIFY);
    }

    int rr = result_conflict(res, rel, loser_path);
    free(loser_path);
    return rr;
}

/* Remote-delete of locally-modified: keep local, drop a
 * `.conflict-deleted` sibling marker (empty file) so the user knows. */
static int handle_delete_conflict(const nh_syncd_reconcile_cfg *cfg,
                                  int home_fd,
                                  const char *rel,
                                  nh_syncd_reconcile_result *res,
                                  char **err,
                                  int64_t clock_now)
{
    char *marker = NULL;
    int rc = build_conflict_path(rel, cfg->device_name, clock_now,
                                 ".conflict-deleted", &marker);
    if (rc != 0) return rc;
    /* We build "<rel>.conflict-<device>-<stamp>.conflict-deleted" —
     * a bit belt-and-braces but keeps the naming pattern uniform. */
    const uint8_t empty[1] = { 0 };
    (void)empty;
    /* Write an empty marker. */
    int wr = write_file_atomic_at(home_fd, marker, (const uint8_t *)"", 0, 0644);
    if (wr != 0) { free(marker); set_err(err, "write marker %s rc=%d", marker, wr); return wr; }
    /* Enqueue the local for push so it makes it back to the remote. */
    if (cfg->push_queue)
        (void)nh_syncd_batcher_push(cfg->push_queue, rel, NH_SYNCD_CHANGE_MODIFY);
    int rr = result_conflict(res, rel, marker);
    free(marker);
    return rr;
}

/* ──────────────────── the reconcile pass ─────────────────────────── */

int nh_syncd_reconcile_from_manifest(const nh_syncd_reconcile_cfg *cfg,
                                     nh_syncd_state           *state,
                                     const char               *root_dir,
                                     const nh_syncd_ignore    *ignore,
                                     const char               *state_dir_for_persist,
                                     nh_syncd_reconcile_result **out_result,
                                     char                    **out_error_msg)
{
    if (out_error_msg) *out_error_msg = NULL;
    if (out_result) *out_result = NULL;
    if (!cfg || !cfg->manifest || !state || !root_dir)
        return NH_SYNCD_ERR_ARG;

    int64_t clock_now = cfg->clock_epoch_secs_override;
    if (clock_now == 0) {
        struct timeval tv; gettimeofday(&tv, NULL);
        clock_now = (int64_t)tv.tv_sec;
    }

    nh_syncd_reconcile_result *res = result_new();
    if (!res) return NH_SYNCD_ERR_OOM;

    /* Open $HOME as a dirfd for openat-relative ops. */
    int home_fd = open(root_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (home_fd < 0) {
        nh_syncd_reconcile_result_free(res);
        set_err(out_error_msg, "open %s rc=%d", root_dir, errno);
        return NH_SYNCD_ERR_IO;
    }

    /* Collect base snapshot rows once (state is mutated in the pass). */
    base_ctx bc = { .home_key = cfg->home_key };
    (void)nh_syncd_state_iter_(state, base_collect_cb, &bc);
    if (bc.err != 0) {
        base_ctx_free(&bc); close(home_fd);
        nh_syncd_reconcile_result_free(res);
        set_err(out_error_msg, "base collect rc=%d", bc.err);
        return bc.err;
    }

    /* Track which manifest entries got matched by a base row so we can
     * detect "remote-only" (no base row). */
    uint8_t *remote_matched = calloc(cfg->manifest->entries_len ? cfg->manifest->entries_len : 1, 1);
    if (!remote_matched) {
        base_ctx_free(&bc); close(home_fd);
        nh_syncd_reconcile_result_free(res);
        return NH_SYNCD_ERR_OOM;
    }

    int rc = 0;

    for (size_t i = 0; i < bc.rows_n; i++) {
        base_row *b = &bc.rows[i];

        /* Ignore gate: matches the pusher's rule. Anything ignored we
         * skip entirely — never delete a file the user asked us to
         * leave alone. */
        if (ignore) {
            struct stat sti;
            uint64_t st_dev = 0; uint32_t st_mode = 0;
            if (fstatat(home_fd, b->rel, &sti, AT_SYMLINK_NOFOLLOW) == 0) {
                st_dev = (uint64_t)sti.st_dev;
                st_mode = (uint32_t)sti.st_mode;
            }
            if (nh_syncd_ignore_check(ignore, b->rel, st_dev, st_mode) != NH_SYNCD_IGNORE_PASS)
                continue;
        }

        /* Locate manifest entry (by path_enc). */
        const nh_porthome_entry *remote = remote_lookup(cfg->manifest, b->path_enc);
        if (remote) {
            /* Mark it. */
            size_t idx = (size_t)(remote - cfg->manifest->entries);
            if (idx < cfg->manifest->entries_len) remote_matched[idx] = 1;
        }

        /* Compute local-change vs base. */
        struct stat st;
        bool have_local = (fstatat(home_fd, b->rel, &st, AT_SYMLINK_NOFOLLOW) == 0);
        bool local_changed = false;
        bool local_absent  = !have_local;
        char local_hash[65] = {0};
        uint64_t local_size = 0;
        uint64_t local_mtime_ns = 0;
        (void)local_size; (void)local_mtime_ns;
        if (have_local) {
            local_size = (uint64_t)st.st_size;
            local_mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                             (uint64_t)st.st_mtim.tv_nsec;
            if (b->kind == NH_SYNCD_KIND_FILE && S_ISREG(st.st_mode)) {
                char abs[PATH_MAX];
                int an = snprintf(abs, sizeof abs, "%s/%s", root_dir, b->rel);
                if (an > 0 && an < (int)sizeof abs) {
                    uint64_t sz = 0;
                    if (file_content_hash_hex(abs, local_hash, &sz) == 0) {
                        local_size = sz;
                    }
                }
                if (strcmp(local_hash, b->content_hash_hex) != 0)
                    local_changed = true;
            } else if (b->kind == NH_SYNCD_KIND_DIR) {
                local_changed = !S_ISDIR(st.st_mode);
            } else if (b->kind == NH_SYNCD_KIND_SYMLINK) {
                if (!S_ISLNK(st.st_mode)) local_changed = true;
                else {
                    char tgt[4096];
                    ssize_t tn = readlinkat(home_fd, b->rel, tgt, sizeof tgt - 1);
                    if (tn < 0) local_changed = true;
                    else {
                        tgt[tn] = '\0';
                        if (!b->symlink_target || strcmp(tgt, b->symlink_target) != 0)
                            local_changed = true;
                    }
                }
            }
        } else {
            /* Local file is gone entirely — that's a local delete. */
            local_changed = true;
        }

        /* Compute remote-change vs base. */
        bool remote_absent = (remote == NULL);
        bool remote_changed = false;
        if (remote_absent) {
            remote_changed = true; /* remote deleted */
        } else {
            /* For files: compare content hash via chunk address list —
             * we lack a "content_hash" on the manifest so use the tuple
             * (kind, size, chunk addresses) as a proxy. In practice a
             * single sealed chunk address is a convergent hash of the
             * plaintext under the home_key, so distinct plaintext →
             * distinct addr. */
            if (remote->kind == NH_PORTHOME_KIND_FILE) {
                if (b->kind != NH_SYNCD_KIND_FILE) remote_changed = true;
                else if (remote->size != b->size) remote_changed = true;
                else {
                    /* Compare chunk addresses against snapshot's
                     * chunk_addrs_hex. Fetch via state's raw json. */
                    /* We'll approximate: any mismatch in chunk count
                     * triggers changed; otherwise compare each addr. */
                    /* Look up the state's raw row directly. */
                    /* NB: snapshot rows aren't in bc but we can hit the
                     * state through the public find() — for chunk_addrs
                     * that isn't exposed, so we access via a helper: */
                    extern int nh_syncd_state_get_chunk_addrs_(const nh_syncd_state *s,
                                                               const char *rel,
                                                               char ***out,
                                                               size_t *out_n);
                    char **addrs = NULL; size_t n = 0;
                    (void)nh_syncd_state_get_chunk_addrs_(state, b->rel, &addrs, &n);
                    if (n != remote->chunks_len) remote_changed = true;
                    else {
                        for (size_t k = 0; k < n; k++) {
                            char rh[65];
                            hex_of(remote->chunks[k].sha256, 32, rh);
                            if (!addrs[k] || strcmp(addrs[k], rh) != 0) {
                                remote_changed = true; break;
                            }
                        }
                    }
                    if (addrs) {
                        for (size_t k = 0; k < n; k++) free(addrs[k]);
                        free(addrs);
                    }
                }
            } else if (remote->kind == NH_PORTHOME_KIND_SYMLINK) {
                if (b->kind != NH_SYNCD_KIND_SYMLINK ||
                    !remote->symlink_target || !b->symlink_target ||
                    strcmp(remote->symlink_target, b->symlink_target) != 0)
                    remote_changed = true;
            } else if (remote->kind == NH_PORTHOME_KIND_DIR) {
                if (b->kind != NH_SYNCD_KIND_DIR) remote_changed = true;
            }
        }

        /* Phase 4 P4-I: lazy-subtree skip (bead nostrc-1u55).
         *
         * If the entry falls under a lazy prefix AND the local slot
         * does not have a materialized copy from a prior reconcile,
         * suppress all writes. We still update snapshot.json below
         * (via apply_remote_entry variants that record metadata) but
         * we never touch the working tree — those bytes come from
         * the FUSE overlay on demand.
         *
         * If a materialized copy already exists (local_absent is
         * false), the maintainer's approval note is explicit:
         * NEVER delete it. We fall through to normal handling for
         * writes but DROP any remote-delete for this path — it's
         * suppressed with a warn log rather than becoming a conflict.
         */
        bool lazy_covers = cfg->lazy && nh_syncd_lazy_covers(cfg->lazy, b->rel);
        if (lazy_covers) {
            if (remote_absent && !local_absent) {
                /* Remote-delete under a lazy subtree that has a
                 * materialized local copy. Refuse to delete. Warn
                 * once so operators see it in the log. */
                fprintf(stderr,
                        "syncd/reconcile: lazy subtree shadowed remote-delete %s "
                        "(kept; additive-only suppression)\n", b->rel);
                /* Snapshot still tracks the entry — do NOT clear it. */
                continue;
            }
            if (!remote_absent && local_absent) {
                /* Lazy entry: record metadata only, no bytes. Snapshot
                 * gets the manifest's kind/mode/size/chunks so the
                 * FUSE overlay can serve reads without a materialized
                 * $HOME copy. */
                const nh_porthome_entry *e = remote;
                char **addrs = NULL;
                if (e->kind == NH_PORTHOME_KIND_FILE && e->chunks_len) {
                    addrs = calloc(e->chunks_len, sizeof *addrs);
                    if (!addrs) { rc = NH_SYNCD_ERR_OOM; goto done; }
                    for (size_t k = 0; k < e->chunks_len; k++) {
                        addrs[k] = malloc(65);
                        if (!addrs[k]) {
                            for (size_t j = 0; j < k; j++) free(addrs[j]);
                            free(addrs); rc = NH_SYNCD_ERR_OOM; goto done;
                        }
                        hex_of(e->chunks[k].sha256, 32, addrs[k]);
                    }
                }
                int ur;
                if (e->kind == NH_PORTHOME_KIND_DIR) {
                    ur = nh_syncd_state_upsert_dir_(state, b->rel,
                            e->mode & 0777, (uint32_t)getuid(),
                            (uint32_t)getgid(), e->mtime_ns);
                } else if (e->kind == NH_PORTHOME_KIND_SYMLINK) {
                    ur = nh_syncd_state_upsert_symlink_(state, b->rel,
                            e->mode & 0777, (uint32_t)getuid(),
                            (uint32_t)getgid(), e->mtime_ns,
                            e->symlink_target ? e->symlink_target : "");
                } else {
                    ur = nh_syncd_state_upsert_file_(state, b->rel,
                            e->mode & 0777, (uint32_t)getuid(),
                            (uint32_t)getgid(), e->mtime_ns,
                            e->size, ""/*content_hash*/,
                            (const char *const *)addrs, e->chunks_len);
                }
                if (addrs) {
                    for (size_t k = 0; k < e->chunks_len; k++) free(addrs[k]);
                    free(addrs);
                }
                if (ur != 0) { rc = ur; goto done; }
                res->applied++;
                continue;
            }
            /* Otherwise (both changed / both present) fall through
             * to normal handling — a lazy entry that DID get
             * materialized in the past is handled like a normal
             * entry: writes still land under $HOME, deletes still
             * turn into conflicts, etc. The suppression is additive
             * for cold entries only. */
        }

        if (local_changed && remote_changed) {
            /* CONFLICT branch. */
            if (remote_absent) {
                /* Remote deleted; local modified — keep + marker. */
                int rr = handle_delete_conflict(cfg, home_fd, b->rel,
                                                res, out_error_msg, clock_now);
                if (rr != 0) { rc = rr; goto done; }
            } else if (local_absent) {
                /* Local was deleted; remote also changed. Treat as
                 * remote-wins (materialize) but still note conflict via
                 * a .conflict-deleted marker at the remote's slot. */
                int ar = apply_remote_entry(cfg, state, home_fd, b->rel, remote, out_error_msg);
                if (ar != 0) { rc = ar; goto done; }
                res->applied++;
                char *marker = NULL;
                if (build_conflict_path(b->rel, cfg->device_name, clock_now,
                                        ".conflict-locally-deleted", &marker) == 0) {
                    (void)write_file_atomic_at(home_fd, marker, (const uint8_t *)"", 0, 0644);
                    (void)result_conflict(res, b->rel, marker);
                    free(marker);
                }
            } else {
                int cr = handle_content_conflict(cfg, state, home_fd, b->rel,
                                                 remote, b, res, out_error_msg, clock_now);
                if (cr != 0) { rc = cr; goto done; }
                res->applied++;
            }
        } else if (local_changed) {
            /* local-only → queue push (I1 batch API). */
            if (cfg->push_queue) {
                nh_syncd_change_kind ck = local_absent
                                         ? NH_SYNCD_CHANGE_DELETE
                                         : NH_SYNCD_CHANGE_MODIFY;
                (void)nh_syncd_batcher_push(cfg->push_queue, b->rel, ck);
            }
            (void)result_push(res, b->rel);
        } else if (remote_changed) {
            /* remote-only. */
            if (remote_absent) {
                /* Remote deleted; local matches base → apply delete. */
                (void)delete_file_at(home_fd, b->rel);
                (void)nh_syncd_state_delete_(state, b->rel);
                res->deleted++;
            } else {
                int ar = apply_remote_entry(cfg, state, home_fd, b->rel, remote, out_error_msg);
                if (ar != 0) { rc = ar; goto done; }
                res->applied++;
            }
        }
        /* else: neither → no-op. */
    }

    /* Remote-only paths (not in base): materialize under encrypted-name
     * (design punt — see docs/reviews/porthome-syncd-pull-2026-09-23.md).
     * We only apply directories and symlinks in this branch — files
     * would need the plaintext name to be usable, and we don't have
     * it. Dirs/symlinks under opaque names are still safe. In v1 we
     * SKIP files silently. */
    for (size_t i = 0; i < cfg->manifest->entries_len; i++) {
        if (remote_matched[i]) continue;
        const nh_porthome_entry *e = &cfg->manifest->entries[i];
        if (!e->path_enc || !rel_ok(e->path_enc)) continue;
        if (e->kind == NH_PORTHOME_KIND_FILE) continue;   /* v1 punt */
        /* Phase 4 P4-I: opaque-name entries under lazy prefixes still
         * skip materialization (path_enc is a hex hash so this check
         * is defensive — the prefix will rarely match — but keeping
         * the guard is cheap). */
        if (cfg->lazy && nh_syncd_lazy_covers(cfg->lazy, e->path_enc)) continue;
        int ar = apply_remote_entry(cfg, state, home_fd, e->path_enc, e, out_error_msg);
        if (ar == 0) res->applied++;
    }

    /* Persist. */
    if (cfg->remote_generation > nh_syncd_state_get_local_generation(state)) {
        /* Bump local up to remote_generation. */
        while (nh_syncd_state_get_local_generation(state) < cfg->remote_generation)
            nh_syncd_state_bump_generation_(state);
    }
    nh_syncd_state_set_remote_generation(state, cfg->remote_generation);
    if (state_dir_for_persist) {
        int srv = nh_syncd_state_save(state, state_dir_for_persist);
        if (srv != NH_SYNCD_OK) set_err(out_error_msg, "state_save rc=%d", srv);
    }

    /* Notification (single per pass). */
    if (res->conflicts_n > 0) {
        size_t total_len = 1;
        for (size_t i = 0; i < res->conflicts_n; i++)
            total_len += strlen(res->conflicts[i].rel) + 1;
        char *joined = malloc(total_len);
        if (joined) {
            joined[0] = '\0';
            for (size_t i = 0; i < res->conflicts_n; i++) {
                if (i) strcat(joined, " ");
                strcat(joined, res->conflicts[i].rel);
            }
            emit_conflict_notification(cfg, res->conflicts_n, joined);
            free(joined);
        }
    }

done:
    free(remote_matched);
    base_ctx_free(&bc);
    close(home_fd);
    if (rc != 0) {
        nh_syncd_reconcile_result_free(res);
        if (out_result) *out_result = NULL;
        return rc;
    }
    if (out_result) *out_result = res;
    else nh_syncd_reconcile_result_free(res);
    return NH_SYNCD_OK;
}

/* ──────────────────── additive $HOME rescan ─────────────────────── */

/* Recursive walk from dirfd. rel_prefix is the current dirfd's rel to
 * $HOME (empty for root). */
static int rescan_walk(int home_fd, const char *rel_prefix,
                      nh_syncd_state *state,
                      const nh_syncd_ignore *ignore,
                      int depth)
{
    if (depth > 32) return 0; /* refuse deep recursion */
    int dfd;
    if (rel_prefix[0] == '\0') {
        dfd = dup(home_fd);
    } else {
        dfd = openat(home_fd, rel_prefix, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (dfd < 0) return 0;
    DIR *d = fdopendir(dfd);
    if (!d) { close(dfd); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *rel = NULL;
        /* nostrc-cvqe: check asprintf — on -1 the ptr is indeterminate */
        if (rel_prefix[0]) {
            if (asprintf(&rel, "%s/%s", rel_prefix, de->d_name) < 0) rel = NULL;
        } else {
            rel = strdup(de->d_name);
        }
        if (!rel) continue;
        struct stat st;
        if (fstatat(home_fd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0) { free(rel); continue; }
        if (ignore) {
            if (nh_syncd_ignore_check(ignore, rel, (uint64_t)st.st_dev,
                                      (uint32_t)st.st_mode) != NH_SYNCD_IGNORE_PASS) {
                free(rel); continue;
            }
        }
        uint32_t mode = st.st_mode & 07777;
        uint32_t uid = (uint32_t)st.st_uid, gid = (uint32_t)st.st_gid;
        uint64_t mt = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
        if (S_ISDIR(st.st_mode)) {
            (void)nh_syncd_state_upsert_dir_(state, rel, mode, uid, gid, mt);
            rescan_walk(home_fd, rel, state, ignore, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[4096];
            ssize_t tn = readlinkat(home_fd, rel, tgt, sizeof tgt - 1);
            if (tn >= 0) {
                tgt[tn] = '\0';
                (void)nh_syncd_state_upsert_symlink_(state, rel, mode, uid, gid, mt, tgt);
            }
        } else if (S_ISREG(st.st_mode)) {
            char abs[PATH_MAX];
            /* Build absolute so file_content_hash_hex can open. */
            /* rel_prefix + rel: dfd is at home so rel is home-rel — we
             * need root_dir to build abs. We don't have it here; we
             * open via openat instead. */
            (void)abs;
            int ffd = openat(home_fd, rel, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            char chash[65] = {0};
            if (ffd >= 0) {
                /* Slurp for hash — bounded. */
                struct stat s2;
                if (fstat(ffd, &s2) == 0 && (uint64_t)s2.st_size <=
                    (uint64_t)NH_SYNCD_CHUNK_SIZE * NH_PORTHOME_MAX_CHUNKS_PER_ENTRY) {
                    uint8_t *buf = malloc((size_t)s2.st_size ? (size_t)s2.st_size : 1);
                    if (buf) {
                        size_t off = 0;
                        while (off < (size_t)s2.st_size) {
                            ssize_t r = read(ffd, buf + off, (size_t)s2.st_size - off);
                            if (r < 0) { if (errno == EINTR) continue; break; }
                            if (r == 0) break;
                            off += (size_t)r;
                        }
                        uint8_t hh[32];
                        if (nh_porthome_sha256(buf, off, hh) == 0) hex_of(hh, 32, chash);
                        free(buf);
                    }
                }
                close(ffd);
            }
            (void)nh_syncd_state_upsert_file_(state, rel, mode, uid, gid, mt,
                                              (uint64_t)st.st_size, chash,
                                              NULL, 0);
        }
        free(rel);
    }
    closedir(d);
    return 0;
}

int nh_syncd_rescan_home_additive(nh_syncd_state *state,
                                  const char *root_dir,
                                  const nh_syncd_ignore *ignore,
                                  const char *state_dir_for_persist)
{
    if (!state || !root_dir) return NH_SYNCD_ERR_ARG;
    int fd = open(root_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NH_SYNCD_ERR_IO;
    (void)rescan_walk(fd, "", state, ignore, 0);
    close(fd);
    if (state_dir_for_persist) {
        (void)nh_syncd_state_save(state, state_dir_for_persist);
        (void)nh_syncd_partial_state_set(state_dir_for_persist);
    }
    return NH_SYNCD_OK;
}

/* ──────────────────── xnxd part 2 — rescan diff ─────────────────────── */

/* Recursive walk that pushes CREATE / MODIFY into a batcher for every
 * on-disk entry that diverges from `state`. Marks each path it visits
 * in `seen` (jansson object: key -> json_true). Never mutates `state`.
 *
 * Deliberate simplifications:
 *   - We compare against the state's cached (size, mtime_ns) tuple —
 *     the same pair the pusher uses for its unchanged short-circuit.
 *     A hash divergence with identical size+mtime is treated as
 *     unchanged; that's the same latitude the inotify path has.
 *   - Ignore checks reuse nh_syncd_ignore_check with the current
 *     stat data; excluded paths are neither pushed nor marked seen,
 *     so the DELETE sweep does not touch them either. */
static int rescan_diff_walk(int home_fd, const char *rel_prefix,
                            const nh_syncd_state *state,
                            const nh_syncd_ignore *ignore,
                            nh_syncd_batcher *batcher,
                            json_t *seen,
                            int depth,
                            nh_syncd_rescan_stats *st)
{
    if (depth > 32) return 0;
    int dfd = (rel_prefix[0] == '\0')
              ? dup(home_fd)
              : openat(home_fd, rel_prefix,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return 0;
    DIR *d = fdopendir(dfd);
    if (!d) { close(dfd); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *rel = NULL;
        if (rel_prefix[0]) {
            if (asprintf(&rel, "%s/%s", rel_prefix, de->d_name) < 0) rel = NULL;
        } else {
            rel = strdup(de->d_name);
        }
        if (!rel) continue;
        struct stat s2;
        if (fstatat(home_fd, rel, &s2, AT_SYMLINK_NOFOLLOW) != 0) {
            free(rel); continue;
        }
        if (ignore) {
            if (nh_syncd_ignore_check(ignore, rel,
                                      (uint64_t)s2.st_dev,
                                      (uint32_t)s2.st_mode)
                != NH_SYNCD_IGNORE_PASS) {
                free(rel); continue;
            }
        }
        json_object_set_new(seen, rel, json_true());
        const nh_syncd_entry *cur = nh_syncd_state_find(state, rel);
        uint64_t mt = (uint64_t)s2.st_mtim.tv_sec * 1000000000ull
                    + (uint64_t)s2.st_mtim.tv_nsec;
        if (S_ISDIR(s2.st_mode)) {
            if (!cur || nh_syncd_entry_kind(cur) != NH_SYNCD_KIND_DIR ||
                nh_syncd_entry_mtime_ns(cur) != mt) {
                if (batcher) (void)nh_syncd_batcher_push(batcher, rel,
                    cur ? NH_SYNCD_CHANGE_MODIFY : NH_SYNCD_CHANGE_CREATE);
                if (st) (cur ? st->modified++ : st->added++);
            } else if (st) {
                st->unchanged++;
            }
            rescan_diff_walk(home_fd, rel, state, ignore, batcher,
                             seen, depth + 1, st);
        } else if (S_ISLNK(s2.st_mode)) {
            if (!cur || nh_syncd_entry_kind(cur) != NH_SYNCD_KIND_SYMLINK ||
                nh_syncd_entry_mtime_ns(cur) != mt) {
                if (batcher) (void)nh_syncd_batcher_push(batcher, rel,
                    cur ? NH_SYNCD_CHANGE_MODIFY : NH_SYNCD_CHANGE_CREATE);
                if (st) (cur ? st->modified++ : st->added++);
            } else if (st) {
                st->unchanged++;
            }
        } else if (S_ISREG(s2.st_mode)) {
            if (!cur || nh_syncd_entry_kind(cur) != NH_SYNCD_KIND_FILE ||
                nh_syncd_entry_size(cur) != (uint64_t)s2.st_size ||
                nh_syncd_entry_mtime_ns(cur) != mt) {
                if (batcher) (void)nh_syncd_batcher_push(batcher, rel,
                    cur ? NH_SYNCD_CHANGE_MODIFY : NH_SYNCD_CHANGE_CREATE);
                if (st) (cur ? st->modified++ : st->added++);
            } else if (st) {
                st->unchanged++;
            }
        }
        free(rel);
    }
    closedir(d);
    return 0;
}

int nh_syncd_rescan_diff(const nh_syncd_state    *state,
                         const char              *root_dir,
                         const nh_syncd_ignore   *ignore,
                         nh_syncd_batcher        *batcher,
                         nh_syncd_rescan_stats   *out_stats)
{
    if (!state || !root_dir) return NH_SYNCD_ERR_ARG;
    if (out_stats) memset(out_stats, 0, sizeof *out_stats);
    int fd = open(root_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NH_SYNCD_ERR_IO;

    json_t *seen = json_object();
    if (!seen) { close(fd); return NH_SYNCD_ERR_OOM; }

    (void)rescan_diff_walk(fd, "", state, ignore, batcher, seen, 0, out_stats);
    close(fd);

    /* Sweep for state entries that we did NOT visit — those are
     * DELETES. Iterate state via the public `_at` accessor. */
    size_t n = nh_syncd_state_file_count(state);
    for (size_t i = 0; i < n; i++) {
        const char *rel = NULL;
        const nh_syncd_entry *e = nh_syncd_state_at(state, i, &rel);
        if (!e || !rel) continue;
        if (json_object_get(seen, rel)) continue;
        /* Skip ignored paths — mirror the walk's filter. Path-only is
         * enough here (we have no stat data for a missing file). */
        if (ignore &&
            nh_syncd_ignore_check_path(ignore, rel) != NH_SYNCD_IGNORE_PASS)
            continue;
        if (batcher) (void)nh_syncd_batcher_push(batcher, rel,
                                                 NH_SYNCD_CHANGE_DELETE);
        if (out_stats) out_stats->deleted++;
    }

    json_decref(seen);
    return NH_SYNCD_OK;
}


