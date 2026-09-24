/*
 * nh_fuse_table.c — snapshot-backed FUSE namespace table.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "nh_fuse_table.h"
#include "nh_syncd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct nh_fuse_table {
    uint64_t          generation;
    uint32_t          uid;
    uint32_t          gid;
    nh_fuse_entry_t  *entries;
    size_t            n;
    size_t            cap;
    /* Synthetic root: kind=DIR, mode 0700. */
    nh_fuse_entry_t   root_entry;
};

/* Refuse hostile / malformed paths. §7.1. */
static bool path_valid(const char *rel) {
    if (!rel || !*rel) return false;
    if (rel[0] == '/') return false;
    size_t n = strlen(rel);
    if (n > NH_FUSE_MAX_PATH_BYTES) return false;
    size_t depth = 0;
    const char *s = rel;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || rel[i] == '/') {
            size_t clen = (size_t)(rel + i - s);
            if (clen == 0) return false;
            if (clen == 1 && s[0] == '.') return false;
            if (clen == 2 && s[0] == '.' && s[1] == '.') return false;
            if (clen > NH_FUSE_MAX_COMPONENT) return false;
            for (size_t k = 0; k < clen; k++) {
                char c = s[k];
                if (c == '\0' || c == '\\') return false;
            }
            depth++;
            if (depth > NH_FUSE_MAX_DEPTH) return false;
            s = rel + i + 1;
        }
    }
    return true;
}

static bool hex64_ok(const char *s) {
    if (!s) return false;
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static int add_entry(nh_fuse_table *t, const nh_fuse_entry_t *e) {
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 128;
        if (nc > NH_FUSE_MAX_ENTRIES) nc = NH_FUSE_MAX_ENTRIES;
        nh_fuse_entry_t *na = realloc(t->entries, nc * sizeof *na);
        if (!na) return -ENOMEM;
        t->entries = na;
        t->cap = nc;
    }
    t->entries[t->n++] = *e;
    return 0;
}

static int entry_cmp(const void *a, const void *b) {
    const nh_fuse_entry_t *ea = a; const nh_fuse_entry_t *eb = b;
    return strcmp(ea->rel, eb->rel);
}

static void entry_free(nh_fuse_entry_t *e) {
    if (!e) return;
    free(e->rel);
    free(e->symlink_target);
    if (e->chunks_hex) {
        for (size_t i = 0; i < e->n_chunks; i++) free(e->chunks_hex[i]);
        free(e->chunks_hex);
    }
    memset(e, 0, sizeof *e);
}

int nh_fuse_table_load(const char *state_dir,
                       uint32_t mount_uid, uint32_t mount_gid,
                       nh_fuse_table **out) {
    if (!state_dir || !out) return -EINVAL;
    *out = NULL;
    nh_syncd_state *st = NULL;
    bool unknown = false;
    int rc = nh_syncd_state_load(state_dir, &st, &unknown);
    if (rc != NH_SYNCD_OK || !st || unknown) {
        if (st) nh_syncd_state_free(st);
        return -ENOENT;
    }
    nh_fuse_table *t = calloc(1, sizeof *t);
    if (!t) { nh_syncd_state_free(st); return -ENOMEM; }
    t->generation = nh_syncd_state_get_local_generation(st);
    t->uid = mount_uid;
    t->gid = mount_gid;
    t->root_entry.kind = NH_FUSE_KIND_DIR;
    t->root_entry.mode = 0700;

    size_t total = nh_syncd_state_file_count(st);
    if (total > NH_FUSE_MAX_ENTRIES) {
        fprintf(stderr, "nh_fuse_table: refusing snapshot with %zu entries (cap %u)\n",
                total, NH_FUSE_MAX_ENTRIES);
        nh_fuse_table_free(t);
        nh_syncd_state_free(st);
        return -E2BIG;
    }

    for (size_t i = 0; i < total; i++) {
        const char *rel = NULL;
        const nh_syncd_entry *se = nh_syncd_state_at(st, i, &rel);
        if (!se || !rel) continue;
        if (!path_valid(rel)) {
            fprintf(stderr, "nh_fuse_table: skipping hostile path (kind=%d)\n",
                    (int)nh_syncd_entry_kind(se));
            continue;
        }
        nh_fuse_entry_t e = {0};
        e.rel = strdup(rel);
        if (!e.rel) { nh_fuse_table_free(t); nh_syncd_state_free(st); return -ENOMEM; }
        switch (nh_syncd_entry_kind(se)) {
            case NH_SYNCD_KIND_DIR:     e.kind = NH_FUSE_KIND_DIR;     break;
            case NH_SYNCD_KIND_SYMLINK: e.kind = NH_FUSE_KIND_SYMLINK; break;
            default:                    e.kind = NH_FUSE_KIND_FILE;    break;
        }
        e.mode = nh_syncd_entry_mode(se) & 0777;
        if (e.mode == 0) e.mode = (e.kind == NH_FUSE_KIND_DIR) ? 0700 : 0600;
        e.mtime_ns = nh_syncd_entry_mtime_ns(se);
        e.size = nh_syncd_entry_size(se);
        const char *hh = nh_syncd_entry_content_hash_hex(se);
        if (hh && *hh && hex64_ok(hh)) {
            strncpy(e.content_hash_hex, hh, 64);
            e.content_hash_hex[64] = '\0';
        }
        if (e.kind == NH_FUSE_KIND_SYMLINK) {
            const char *tgt = nh_syncd_entry_symlink_target(se);
            e.symlink_target = strdup(tgt ? tgt : "");
            if (!e.symlink_target) { entry_free(&e); nh_fuse_table_free(t); nh_syncd_state_free(st); return -ENOMEM; }
        }
        if (e.kind == NH_FUSE_KIND_FILE) {
            size_t nc = nh_syncd_entry_chunk_count(se);
            if (nc > NH_FUSE_MAX_CHUNKS) {
                fprintf(stderr, "nh_fuse_table: skipping %s (chunks=%zu > cap)\n", rel, nc);
                entry_free(&e);
                continue;
            }
            if (nc) {
                e.chunks_hex = calloc(nc, sizeof *e.chunks_hex);
                if (!e.chunks_hex) { entry_free(&e); nh_fuse_table_free(t); nh_syncd_state_free(st); return -ENOMEM; }
                for (size_t k = 0; k < nc; k++) {
                    const char *ch = nh_syncd_entry_chunk_at(se, k);
                    if (!ch || !hex64_ok(ch)) {
                        fprintf(stderr, "nh_fuse_table: %s chunk[%zu] not 64-hex; skipping entry\n", rel, k);
                        for (size_t j = 0; j < k; j++) free(e.chunks_hex[j]);
                        free(e.chunks_hex); e.chunks_hex = NULL;
                        break;
                    }
                    e.chunks_hex[k] = strdup(ch);
                    if (!e.chunks_hex[k]) {
                        for (size_t j = 0; j < k; j++) free(e.chunks_hex[j]);
                        free(e.chunks_hex); e.chunks_hex = NULL;
                        break;
                    }
                }
                if (!e.chunks_hex) { entry_free(&e); continue; }
                e.n_chunks = nc;
            }
        }
        int ar = add_entry(t, &e);
        if (ar != 0) { entry_free(&e); nh_fuse_table_free(t); nh_syncd_state_free(st); return ar; }
    }

    nh_syncd_state_free(st);
    qsort(t->entries, t->n, sizeof *t->entries, entry_cmp);
    *out = t;
    return 0;
}

void nh_fuse_table_free(nh_fuse_table *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) entry_free(&t->entries[i]);
    free(t->entries);
    free(t);
}

uint64_t nh_fuse_table_generation(const nh_fuse_table *t) { return t ? t->generation : 0; }
size_t   nh_fuse_table_entry_count(const nh_fuse_table *t) { return t ? t->n : 0; }
uint32_t nh_fuse_table_uid(const nh_fuse_table *t) { return t ? t->uid : 0; }
uint32_t nh_fuse_table_gid(const nh_fuse_table *t) { return t ? t->gid : 0; }

const nh_fuse_entry_t *nh_fuse_table_find(const nh_fuse_table *t, const char *rel) {
    if (!t) return NULL;
    if (!rel || !*rel) return &t->root_entry;
    /* Binary search. */
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(t->entries[mid].rel, rel);
        if (c == 0) return &t->entries[mid];
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    /* Not a direct hit — check if `rel` names a directory that
     * exists implicitly (some entry starts with `rel/`). */
    size_t pn = strlen(rel);
    for (size_t i = 0; i < t->n; i++) {
        if (strncmp(t->entries[i].rel, rel, pn) == 0 &&
            t->entries[i].rel[pn] == '/') {
            /* Synthesize an on-demand directory entry pointing back
             * to a static; return via a tls slot so pointer lifetime
             * matches the table's. */
            static _Thread_local nh_fuse_entry_t g_synth;
            memset(&g_synth, 0, sizeof g_synth);
            g_synth.kind = NH_FUSE_KIND_DIR;
            g_synth.mode = 0700;
            /* We cannot easily fake `rel` here without a heap alloc
             * whose lifetime we'd have to manage; the callers only
             * use `kind`/`mode` for a synthesized dir, so leave rel
             * NULL. */
            return &g_synth;
        }
    }
    return NULL;
}

int nh_fuse_table_readdir(const nh_fuse_table *t, const char *rel_dir,
                          nh_fuse_table_dir_cb cb, void *ud) {
    if (!t || !cb) return -EINVAL;
    if (!rel_dir) rel_dir = "";
    size_t dn = strlen(rel_dir);
    for (size_t i = 0; i < t->n; i++) {
        const char *r = t->entries[i].rel;
        if (dn == 0) {
            /* Root children: no '/' in r. */
            if (strchr(r, '/') != NULL) {
                /* Include the first-component synthetic dir only once. */
                /* Not the direct child — skip. */
                continue;
            }
            int rc = cb(ud, r, &t->entries[i]);
            if (rc != 0) return rc;
        } else {
            if (strncmp(r, rel_dir, dn) != 0) continue;
            if (r[dn] != '/') continue;
            const char *rest = r + dn + 1;
            if (strchr(rest, '/') != NULL) continue; /* deeper */
            int rc = cb(ud, rest, &t->entries[i]);
            if (rc != 0) return rc;
        }
    }
    /* Also emit intermediate directories: any entry deeper than 1
     * beneath rel_dir contributes a synthetic child with the same
     * first component. To avoid duplicates we accumulate seen names
     * in a small dedup array on the stack (bounded by max entries
     * per directory — assumed reasonable). */
    /* For simplicity + correctness, we iterate again and emit
     * synthesized children for prefixes we haven't already emitted.
     * O(n^2) worst case; acceptable for v1 with entries capped at
     * 500k and typical directory fanout tiny. */
    for (size_t i = 0; i < t->n; i++) {
        const char *r = t->entries[i].rel;
        const char *rest;
        if (dn == 0) {
            if (strchr(r, '/') == NULL) continue;
            rest = r;
        } else {
            if (strncmp(r, rel_dir, dn) != 0 || r[dn] != '/') continue;
            rest = r + dn + 1;
        }
        const char *slash = strchr(rest, '/');
        if (!slash) continue;
        size_t clen = (size_t)(slash - rest);
        /* De-dup against direct children AND earlier synthesized
         * dirs. Naive scan back. */
        char name[NH_FUSE_MAX_COMPONENT + 1];
        if (clen > NH_FUSE_MAX_COMPONENT) continue;
        memcpy(name, rest, clen); name[clen] = '\0';

        bool already = false;
        /* Direct child with same name? */
        for (size_t k = 0; k < t->n && !already; k++) {
            const char *rr = t->entries[k].rel;
            const char *rrest;
            if (dn == 0) rrest = rr;
            else {
                if (strncmp(rr, rel_dir, dn) != 0 || rr[dn] != '/') continue;
                rrest = rr + dn + 1;
            }
            if (strchr(rrest, '/') != NULL) continue;
            if (strcmp(rrest, name) == 0) { already = true; break; }
        }
        if (already) continue;
        /* Earlier synthesized dir with same name? */
        bool earlier = false;
        for (size_t k = 0; k < i && !earlier; k++) {
            const char *rr = t->entries[k].rel;
            const char *rrest;
            if (dn == 0) rrest = rr;
            else {
                if (strncmp(rr, rel_dir, dn) != 0 || rr[dn] != '/') continue;
                rrest = rr + dn + 1;
            }
            const char *ss = strchr(rrest, '/');
            if (!ss) continue;
            size_t kl = (size_t)(ss - rrest);
            if (kl == clen && strncmp(rrest, name, clen) == 0) { earlier = true; break; }
        }
        if (earlier) continue;

        static _Thread_local nh_fuse_entry_t g_synth;
        memset(&g_synth, 0, sizeof g_synth);
        g_synth.kind = NH_FUSE_KIND_DIR;
        g_synth.mode = 0700;
        int rc = cb(ud, name, &g_synth);
        if (rc != 0) return rc;
    }
    return 0;
}
