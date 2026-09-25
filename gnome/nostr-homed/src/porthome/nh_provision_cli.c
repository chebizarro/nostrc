/*
 * nh_provision_cli.c — helpers for nostr-homed-provision (bead 5fdu/89rj).
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Every function here is either pure (JSON codec) or filesystem-only
 * (dry-run walk). Nothing here opens a socket. Nothing here derives
 * secrets on its own — the CLI main file owns the sensitive
 * generation/derivation path via nh_porthome_wrapkey / nh_porthome_key_derive.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nh_provision_cli.h"

#include "nh_porthome_crypto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════
 * Tiny JSON emitter — hand-rolled so this file has NO jansson dep.
 * ═════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    err;
} sb_t;

static void sb_reserve(sb_t *s, size_t extra) {
    if (s->err) return;
    if (s->len + extra + 1 <= s->cap) return;
    size_t nc = s->cap ? s->cap * 2 : 256;
    while (nc < s->len + extra + 1) nc *= 2;
    char *nb = realloc(s->buf, nc);
    if (!nb) { s->err = 1; return; }
    s->buf = nb;
    s->cap = nc;
}

static void sb_append(sb_t *s, const char *p, size_t n) {
    sb_reserve(s, n);
    if (s->err) return;
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sb_str(sb_t *s, const char *p) { sb_append(s, p, strlen(p)); }

static void sb_json_string(sb_t *s, const char *in) {
    sb_append(s, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\'; esc[1] = (char)*p; sb_append(s, esc, 2);
        } else if (*p == '\n') { sb_append(s, "\\n", 2); }
          else if (*p == '\r') { sb_append(s, "\\r", 2); }
          else if (*p == '\t') { sb_append(s, "\\t", 2); }
          else if (*p < 0x20)  {
              int n = snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
              if (n > 0) sb_append(s, esc, (size_t)n);
          }
          else sb_append(s, (const char *)p, 1);
    }
    sb_append(s, "\"", 1);
}

static void sb_u64(sb_t *s, uint64_t v) {
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    if (n > 0) sb_append(s, buf, (size_t)n);
}

/* ═══════════════════════════════════════════════════════════════════
 * Tiny JSON scanner (bounded; refuses nesting > 3, arrays only of
 * strings, no floats). Not a full parser — just enough to read the
 * shape we emit.
 * ═════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *p;
    const char *end;
    int         err;
} sc_t;

static void sc_skip_ws(sc_t *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' ||
                             *s->p == '\n' || *s->p == '\r')) s->p++;
}

static int sc_expect(sc_t *s, char c) {
    sc_skip_ws(s);
    if (s->p >= s->end || *s->p != c) { s->err = 1; return -1; }
    s->p++;
    return 0;
}

/* Read a JSON string into `out` (cap includes NUL). On success returns
 * 0 and NUL-terminates. Refuses \u escapes for simplicity (our writer
 * never emits them for the field bodies we care about). */
static int sc_string(sc_t *s, char *out, size_t cap) {
    sc_skip_ws(s);
    if (s->p >= s->end || *s->p != '"') { s->err = 1; return -1; }
    s->p++;
    size_t o = 0;
    while (s->p < s->end && *s->p != '"') {
        if (o + 1 >= cap) { s->err = 1; return -1; }
        if (*s->p == '\\') {
            s->p++;
            if (s->p >= s->end) { s->err = 1; return -1; }
            char c = *s->p++;
            switch (c) {
            case '"': case '\\': case '/': out[o++] = c;      break;
            case 'n':                       out[o++] = '\n';   break;
            case 'r':                       out[o++] = '\r';   break;
            case 't':                       out[o++] = '\t';   break;
            default:                        s->err = 1; return -1;
            }
        } else {
            out[o++] = *s->p++;
        }
    }
    if (s->p >= s->end || *s->p != '"') { s->err = 1; return -1; }
    s->p++;
    out[o] = '\0';
    return 0;
}

/* Parse an unsigned integer literal. Returns 0 on success. */
static int sc_u64(sc_t *s, uint64_t *out) {
    sc_skip_ws(s);
    uint64_t v = 0;
    if (s->p >= s->end || !isdigit((unsigned char)*s->p)) { s->err = 1; return -1; }
    while (s->p < s->end && isdigit((unsigned char)*s->p)) {
        v = v * 10ull + (uint64_t)(*s->p - '0');
        s->p++;
    }
    *out = v;
    return 0;
}

/* Parse an array of strings into `arr` (each row cap NH_PROV_MAX_URL_LEN+1).
 * Fills `*out_n` with count. Refuses > NH_PROV_MAX_URLS. */
static int sc_str_array(sc_t *s,
                        char arr[NH_PROV_MAX_URLS][NH_PROV_MAX_URL_LEN + 1],
                        size_t *out_n) {
    if (sc_expect(s, '[') != 0) return -1;
    size_t n = 0;
    sc_skip_ws(s);
    if (s->p < s->end && *s->p == ']') { s->p++; *out_n = 0; return 0; }
    for (;;) {
        if (n >= NH_PROV_MAX_URLS) { s->err = 1; return -1; }
        if (sc_string(s, arr[n], NH_PROV_MAX_URL_LEN + 1) != 0) return -1;
        n++;
        sc_skip_ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        break;
    }
    if (sc_expect(s, ']') != 0) return -1;
    *out_n = n;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * account struct
 * ═════════════════════════════════════════════════════════════════ */

void nh_prov_account_init(nh_prov_account *a) {
    memset(a, 0, sizeof *a);
    snprintf(a->d_tag, sizeof a->d_tag, "%s",
             "nostr-homed.home.v1:personal");
}

char *nh_prov_account_to_json(const nh_prov_account *a, int redact) {
    if (!a) return NULL;
    sb_t s = {0};
    sb_str(&s, "{\n  \"schema\": 1,\n");

    sb_str(&s, "  \"account_pubkey_hex\": ");
    sb_json_string(&s, a->account_pubkey_hex);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"account_nsec_hex\":   ");
    sb_json_string(&s, redact ? "REDACTED" : a->account_nsec_hex);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"wrap_seed_hex\":      ");
    sb_json_string(&s, redact ? "REDACTED" : a->wrap_seed_hex);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"home_key_hex\":       ");
    sb_json_string(&s, redact ? "REDACTED" : a->home_key_hex);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"root_id_hex\":        ");
    sb_json_string(&s, a->root_id_hex);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"d_tag\":              ");
    sb_json_string(&s, a->d_tag);
    sb_str(&s, ",\n");

    sb_str(&s, "  \"home_relays\":        [");
    for (size_t i = 0; i < a->n_home_relays; i++) {
        if (i) sb_str(&s, ", ");
        sb_json_string(&s, a->home_relays[i]);
    }
    sb_str(&s, "],\n");

    sb_str(&s, "  \"blossom_servers\":    [");
    for (size_t i = 0; i < a->n_blossom_servers; i++) {
        if (i) sb_str(&s, ", ");
        sb_json_string(&s, a->blossom_servers[i]);
    }
    sb_str(&s, "],\n");

    sb_str(&s, "  \"generation\":         ");
    sb_u64(&s, a->generation);
    sb_str(&s, "\n}\n");

    if (s.err) { free(s.buf); return NULL; }
    return s.buf;
}

/* Very small key-order-agnostic member scanner: walk the object, dispatch
 * each recognized key. Refuses unknown keys. */
int nh_prov_account_from_json(const char *json, size_t len,
                              nh_prov_account *out) {
    if (!json || !out) return -1;
    nh_prov_account_init(out);
    sc_t s = { json, json + len, 0 };
    if (sc_expect(&s, '{') != 0) return -1;

    int seen_schema = 0;
    for (;;) {
        sc_skip_ws(&s);
        if (s.p < s.end && *s.p == '}') { s.p++; break; }

        char keybuf[64];
        if (sc_string(&s, keybuf, sizeof keybuf) != 0) return -1;
        if (sc_expect(&s, ':') != 0) return -1;

        if (!strcmp(keybuf, "schema")) {
            uint64_t v = 0;
            if (sc_u64(&s, &v) != 0) return -1;
            if (v != 1) return -1;
            seen_schema = 1;
        } else if (!strcmp(keybuf, "account_pubkey_hex")) {
            if (sc_string(&s, out->account_pubkey_hex,
                          sizeof out->account_pubkey_hex) != 0) return -1;
        } else if (!strcmp(keybuf, "account_nsec_hex")) {
            if (sc_string(&s, out->account_nsec_hex,
                          sizeof out->account_nsec_hex) != 0) return -1;
        } else if (!strcmp(keybuf, "wrap_seed_hex")) {
            if (sc_string(&s, out->wrap_seed_hex,
                          sizeof out->wrap_seed_hex) != 0) return -1;
        } else if (!strcmp(keybuf, "home_key_hex")) {
            if (sc_string(&s, out->home_key_hex,
                          sizeof out->home_key_hex) != 0) return -1;
        } else if (!strcmp(keybuf, "root_id_hex")) {
            if (sc_string(&s, out->root_id_hex,
                          sizeof out->root_id_hex) != 0) return -1;
        } else if (!strcmp(keybuf, "d_tag")) {
            if (sc_string(&s, out->d_tag, sizeof out->d_tag) != 0) return -1;
        } else if (!strcmp(keybuf, "home_relays")) {
            if (sc_str_array(&s, out->home_relays, &out->n_home_relays) != 0)
                return -1;
        } else if (!strcmp(keybuf, "blossom_servers")) {
            if (sc_str_array(&s, out->blossom_servers,
                              &out->n_blossom_servers) != 0) return -1;
        } else if (!strcmp(keybuf, "generation")) {
            if (sc_u64(&s, &out->generation) != 0) return -1;
        } else {
            /* Unknown top-level key — strict refuse. */
            return -1;
        }
        sc_skip_ws(&s);
        if (s.p < s.end && *s.p == ',') { s.p++; continue; }
    }
    if (!seen_schema) return -1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * split_csv / free_csv
 * ═════════════════════════════════════════════════════════════════ */

char **nh_prov_split_csv(const char *s, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!s || !*s) return NULL;
    size_t n = 1;
    for (const char *p = s; *p; p++) if (*p == ',') n++;
    char **arr = calloc(n + 1, sizeof *arr);
    if (!arr) return NULL;
    size_t i = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == ',' || *p == '\0') {
            size_t len = (size_t)(p - start);
            /* Trim */
            while (len > 0 && (*start == ' ' || *start == '\t')) { start++; len--; }
            while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
            if (len > 0) {
                char *item = malloc(len + 1);
                if (!item) { for (size_t j = 0; j < i; j++) free(arr[j]); free(arr); return NULL; }
                memcpy(item, start, len); item[len] = '\0';
                arr[i++] = item;
            }
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    arr[i] = NULL;
    if (out_n) *out_n = i;
    return arr;
}

void nh_prov_free_csv(char **arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* ═══════════════════════════════════════════════════════════════════
 * file utilities
 * ═════════════════════════════════════════════════════════════════ */

int nh_prov_slurp_file(const char *path, size_t cap,
                       char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); return -e; }
    if ((size_t)st.st_size > cap) { close(fd); return -E2BIG; }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) { close(fd); return -ENOMEM; }
    size_t off = 0;
    while (off < sz) {
        ssize_t n = read(fd, buf + off, sz - off);
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -errno; }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = '\0';
    if (out)     *out     = buf; else free(buf);
    if (out_len) *out_len = off;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * push --dry-run: walk tree, count chunks, print manifest plan
 * ═════════════════════════════════════════════════════════════════ */

/* Recursive walk. Bounded depth (32) to match syncd's rescan cap. */
static int walk_(const char *root_abs, const char *rel,
                 int depth, uint64_t chunk_size,
                 FILE *out,
                 uint64_t *tot_bytes, uint64_t *tot_files,
                 uint64_t *tot_chunks) {
    if (depth > 32) return 0; /* silently cap */
    char abs[4096];
    if (rel && *rel)
        snprintf(abs, sizeof abs, "%s/%s", root_abs, rel);
    else
        snprintf(abs, sizeof abs, "%s", root_abs);

    DIR *d = opendir(abs);
    if (!d) return -errno;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        /* Skip common state / cache dirs. */
        if (!strcmp(de->d_name, ".local") ||
            !strcmp(de->d_name, ".cache") ||
            !strcmp(de->d_name, ".git")) continue;

        char child_rel[4096];
        int nr;
        if (rel && *rel)
            nr = snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        else
            nr = snprintf(child_rel, sizeof child_rel, "%s", de->d_name);
        if (nr < 0 || (size_t)nr >= sizeof child_rel) continue; /* path too long */
        char child_abs[4096];
        int na = snprintf(child_abs, sizeof child_abs, "%s/%s", abs, de->d_name);
        if (na < 0 || (size_t)na >= sizeof child_abs) continue; /* path too long */

        struct stat st;
        if (lstat(child_abs, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            fprintf(out, "{\"kind\":\"dir\",\"path\":\"%s\",\"mode\":%u}\n",
                    child_rel, (unsigned)(st.st_mode & 0777));
            int r = walk_(root_abs, child_rel, depth + 1, chunk_size,
                          out, tot_bytes, tot_files, tot_chunks);
            if (r != 0) { closedir(d); return r; }
        } else if (S_ISREG(st.st_mode)) {
            uint64_t sz = (uint64_t)st.st_size;
            uint64_t nc = sz ? (sz + chunk_size - 1) / chunk_size : 0;
            fprintf(out,
                "{\"kind\":\"file\",\"path\":\"%s\",\"mode\":%u,"
                "\"size\":%llu,\"chunks\":%llu}\n",
                child_rel, (unsigned)(st.st_mode & 0777),
                (unsigned long long)sz, (unsigned long long)nc);
            *tot_bytes += sz;
            *tot_files += 1;
            *tot_chunks += nc;
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[4096];
            ssize_t tn = readlinkat(AT_FDCWD, child_abs, tgt, sizeof tgt - 1);
            if (tn < 0) tn = 0;
            tgt[tn] = '\0';
            fprintf(out,
                "{\"kind\":\"symlink\",\"path\":\"%s\",\"target\":\"%s\"}\n",
                child_rel, tgt);
        }
    }
    closedir(d);
    return 0;
}

int nh_prov_push_dry_run(const char *home_abs, uint64_t chunk_size,
                         FILE *out, uint64_t *out_total_bytes,
                         uint64_t *out_total_files,
                         uint64_t *out_total_chunks) {
    if (!home_abs || !out) return -EINVAL;
    if (chunk_size == 0) chunk_size = 4u * 1024u * 1024u;
    uint64_t b = 0, f = 0, c = 0;
    int r = walk_(home_abs, "", 0, chunk_size, out, &b, &f, &c);
    if (out_total_bytes)  *out_total_bytes  = b;
    if (out_total_files)  *out_total_files  = f;
    if (out_total_chunks) *out_total_chunks = c;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * fetch_ctl serializer
 * ═════════════════════════════════════════════════════════════════ */

char *nh_prov_render_fetch_ctl(const nh_prov_account *a,
                               uint32_t relay_timeout_ms,
                               uint64_t bandwidth_cap_bytes,
                               uint32_t per_file_timeout_sec,
                               uint64_t max_total_bytes,
                               int allow_insecure,
                               size_t *out_len) {
    if (!a) return NULL;
    sb_t s = {0};
    sb_str(&s, "{");
    sb_str(&s, "\"account_pubkey_hex\":");
    sb_json_string(&s, a->account_pubkey_hex);
    sb_str(&s, ",\"home_root_id_hex\":");
    sb_json_string(&s, a->root_id_hex);
    sb_str(&s, ",\"home_key_hex\":");
    sb_json_string(&s, a->home_key_hex);
    sb_str(&s, ",\"d_tag\":");
    sb_json_string(&s, a->d_tag);
    sb_str(&s, ",\"relays\":[");
    for (size_t i = 0; i < a->n_home_relays; i++) {
        if (i) sb_str(&s, ",");
        sb_json_string(&s, a->home_relays[i]);
    }
    sb_str(&s, "],\"blossom_servers\":[");
    for (size_t i = 0; i < a->n_blossom_servers; i++) {
        if (i) sb_str(&s, ",");
        sb_json_string(&s, a->blossom_servers[i]);
    }
    sb_str(&s, "],\"bandwidth_cap_bytes\":");
    sb_u64(&s, bandwidth_cap_bytes);
    sb_str(&s, ",\"per_file_timeout_sec\":");
    sb_u64(&s, (uint64_t)per_file_timeout_sec);
    sb_str(&s, ",\"max_total_bytes\":");
    sb_u64(&s, max_total_bytes);
    sb_str(&s, ",\"relay_timeout_ms\":");
    sb_u64(&s, (uint64_t)relay_timeout_ms);
    sb_str(&s, ",\"allow_insecure\":");
    sb_str(&s, allow_insecure ? "true" : "false");
    sb_str(&s, "}");
    if (s.err) { free(s.buf); return NULL; }
    if (out_len) *out_len = s.len;
    return s.buf;
}

/* ═══════════════════════════════════════════════════════════════════
 * Real-push helpers (walk, chunk-size normalise, min-replication check,
 * atomic account/pinned.json writers)
 * ═════════════════════════════════════════════════════════════════ */

uint64_t nh_prov_normalize_chunk_size(uint64_t requested) {
    const uint64_t DEFAULT_CHUNK = 4u * 1024u * 1024u;
    const uint64_t MIN_CHUNK     = 64u * 1024u;
    const uint64_t MAX_CHUNK     = 8u * 1024u * 1024u;
    if (requested == 0) return DEFAULT_CHUNK;
    if (requested < MIN_CHUNK) return MIN_CHUNK;
    if (requested > MAX_CHUNK) return MAX_CHUNK;
    return requested;
}

static int is_hex64_local(const char *s) {
    if (!s) return 0;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return s[64] == '\0';
}

static int walk_entries_(const char *root_abs, const char *rel, int depth,
                         FILE *warn,
                         nh_prov_walk_entry **arr, size_t *n, size_t *cap,
                         size_t *skipped) {
    if (depth > 32) return 0;
    char abs[4096];
    int na;
    if (rel && *rel)
        na = snprintf(abs, sizeof abs, "%s/%s", root_abs, rel);
    else
        na = snprintf(abs, sizeof abs, "%s", root_abs);
    if (na < 0 || (size_t)na >= sizeof abs) return -ENAMETOOLONG;

    DIR *d = opendir(abs);
    if (!d) return -errno;

    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        /* Skip local caches and version-control state at every level —
         * these are per-machine and would only bloat the push. */
        if (!strcmp(de->d_name, ".local") ||
            !strcmp(de->d_name, ".cache") ||
            !strcmp(de->d_name, ".git")) continue;

        char child_rel[4096];
        int nr;
        if (rel && *rel)
            nr = snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        else
            nr = snprintf(child_rel, sizeof child_rel, "%s", de->d_name);
        if (nr < 0 || (size_t)nr >= sizeof child_rel) continue;

        char child_abs[4096];
        int ca = snprintf(child_abs, sizeof child_abs, "%s/%s", abs, de->d_name);
        if (ca < 0 || (size_t)ca >= sizeof child_abs) continue;

        struct stat st;
        if (lstat(child_abs, &st) != 0) continue;

        /* Refuse world-writable + setuid/setgid — the manifest applier
         * would strip those bits anyway; skip loudly so the operator
         * knows this file will not round-trip verbatim. Symlinks are
         * exempt: their inode permissions are meaningless (POSIX
         * ignores them for access; the target's mode is what counts). */
        if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
            if ((st.st_mode & S_IWOTH) ||
                (st.st_mode & S_ISUID) ||
                (st.st_mode & S_ISGID)) {
                if (warn)
                    fprintf(warn,
                        "warning: skipping %s (mode=0%o has world-writable or setuid/setgid)\n",
                        child_rel, (unsigned)(st.st_mode & 07777));
                if (skipped) (*skipped)++;
                continue;
            }
        }

        nh_prov_walk_entry e;
        memset(&e, 0, sizeof e);
        e.rel_path = strdup(child_rel);
        if (!e.rel_path) { rc = -ENOMEM; break; }
        e.mode     = (uint32_t)(st.st_mode & 0777);
        e.uid_hint = (uint32_t)st.st_uid;
        e.gid_hint = (uint32_t)st.st_gid;
        e.mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull
                   + (uint64_t)st.st_mtim.tv_nsec;

        if (S_ISDIR(st.st_mode)) {
            e.kind = NH_PROV_WALK_KIND_DIR;
            e.size = 0;
        } else if (S_ISREG(st.st_mode)) {
            e.kind = NH_PROV_WALK_KIND_FILE;
            e.size = (uint64_t)st.st_size;
        } else if (S_ISLNK(st.st_mode)) {
            e.kind = NH_PROV_WALK_KIND_SYMLINK;
            char tgt[4096];
            ssize_t tn = readlink(child_abs, tgt, sizeof tgt - 1);
            if (tn < 0) tn = 0;
            tgt[tn] = 0;
            e.symlink_target = strdup(tgt);
            if (!e.symlink_target) {
                free(e.rel_path);
                rc = -ENOMEM; break;
            }
            e.size = 0;
        } else {
            /* Device / fifo / socket: refused (matches copy_tree policy). */
            if (warn)
                fprintf(warn, "warning: skipping %s (special file)\n", child_rel);
            if (skipped) (*skipped)++;
            free(e.rel_path);
            continue;
        }

        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 32;
            nh_prov_walk_entry *nb = realloc(*arr, nc * sizeof(**arr));
            if (!nb) {
                free(e.rel_path); free(e.symlink_target);
                rc = -ENOMEM; break;
            }
            *arr = nb; *cap = nc;
        }
        (*arr)[(*n)++] = e;

        if (S_ISDIR(st.st_mode)) {
            rc = walk_entries_(root_abs, child_rel, depth + 1,
                               warn, arr, n, cap, skipped);
            if (rc != 0) break;
        }
    }
    closedir(d);
    return rc;
}

int nh_prov_walk_home(const char *home_abs,
                      FILE *warn,
                      nh_prov_walk_entry **out_entries,
                      size_t *out_n,
                      size_t *out_skipped) {
    if (!home_abs || !out_entries || !out_n) return -EINVAL;
    nh_prov_walk_entry *arr = NULL;
    size_t n = 0, cap = 0, skipped = 0;
    int rc = walk_entries_(home_abs, "", 0, warn, &arr, &n, &cap, &skipped);
    if (rc != 0) {
        nh_prov_free_walk(arr, n);
        *out_entries = NULL; *out_n = 0;
        if (out_skipped) *out_skipped = 0;
        return rc;
    }
    *out_entries = arr;
    *out_n = n;
    if (out_skipped) *out_skipped = skipped;
    return 0;
}

void nh_prov_free_walk(nh_prov_walk_entry *entries, size_t n) {
    if (!entries) return;
    for (size_t i = 0; i < n; i++) {
        free(entries[i].rel_path);
        free(entries[i].symlink_target);
    }
    free(entries);
}

size_t nh_prov_count_full_replicas(const uint32_t *chunks_uploaded,
                                   const uint32_t *chunks_failed,
                                   size_t n_servers,
                                   uint64_t n_chunks) {
    if (!chunks_uploaded || !chunks_failed || n_servers == 0) return 0;
    size_t r = 0;
    for (size_t si = 0; si < n_servers; si++) {
        if ((uint64_t)chunks_uploaded[si] == n_chunks &&
            chunks_failed[si] == 0) r++;
    }
    return r;
}

/* Small helper: atomically rewrite `path` with `body` (len bytes), mode 0600. */
static int atomic_write_0600(const char *path, const char *body, size_t len) {
    char tmp[1200];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) return -ENAMETOOLONG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, body + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno; close(fd); unlink(tmp); return -e;
        }
        off += (size_t)w;
    }
    if (fchmod(fd, 0600) != 0) { int e = errno; close(fd); unlink(tmp); return -e; }
    if (fsync(fd) != 0)        { int e = errno; close(fd); unlink(tmp); return -e; }
    close(fd);
    if (rename(tmp, path) != 0) { int e = errno; unlink(tmp); return -e; }
    return 0;
}

int nh_prov_account_write_file(const char *path, const nh_prov_account *a) {
    if (!path || !a) return -EINVAL;
    char *body = nh_prov_account_to_json(a, 0);
    if (!body) return -ENOMEM;
    int rc = atomic_write_0600(path, body, strlen(body));
    /* body carries secrets — wipe before free. */
    size_t bl = strlen(body);
    volatile char *p = (volatile char *)body;
    for (size_t i = 0; i < bl; i++) p[i] = 0;
    free(body);
    return rc;
}

/* ── pinned.json (schema:1, capacity, generations:[{gen,hashes}]) ──
 * Hand-rolled writer so this file stays jansson-free — matches the
 * shape nh_syncd_pin_ring reads. Load-modify-write is intentionally
 * limited: on any parse failure we start a fresh ring at the current
 * gen so a corrupted local file cannot block a push. */

/* Extremely small scanner: pull out generations array as-is (we do not
 * need to re-parse the hashes; we only need to keep the last (cap-1)
 * gens if this generation is not already at the tail). */
static int pinned_load_(const char *path,
                        uint64_t **out_gens, size_t *out_n,
                        size_t max_slots) {
    *out_gens = NULL; *out_n = 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0; /* absent → empty */
    if ((size_t)st.st_size > 4u * 1024u * 1024u) return 0; /* too big → reset */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return 0; }
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (r <= 0) { free(buf); return 0; }
    buf[r] = 0;
    /* Find "generations":[ ... ] payload and pull out each "gen": <int> occurrence. */
    const char *gp = strstr(buf, "\"generations\"");
    if (!gp) { free(buf); return 0; }
    uint64_t *gens = calloc(max_slots, sizeof *gens);
    if (!gens) { free(buf); return 0; }
    size_t n = 0;
    const char *p = gp;
    while ((p = strstr(p, "\"gen\""))) {
        p += 5; /* past "gen" */
        while (*p == ':' || *p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') break;
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; }
        if (n < max_slots) gens[n++] = v;
    }
    free(buf);
    *out_gens = gens;
    *out_n = n;
    return 0;
}

int nh_prov_pinned_ring_promote(const char *path,
                                uint64_t generation,
                                const char *const *hashes,
                                size_t n_hashes,
                                size_t ring_capacity) {
    if (!path) return -EINVAL;
    if (ring_capacity == 0) ring_capacity = 10;

    uint64_t *old_gens = NULL;
    size_t    old_n    = 0;
    (void)pinned_load_(path, &old_gens, &old_n, ring_capacity);

    /* Compose gens list = old (dropping any equal to `generation`),
     * then append the new gen; trim to capacity from the front. */
    uint64_t *g = calloc(ring_capacity + 1, sizeof *g);
    if (!g) { free(old_gens); return -ENOMEM; }
    size_t gn = 0;
    for (size_t i = 0; i < old_n; i++) {
        if (old_gens[i] == generation) continue;
        if (gn < ring_capacity) g[gn++] = old_gens[i];
    }
    /* Even when we already had the max, we still need room for the new
     * head — pop the oldest to make room. */
    if (gn == ring_capacity) {
        memmove(&g[0], &g[1], (ring_capacity - 1) * sizeof *g);
        gn = ring_capacity - 1;
    }
    g[gn++] = generation;
    free(old_gens);

    /* Serialize. Only the CURRENT generation carries its actual hash
     * list; older slots are re-emitted with empty hashes because we
     * did not parse them. That is safe for retention accounting: the
     * pin-ring's `effective_pins` union just widens, it never
     * incorrectly evicts. syncd's later promote overwrites verbatim. */
    sb_t s = {0};
    sb_str(&s, "{\n  \"schema\": 1,\n  \"capacity\": ");
    sb_u64(&s, (uint64_t)ring_capacity);
    sb_str(&s, ",\n  \"generations\": [\n");
    for (size_t i = 0; i < gn; i++) {
        int is_last = (i + 1 == gn);
        sb_str(&s, "    {\"gen\": ");
        sb_u64(&s, g[i]);
        sb_str(&s, ", \"hashes\": [");
        if (is_last) {
            for (size_t j = 0, first = 1; j < n_hashes; j++) {
                if (!hashes[j] || !is_hex64_local(hashes[j])) continue;
                if (!first) sb_str(&s, ", ");
                first = 0;
                sb_str(&s, "\"");
                sb_str(&s, hashes[j]);
                sb_str(&s, "\"");
            }
        }
        sb_str(&s, "]}");
        if (!is_last) sb_str(&s, ",");
        sb_str(&s, "\n");
    }
    sb_str(&s, "  ]\n}\n");
    free(g);
    if (s.err) { free(s.buf); return -ENOMEM; }

    /* Ensure parent dir exists (mkdir -p on the directory containing path). */
    char *dup = strdup(path);
    if (dup) {
        char *slash = strrchr(dup, '/');
        if (slash && slash != dup) {
            *slash = 0;
            for (char *cp = dup + 1; *cp; cp++) {
                if (*cp == '/') {
                    *cp = 0;
                    (void)mkdir(dup, 0700);
                    *cp = '/';
                }
            }
            (void)mkdir(dup, 0700);
        }
        free(dup);
    }

    int rc = atomic_write_0600(path, s.buf, s.len);
    free(s.buf);
    return rc;
}
