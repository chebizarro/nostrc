/*
 * nh_fuse_source.c — 4-tier read ladder implementation.
 *
 * SPDX-License-Identifier: MIT
 *
 * Design: docs/designs/nostrfs-porthome-overlay.md §3 (4-tier ladder),
 * §4 (encryption at the mount boundary), §6 (offline semantics).
 *
 * Single-threaded in v1 (design D13). Not thread safe; the FUSE main
 * runs `fuse_loop` (not `_mt`) so this is fine.
 */

#define _GNU_SOURCE
#include "nh_fuse_source.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef NH_FUSE_CHUNK_CACHE_DEFAULT
#define NH_FUSE_CHUNK_CACHE_DEFAULT (64ull * 1024ull * 1024ull) /* 64 MiB */
#endif

#ifndef NH_FUSE_OFFLINE_BUDGET_MS_DEFAULT
#define NH_FUSE_OFFLINE_BUDGET_MS_DEFAULT 15000L
#endif

/* Tier-1 entry. `hex` doubles as the LRU key.
 * Plaintext buffer is heap-allocated; tls scratch is not appropriate
 * because a callback may need it after the caller returns via FUSE.
 * We aggressively cleanse plaintext on eviction / close. */
typedef struct chunk_lru_node {
    char                   hex[65];
    uint8_t               *pt;
    size_t                 pt_len;
    /* Doubly-linked LRU order — head = most recently used. */
    struct chunk_lru_node *prev;
    struct chunk_lru_node *next;
} chunk_lru_node;

struct nh_fuse_source {
    char                   *home_dir;
    bool                    tier0_enabled;
    nh_syncd_cache         *cache;
    nh_porthome_blossom_t  *blossom;

    /* mlock'd 32 bytes. */
    uint8_t                *home_key_locked;

    /* Chunk LRU. */
    size_t                  chunk_cache_bytes;
    size_t                  chunk_bytes_used;
    chunk_lru_node         *lru_head;
    chunk_lru_node         *lru_tail;

    long                    offline_budget_ms;

    void                  (*on_miss)(void *ud, const char *rel_path, int errcode);
    void                   *on_miss_ud;

    nh_fuse_source_stats_t  stats;
};

static uint64_t now_wall_secs(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec;
}

/* ─── LRU management ─────────────────────────────────────────────── */

static void lru_unlink(nh_fuse_source *s, chunk_lru_node *n) {
    if (n->prev) n->prev->next = n->next; else s->lru_head = n->next;
    if (n->next) n->next->prev = n->prev; else s->lru_tail = n->prev;
    n->prev = n->next = NULL;
}

static void lru_push_head(nh_fuse_source *s, chunk_lru_node *n) {
    n->prev = NULL;
    n->next = s->lru_head;
    if (s->lru_head) s->lru_head->prev = n;
    s->lru_head = n;
    if (!s->lru_tail) s->lru_tail = n;
}

static void lru_evict_free(nh_fuse_source *s, chunk_lru_node *n) {
    if (n->pt) {
        OPENSSL_cleanse(n->pt, n->pt_len);
        free(n->pt);
    }
    s->chunk_bytes_used -= n->pt_len;
    free(n);
}

static void lru_trim(nh_fuse_source *s) {
    while (s->chunk_bytes_used > s->chunk_cache_bytes && s->lru_tail) {
        chunk_lru_node *n = s->lru_tail;
        lru_unlink(s, n);
        lru_evict_free(s, n);
    }
}

static chunk_lru_node *lru_lookup(nh_fuse_source *s, const char *hex) {
    for (chunk_lru_node *n = s->lru_head; n; n = n->next) {
        if (strcmp(n->hex, hex) == 0) return n;
    }
    return NULL;
}

static int lru_insert(nh_fuse_source *s, const char *hex,
                      uint8_t *pt, size_t pt_len) {
    chunk_lru_node *n = calloc(1, sizeof *n);
    if (!n) return -ENOMEM;
    strncpy(n->hex, hex, 64); n->hex[64] = '\0';
    n->pt = pt;
    n->pt_len = pt_len;
    lru_push_head(s, n);
    s->chunk_bytes_used += pt_len;
    lru_trim(s);
    return 0;
}

/* ─── open / close ───────────────────────────────────────────────── */

int nh_fuse_source_open(const nh_fuse_source_cfg *cfg, nh_fuse_source **out) {
    if (!cfg || !out) return -EINVAL;
    nh_fuse_source *s = calloc(1, sizeof *s);
    if (!s) return -ENOMEM;
    s->home_dir = cfg->home_dir ? strdup(cfg->home_dir) : NULL;
    s->tier0_enabled = cfg->tier0_enabled && (cfg->home_dir != NULL);
    s->cache = cfg->cache;
    s->blossom = cfg->blossom;
    s->chunk_cache_bytes = cfg->chunk_cache_bytes ? cfg->chunk_cache_bytes
                                                  : NH_FUSE_CHUNK_CACHE_DEFAULT;
    s->offline_budget_ms = cfg->offline_budget_ms ? cfg->offline_budget_ms
                                                  : NH_FUSE_OFFLINE_BUDGET_MS_DEFAULT;
    s->on_miss = cfg->on_miss;
    s->on_miss_ud = cfg->on_miss_ud;

    /* Copy home_key onto an mlock'd page. */
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    void *page = NULL;
    if (posix_memalign(&page, (size_t)ps, (size_t)ps) != 0 || !page) {
        free(s->home_dir); free(s);
        return -ENOMEM;
    }
    memset(page, 0, (size_t)ps);
    memcpy(page, cfg->home_key, NH_PORTHOME_KEY_LEN);
    /* Best effort — RLIMIT_MEMLOCK may reject. Cleanse-on-close is
     * the durable defence. */
    (void)mlock(page, (size_t)ps);
#ifdef MADV_DONTDUMP
    (void)madvise(page, (size_t)ps, MADV_DONTDUMP);
#endif
    s->home_key_locked = (uint8_t *)page;

    *out = s;
    return 0;
}

void nh_fuse_source_close(nh_fuse_source *s) {
    if (!s) return;
    while (s->lru_head) {
        chunk_lru_node *n = s->lru_head;
        lru_unlink(s, n);
        lru_evict_free(s, n);
    }
    if (s->home_key_locked) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) ps = 4096;
        OPENSSL_cleanse(s->home_key_locked, NH_PORTHOME_KEY_LEN);
        (void)munlock(s->home_key_locked, (size_t)ps);
        free(s->home_key_locked);
    }
    free(s->home_dir);
    free(s);
}

void nh_fuse_source_disable_tier0(nh_fuse_source *s) {
    if (s) s->tier0_enabled = false;
}

void nh_fuse_source_stats(const nh_fuse_source *s, nh_fuse_source_stats_t *out) {
    if (!s || !out) return;
    *out = s->stats;
}

/* ─── tier 2 / 3 chunk retrieval ─────────────────────────────────── */

static void note_miss(nh_fuse_source *s, const char *rel, int errcode) {
    s->stats.misses++;
    s->stats.last_miss_epoch = now_wall_secs();
    if (s->on_miss) s->on_miss(s->on_miss_ud, rel, errcode);
}

int nh_fuse_source_chunk(nh_fuse_source *s,
                         const char sha256_hex[65],
                         const uint8_t **out_pt, size_t *out_len) {
    if (!s || !sha256_hex || !out_pt || !out_len) return -EINVAL;
    *out_pt = NULL; *out_len = 0;

    /* Tier 1: plaintext LRU. */
    chunk_lru_node *hit = lru_lookup(s, sha256_hex);
    if (hit) {
        /* MRU touch. */
        lru_unlink(s, hit);
        lru_push_head(s, hit);
        *out_pt = hit->pt;
        *out_len = hit->pt_len;
        s->stats.hits_chunk_lru++;
        return 0;
    }

    /* Tier 2: local sealed-blob cache. */
    if (s->cache) {
        char *path = nh_syncd_cache_get_path(s->cache, sha256_hex);
        if (path) {
            /* Read the sealed blob, decrypt, insert plaintext. */
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            free(path);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0) {
                    uint8_t *ct = malloc((size_t)st.st_size);
                    if (!ct) { close(fd); return -ENOMEM; }
                    size_t off = 0;
                    while (off < (size_t)st.st_size) {
                        ssize_t r = read(fd, ct + off, (size_t)st.st_size - off);
                        if (r < 0) { if (errno == EINTR) continue; free(ct); close(fd); return -EIO; }
                        if (r == 0) break;
                        off += (size_t)r;
                    }
                    close(fd);
                    uint8_t *pt = NULL; size_t pt_len = 0;
                    int dr = nh_porthome_decrypt_chunk(s->home_key_locked,
                                                      ct, off, &pt, &pt_len);
                    OPENSSL_cleanse(ct, off);
                    free(ct);
                    if (dr != 0) { note_miss(s, NULL, EIO); return -EIO; }
                    int ir = lru_insert(s, sha256_hex, pt, pt_len);
                    if (ir != 0) { OPENSSL_cleanse(pt, pt_len); free(pt); return ir; }
                    *out_pt = pt; *out_len = pt_len;
                    s->stats.hits_cache++;
                    return 0;
                }
                close(fd);
            }
        }
    }

    /* Tier 3: Blossom fetch — content-sha verified in the wrapper. */
    if (!s->blossom) { note_miss(s, NULL, EIO); return -EIO; }

    /* Bounded budget. The Blossom wrapper does its own per-request
     * timeout; we do NOT layer a second one on top (double-budget
     * bugs) — instead we log and treat any negative rc as terminal. */
    uint8_t *ct = NULL; size_t ct_len = 0;
    int rc = nh_porthome_blossom_fetch(s->blossom, sha256_hex, &ct, &ct_len);
    if (rc != 0 || !ct) {
        note_miss(s, NULL, EIO);
        return (rc == NH_PORTHOME_BLOSSOM_ERR_NOT_FOUND) ? -ENOENT : -EIO;
    }
    /* Populate tier 2 (best-effort; ignore quota errors). */
    if (s->cache) (void)nh_syncd_cache_put(s->cache, sha256_hex, ct, ct_len);

    uint8_t *pt = NULL; size_t pt_len = 0;
    int dr = nh_porthome_decrypt_chunk(s->home_key_locked, ct, ct_len, &pt, &pt_len);
    OPENSSL_cleanse(ct, ct_len);
    free(ct);
    if (dr != 0) {
        /* AEAD failure — cache line is now junk (still bytes-match the
         * declared sha but a mis-derived key would fail every read).
         * We already populated the cache; that is intentional — the
         * cache is content-addressed and the same sha will decrypt
         * with the correct key on the next mount. */
        note_miss(s, NULL, EIO);
        return -EIO;
    }
    int ir = lru_insert(s, sha256_hex, pt, pt_len);
    if (ir != 0) { OPENSSL_cleanse(pt, pt_len); free(pt); return ir; }
    *out_pt = pt; *out_len = pt_len;
    s->stats.fetches++;
    return 0;
}

/* ─── full pread service ─────────────────────────────────────────── */

ssize_t nh_fuse_source_pread(nh_fuse_source *s,
                             const char *rel,
                             const char *content_hash_hex,
                             uint64_t declared_size,
                             const char * const *chunks_hex,
                             size_t n_chunks,
                             size_t chunk_size,
                             void *buf, size_t len, off_t off) {
    if (!s || !buf) return -EINVAL;
    if (len == 0) return 0;
    (void)content_hash_hex;

    /* Tier 0: pread from $HOME/<rel> when state is `ready` AND the
     * declared size matches the stat. content_hash_hex is defensive
     * only — we do not re-hash on every read (that would be prohibitively
     * expensive), we trust the syncd's snapshot. If the on-disk file's
     * st_size differs from the snapshot's `size`, syncd has an
     * inconsistency and we fall through to the sealed path. */
    if (s->tier0_enabled && rel && s->home_dir) {
        char abs[4096];
        int n = snprintf(abs, sizeof abs, "%s/%s", s->home_dir, rel);
        if (n > 0 && n < (int)sizeof abs) {
            int fd = open(abs, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                    (uint64_t)st.st_size == declared_size) {
                    ssize_t r = pread(fd, buf, len, off);
                    if (r >= 0) {
                        close(fd);
                        s->stats.hits_local++;
                        return r;
                    }
                }
                close(fd);
            }
        }
    }

    if (!chunks_hex || n_chunks == 0 || chunk_size == 0) {
        /* Zero-byte file (files with no chunks). */
        if (declared_size == 0 && (uint64_t)off >= declared_size) return 0;
        return 0;
    }
    if ((uint64_t)off >= declared_size) return 0;
    if ((uint64_t)off + (uint64_t)len > declared_size)
        len = (size_t)(declared_size - (uint64_t)off);

    uint8_t *dst = (uint8_t *)buf;
    size_t emitted = 0;
    off_t cur = off;

    while (emitted < len) {
        size_t idx = (size_t)(cur / (off_t)chunk_size);
        if (idx >= n_chunks) break;
        const uint8_t *pt = NULL; size_t pt_len = 0;
        int cr = nh_fuse_source_chunk(s, chunks_hex[idx], &pt, &pt_len);
        if (cr != 0) {
            if (emitted == 0) return cr; /* propagate the negative errno */
            /* We already produced bytes for this read; return short. */
            return (ssize_t)emitted;
        }
        size_t inside = (size_t)(cur - (off_t)idx * (off_t)chunk_size);
        if (inside >= pt_len) {
            /* Corrupt chunk boundary — bail. */
            if (emitted == 0) return -EIO;
            return (ssize_t)emitted;
        }
        size_t avail = pt_len - inside;
        size_t take = len - emitted;
        if (take > avail) take = avail;
        memcpy(dst + emitted, pt + inside, take);
        emitted += take;
        cur += (off_t)take;
    }
    return (ssize_t)emitted;
}
