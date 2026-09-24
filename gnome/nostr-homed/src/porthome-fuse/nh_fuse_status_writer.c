/*
 * nh_fuse_status_writer.c — see nh_fuse_status_writer.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Bead: nostrc-k4j4.
 */
#define _GNU_SOURCE
#include "nh_fuse_status_writer.h"

#include "nh_fuse_status.h"
#include "nh_porthome_status.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t             ts;
    nh_notify_category  cat;
    uint32_t            key_hash8;
    char                summary[64];
    bool                delivered;
} recent_slot;

struct nh_fuse_status_writer {
    pthread_mutex_t     mu;
    bool                mounted;
    char                mountpoint[512];
    uint64_t            generation;
    uint64_t            cache_bytes;
    nh_fuse_source_stats_t stats;
    char                last_error_class[64];
    int64_t             last_error_ts;
    uint32_t            evict_rate_1h;

    recent_slot         recent[NH_FUSE_STATUS_RECENT_CAP];
    unsigned            recent_count;
    unsigned            recent_head;
};

/* FNV-1a — independent copy so this TU is link-safe from callers that
 * don't pull the notifier internals in. Matches the syncd side. */
static uint32_t fnv1a32(const char *s) {
    uint32_t h = 0x811c9dc5u;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= *p;
        h *= 0x01000193u;
    }
    return h;
}

nh_fuse_status_writer *nh_fuse_status_writer_new(void) {
    nh_fuse_status_writer *w = calloc(1, sizeof *w);
    if (!w) {
        fprintf(stderr, "nh_fuse_status_writer_new: OOM\n");
        abort();
    }
    pthread_mutex_init(&w->mu, NULL);
    return w;
}

void nh_fuse_status_writer_free(nh_fuse_status_writer *w) {
    if (!w) return;
    pthread_mutex_destroy(&w->mu);
    free(w);
}

#define WITH_LOCK(w, body) do { \
    if (!(w)) return; \
    pthread_mutex_lock(&(w)->mu); \
    body; \
    pthread_mutex_unlock(&(w)->mu); \
} while (0)

void nh_fuse_status_writer_set_mounted(nh_fuse_status_writer *w, bool m) {
    WITH_LOCK(w, { w->mounted = m; });
}
void nh_fuse_status_writer_set_mountpoint(nh_fuse_status_writer *w,
                                          const char *mp) {
    WITH_LOCK(w, {
        snprintf(w->mountpoint, sizeof w->mountpoint, "%s", mp ? mp : "");
    });
}
void nh_fuse_status_writer_set_generation(nh_fuse_status_writer *w, uint64_t g) {
    WITH_LOCK(w, { w->generation = g; });
}
void nh_fuse_status_writer_set_cache_bytes(nh_fuse_status_writer *w, uint64_t b) {
    WITH_LOCK(w, { w->cache_bytes = b; });
}
void nh_fuse_status_writer_set_stats(nh_fuse_status_writer *w,
                                     const nh_fuse_source_stats_t *st) {
    WITH_LOCK(w, {
        if (st) w->stats = *st;
        else    memset(&w->stats, 0, sizeof w->stats);
    });
}
void nh_fuse_status_writer_set_last_error(nh_fuse_status_writer *w,
                                          const char *class_slug, int64_t ts) {
    WITH_LOCK(w, {
        snprintf(w->last_error_class, sizeof w->last_error_class, "%s",
                 class_slug ? class_slug : "");
        w->last_error_ts = ts;
    });
}
void nh_fuse_status_writer_set_evict_rate(nh_fuse_status_writer *w, uint32_t r) {
    WITH_LOCK(w, { w->evict_rate_1h = r; });
}

unsigned nh_fuse_status_writer_recent_count(const nh_fuse_status_writer *w) {
    if (!w) return 0;
    /* const cast for mutex — the mu is a mutable field. */
    pthread_mutex_t *mu = (pthread_mutex_t *)&w->mu;
    pthread_mutex_lock(mu);
    unsigned n = w->recent_count;
    pthread_mutex_unlock(mu);
    return n;
}

static void recent_append_locked(nh_fuse_status_writer *w,
                                 int64_t ts,
                                 nh_notify_category cat,
                                 const char *key,
                                 const char *summary,
                                 bool delivered) {
    unsigned next = (w->recent_head + 1u) % NH_FUSE_STATUS_RECENT_CAP;
    unsigned target = (w->recent_count == 0) ? 0u : next;
    recent_slot *s = &w->recent[target];
    s->ts        = ts;
    s->cat       = cat;
    s->key_hash8 = fnv1a32(key ? key : "");
    snprintf(s->summary, sizeof s->summary, "%s", summary ? summary : "");
    s->delivered = delivered;
    w->recent_head = target;
    if (w->recent_count < NH_FUSE_STATUS_RECENT_CAP) w->recent_count++;
}

void nh_fuse_status_writer_notify_record(void *ud,
                                         int64_t epoch_secs,
                                         nh_notify_category cat,
                                         const char *key,
                                         const char *summary,
                                         const char *body,
                                         bool delivered) {
    (void)body;
    nh_fuse_status_writer *w = ud;
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    recent_append_locked(w, epoch_secs, cat, key, summary, delivered);
    pthread_mutex_unlock(&w->mu);
    /* Re-emit so operators see the new entry within one poll tick. */
    (void)nh_fuse_status_writer_emit(w, NULL);
}

static int appendf(char *buf, size_t cap, size_t *off,
                   const char *fmt, ...) {
    if (*off >= cap) return -1;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= cap - *off) return -1;
    *off += (size_t)n;
    return 0;
}

int nh_fuse_status_writer_emit(nh_fuse_status_writer *w, const char *path) {
    if (!w) return -1;

    /* Snapshot fields under the lock. */
    pthread_mutex_lock(&w->mu);
    bool mounted = w->mounted;
    uint64_t gen = w->generation;
    uint64_t cb  = w->cache_bytes;
    nh_fuse_source_stats_t st = w->stats;
    char mp[512]; memcpy(mp, w->mountpoint, sizeof mp);
    char err[64]; memcpy(err, w->last_error_class, sizeof err);
    int64_t err_ts = w->last_error_ts;
    uint32_t evr = w->evict_rate_1h;
    unsigned rc  = w->recent_count;
    unsigned head = w->recent_head;
    recent_slot recent[NH_FUSE_STATUS_RECENT_CAP];
    memcpy(recent, w->recent, sizeof recent);
    pthread_mutex_unlock(&w->mu);

    char mp_esc[1024]  = {0};
    char err_esc[128]  = {0};
    (void)nh_porthome_json_escape(mp,  mp_esc,  sizeof mp_esc);
    (void)nh_porthome_json_escape(err, err_esc, sizeof err_esc);

    char body[4096];
    size_t off = 0;
    if (appendf(body, sizeof body, &off,
                "{\"mounted\":%s,"
                "\"mountpoint\":\"%s\","
                "\"generation\":%" PRIu64 ","
                "\"cache_bytes\":%" PRIu64 ","
                "\"hits_local\":%" PRIu64 ","
                "\"hits_cache\":%" PRIu64 ","
                "\"fetches\":%" PRIu64 ","
                "\"misses\":%" PRIu64 ","
                "\"last_miss_epoch\":%" PRIu64 ","
                "\"last_error_class\":\"%s\","
                "\"last_error_ts\":%" PRId64 ","
                "\"evict_rate_1h\":%u,"
                "\"recent\":[",
                mounted ? "true" : "false",
                mp_esc,
                gen, cb,
                st.hits_local, st.hits_cache, st.fetches, st.misses,
                st.last_miss_epoch,
                err_esc, err_ts, evr) != 0)
        return -1;

    for (unsigned i = 0; i < rc; ++i) {
        unsigned idx = (head + NH_FUSE_STATUS_RECENT_CAP - i)
                       % NH_FUSE_STATUS_RECENT_CAP;
        const recent_slot *s = &recent[idx];
        char sum_esc[128];
        (void)nh_porthome_json_escape(s->summary, sum_esc, sizeof sum_esc);
        if (appendf(body, sizeof body, &off,
                    "%s{\"ts\":%" PRId64
                    ",\"cat\":\"%s\","
                    "\"key_hash8\":\"%08x\","
                    "\"summary\":\"%s\","
                    "\"delivered\":%s}",
                    i ? "," : "",
                    (int64_t)s->ts,
                    nh_porthome_notify_category_slug(s->cat),
                    (unsigned)s->key_hash8,
                    sum_esc,
                    s->delivered ? "true" : "false") != 0)
            return -1;
    }
    if (appendf(body, sizeof body, &off, "]}") != 0) return -1;

    if (path)
        return nh_porthome_status_write_key(path, "fuse", body);
    return nh_porthome_status_write_key_default("fuse", body);
}
