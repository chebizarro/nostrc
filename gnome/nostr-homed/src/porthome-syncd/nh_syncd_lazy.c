/*
 * nh_syncd_lazy.c — lazy-subtree prefix matcher (Phase 4 P4-I).
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED — gated by
 * NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL. See
 * docs/designs/nostrfs-porthome-overlay.md §10 / §14 for the
 * design (D14 — lazy subtrees) and the maintainer approval note
 * that authorised this module (additive suppression only).
 *
 * Bead: nostrc-1u55 (P4-I), parent nostrc-h10m.
 *
 * Behaviour:
 *   - Prefixes are $HOME-relative directory paths, no leading '/'.
 *   - A prefix matches `rel_path` when the path is exactly the
 *     prefix or starts with `<prefix>/`.
 *   - Prefixes strip any trailing slashes on load so users can
 *     write `Portable/` or `Portable` — both mean the same thing.
 *   - Empty prefix list ⇒ matcher never matches ⇒ reconcile is
 *     behaviour-preserving.
 *   - The reconciler consults `nh_syncd_lazy_covers` before
 *     materializing a remote entry OR before applying a
 *     remote-delete. In both cases a match means SKIP — never
 *     write, never delete. Additive suppression only.
 */

#include "nh_syncd.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct nh_syncd_lazy {
    char   **prefixes;
    size_t   n;
    size_t   cap;
};

static char *dup_and_normalise(const char *raw) {
    if (!raw) return NULL;
    /* Skip leading whitespace and slashes. */
    const char *s = raw;
    while (*s == ' ' || *s == '\t') s++;
    while (*s == '/') s++;
    if (!*s || *s == '#') return NULL;
    /* Copy, strip trailing whitespace and slashes. */
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '/' || s[n - 1] == ' ' ||
                     s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n'))
        n--;
    if (n == 0) return NULL;
    /* Reject any '..' or '.' path component, NUL, or backslash. */
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\0' || s[i] == '\\') return NULL;
    }
    /* Walk components. */
    const char *cstart = s;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            size_t clen = (size_t)(s + i - cstart);
            if (clen == 0) return NULL;
            if (clen == 1 && cstart[0] == '.') return NULL;
            if (clen == 2 && cstart[0] == '.' && cstart[1] == '.') return NULL;
            cstart = s + i + 1;
        }
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

int nh_syncd_lazy_new_empty(nh_syncd_lazy **out) {
    if (!out) return NH_SYNCD_ERR_ARG;
    nh_syncd_lazy *lz = calloc(1, sizeof *lz);
    if (!lz) return NH_SYNCD_ERR_OOM;
    *out = lz;
    return NH_SYNCD_OK;
}

int nh_syncd_lazy_add_prefix(nh_syncd_lazy *lz, const char *prefix) {
    if (!lz || !prefix) return NH_SYNCD_ERR_ARG;
    char *norm = dup_and_normalise(prefix);
    if (!norm) return NH_SYNCD_OK; /* nothing to add (comment / empty) */
    if (lz->n == lz->cap) {
        size_t nc = lz->cap ? lz->cap * 2 : 4;
        char **np = realloc(lz->prefixes, nc * sizeof *np);
        if (!np) { free(norm); return NH_SYNCD_ERR_OOM; }
        lz->prefixes = np;
        lz->cap = nc;
    }
    lz->prefixes[lz->n++] = norm;
    return NH_SYNCD_OK;
}

int nh_syncd_lazy_new_from_string(const char *csv, nh_syncd_lazy **out) {
    if (!out) return NH_SYNCD_ERR_ARG;
    int rc = nh_syncd_lazy_new_empty(out);
    if (rc != NH_SYNCD_OK) return rc;
    if (!csv || !*csv) return NH_SYNCD_OK;
    /* Split on ',' and '\n' — both are treated as separators so a
     * single config file line can contain multiple values and a
     * multi-line file works too. */
    const char *p = csv;
    const char *start = p;
    for (;; p++) {
        if (*p == ',' || *p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - start);
            char *chunk = malloc(len + 1);
            if (!chunk) { nh_syncd_lazy_free(*out); *out = NULL; return NH_SYNCD_ERR_OOM; }
            memcpy(chunk, start, len);
            chunk[len] = '\0';
            /* Strip inline comment. */
            char *hash = strchr(chunk, '#');
            if (hash) *hash = '\0';
            int ar = nh_syncd_lazy_add_prefix(*out, chunk);
            free(chunk);
            if (ar != NH_SYNCD_OK) { nh_syncd_lazy_free(*out); *out = NULL; return ar; }
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    return NH_SYNCD_OK;
}

int nh_syncd_lazy_new(const char *home_dir, nh_syncd_lazy **out) {
    if (!out) return NH_SYNCD_ERR_ARG;
    if (!home_dir) return nh_syncd_lazy_new_empty(out);
    /* Two loading sources, checked in this order:
     *   1. $NOSTR_HOMED_SYNCD_LAZY_SUBTREES  (comma/newline separated)
     *   2. $HOME/.config/nostr-homed/lazy    (file, one prefix per line)
     * Both are opt-in; empty = OFF.
     */
    int rc = nh_syncd_lazy_new_empty(out);
    if (rc != NH_SYNCD_OK) return rc;
    const char *env = getenv("NOSTR_HOMED_SYNCD_LAZY_SUBTREES");
    if (env && *env) {
        nh_syncd_lazy *from_env = NULL;
        if (nh_syncd_lazy_new_from_string(env, &from_env) == NH_SYNCD_OK && from_env) {
            for (size_t i = 0; i < from_env->n; i++) {
                (void)nh_syncd_lazy_add_prefix(*out, from_env->prefixes[i]);
            }
            nh_syncd_lazy_free(from_env);
        }
    }
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/.config/nostr-homed/lazy", home_dir);
    if (n > 0 && n < (int)sizeof path) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                (void)nh_syncd_lazy_add_prefix(*out, line);
            }
            fclose(f);
        }
    }
    return NH_SYNCD_OK;
}

void nh_syncd_lazy_free(nh_syncd_lazy *lz) {
    if (!lz) return;
    for (size_t i = 0; i < lz->n; i++) free(lz->prefixes[i]);
    free(lz->prefixes);
    free(lz);
}

size_t nh_syncd_lazy_prefix_count(const nh_syncd_lazy *lz) {
    return lz ? lz->n : 0;
}

bool nh_syncd_lazy_covers(const nh_syncd_lazy *lz, const char *rel_path) {
    if (!lz || lz->n == 0 || !rel_path || !*rel_path) return false;
    for (size_t i = 0; i < lz->n; i++) {
        const char *pfx = lz->prefixes[i];
        size_t pn = strlen(pfx);
        if (pn == 0) continue;
        if (strncmp(rel_path, pfx, pn) != 0) continue;
        char nxt = rel_path[pn];
        if (nxt == '\0' || nxt == '/') return true;
    }
    return false;
}
