/*
 * nh_syncd_state.c — persist/restore state under ~/.local/state/nostr-homed/
 *
 * SPDX-License-Identifier: MIT
 * See nh_syncd.h for the on-disk schema. Persistence is atomic:
 * every write goes to a `.tmp` sibling then rename(2) into place.
 */

#include "nh_syncd.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static int write_atomic(const char *dir, const char *name,
                        const void *data, size_t len) {
    char *tmp = malloc(strlen(dir) + 1 + strlen(name) + 5);
    char *fin = joinp(dir, name);
    if (!tmp || !fin) { free(tmp); free(fin); return NH_SYNCD_ERR_OOM; }
    sprintf(tmp, "%s/%s.tmp", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    const char *p = data; size_t rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
        rem -= (size_t)w; p += w;
    }
    if (fsync(fd) < 0) { close(fd); unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    if (close(fd) < 0) { unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    if (rename(tmp, fin) < 0) { unlink(tmp); free(tmp); free(fin); return NH_SYNCD_ERR_IO; }
    free(tmp); free(fin);
    return NH_SYNCD_OK;
}

static int read_file_bytes(const char *path, char **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? -2 : NH_SYNCD_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NH_SYNCD_ERR_IO; }
    /* Hard cap so a bad file cannot exhaust memory. 32 MiB is way more
     * than any conceivable snapshot.json. */
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

    char *dump = json_dumps(j, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(j);
    if (!dump) return NH_SYNCD_ERR_JSON;
    rc = write_atomic(state_dir, "snapshot.json", dump, strlen(dump));
    free(dump);
    if (rc < 0) return rc;

    /* Also write the standalone generation file for external observers. */
    char genbuf[32];
    int n = snprintf(genbuf, sizeof genbuf, "%llu\n",
                     (unsigned long long)s->local_generation);
    if (n < 0 || n >= (int)sizeof genbuf) return NH_SYNCD_ERR_IO;
    return write_atomic(state_dir, "generation", genbuf, (size_t)n);
}

/* ─── load ─────────────────────────────────────────────────────────── */

int nh_syncd_state_load(const char *state_dir,
                        nh_syncd_state **out,
                        bool *out_snapshot_unknown)
{
    if (!state_dir || !out) return NH_SYNCD_ERR_ARG;
    if (out_snapshot_unknown) *out_snapshot_unknown = false;
    *out = NULL;

    char *snap_path = joinp(state_dir, "snapshot.json");
    if (!snap_path) return NH_SYNCD_ERR_OOM;
    char *buf = NULL; size_t buflen = 0;
    int rc = read_file_bytes(snap_path, &buf, &buflen);
    free(snap_path);
    if (rc == -2) {
        /* Missing — return a fresh empty state. Caller (push closure)
         * still refuses to run until snapshot base is known via
         * NH_SYNCD_ERR_SNAPSHOT_UNKNOWN; init happens through
         * nh_syncd_state_new. */
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        nh_syncd_state *s = calloc(1, sizeof *s);
        if (!s) return NH_SYNCD_ERR_OOM;
        s->files = json_object();
        s->root = strdup("");
        s->d_tag = strdup("");
        s->account_pubkey_hex = strdup("");
        if (!s->files || !s->root || !s->d_tag || !s->account_pubkey_hex) {
            nh_syncd_state_free(s);
            return NH_SYNCD_ERR_OOM;
        }
        *out = s;
        return NH_SYNCD_OK;
    }
    if (rc < 0) return rc;

    json_error_t je;
    json_t *j = json_loadb(buf, buflen, 0, &je);
    free(buf);
    if (!j) {
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        return NH_SYNCD_ERR_JSON;
    }
    if (!json_is_object(j)) { json_decref(j); if (out_snapshot_unknown) *out_snapshot_unknown = true; return NH_SYNCD_ERR_JSON; }

    json_t *schema = json_object_get(j, "schema");
    if (!json_is_integer(schema) || json_integer_value(schema) != 1) {
        json_decref(j);
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        return NH_SYNCD_ERR_JSON;
    }
    json_t *jgen  = json_object_get(j, "generation");
    json_t *jroot = json_object_get(j, "root");
    json_t *jdtag = json_object_get(j, "d_tag");
    json_t *jpk   = json_object_get(j, "account_pubkey_hex");
    json_t *jrid  = json_object_get(j, "root_id_hex");
    json_t *jfiles= json_object_get(j, "files");

    if (!json_is_integer(jgen) || !json_is_string(jroot) ||
        !json_is_string(jdtag) || !json_is_string(jpk) ||
        !json_is_string(jrid) || !json_is_object(jfiles)) {
        json_decref(j);
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        return NH_SYNCD_ERR_JSON;
    }
    if (!hex64_ok(json_string_value(jrid))) {
        json_decref(j);
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
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
        if (out_snapshot_unknown) *out_snapshot_unknown = true;
        return NH_SYNCD_ERR_JSON;
    }
    s->files = json_incref(jfiles);
    json_decref(j);
    if (!s->root || !s->d_tag || !s->account_pubkey_hex || !s->files) {
        nh_syncd_state_free(s);
        return NH_SYNCD_ERR_OOM;
    }

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

    *out = s;
    return NH_SYNCD_OK;
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
    g_entry.content_hash_hex[0] = '\0';
    strncpy(g_entry.content_hash_hex, g_hash, sizeof g_entry.content_hash_hex - 1);
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
