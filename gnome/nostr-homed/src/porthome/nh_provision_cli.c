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
        if (rel && *rel)
            snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        else
            snprintf(child_rel, sizeof child_rel, "%s", de->d_name);
        char child_abs[4096];
        snprintf(child_abs, sizeof child_abs, "%s/%s", abs, de->d_name);

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
