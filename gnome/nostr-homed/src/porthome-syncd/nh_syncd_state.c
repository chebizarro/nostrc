/*
 * nh_syncd_state.c — persist/restore state under ~/.local/state/nostr-homed/
 *
 * SPDX-License-Identifier: MIT
 * See nh_syncd.h for the on-disk schema. Persistence is atomic:
 * every write goes to a `.tmp` sibling then rename(2) into place.
 */

#define _GNU_SOURCE
#include "nh_syncd.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Simple open-addressed C hashmap keyed by string. jansson gives us
 * that for free (json_object_get / _set), so files are stored as a
 * jansson object embedded in the state and only "cooked" out on read
 * via the accessor. That keeps this file small and — more importantly
 * — avoids a second serialization path that could disagree with the
 * on-disk schema. */

struct nh_syncd_entry {
    /* Owned. */
    char          *rel_path;
    nh_syncd_kind  kind;
    uint32_t       mode;
    uint32_t       uid;
    uint32_t       gid;
    uint64_t       mtime_ns;
    uint64_t       size;
    /* NUL-terminated. 64 hex chars for files with content; "" otherwise. */
    char           content_hash_hex[65];
    /* For files: heap array of NUL-terminated 64-hex addresses. */
    char         **chunk_addrs_hex;
    size_t         chunk_addrs_n;
    /* For symlinks: heap C-string. NULL otherwise. */
    char          *symlink_target;
    /* Phase 4 P4-I: borrowed pointers into the underlying jansson
     * `chunk_addrs_hex` array and `symlink_target` string for the
     * currently-bound row. Populated by _find / _at at each call. */
    json_t        *row_ref;
};

struct nh_syncd_state {
    uint64_t       local_generation;
    uint64_t       remote_generation;
    char          *root;                /* owned */
    char          *d_tag;               /* owned */
    char          *account_pubkey_hex;  /* owned */
    uint8_t        root_id[NH_PORTHOME_SHA256_LEN];
    /* Files map: keyed by rel_path. json_object of entry rows so we
     * always speak the on-disk schema. */
    json_t        *files;
};

void nh_syncd_state_free(nh_syncd_state *s) {
    if (!s) return;
    free(s->root);
    free(s->d_tag);
    free(s->account_pubkey_hex);
    if (s->files) json_decref(s->files);
    free(s);
}

int nh_syncd_state_new(const char *root_abs,
                       const char *d_tag,
                       const char *account_pubkey_hex_or_empty,
                       const uint8_t root_id[NH_PORTHOME_SHA256_LEN],
                       nh_syncd_state **out)
{
    if (!root_abs || !d_tag || !root_id || !out) return NH_SYNCD_ERR_ARG;
    nh_syncd_state *s = calloc(1, sizeof *s);
    if (!s) return NH_SYNCD_ERR_OOM;
    s->root = strdup(root_abs);
    s->d_tag = strdup(d_tag);
    s->account_pubkey_hex = strdup(account_pubkey_hex_or_empty ?
                                   account_pubkey_hex_or_empty : "");
    s->files = json_object();
    if (!s->root || !s->d_tag || !s->account_pubkey_hex || !s->files) {
        nh_syncd_state_free(s);
        return NH_SYNCD_ERR_OOM;
    }
    memcpy(s->root_id, root_id, NH_PORTHOME_SHA256_LEN);
    *out = s;
    return NH_SYNCD_OK;
}

uint64_t nh_syncd_state_get_local_generation(const nh_syncd_state *s) {
    return s ? s->local_generation : 0;
}
uint64_t nh_syncd_state_get_remote_generation(const nh_syncd_state *s) {
    return s ? s->remote_generation : 0;
}
void nh_syncd_state_set_remote_generation(nh_syncd_state *s, uint64_t g) {
    if (s && g > s->remote_generation) s->remote_generation = g;
}
const char *nh_syncd_state_get_root(const nh_syncd_state *s) {
    return s ? s->root : NULL;
}
const char *nh_syncd_state_get_d_tag(const nh_syncd_state *s) {
    return s ? s->d_tag : NULL;
}
const char *nh_syncd_state_get_account_pubkey_hex(const nh_syncd_state *s) {
    return s ? s->account_pubkey_hex : NULL;
}
size_t nh_syncd_state_file_count(const nh_syncd_state *s) {
    return s && s->files ? (size_t)json_object_size(s->files) : 0;
}

static nh_syncd_kind str_to_kind(const char *s) {
    if (!s) return NH_SYNCD_KIND_FILE;
    if (!strcmp(s, "dir"))     return NH_SYNCD_KIND_DIR;
    if (!strcmp(s, "symlink")) return NH_SYNCD_KIND_SYMLINK;
    return NH_SYNCD_KIND_FILE;
}

/* ─── low-level path helpers ─────────────────────────────────────────── */

static char *joinp(const char *dir, const char *name) {
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

/* Best-effort mkdir -p. */
static int mkdirp(const char *path) {
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

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data; size_t rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        rem -= (size_t)w; p += w;
    }
    return 0;
}

/* Fills the temp file; returns 0 on success, a NH_SYNCD_ERR_* otherwise. */
typedef int (*write_fill_fn)(int fd, void *ud);

typedef struct { const void *data; size_t len; } bytes_fill;
static int fill_bytes(int fd, void *ud) {
    const bytes_fill *b = ud;
    return write_all(fd, b->data, b->len) == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_IO;
}

/* tmp sibling + fsync + rename(2): readers see the old file or the new
 * one, never a torn write. */
static int write_atomic_with(const char *dir, const char *name,
                             write_fill_fn fill, void *ud) {
    char *tmp = malloc(strlen(dir) + 1 + strlen(name) + 5);
    char *fin = joinp(dir, name);
    if (!tmp || !fin) { free(tmp); free(fin); return NH_SYNCD_ERR_OOM; }
    sprintf(tmp, "%s/%s.tmp", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    int frc = fill(fd, ud);
    if (frc != NH_SYNCD_OK) { close(fd); unlink(tmp); free(tmp); free(fin); return frc; }
    if (fsync(fd) < 0) { close(fd); unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    if (close(fd) < 0) { unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    if (rename(tmp, fin) < 0) { unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    free(tmp); free(fin);
    return NH_SYNCD_OK;
}

static int write_atomic(const char *dir, const char *name,
                        const void *data, size_t len) {
    bytes_fill b = { data, len };
    return write_atomic_with(dir, name, fill_bytes, &b);
}

/* Small-file reader for remote.json (a single integer). snapshot.json
 * does NOT go through here — it is stream-parsed under its own,
 * configurable ceiling (nostrc-5y2t); see nh_syncd_state_load. */
static int read_file_bytes(const char *path, char **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? -2 : NH_SYNCD_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NH_SYNCD_ERR_IO; }
    /* Hard cap so a bad file cannot exhaust memory. */
    if (st.st_size < 0 || (size_t)st.st_size > 32u * 1024u * 1024u) {
        close(fd); return NH_SYNCD_ERR_IO;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NH_SYNCD_ERR_OOM; }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NH_SYNCD_ERR_IO; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = '\0';
    *out = buf; *out_len = off;
    return 0;
}

/* ─── hex helpers ─────────────────────────────────────────────────── */

static int hex64_ok(const char *s) {
    if (!s) return 0;
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* ─── save ─────────────────────────────────────────────────────────── */

typedef struct {
    int    fd;
    int    failed;
    size_t n;
    char   buf[64 * 1024];
} dump_buf;

static int dump_flush(dump_buf *d) {
    if (d->n && write_all(d->fd, d->buf, d->n) != 0) { d->failed = 1; return -1; }
    d->n = 0;
    return 0;
}

/* json_dump_callback emits one token at a time; batch them. */
static int dump_cb(const char *s, size_t len, void *data) {
    dump_buf *d = data;
    if (d->n + len > sizeof d->buf && dump_flush(d) != 0) return -1;
    if (len > sizeof d->buf) {
        if (write_all(d->fd, s, len) != 0) { d->failed = 1; return -1; }
        return 0;
    }
    memcpy(d->buf + d->n, s, len);
    d->n += len;
    return 0;
}

typedef struct { json_t *j; } json_fill;
static int fill_json(int fd, void *ud) {
    const json_fill *jf = ud;
    dump_buf *d = malloc(sizeof *d);
    if (!d) return NH_SYNCD_ERR_OOM;
    d->fd = fd; d->failed = 0; d->n = 0;
    int rc = json_dump_callback(jf->j, dump_cb, d, JSON_SORT_KEYS);
    if (rc == 0) rc = dump_flush(d);
    int failed = d->failed;
    free(d);
    if (rc == 0) return NH_SYNCD_OK;
    return failed ? NH_SYNCD_ERR_IO : NH_SYNCD_ERR_JSON;
}

int nh_syncd_state_save(const nh_syncd_state *s, const char *state_dir) {
    if (!s || !state_dir) return NH_SYNCD_ERR_ARG;
    int rc = mkdirp(state_dir);
    if (rc < 0) return rc;

    char root_id_hex[65];
    nh_porthome_hex64(s->root_id, root_id_hex);

    json_t *j = json_object();
    if (!j) return NH_SYNCD_ERR_OOM;
    json_object_set_new(j, "schema", json_integer(1));
    json_object_set_new(j, "generation", json_integer((json_int_t)s->local_generation));
    json_object_set_new(j, "root", json_string(s->root));
    json_object_set_new(j, "d_tag", json_string(s->d_tag));
    json_object_set_new(j, "account_pubkey_hex", json_string(s->account_pubkey_hex));
    json_object_set_new(j, "root_id_hex", json_string(root_id_hex));
    /* Copy the files object (it already matches the schema). */
    json_object_set(j, "files", s->files);

    /* nostrc-5y2t: stream the dump straight into the temp file instead
     * of materialising it with json_dumps (a 200k-entry home is a
     * 64 MB string on top of the live tree). No JSON_INDENT: the
     * indentation was ~1/3 of the bytes and nothing reads it by eye. */
    json_fill jf = { j };
    rc = write_atomic_with(state_dir, "snapshot.json", fill_json, &jf);
    json_decref(j);
    if (rc < 0) return rc;

    /* Also write the standalone generation file for external observers. */
    char genbuf[32];
    int n = snprintf(genbuf, sizeof genbuf, "%llu\n",
                     (unsigned long long)s->local_generation);
    if (n < 0 || n >= (int)sizeof genbuf) return NH_SYNCD_ERR_IO;
    return write_atomic(state_dir, "generation", genbuf, (size_t)n);
}

/* ─── load ─────────────────────────────────────────────────────────── */

uint64_t nh_syncd_snapshot_max_bytes(void) {
    const char *e = getenv("NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES");
    if (e && *e) {
        char *end = NULL;
        errno = 0;
        unsigned long long v = strtoull(e, &end, 10);
        if (errno == 0 && end && *end == '\0' && v > 0) return (uint64_t)v;
        fprintf(stderr, "syncd: ignoring invalid NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES=\"%s\"\n", e);
    }
    return NH_SYNCD_SNAPSHOT_MAX_BYTES_DEFAULT;
}

const char *nh_syncd_state_load_error_class(int rc) {
    switch (rc) {
    case NH_SYNCD_OK:                     return "";
    case NH_SYNCD_ERR_SNAPSHOT_TOO_LARGE: return "snapshot-too-large";
    case NH_SYNCD_ERR_SNAPSHOT_SCHEMA:    return "snapshot-schema";
    case NH_SYNCD_ERR_JSON:               return "snapshot-corrupt";
    case NH_SYNCD_ERR_OOM:                return "snapshot-oom";
    default:                              return "snapshot-unreadable";
    }
}

static nh_syncd_state *state_empty(void) {
    nh_syncd_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->files = json_object();
    s->root = strdup("");
    s->d_tag = strdup("");
    s->account_pubkey_hex = strdup("");
    if (!s->files || !s->root || !s->d_tag || !s->account_pubkey_hex) {
        nh_syncd_state_free(s);
        return NULL;
    }
    return s;
}

/* json_load_callback source: read(2) straight from the fd (no stdio
 * locking per byte), and re-check the ceiling while streaming so a
 * file that grows after fstat cannot sneak past it. */
typedef struct {
    int      fd;
    uint64_t seen;
    uint64_t cap;
    int      err;   /* errno of a read failure, or EFBIG */
} snap_reader;

static size_t snap_read_cb(void *buf, size_t len, void *data) {
    snap_reader *r = data;
    for (;;) {
        ssize_t n = read(r->fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            r->err = errno;
            return (size_t)-1;
        }
        r->seen += (uint64_t)n;
        if (r->seen > r->cap) { r->err = EFBIG; return (size_t)-1; }
        return (size_t)n;
    }
}

/* Validate the parsed document and build the state. Consumes `j`. */
static int state_from_json(json_t *j, const char *path, nh_syncd_state **out) {
    const char *why = NULL;
    json_t *schema = json_object_get(j, "schema");
    json_t *jgen  = json_object_get(j, "generation");
    json_t *jroot = json_object_get(j, "root");
    json_t *jdtag = json_object_get(j, "d_tag");
    json_t *jpk   = json_object_get(j, "account_pubkey_hex");
    json_t *jrid  = json_object_get(j, "root_id_hex");
    json_t *jfiles= json_object_get(j, "files");

    if (!json_is_object(j)) why = "top level is not an object";
    else if (!json_is_integer(schema)) why = "missing/non-integer \"schema\"";
    else if (json_integer_value(schema) != 1) {
        /* A well-formed snapshot from a NEWER daemon (downgrade). Not
         * corruption: refuse, never quarantine a valid file. */
        fprintf(stderr,
                "syncd: %s has schema %lld; this daemon only reads schema 1 — "
                "refusing to load (or overwrite) it\n",
                path, (long long)json_integer_value(schema));
        json_decref(j);
        return NH_SYNCD_ERR_SNAPSHOT_SCHEMA;
    }
    else if (!json_is_integer(jgen) || !json_is_string(jroot) ||
             !json_is_string(jdtag) || !json_is_string(jpk) ||
             !json_is_string(jrid) || !json_is_object(jfiles))
        why = "missing or mistyped top-level field";
    else if (!hex64_ok(json_string_value(jrid)))
        why = "root_id_hex is not 64 lowercase hex";
    if (why) {
        fprintf(stderr, "syncd: %s is corrupt: %s\n", path, why);
        json_decref(j);
        return NH_SYNCD_ERR_JSON;
    }

    nh_syncd_state *s = calloc(1, sizeof *s);
    if (!s) { json_decref(j); return NH_SYNCD_ERR_OOM; }
    s->local_generation = (uint64_t)json_integer_value(jgen);
    s->root = strdup(json_string_value(jroot));
    s->d_tag = strdup(json_string_value(jdtag));
    s->account_pubkey_hex = strdup(json_string_value(jpk));
    if (nh_porthome_from_hex64(json_string_value(jrid), s->root_id) != 0) {
        json_decref(j); nh_syncd_state_free(s);
        fprintf(stderr, "syncd: %s is corrupt: root_id_hex does not decode\n", path);
        return NH_SYNCD_ERR_JSON;
    }
    s->files = json_incref(jfiles);
    json_decref(j);
    if (!s->root || !s->d_tag || !s->account_pubkey_hex || !s->files) {
        nh_syncd_state_free(s);
        return NH_SYNCD_ERR_OOM;
    }
    *out = s;
    return NH_SYNCD_OK;
}

int nh_syncd_state_load(const char *state_dir,
                        nh_syncd_state **out,
                        bool *out_snapshot_unknown)
{
    if (!state_dir || !out) return NH_SYNCD_ERR_ARG;
    if (out_snapshot_unknown) *out_snapshot_unknown = false;
    *out = NULL;

    char *snap_path = joinp(state_dir, "snapshot.json");
    if (!snap_path) return NH_SYNCD_ERR_OOM;
    int rc = NH_SYNCD_ERR_IO;
    int fd = open(snap_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        /* Missing — return a fresh empty state. Caller (push closure)
         * still refuses to run until snapshot base is known via
         * NH_SYNCD_ERR_SNAPSHOT_UNKNOWN; init happens through
         * nh_syncd_state_new. */
        free(snap_path);
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        nh_syncd_state *s = state_empty();
        if (!s) return NH_SYNCD_ERR_OOM;
        *out = s;
        return NH_SYNCD_OK;
    }
    if (fd < 0) {
        fprintf(stderr, "syncd: cannot open %s: %s\n", snap_path, strerror(errno));
        goto fail;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "syncd: cannot stat %s: %s\n", snap_path, strerror(errno));
        goto fail;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "syncd: %s is not a regular file\n", snap_path);
        goto fail;
    }
    snap_reader rd = { fd, 0, nh_syncd_snapshot_max_bytes(), 0 };
    if ((uint64_t)st.st_size > rd.cap) {
        rd.err = EFBIG;
    } else {
        json_error_t je;
        json_t *j = json_load_callback(snap_read_cb, &rd, 0, &je);
        if (j) {
            rc = state_from_json(j, snap_path, out);
            goto done;
        }
        if (!rd.err) {
            fprintf(stderr, "syncd: %s is corrupt: %s (line %d, column %d)\n",
                    snap_path, je.text, je.line, je.column);
            rc = NH_SYNCD_ERR_JSON;
            goto done;
        }
    }
    if (rd.err == EFBIG) {
        fprintf(stderr,
                "syncd: %s is %llu bytes, over the %llu-byte snapshot ceiling — "
                "refusing to load (or overwrite) it; raise "
                "NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES if this home really is "
                "that large\n",
                snap_path,
                (unsigned long long)((uint64_t)st.st_size > rd.seen ? (uint64_t)st.st_size : rd.seen),
                (unsigned long long)rd.cap);
        rc = NH_SYNCD_ERR_SNAPSHOT_TOO_LARGE;
    } else {
        fprintf(stderr, "syncd: read error on %s: %s\n", snap_path, strerror(rd.err));
        rc = NH_SYNCD_ERR_IO;
    }
    goto done;
fail:
    rc = NH_SYNCD_ERR_IO;
done:
    if (fd >= 0) close(fd);
    free(snap_path);
    if (rc != NH_SYNCD_OK) {
        /* *out stays NULL: a failed load must never look like an empty
         * home, or the next save would overwrite the unreadable file. */
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        return rc;
    }

    nh_syncd_state *s = *out;
    /* Also parse remote.json if present (I2 will produce it). */
    char *remote_path = joinp(state_dir, "remote.json");
    if (remote_path) {
        char *rbuf = NULL; size_t rlen = 0;
        int rr = read_file_bytes(remote_path, &rbuf, &rlen);
        if (rr == 0 && rbuf) {
            json_error_t re;
            json_t *rj = json_loadb(rbuf, rlen, 0, &re);
            if (rj && json_is_object(rj)) {
                json_t *rg = json_object_get(rj, "remote_generation");
                if (json_is_integer(rg))
                    s->remote_generation = (uint64_t)json_integer_value(rg);
            }
            if (rj) json_decref(rj);
        }
        free(rbuf);
        free(remote_path);
    }
    return NH_SYNCD_OK;
}

int nh_syncd_state_quarantine_snapshot(const char *state_dir, char **out_path) {
    if (out_path) *out_path = NULL;
    if (!state_dir) return NH_SYNCD_ERR_ARG;
    char *src = joinp(state_dir, "snapshot.json");
    if (!src) return NH_SYNCD_ERR_OOM;
    long long now = (long long)time(NULL);
    /* Never clobber an earlier quarantine: the caller holds sync.lock,
     * so check-then-rename cannot race another daemon. */
    for (unsigned i = 0; i < 1000; i++) {
        char *dst = NULL;
        int n = i == 0
            ? asprintf(&dst, "%s/snapshot.json.corrupt.%lld", state_dir, now)
            : asprintf(&dst, "%s/snapshot.json.corrupt.%lld.%u", state_dir, now, i);
        if (n < 0) { free(src); return NH_SYNCD_ERR_OOM; }
        struct stat st;
        if (lstat(dst, &st) == 0) { free(dst); continue; }
        if (errno != ENOENT || rename(src, dst) < 0) {
            fprintf(stderr, "syncd: cannot move %s aside to %s: %s\n",
                    src, dst, strerror(errno));
            free(dst); free(src);
            return NH_SYNCD_ERR_IO;
        }
        free(src);
        if (out_path) *out_path = dst; else free(dst);
        return NH_SYNCD_OK;
    }
    free(src);
    return NH_SYNCD_ERR_IO;
}

/* ─── entry accessors and updates ─────────────────────────────────── */

const nh_syncd_entry *nh_syncd_state_find(const nh_syncd_state *s, const char *rel) {
    /* Fast-path: we return a materialized snapshot copy, cached on a
     * tiny arena so the pointer's lifetime matches the state's next
     * call to _find. Callers are expected to consume the pointer
     * immediately; the daemon's push path never holds two entries at
     * once. */
    static _Thread_local nh_syncd_entry g_entry;
    static _Thread_local char           g_hash[65];
    if (!s || !rel || !s->files) return NULL;
    json_t *row = json_object_get(s->files, rel);
    if (!json_is_object(row)) return NULL;

    memset(&g_entry, 0, sizeof g_entry);
    g_entry.row_ref = row; /* borrowed for the current lookup */
    json_t *jk = json_object_get(row, "kind");
    json_t *jm = json_object_get(row, "mode");
    json_t *ju = json_object_get(row, "uid");
    json_t *jg = json_object_get(row, "gid");
    json_t *jmt= json_object_get(row, "mtime_ns");
    json_t *jsz= json_object_get(row, "size");
    json_t *jh = json_object_get(row, "content_hash_hex");
    if (!json_is_string(jk)) return NULL;
    g_entry.kind = str_to_kind(json_string_value(jk));
    g_entry.mode = json_is_integer(jm) ? (uint32_t)json_integer_value(jm) : 0;
    g_entry.uid  = json_is_integer(ju) ? (uint32_t)json_integer_value(ju) : 0;
    g_entry.gid  = json_is_integer(jg) ? (uint32_t)json_integer_value(jg) : 0;
    g_entry.mtime_ns = json_is_integer(jmt) ? (uint64_t)json_integer_value(jmt) : 0;
    g_entry.size = json_is_integer(jsz) ? (uint64_t)json_integer_value(jsz) : 0;
    if (json_is_string(jh)) {
        strncpy(g_hash, json_string_value(jh), sizeof g_hash - 1);
        g_hash[sizeof g_hash - 1] = '\0';
    } else g_hash[0] = '\0';
    /* Publish through a static so accessors don't fabricate pointers. */
    /* nostrc-cvqe: null-term guaranteed — snprintf caps at buf-1 and NULs */
    snprintf(g_entry.content_hash_hex, sizeof g_entry.content_hash_hex,
             "%s", g_hash);
    return &g_entry;
}

nh_syncd_kind nh_syncd_entry_kind(const nh_syncd_entry *e)     { return e ? e->kind : 0; }
uint64_t nh_syncd_entry_size(const nh_syncd_entry *e)          { return e ? e->size : 0; }
uint64_t nh_syncd_entry_mtime_ns(const nh_syncd_entry *e)      { return e ? e->mtime_ns : 0; }
const char *nh_syncd_entry_content_hash_hex(const nh_syncd_entry *e) {
    return e ? e->content_hash_hex : "";
}
uint32_t nh_syncd_entry_mode(const nh_syncd_entry *e) { return e ? e->mode : 0; }
uint32_t nh_syncd_entry_uid(const nh_syncd_entry *e)  { return e ? e->uid  : 0; }
uint32_t nh_syncd_entry_gid(const nh_syncd_entry *e)  { return e ? e->gid  : 0; }

/* ────────────────────────────────────────────────────────────────────
 * Additive getters for external readers (Phase 4 P4-I, bead nostrc-1u55).
 *
 * Chunk / symlink accessors read the jansson row stashed on the tls
 * entry by the most recent _find / _at call. Callers must consume
 * these accessors on the entry they just obtained, before any
 * subsequent state call on the same thread — this matches the
 * lifetime rule the header documents.
 * ──────────────────────────────────────────────────────────────────── */

static _Thread_local char g_chunk_scratch[65];
static _Thread_local char g_symlink_scratch[4096];

size_t nh_syncd_entry_chunk_count(const nh_syncd_entry *e) {
    if (!e || !e->row_ref) return 0;
    json_t *arr = json_object_get(e->row_ref, "chunk_addrs_hex");
    if (!json_is_array(arr)) return 0;
    return (size_t)json_array_size(arr);
}

const char *nh_syncd_entry_chunk_at(const nh_syncd_entry *e, size_t i) {
    if (!e || !e->row_ref) return NULL;
    json_t *arr = json_object_get(e->row_ref, "chunk_addrs_hex");
    if (!json_is_array(arr) || i >= json_array_size(arr)) return NULL;
    const char *v = json_string_value(json_array_get(arr, i));
    if (!v) return NULL;
    strncpy(g_chunk_scratch, v, sizeof g_chunk_scratch - 1);
    g_chunk_scratch[sizeof g_chunk_scratch - 1] = '\0';
    return g_chunk_scratch;
}

const char *nh_syncd_entry_symlink_target(const nh_syncd_entry *e) {
    if (!e || !e->row_ref) return "";
    const char *v = json_string_value(json_object_get(e->row_ref, "symlink_target"));
    if (!v) return "";
    strncpy(g_symlink_scratch, v, sizeof g_symlink_scratch - 1);
    g_symlink_scratch[sizeof g_symlink_scratch - 1] = '\0';
    return g_symlink_scratch;
}

const nh_syncd_entry *nh_syncd_state_at(const nh_syncd_state *s, size_t i,
                                        const char **out_rel_path) {
    if (!s || !s->files) return NULL;
    size_t n = (size_t)json_object_size(s->files);
    if (i >= n) return NULL;
    size_t k = 0;
    const char *key = NULL;
    json_t *val = NULL;
    json_object_foreach(s->files, key, val) {
        if (k == i) {
            if (out_rel_path) *out_rel_path = key;
            return nh_syncd_state_find(s, key);
        }
        k++;
    }
    return NULL;
}

/* Internal helper used by the pusher. Set or replace a file entry. */
int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                const char *rel,
                                uint32_t mode, uint32_t uid, uint32_t gid,
                                uint64_t mtime_ns, uint64_t size,
                                const char *content_hash_hex,
                                const char *const *chunk_addrs_hex,
                                size_t chunk_addrs_n);
int nh_syncd_state_upsert_dir_(nh_syncd_state *s,
                               const char *rel,
                               uint32_t mode, uint32_t uid, uint32_t gid,
                               uint64_t mtime_ns);
int nh_syncd_state_upsert_symlink_(nh_syncd_state *s,
                                   const char *rel,
                                   uint32_t mode, uint32_t uid, uint32_t gid,
                                   uint64_t mtime_ns,
                                   const char *target);
int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel);
void nh_syncd_state_bump_generation_(nh_syncd_state *s);
/* Iterate every file entry, invoking cb; iteration order is jansson's
 * insertion order (which we use to rebuild the manifest deterministically
 * per push). */
typedef int (*nh_syncd_state_iter_fn)(void *ud, const char *rel, json_t *row);
int nh_syncd_state_iter_(const nh_syncd_state *s,
                         nh_syncd_state_iter_fn cb, void *ud);

int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                const char *rel,
                                uint32_t mode, uint32_t uid, uint32_t gid,
                                uint64_t mtime_ns, uint64_t size,
                                const char *content_hash_hex,
                                const char *const *chunk_addrs_hex,
                                size_t chunk_addrs_n)
{
    if (!s || !rel) return NH_SYNCD_ERR_ARG;
    json_t *row = json_object();
    if (!row) return NH_SYNCD_ERR_OOM;
    json_object_set_new(row, "kind", json_string("file"));
    json_object_set_new(row, "mode", json_integer(mode));
    json_object_set_new(row, "uid",  json_integer(uid));
    json_object_set_new(row, "gid",  json_integer(gid));
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mtime_ns));
    json_object_set_new(row, "size", json_integer((json_int_t)size));
    json_object_set_new(row, "content_hash_hex",
                        json_string(content_hash_hex ? content_hash_hex : ""));
    json_t *arr = json_array();
    for (size_t i = 0; i < chunk_addrs_n; i++)
        json_array_append_new(arr, json_string(chunk_addrs_hex[i]));
    json_object_set_new(row, "chunk_addrs_hex", arr);
    int rc = json_object_set_new(s->files, rel, row);
    return rc == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_OOM;
}

int nh_syncd_state_upsert_dir_(nh_syncd_state *s,
                               const char *rel,
                               uint32_t mode, uint32_t uid, uint32_t gid,
                               uint64_t mtime_ns)
{
    if (!s || !rel) return NH_SYNCD_ERR_ARG;
    json_t *row = json_object();
    if (!row) return NH_SYNCD_ERR_OOM;
    json_object_set_new(row, "kind", json_string("dir"));
    json_object_set_new(row, "mode", json_integer(mode));
    json_object_set_new(row, "uid",  json_integer(uid));
    json_object_set_new(row, "gid",  json_integer(gid));
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mtime_ns));
    json_object_set_new(row, "size", json_integer(0));
    json_object_set_new(row, "content_hash_hex", json_string(""));
    int rc = json_object_set_new(s->files, rel, row);
    return rc == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_OOM;
}

int nh_syncd_state_upsert_symlink_(nh_syncd_state *s,
                                   const char *rel,
                                   uint32_t mode, uint32_t uid, uint32_t gid,
                                   uint64_t mtime_ns,
                                   const char *target)
{
    if (!s || !rel || !target) return NH_SYNCD_ERR_ARG;
    json_t *row = json_object();
    if (!row) return NH_SYNCD_ERR_OOM;
    json_object_set_new(row, "kind", json_string("symlink"));
    json_object_set_new(row, "mode", json_integer(mode));
    json_object_set_new(row, "uid",  json_integer(uid));
    json_object_set_new(row, "gid",  json_integer(gid));
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mtime_ns));
    json_object_set_new(row, "size", json_integer(0));
    json_object_set_new(row, "content_hash_hex", json_string(""));
    json_object_set_new(row, "symlink_target", json_string(target));
    int rc = json_object_set_new(s->files, rel, row);
    return rc == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_OOM;
}

int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel) {
    if (!s || !rel) return NH_SYNCD_ERR_ARG;
    json_object_del(s->files, rel);
    return NH_SYNCD_OK;
}

void nh_syncd_state_bump_generation_(nh_syncd_state *s) {
    if (s) s->local_generation++;
}

int nh_syncd_state_iter_(const nh_syncd_state *s,
                         nh_syncd_state_iter_fn cb, void *ud)
{
    if (!s || !cb) return NH_SYNCD_ERR_ARG;
    const char *key; json_t *val;
    json_object_foreach(s->files, key, val) {
        int rc = cb(ud, key, val);
        if (rc != 0) return rc;
    }
    return NH_SYNCD_OK;
}

/* Not in the public header — used by the pusher (persist remote gen). */
int nh_syncd_state_save_remote_(const nh_syncd_state *s, const char *state_dir) {
    if (!s || !state_dir) return NH_SYNCD_ERR_ARG;
    json_t *j = json_object();
    if (!j) return NH_SYNCD_ERR_OOM;
    json_object_set_new(j, "remote_generation",
                        json_integer((json_int_t)s->remote_generation));
    char *d = json_dumps(j, JSON_INDENT(2));
    json_decref(j);
    if (!d) return NH_SYNCD_ERR_JSON;
    int rc = write_atomic(state_dir, "remote.json", d, strlen(d));
    free(d);
    return rc;
}

/* Not in the public header — used by the pull-path reconciler (I2). Copy
 * out the recorded chunk_addrs_hex for `rel` as a heap array of heap
 * strings, plus its length. On absent / non-file rows, *out_n = 0 and
 * *out is NULL. Caller frees each string then the array. */
int nh_syncd_state_get_chunk_addrs_(const nh_syncd_state *s,
                                    const char *rel,
                                    char ***out,
                                    size_t *out_n)
{
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!s || !rel || !s->files) return NH_SYNCD_ERR_ARG;
    json_t *row = json_object_get(s->files, rel);
    if (!json_is_object(row)) return NH_SYNCD_OK;
    json_t *arr = json_object_get(row, "chunk_addrs_hex");
    if (!json_is_array(arr)) return NH_SYNCD_OK;
    size_t n = json_array_size(arr);
    if (n == 0) return NH_SYNCD_OK;
    char **a = calloc(n, sizeof *a);
    if (!a) return NH_SYNCD_ERR_OOM;
    for (size_t i = 0; i < n; i++) {
        const char *v = json_string_value(json_array_get(arr, i));
        a[i] = strdup(v ? v : "");
        if (!a[i]) {
            for (size_t k = 0; k < i; k++) free(a[k]);
            free(a);
            return NH_SYNCD_ERR_OOM;
        }
    }
    if (out) *out = a; else {
        for (size_t i = 0; i < n; i++) free(a[i]);
        free(a);
    }
    if (out_n) *out_n = n;
    return NH_SYNCD_OK;
}
