/*
 * nh_porthome_status.c — unified porthome-status.json merge helper.
 *
 * SPDX-License-Identifier: MIT
 *
 * See nh_porthome_status.h. Deliberately no jansson dep — a hand
 * written top-level object splitter keeps this file link-safe from
 * the FUSE binary (which never touches jansson) and the syncd core
 * without introducing a shared jansson requirement.
 *
 * Bead: nostrc-h10m.1.
 */

#define _GNU_SOURCE
#include "nh_porthome_status.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ─────────────── path resolution ─────────────── */
char *nh_porthome_status_default_path(void) {
    const char *xdg = getenv("XDG_STATE_HOME");
    char buf[1024];
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/nostr-homed/porthome-status.json", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) { errno = ENOENT; return NULL; }
        snprintf(buf, sizeof buf,
                 "%s/.local/state/nostr-homed/porthome-status.json", home);
    }
    return strdup(buf);
}

static int mkdir_p(const char *path, mode_t mode) {
    /* Walks the string, creating each parent dir if missing. */
    char tmp[1024];
    size_t L = strlen(path);
    if (L >= sizeof tmp) return -ENAMETOOLONG;
    memcpy(tmp, path, L + 1);
    for (size_t i = 1; i < L; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -errno;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -errno;
    return 0;
}

int nh_porthome_status_ensure_dir(void) {
    char *p = nh_porthome_status_default_path();
    if (!p) return -ENOENT;
    /* Strip the file basename. */
    char *slash = strrchr(p, '/');
    if (!slash) { free(p); return -EINVAL; }
    *slash = 0;
    int rc = mkdir_p(p, 0700);
    free(p);
    return rc;
}

/* ─────────────── JSON escape (minimal, sufficient for status use) ─── */
int nh_porthome_json_escape(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    size_t o = 0;
    if (!in) in = "";
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        unsigned char c = *p;
        const char *rep = NULL;
        char buf[8];
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    rep = buf;
                }
                break;
        }
        if (rep) {
            size_t L = strlen(rep);
            if (o + L + 1 > cap) return -1;
            memcpy(out + o, rep, L);
            o += L;
        } else {
            if (o + 1 + 1 > cap) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return (int)o;
}

/* ─────────────── minimal JSON scanner for the top level ───────────── */
/* Skip whitespace. */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p;
}

/* Skip a JSON string starting at *p (which must be '"'). Returns the
 * pointer just past the closing '"', or NULL on parse failure. */
static const char *skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    ++p;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        ++p;
    }
    return NULL;
}

/* Skip a JSON value at *p. Handles strings, numbers, true/false/null,
 * arrays, objects. Returns the pointer just past the value, or NULL. */
static const char *skip_value(const char *p, const char *end) {
    p = skip_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char open  = *p;
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *q = skip_string(p, end);
                if (!q) return NULL;
                p = q;
                continue;
            }
            if (*p == open) ++depth;
            else if (*p == close) {
                --depth;
                if (depth == 0) return p + 1;
            }
            ++p;
        }
        return NULL;
    }
    /* Bare literal: number / true / false / null. */
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           !isspace((unsigned char)*p))
        ++p;
    return p;
}

/* Locate the value span for `key` within a top-level object body.
 * Returns 0 with [*val_start, *val_end) on hit, -ENOENT if absent,
 * -EINVAL on parse failure. */
static int find_key_span(const char *body, size_t body_len,
                         const char *key,
                         const char **val_start,
                         const char **val_end) {
    const char *p   = body;
    const char *end = body + body_len;
    size_t klen = strlen(key);

    p = skip_ws(p, end);
    if (p >= end || *p != '{') return -EINVAL;
    ++p;

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) return -EINVAL;
        if (*p == '}') return -ENOENT;
        if (*p != '"') return -EINVAL;
        const char *k_start = p + 1;
        const char *k_end   = skip_string(p, end);
        if (!k_end) return -EINVAL;
        /* k_end points just past the closing quote. */
        size_t this_klen = (size_t)((k_end - 1) - k_start);
        bool match = (this_klen == klen &&
                      memcmp(k_start, key, klen) == 0);
        p = k_end;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return -EINVAL;
        ++p;
        p = skip_ws(p, end);
        const char *v_start = p;
        const char *v_end   = skip_value(p, end);
        if (!v_end) return -EINVAL;
        if (match) {
            *val_start = v_start;
            *val_end   = v_end;
            return 0;
        }
        p = v_end;
        p = skip_ws(p, end);
        if (p >= end) return -EINVAL;
        if (*p == ',') { ++p; continue; }
        if (*p == '}') return -ENOENT;
        return -EINVAL;
    }
    return -EINVAL;
}

int nh_porthome_status_get_key(const char *body, size_t body_len,
                               const char *key,
                               char **out_body_json) {
    if (!body || !key || !out_body_json) return -EINVAL;
    const char *vs = NULL, *ve = NULL;
    int rc = find_key_span(body, body_len, key, &vs, &ve);
    if (rc != 0) return rc;
    size_t L = (size_t)(ve - vs);
    char *o = malloc(L + 1);
    if (!o) return -ENOMEM;
    memcpy(o, vs, L);
    o[L] = 0;
    *out_body_json = o;
    return 0;
}

/* ─────────────── read whole file ─────────────── */
int nh_porthome_status_read(const char *path,
                            char **out_body,
                            size_t *out_len) {
    if (!path || !out_body || !out_len) return -EINVAL;
    *out_body = NULL;
    *out_len  = 0;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            /* Missing → empty document. */
            const char *seed = "{\"schema\":1}";
            size_t L = strlen(seed);
            char *b = malloc(L + 1);
            if (!b) return -ENOMEM;
            memcpy(b, seed, L + 1);
            *out_body = b;
            *out_len  = L;
            return 0;
        }
        return -errno;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = -errno; close(fd); return e; }
    if (st.st_size < 0 || st.st_size > (off_t)(4 * 1024 * 1024)) {
        close(fd);
        return -EFBIG;
    }
    size_t sz = (size_t)st.st_size;
    char *b = malloc(sz + 1);
    if (!b) { close(fd); return -ENOMEM; }
    ssize_t off = 0;
    while ((size_t)off < sz) {
        ssize_t r = read(fd, b + off, sz - (size_t)off);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = -errno; free(b); close(fd); return e;
        }
        if (r == 0) break;
        off += r;
    }
    close(fd);
    b[off] = 0;

    /* Validate that the doc is a top-level object. Corrupt → empty. */
    const char *p = skip_ws(b, b + off);
    if (p >= b + off || *p != '{') {
        free(b);
        const char *seed = "{\"schema\":1}";
        size_t L = strlen(seed);
        char *b2 = malloc(L + 1);
        if (!b2) return -ENOMEM;
        memcpy(b2, seed, L + 1);
        *out_body = b2;
        *out_len  = L;
        return 0;
    }
    /* Also sanity-check the last non-ws char is '}'. */
    ssize_t tail = off - 1;
    while (tail >= 0 && isspace((unsigned char)b[tail])) --tail;
    if (tail < 0 || b[tail] != '}') {
        free(b);
        const char *seed = "{\"schema\":1}";
        size_t L = strlen(seed);
        char *b2 = malloc(L + 1);
        if (!b2) return -ENOMEM;
        memcpy(b2, seed, L + 1);
        *out_body = b2;
        *out_len  = L;
        return 0;
    }
    *out_body = b;
    *out_len  = (size_t)off;
    return 0;
}

/* ─────────────── merge: replace one top-level key ────────────── */

/* Build the merged JSON body: read existing (or seed), delete the
 * key if present, then splice in the new key body. Result is written
 * to *out with size *out_len (heap-allocated). */
static int splice_key(const char *existing, size_t existing_len,
                      const char *key, const char *key_body,
                      char **out, size_t *out_len) {
    /* Find the current span of the key, if any. */
    const char *vs = NULL, *ve = NULL;
    int rc = find_key_span(existing, existing_len, key, &vs, &ve);
    /* Bail on genuine parse errors — we already treated the read
     * layer's corrupt input as "empty", so an EINVAL here is very
     * unlikely. If it happens, restart from an empty document. */
    if (rc == -EINVAL) {
        existing     = "{\"schema\":1}";
        existing_len = strlen(existing);
        rc = -ENOENT;
    }

    /* Buffer size upper bound. */
    size_t body_len = key_body ? strlen(key_body) : 0;
    size_t klen     = strlen(key);
    size_t need     = existing_len + body_len + klen + 32;
    char *buf = malloc(need + 1);
    if (!buf) return -ENOMEM;

    if (rc == 0) {
        /* Replace the existing value span. If key_body is NULL, drop
         * the whole "key: value" clause plus one trailing comma. */
        if (key_body) {
            /* Copy [start .. vs), then key_body, then [ve .. end). */
            size_t pre  = (size_t)(vs - existing);
            size_t post = existing_len - (size_t)(ve - existing);
            memcpy(buf, existing, pre);
            memcpy(buf + pre, key_body, body_len);
            memcpy(buf + pre + body_len, ve, post);
            buf[pre + body_len + post] = 0;
            *out     = buf;
            *out_len = pre + body_len + post;
            return 0;
        }
        /* Deletion is uncommon; fall through to strip-and-rebuild. */
    }

    /* Insert: strip trailing '}', append ",\"key\":body}" (or
     * "\"key\":body}" if the source was empty). */
    ssize_t tail = (ssize_t)existing_len - 1;
    while (tail >= 0 && isspace((unsigned char)existing[tail])) --tail;
    if (tail < 0 || existing[tail] != '}') {
        free(buf);
        return -EINVAL;
    }
    /* Copy everything up to (but not including) the closing '}'. */
    memcpy(buf, existing, (size_t)tail);
    size_t o = (size_t)tail;

    /* If there's anything meaningful before the '}', we need a
     * comma. Detect emptiness by scanning back for the opening '{'. */
    ssize_t last = tail - 1;
    while (last >= 0 && isspace((unsigned char)existing[last])) --last;
    bool empty_obj = (last >= 0 && existing[last] == '{');
    if (!empty_obj) buf[o++] = ',';
    /* "\"key\":body}" */
    buf[o++] = '"';
    memcpy(buf + o, key, klen);
    o += klen;
    buf[o++] = '"';
    buf[o++] = ':';
    if (key_body) {
        memcpy(buf + o, key_body, body_len);
        o += body_len;
    } else {
        memcpy(buf + o, "null", 4);
        o += 4;
    }
    buf[o++] = '}';
    buf[o]   = 0;
    *out     = buf;
    *out_len = o;
    return 0;
}

/* Write an atomic replacement. `path.lock` serialises concurrent
 * writers; `path.tmp.<pid>` is the staging file we rename. Mode 0600. */
int nh_porthome_status_write_key(const char *path,
                                 const char *key,
                                 const char *key_body_json) {
    if (!path || !key) return -EINVAL;

    /* Ensure parent dir exists (best-effort — the caller may have
     * done this already). */
    {
        char tmp[1024];
        size_t L = strlen(path);
        if (L >= sizeof tmp) return -ENAMETOOLONG;
        memcpy(tmp, path, L + 1);
        char *slash = strrchr(tmp, '/');
        if (slash) {
            *slash = 0;
            (void)mkdir_p(tmp, 0700);
        }
    }

    /* Acquire lock. */
    char lockpath[1200];
    if (snprintf(lockpath, sizeof lockpath, "%s.lock", path)
        >= (int)sizeof lockpath)
        return -ENAMETOOLONG;
    int lfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lfd < 0) return -errno;
    if (flock(lfd, LOCK_EX) != 0) { int e = -errno; close(lfd); return e; }

    /* Read current, splice, write, rename. */
    char  *cur = NULL; size_t cur_len = 0;
    int rc = nh_porthome_status_read(path, &cur, &cur_len);
    if (rc != 0) { close(lfd); return rc; }

    char *merged = NULL; size_t merged_len = 0;
    rc = splice_key(cur, cur_len, key, key_body_json, &merged, &merged_len);
    free(cur);
    if (rc != 0) { close(lfd); return rc; }

    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid())
        >= (int)sizeof tmp) {
        free(merged); close(lfd); return -ENAMETOOLONG;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { int e = -errno; free(merged); close(lfd); return e; }
    ssize_t off = 0;
    while ((size_t)off < merged_len) {
        ssize_t w = write(fd, merged + off, merged_len - (size_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = -errno;
            close(fd); (void)unlink(tmp); free(merged); close(lfd);
            return e;
        }
        off += w;
    }
    /* Trailing newline for `cat` friendliness. Best-effort. */
    (void)write(fd, "\n", 1);
    if (close(fd) != 0) {
        int e = -errno;
        (void)unlink(tmp); free(merged); close(lfd); return e;
    }
    if (rename(tmp, path) != 0) {
        int e = -errno;
        (void)unlink(tmp); free(merged); close(lfd); return e;
    }
    free(merged);
    close(lfd);
    return 0;
}

int nh_porthome_status_write_key_default(const char *key,
                                         const char *key_body_json) {
    char *p = nh_porthome_status_default_path();
    if (!p) return -ENOMEM;
    int rc = nh_porthome_status_write_key(p, key, key_body_json);
    free(p);
    return rc;
}
