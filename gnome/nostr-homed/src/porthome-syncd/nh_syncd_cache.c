/*
 * nh_syncd_cache.c — content-addressed local blob cache with LRU +
 *                    generation pinning (I3 of Phase 3).
 *
 * SPDX-License-Identifier: MIT
 *
 * Layout under `dir`:
 *   dir/aa/bb/aabbccdd…                (mode 0600)
 *   dir/aa/bb/aabbccdd….tmp.<pid>.<n>  (atomic writer scratch)
 *
 * Small in-memory pin registry (refcounted) protects blobs while the
 * daemon is running; persistent pins live in the generation ring
 * (nh_syncd_pin_ring). Sweep is O(cache); the whole design assumes a
 * quota in the low-GiB range, so a full walk is cheap enough (< 5 ms
 * per 10 000 entries on a warm cache).
 *
 * Bead: nostrc-p6qp.
 */

#define _GNU_SOURCE
#include "nh_syncd_cache.h"
#include "nh_porthome_crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ─── small helpers ───────────────────────────────────────────────── */

static void log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("nh_syncd_cache: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* mkdir -p, mode 0700. */
static int mkdirp(const char *path) {
    if (!path || !*path) return NH_SYNCD_CACHE_ERR_ARG;
    char *dup = xstrdup(path);
    if (!dup) return NH_SYNCD_CACHE_ERR_OOM;
    for (char *p = dup + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dup, NH_SYNCD_CACHE_DIR_MODE) != 0 && errno != EEXIST) {
                free(dup); return NH_SYNCD_CACHE_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdir(dup, NH_SYNCD_CACHE_DIR_MODE) != 0 && errno != EEXIST) {
        free(dup); return NH_SYNCD_CACHE_ERR_IO;
    }
    free(dup);
    return NH_SYNCD_CACHE_OK;
}

static bool is_hex64(const char *s) {
    if (!s) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        if (!c) return false;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[64] == '\0';
}

/* Build "dir/aa/bb/aabbccdd…". `out` must hold at least PATH_MAX bytes. */
static int build_blob_path(const char *dir, const char *hex64,
                           char *out, size_t outn) {
    if (!dir || !hex64 || !is_hex64(hex64) || !out) return NH_SYNCD_CACHE_ERR_ARG;
    int n = snprintf(out, outn, "%s/%c%c/%c%c/%s",
                     dir, hex64[0], hex64[1], hex64[2], hex64[3], hex64);
    if (n < 0 || (size_t)n >= outn) return NH_SYNCD_CACHE_ERR_IO;
    return NH_SYNCD_CACHE_OK;
}

/* Ensure `dir/aa/bb/` exists. */
static int ensure_shard(const char *dir, const char *hex64) {
    char shard[PATH_MAX];
    int n = snprintf(shard, sizeof shard, "%s/%c%c",
                     dir, hex64[0], hex64[1]);
    if (n < 0 || (size_t)n >= sizeof shard) return NH_SYNCD_CACHE_ERR_IO;
    if (mkdir(shard, NH_SYNCD_CACHE_DIR_MODE) != 0 && errno != EEXIST)
        return NH_SYNCD_CACHE_ERR_IO;
    n = snprintf(shard, sizeof shard, "%s/%c%c/%c%c",
                 dir, hex64[0], hex64[1], hex64[2], hex64[3]);
    if (n < 0 || (size_t)n >= sizeof shard) return NH_SYNCD_CACHE_ERR_IO;
    if (mkdir(shard, NH_SYNCD_CACHE_DIR_MODE) != 0 && errno != EEXIST)
        return NH_SYNCD_CACHE_ERR_IO;
    return NH_SYNCD_CACHE_OK;
}

/* ─── path helpers ────────────────────────────────────────────────── */

static char *join_path(const char *a, const char *b) {
    if (!a || !b) return NULL;
    size_t na = strlen(a), nb = strlen(b);
    size_t sep = (na && a[na - 1] == '/') ? 0 : 1;
    char *out = (char *)malloc(na + sep + nb + 1);
    if (!out) return NULL;
    memcpy(out, a, na);
    if (sep) out[na] = '/';
    memcpy(out + na + sep, b, nb);
    out[na + sep + nb] = '\0';
    return out;
}

char *nh_syncd_cache_default_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return join_path(xdg, "nostr-homed/blobs");
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    return join_path(home, ".cache/nostr-homed/blobs");
}

char *nh_syncd_cache_default_pin_path(void) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && *xdg) return join_path(xdg, "nostr-homed/pinned.json");
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    return join_path(home, ".local/state/nostr-homed/pinned.json");
}

uint64_t nh_syncd_cache_default_quota(const char *cache_dir) {
    uint64_t abs_cap = NH_SYNCD_CACHE_DEFAULT_QUOTA_BYTES;
    if (!cache_dir || !*cache_dir) return abs_cap;
    /* Walk up to the first existing ancestor for statvfs. */
    char *dup = xstrdup(cache_dir);
    if (!dup) return abs_cap;
    struct statvfs st;
    int have = -1;
    while (*dup) {
        if (statvfs(dup, &st) == 0) { have = 0; break; }
        char *slash = strrchr(dup, '/');
        if (!slash || slash == dup) { /* try "/" */
            if (statvfs("/", &st) == 0) have = 0;
            break;
        }
        *slash = '\0';
    }
    free(dup);
    if (have != 0) return abs_cap;
    uint64_t fs_bytes = (uint64_t)st.f_frsize * (uint64_t)st.f_blocks;
    uint64_t fs_cap = (fs_bytes / 100ull) * (uint64_t)NH_SYNCD_CACHE_FS_PERCENT;
    return fs_cap < abs_cap ? fs_cap : abs_cap;
}

/* ─────────────── Phase 5 I3 quota override helpers ─────────────── */

static char *quota_config_path_(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char buf[512];
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/%s", xdg, NH_SYNCD_CACHE_QUOTA_CONFIG_REL);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(buf, sizeof buf, "%s/.config/%s", home,
                 NH_SYNCD_CACHE_QUOTA_CONFIG_REL);
    }
    return xstrdup(buf);
}

/* Parse a bytes value from a NUL-terminated string. Trims trailing
 * whitespace/newline. Returns 0 on parse error or values outside the
 * override guard band. */
static uint64_t parse_override_bytes_(const char *raw) {
    if (!raw || !*raw) return 0;
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (!*raw) return 0;
    char *endp = NULL;
    unsigned long long v = strtoull(raw, &endp, 10);
    if (!endp || endp == raw) return 0;
    while (*endp) {
        if (*endp != ' ' && *endp != '\t' && *endp != '\n' &&
            *endp != '\r') return 0;
        ++endp;
    }
    if (v < NH_SYNCD_CACHE_QUOTA_OVERRIDE_MIN) return 0;
    if (v > NH_SYNCD_CACHE_QUOTA_OVERRIDE_MAX) return 0;
    return (uint64_t)v;
}

static uint64_t load_override_from_file_(void) {
    char *p = quota_config_path_();
    if (!p) return 0;
    FILE *f = fopen(p, "re");
    free(p);
    if (!f) return 0;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    (void)n;
    fclose(f);
    return parse_override_bytes_(buf);
}

uint64_t nh_syncd_cache_effective_quota(const char *cache_dir,
                                        const char **out_source) {
    if (out_source) *out_source = "default";
    const char *env = getenv(NH_SYNCD_CACHE_QUOTA_ENV);
    if (env && *env) {
        uint64_t v = parse_override_bytes_(env);
        if (v) {
            if (out_source) *out_source = "env";
            return v;
        }
        log_err("effective_quota: env %s=%s rejected (must be [%llu, %llu])",
                NH_SYNCD_CACHE_QUOTA_ENV, env,
                (unsigned long long)NH_SYNCD_CACHE_QUOTA_OVERRIDE_MIN,
                (unsigned long long)NH_SYNCD_CACHE_QUOTA_OVERRIDE_MAX);
    }
    uint64_t cfg = load_override_from_file_();
    if (cfg) {
        if (out_source) *out_source = "config";
        return cfg;
    }
    return nh_syncd_cache_default_quota(cache_dir);
}

/* ─── pin registry (in-memory refcount table) ─────────────────────── */

typedef struct pin_node {
    struct pin_node *next;
    char hex[65];
    uint32_t refcount;
} pin_node;

#define PIN_BUCKETS 257

static size_t pin_bucket(const char *hex) {
    /* First 8 hex chars → 32 bits; strong enough for a 257-bucket table. */
    uint32_t h = 0;
    for (size_t i = 0; i < 8 && hex[i]; ++i) h = h * 31u + (uint8_t)hex[i];
    return h % PIN_BUCKETS;
}

struct nh_syncd_cache {
    char     *dir;
    uint64_t  quota_bytes;
    /* Phase 5 I3: quota source slug ("env"|"config"|"default") kept
     * for the status writer. Points into rodata; not owned. */
    const char *quota_source;
    /* Thrash detector (§6.5): rolling 1-hour window. */
    unsigned  thrash_threshold;
    unsigned  evict_count;          /* within the current window     */
    int64_t   window_start_epoch;   /* head of the current window    */
    int       window_notified;      /* already fired for this window */
    int       auto_evict_on_put;    /* Phase 5 I3 opt-in            */
    /* Evict-notify seam. */
    nh_syncd_cache_evict_notify_fn evict_notify_fn;
    void                          *evict_notify_ud;
    pin_node *pins[PIN_BUCKETS];
    pthread_mutex_t mu; /* guards pins + rng seed + thrash state */
    uint32_t  rng_state;
};

static pin_node *pin_find(nh_syncd_cache *c, const char *hex) {
    size_t b = pin_bucket(hex);
    for (pin_node *n = c->pins[b]; n; n = n->next) {
        if (memcmp(n->hex, hex, 64) == 0) return n;
    }
    return NULL;
}

int nh_syncd_cache_pin(nh_syncd_cache *c, const char *hex) {
    if (!c || !is_hex64(hex)) return NH_SYNCD_CACHE_ERR_ARG;
    pthread_mutex_lock(&c->mu);
    pin_node *n = pin_find(c, hex);
    if (!n) {
        n = (pin_node *)calloc(1, sizeof *n);
        if (!n) { pthread_mutex_unlock(&c->mu); return NH_SYNCD_CACHE_ERR_OOM; }
        memcpy(n->hex, hex, 65);
        size_t b = pin_bucket(hex);
        n->next = c->pins[b];
        c->pins[b] = n;
    }
    n->refcount++;
    pthread_mutex_unlock(&c->mu);
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_cache_unpin(nh_syncd_cache *c, const char *hex) {
    if (!c || !is_hex64(hex)) return NH_SYNCD_CACHE_ERR_ARG;
    pthread_mutex_lock(&c->mu);
    size_t b = pin_bucket(hex);
    pin_node *prev = NULL;
    for (pin_node *n = c->pins[b]; n; prev = n, n = n->next) {
        if (memcmp(n->hex, hex, 64) == 0) {
            if (n->refcount > 0) n->refcount--;
            if (n->refcount == 0) {
                if (prev) prev->next = n->next; else c->pins[b] = n->next;
                free(n);
            }
            pthread_mutex_unlock(&c->mu);
            return NH_SYNCD_CACHE_OK;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return NH_SYNCD_CACHE_ERR_NOT_FOUND;
}

bool nh_syncd_cache_is_pinned(const nh_syncd_cache *c, const char *hex) {
    if (!c || !is_hex64(hex)) return false;
    nh_syncd_cache *mc = (nh_syncd_cache *)c; /* const-cast for mutex */
    pthread_mutex_lock(&mc->mu);
    pin_node *n = pin_find(mc, hex);
    bool pinned = n && n->refcount > 0;
    pthread_mutex_unlock(&mc->mu);
    return pinned;
}

/* ─── open / close ────────────────────────────────────────────────── */

int nh_syncd_cache_open(const char *dir, uint64_t quota_bytes,
                        nh_syncd_cache **out) {
    if (!dir || !out) return NH_SYNCD_CACHE_ERR_ARG;
    int rc = mkdirp(dir);
    if (rc != NH_SYNCD_CACHE_OK) return rc;
    /* Tighten the top-level dir mode in case it pre-existed with a
     * looser mask. mkdirp() only creates with 0700, but chmod is
     * cheap insurance. */
    (void)chmod(dir, NH_SYNCD_CACHE_DIR_MODE);

    nh_syncd_cache *c = (nh_syncd_cache *)calloc(1, sizeof *c);
    if (!c) return NH_SYNCD_CACHE_ERR_OOM;
    c->dir = xstrdup(dir);
    if (!c->dir) { free(c); return NH_SYNCD_CACHE_ERR_OOM; }
    if (quota_bytes) {
        c->quota_bytes  = quota_bytes;
        c->quota_source = "explicit";
    } else {
        c->quota_bytes  = nh_syncd_cache_effective_quota(dir, &c->quota_source);
    }
    /* Thrash threshold (§6.5). Env override widens or narrows the
     * default. Values outside [1, 100000] are ignored — a runaway env
     * shouldn't silently disable the guard. */
    c->thrash_threshold = NH_SYNCD_CACHE_THRASH_DEFAULT_LIMIT;
    const char *th_env = getenv(NH_SYNCD_CACHE_THRASH_ENV);
    if (th_env && *th_env) {
        char *endp = NULL;
        unsigned long v = strtoul(th_env, &endp, 10);
        if (endp && *endp == '\0' && v >= 1ul && v <= 100000ul)
            c->thrash_threshold = (unsigned)v;
    }
    c->evict_count        = 0;
    c->window_start_epoch = 0;
    c->window_notified    = 0;
    c->evict_notify_fn    = NULL;
    c->evict_notify_ud    = NULL;
    pthread_mutex_init(&c->mu, NULL);
    struct timeval tv; gettimeofday(&tv, NULL);
    c->rng_state = (uint32_t)(tv.tv_usec ^ (getpid() << 16));

    /* Best-effort tmp cleanup at open time. */
    (void)nh_syncd_cache_cleanup_stale_tmps(c);

    fprintf(stderr,
            "nh_syncd_cache: dir=%s quota_bytes=%llu (source=%s) thrash_threshold=%u/hr\n",
            c->dir, (unsigned long long)c->quota_bytes,
            c->quota_source ? c->quota_source : "default",
            c->thrash_threshold);

    *out = c;
    return NH_SYNCD_CACHE_OK;
}

void nh_syncd_cache_close(nh_syncd_cache *c) {
    if (!c) return;
    for (size_t i = 0; i < PIN_BUCKETS; ++i) {
        pin_node *n = c->pins[i];
        while (n) { pin_node *nx = n->next; free(n); n = nx; }
    }
    pthread_mutex_destroy(&c->mu);
    free(c->dir);
    free(c);
}

const char *nh_syncd_cache_dir(const nh_syncd_cache *c) { return c ? c->dir : NULL; }
uint64_t nh_syncd_cache_quota_bytes(const nh_syncd_cache *c) { return c ? c->quota_bytes : 0; }

uint64_t nh_syncd_cache_reload_quota(nh_syncd_cache *c) {
    if (!c) return 0;
    const char *src = "default";
    uint64_t v = nh_syncd_cache_effective_quota(c->dir, &src);
    pthread_mutex_lock(&c->mu);
    c->quota_bytes  = v;
    c->quota_source = src;
    pthread_mutex_unlock(&c->mu);
    fprintf(stderr,
            "nh_syncd_cache: reload dir=%s quota_bytes=%llu (source=%s)\n",
            c->dir, (unsigned long long)v, src);
    return v;
}

void nh_syncd_cache_set_evict_notify(nh_syncd_cache *c,
                                     nh_syncd_cache_evict_notify_fn fn,
                                     void *ud) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->evict_notify_fn = fn;
    c->evict_notify_ud = ud;
    pthread_mutex_unlock(&c->mu);
}

void nh_syncd_cache_set_auto_evict(nh_syncd_cache *c, bool on) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->auto_evict_on_put = on ? 1 : 0;
    pthread_mutex_unlock(&c->mu);
}

unsigned nh_syncd_cache_evict_count_1h(const nh_syncd_cache *c) {
    if (!c) return 0;
    nh_syncd_cache *mc = (nh_syncd_cache *)c;
    pthread_mutex_lock(&mc->mu);
    /* Roll the window forward on read so callers see a fresh number. */
    int64_t now = (int64_t)time(NULL);
    if (mc->window_start_epoch &&
        now - mc->window_start_epoch >= (int64_t)NH_SYNCD_CACHE_THRASH_WINDOW_SECS) {
        mc->evict_count        = 0;
        mc->window_start_epoch = 0;
        mc->window_notified    = 0;
    }
    unsigned v = mc->evict_count;
    pthread_mutex_unlock(&mc->mu);
    return v;
}

unsigned nh_syncd_cache_thrash_threshold(const nh_syncd_cache *c) {
    return c ? c->thrash_threshold : 0u;
}

/* Update the rolling thrash window with `n_new` evictions. Fires the
 * notify hook AT MOST ONCE PER WINDOW when count > threshold. */
static void thrash_note_locked(nh_syncd_cache *c, unsigned n_new) {
    if (n_new == 0) return;
    int64_t now = (int64_t)time(NULL);
    if (!c->window_start_epoch ||
        now - c->window_start_epoch >= (int64_t)NH_SYNCD_CACHE_THRASH_WINDOW_SECS) {
        c->window_start_epoch = now;
        c->evict_count        = 0;
        c->window_notified    = 0;
    }
    c->evict_count += n_new;
    if (!c->window_notified &&
        c->evict_count > c->thrash_threshold &&
        c->evict_notify_fn) {
        nh_syncd_cache_evict_notify_fn fn = c->evict_notify_fn;
        void *ud                          = c->evict_notify_ud;
        unsigned cnt                       = c->evict_count;
        unsigned thr                       = c->thrash_threshold;
        int64_t  start                     = c->window_start_epoch;
        c->window_notified = 1;
        /* Release the lock before firing — the notifier may re-enter
         * status writers or the notification seam that fork()s. */
        pthread_mutex_unlock(&c->mu);
        fn(ud, cnt, thr, start);
        pthread_mutex_lock(&c->mu);
    }
}



/* ─── put / get / has ─────────────────────────────────────────────── */

bool nh_syncd_cache_has(const nh_syncd_cache *c, const char *hex) {
    if (!c || !is_hex64(hex)) return false;
    char path[PATH_MAX];
    if (build_blob_path(c->dir, hex, path, sizeof path) != NH_SYNCD_CACHE_OK)
        return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *nh_syncd_cache_get_path(nh_syncd_cache *c, const char *hex) {
    if (!c || !is_hex64(hex)) return NULL;
    char path[PATH_MAX];
    if (build_blob_path(c->dir, hex, path, sizeof path) != NH_SYNCD_CACHE_OK)
        return NULL;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    /* Touch atime for LRU. utimensat with UTIME_NOW keeps mtime. */
    struct timespec times[2] = {
        { .tv_sec = 0, .tv_nsec = UTIME_NOW },
        { .tv_sec = 0, .tv_nsec = UTIME_OMIT },
    };
    (void)utimensat(AT_FDCWD, path, times, 0);
    return xstrdup(path);
}

static uint32_t rng_next(nh_syncd_cache *c) {
    /* xorshift32 — good enough for a tmp suffix. */
    uint32_t x = c->rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c->rng_state = x;
    return x;
}

int nh_syncd_cache_put(nh_syncd_cache *c, const char *hex,
                       const uint8_t *data, size_t len) {
    if (!c || !is_hex64(hex) || (!data && len)) return NH_SYNCD_CACHE_ERR_ARG;

    /* Verify sha256(data). */
    uint8_t got[32];
    if (nh_porthome_sha256(data, len, got) != 0)
        return NH_SYNCD_CACHE_ERR_OOM;
    char got_hex[65]; nh_porthome_hex64(got, got_hex);
    if (memcmp(got_hex, hex, 64) != 0) {
        log_err("put: hash mismatch (claimed %s, got %s)", hex, got_hex);
        return NH_SYNCD_CACHE_ERR_HASH;
    }

    /* Reject single blobs larger than the quota (a runaway would
     * force us into permanent eviction pressure). */
    if ((uint64_t)len > c->quota_bytes)
        return NH_SYNCD_CACHE_ERR_QUOTA;

    int rc = ensure_shard(c->dir, hex);
    if (rc != NH_SYNCD_CACHE_OK) return rc;

    char target[PATH_MAX];
    rc = build_blob_path(c->dir, hex, target, sizeof target);
    if (rc != NH_SYNCD_CACHE_OK) return rc;

    /* Fast dedup: if the target already exists, our job is done. */
    struct stat st;
    if (stat(target, &st) == 0 && S_ISREG(st.st_mode))
        return NH_SYNCD_CACHE_OK;

    /* tmp path: target + ".tmp.<pid>.<u32hex>". */
    char tmp[PATH_MAX];
    pthread_mutex_lock(&c->mu);
    uint32_t suf = rng_next(c);
    pthread_mutex_unlock(&c->mu);
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%d.%08x",
                     target, (int)getpid(), suf);
    if (n < 0 || (size_t)n >= sizeof tmp) return NH_SYNCD_CACHE_ERR_IO;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  NH_SYNCD_CACHE_FILE_MODE);
    if (fd < 0) {
        log_err("put: open %s: %s", tmp, strerror(errno));
        return NH_SYNCD_CACHE_ERR_IO;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            log_err("put: write %s: %s", tmp, strerror(errno));
            close(fd); unlink(tmp);
            return NH_SYNCD_CACHE_ERR_IO;
        }
        off += (size_t)w;
    }
    /* Enforce mode explicitly (umask may have widened it). */
    if (fchmod(fd, NH_SYNCD_CACHE_FILE_MODE) != 0) {
        log_err("put: fchmod %s: %s", tmp, strerror(errno));
        close(fd); unlink(tmp);
        return NH_SYNCD_CACHE_ERR_IO;
    }
    if (fsync(fd) != 0) {
        log_err("put: fsync %s: %s", tmp, strerror(errno));
        close(fd); unlink(tmp);
        return NH_SYNCD_CACHE_ERR_IO;
    }
    close(fd);

    if (rename(tmp, target) != 0) {
        /* If a concurrent writer beat us, target now exists — success. */
        if (stat(target, &st) == 0 && S_ISREG(st.st_mode)) {
            unlink(tmp);
            goto put_enforce;
        }
        log_err("put: rename %s -> %s: %s", tmp, target, strerror(errno));
        unlink(tmp);
        return NH_SYNCD_CACHE_ERR_IO;
    }
put_enforce:
    /* Phase 5 I3: auto-eviction after put. Two failure modes we
     * surface distinctly:
     *   - sweep couldn't bring us back under quota (all remaining
     *     bytes are pinned) → NH_SYNCD_CACHE_ERR_QUOTA_PINNED
     *   - sweep evicted the blob we just wrote (the only unpinned
     *     candidate, and it happened to be the LRU pick because
     *     the pinned generation set already saturates the cache)
     *     → also NH_SYNCD_CACHE_ERR_QUOTA_PINNED. The caller must
     *     surface LIMITED_MODE and treat this fetch as dropped. */
    if (c->auto_evict_on_put &&
        nh_syncd_cache_used_bytes(c) > c->quota_bytes) {
        size_t   ev = 0;
        uint64_t rc = 0;
        (void)nh_syncd_cache_sweep(c, &ev, &rc);
        if (nh_syncd_cache_used_bytes(c) > c->quota_bytes ||
            !nh_syncd_cache_has(c, hex)) {
            log_err("put: quota %llu exceeded and blob %s could not "
                    "be retained (pins block reclaim)",
                    (unsigned long long)c->quota_bytes, hex);
            return NH_SYNCD_CACHE_ERR_QUOTA_PINNED;
        }
    }
    return NH_SYNCD_CACHE_OK;
}

/* ─── walk / sweep / cleanup ──────────────────────────────────────── */

typedef void (*walk_cb)(void *ud, const char *shard_path, const char *file_name,
                        const struct stat *st);

/* Walk dir/aa/bb/<file>, calling cb for every regular file that's not
 * a `.tmp.*` scratch file. */
static void walk_blobs(const char *dir, walk_cb cb, void *ud) {
    DIR *d1 = opendir(dir);
    if (!d1) return;
    struct dirent *e1;
    char p1[PATH_MAX];
    while ((e1 = readdir(d1))) {
        if (e1->d_name[0] == '.' && (e1->d_name[1] == '\0' ||
            (e1->d_name[1] == '.' && e1->d_name[2] == '\0'))) continue;
        if (strlen(e1->d_name) != 2) continue;
        int n = snprintf(p1, sizeof p1, "%s/%s", dir, e1->d_name);
        if (n < 0 || (size_t)n >= sizeof p1) continue;
        DIR *d2 = opendir(p1);
        if (!d2) continue;
        struct dirent *e2;
        char p2[PATH_MAX];
        while ((e2 = readdir(d2))) {
            if (e2->d_name[0] == '.' && (e2->d_name[1] == '\0' ||
                (e2->d_name[1] == '.' && e2->d_name[2] == '\0'))) continue;
            if (strlen(e2->d_name) != 2) continue;
            n = snprintf(p2, sizeof p2, "%s/%s", p1, e2->d_name);
            if (n < 0 || (size_t)n >= sizeof p2) continue;
            DIR *d3 = opendir(p2);
            if (!d3) continue;
            struct dirent *e3;
            char p3[PATH_MAX];
            while ((e3 = readdir(d3))) {
                if (e3->d_name[0] == '.') continue;
                n = snprintf(p3, sizeof p3, "%s/%s", p2, e3->d_name);
                if (n < 0 || (size_t)n >= sizeof p3) continue;
                struct stat st;
                if (lstat(p3, &st) != 0) continue;
                if (!S_ISREG(st.st_mode)) continue;
                cb(ud, p2, e3->d_name, &st);
            }
            closedir(d3);
        }
        closedir(d2);
    }
    closedir(d1);
}

typedef struct { uint64_t bytes; size_t count; } used_ctx;

static void used_cb(void *ud, const char *shard, const char *name,
                    const struct stat *st) {
    (void)shard;
    used_ctx *u = (used_ctx *)ud;
    /* Skip .tmp.* scratch files: they contribute to disk usage but
     * aren't real blobs and get counted separately. */
    if (strstr(name, ".tmp.")) return;
    u->bytes += (uint64_t)st->st_size;
    u->count++;
}

uint64_t nh_syncd_cache_used_bytes(const nh_syncd_cache *c) {
    if (!c) return 0;
    used_ctx u = { 0, 0 };
    walk_blobs(c->dir, used_cb, &u);
    return u.bytes;
}

typedef struct {
    time_t   atime;
    time_t   mtime;
    uint64_t size;
    char     path[PATH_MAX];
    char     name[128]; /* the 64-hex sha (extra room for safety) */
} lru_entry;

typedef struct {
    lru_entry *arr;
    size_t     len;
    size_t     cap;
    uint64_t   total_bytes;
} lru_ctx;

static void lru_cb(void *ud, const char *shard, const char *name,
                   const struct stat *st) {
    lru_ctx *lc = (lru_ctx *)ud;
    if (strstr(name, ".tmp.")) return;
    if (lc->len == lc->cap) {
        size_t nc = lc->cap ? lc->cap * 2 : 256;
        lru_entry *nx = (lru_entry *)realloc(lc->arr, nc * sizeof *nx);
        if (!nx) return;
        lc->arr = nx; lc->cap = nc;
    }
    lru_entry *e = &lc->arr[lc->len++];
    e->atime = st->st_atime;
    e->mtime = st->st_mtime;
    e->size  = (uint64_t)st->st_size;
    snprintf(e->path, sizeof e->path, "%s/%s", shard, name);
    snprintf(e->name, sizeof e->name, "%s", name);
    lc->total_bytes += e->size;
}

static int lru_cmp(const void *a_, const void *b_) {
    const lru_entry *a = (const lru_entry *)a_;
    const lru_entry *b = (const lru_entry *)b_;
    if (a->atime != b->atime) return (a->atime < b->atime) ? -1 : 1;
    if (a->mtime != b->mtime) return (a->mtime < b->mtime) ? -1 : 1;
    return strcmp(a->name, b->name);
}

int nh_syncd_cache_sweep(nh_syncd_cache *c,
                         size_t *out_evicted, uint64_t *out_reclaimed) {
    if (!c) return NH_SYNCD_CACHE_ERR_ARG;
    if (out_evicted) *out_evicted = 0;
    if (out_reclaimed) *out_reclaimed = 0;

    lru_ctx lc = { NULL, 0, 0, 0 };
    walk_blobs(c->dir, lru_cb, &lc);
    if (lc.total_bytes <= c->quota_bytes) {
        free(lc.arr);
        return NH_SYNCD_CACHE_OK;
    }

    qsort(lc.arr, lc.len, sizeof lc.arr[0], lru_cmp);

    uint64_t need_drop = lc.total_bytes - c->quota_bytes;
    uint64_t reclaimed = 0;
    size_t   evicted = 0;
    for (size_t i = 0; i < lc.len && reclaimed < need_drop; ++i) {
        if (nh_syncd_cache_is_pinned(c, lc.arr[i].name)) continue;
        if (unlink(lc.arr[i].path) == 0) {
            reclaimed += lc.arr[i].size;
            evicted++;
        } else {
            log_err("sweep: unlink %s: %s",
                    lc.arr[i].path, strerror(errno));
        }
    }
    if (out_evicted) *out_evicted = evicted;
    if (out_reclaimed) *out_reclaimed = reclaimed;
    free(lc.arr);
    /* Phase 5 I3: thrash accounting. Runs under the cache mutex so
     * the window rollover and notify decision are consistent. */
    if (evicted) {
        pthread_mutex_lock(&c->mu);
        thrash_note_locked(c, (unsigned)evicted);
        pthread_mutex_unlock(&c->mu);
    }
    return NH_SYNCD_CACHE_OK;
}

typedef struct { time_t cutoff; } tmp_ctx;

static void tmp_cb(void *ud, const char *shard, const char *name,
                   const struct stat *st) {
    tmp_ctx *tc = (tmp_ctx *)ud;
    if (!strstr(name, ".tmp.")) return;
    if (st->st_mtime >= tc->cutoff) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", shard, name);
    if (n < 0 || (size_t)n >= sizeof path) return;
    (void)unlink(path);
}

int nh_syncd_cache_cleanup_stale_tmps(nh_syncd_cache *c) {
    if (!c) return NH_SYNCD_CACHE_ERR_ARG;
    tmp_ctx tc = { time(NULL) - 3600 };
    walk_blobs(c->dir, tmp_cb, &tc);
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_cache_forbid_remote_delete(const char *reason) {
    log_err("BUD-02 DELETE is forbidden in v1 (§6.5): %s",
            reason ? reason : "(no reason given)");
    return NH_SYNCD_CACHE_ERR_ARG;
}
