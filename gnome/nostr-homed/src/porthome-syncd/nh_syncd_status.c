/*
 * nh_syncd_status.c — syncd contribution to porthome-status.json.
 *
 * SPDX-License-Identifier: MIT
 *
 * See nh_syncd_status.h. Deliberately no jansson dep — the body is a
 * hand-serialised object that reuses nh_porthome_json_escape from the
 * common library. Format is stable within v1 (schema field on the
 * outer document; each key body is a flat object).
 *
 * Beads: nostrc-8hw8, nostrc-h10m.1.1.
 */
#define _GNU_SOURCE
#include "nh_syncd_status.h"

#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <inttypes.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t             ts;
    nh_notify_category  cat;
    uint32_t            key_hash8; /* first 32 bits of fnv1a(key)         */
    char                summary[64];
    bool                delivered;
    /* Head-dedup counter (bead nostrc-u40q). A brand-new entry starts
     * at 1; if the next record shares the (cat, key_hash8) of the
     * current head slot we bump this in place and refresh `ts`
     * instead of pushing a new slot. Emitted as an OPTIONAL "count"
     * field on the JSON body — omitted when count==1 so pre-u40q
     * consumers of porthome-status.json.recent[] parse unchanged. */
    uint32_t            count;
} recent_slot;

struct nh_syncd_status_writer {
    pthread_mutex_t     mu;
    /* Snapshot of the current field set. Copied under the lock. */
    nh_syncd_state_slug state;
    uint64_t            last_push_gen;
    uint64_t            last_pull_gen;
    char                last_error[64];
    uint32_t            pinned_count;
    uint64_t            cache_bytes;
    uint64_t            cache_quota;
    char                cache_quota_source[24];
    uint32_t            evict_rate_1h;
    /* xnxd part 1: last push's worst-chunk replication summary. */
    uint32_t            last_upload_servers_ok;
    uint32_t            last_upload_servers_total;
    char                last_upload_error_class[48];
    /* Rolling recent[] — head is the most recent slot. `count` grows
     * up to NH_SYNCD_STATUS_RECENT_CAP and then stays saturated. */
    recent_slot         recent[NH_SYNCD_STATUS_RECENT_CAP];
    unsigned            recent_count;
    unsigned            recent_head; /* index of most-recent slot     */
};

/* FNV-1a — matches the notifier's hashing. Independent copy so
 * this library stays link-safe from callers that don't pull the
 * notifier in. */
static uint32_t fnv1a32(const char *s) {
    uint32_t h = 0x811c9dc5u;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= *p;
        h *= 0x01000193u;
    }
    return h;
}

const char *nh_syncd_status_state_slug(nh_syncd_state_slug s) {
    switch (s) {
        case NH_SYNCD_STATE_IDLE:        return "idle";
        case NH_SYNCD_STATE_PULLING:     return "pulling";
        case NH_SYNCD_STATE_PUSHING:     return "pushing";
        case NH_SYNCD_STATE_RECONCILING: return "reconciling";
        case NH_SYNCD_STATE_LIMITED:     return "limited";
        case NH_SYNCD_STATE_OFFLINE:     return "offline";
        case NH_SYNCD_STATE_ERROR:       return "error";
    }
    return "idle";
}

nh_syncd_status_writer *nh_syncd_status_writer_new(void) {
    nh_syncd_status_writer *w = calloc(1, sizeof *w);
    if (!w) {
        fprintf(stderr, "nh_syncd_status_writer_new: OOM\n");
        abort();
    }
    pthread_mutex_init(&w->mu, NULL);
    w->state = NH_SYNCD_STATE_IDLE;
    snprintf(w->cache_quota_source, sizeof w->cache_quota_source, "default");
    return w;
}

void nh_syncd_status_writer_free(nh_syncd_status_writer *w) {
    if (!w) return;
    pthread_mutex_destroy(&w->mu);
    free(w);
}

/* ─────────────── field setters ─────────────── */

#define WITH_LOCK(w, body) do { \
    if (!(w)) return; \
    pthread_mutex_lock(&(w)->mu); \
    body; \
    pthread_mutex_unlock(&(w)->mu); \
} while (0)

void nh_syncd_status_set_state(nh_syncd_status_writer *w,
                               nh_syncd_state_slug s) {
    WITH_LOCK(w, { w->state = s; });
}
void nh_syncd_status_set_last_push_gen(nh_syncd_status_writer *w,
                                       uint64_t gen) {
    WITH_LOCK(w, { w->last_push_gen = gen; });
}
void nh_syncd_status_set_last_pull_gen(nh_syncd_status_writer *w,
                                       uint64_t gen) {
    WITH_LOCK(w, { w->last_pull_gen = gen; });
}
void nh_syncd_status_set_last_error(nh_syncd_status_writer *w,
                                    const char *class_slug) {
    WITH_LOCK(w, {
        snprintf(w->last_error, sizeof w->last_error, "%s",
                 class_slug ? class_slug : "");
    });
}
void nh_syncd_status_set_pinned_count(nh_syncd_status_writer *w,
                                      uint32_t n) {
    WITH_LOCK(w, { w->pinned_count = n; });
}
void nh_syncd_status_set_cache_bytes(nh_syncd_status_writer *w,
                                     uint64_t bytes) {
    WITH_LOCK(w, { w->cache_bytes = bytes; });
}
void nh_syncd_status_set_cache_quota(nh_syncd_status_writer *w,
                                     uint64_t bytes,
                                     const char *source_slug) {
    WITH_LOCK(w, {
        w->cache_quota = bytes;
        snprintf(w->cache_quota_source, sizeof w->cache_quota_source,
                 "%s", source_slug ? source_slug : "default");
    });
}
void nh_syncd_status_set_evict_rate(nh_syncd_status_writer *w,
                                    uint32_t per_hour) {
    WITH_LOCK(w, { w->evict_rate_1h = per_hour; });
}
void nh_syncd_status_set_last_upload_servers(nh_syncd_status_writer *w,
                                             uint32_t ok, uint32_t total) {
    WITH_LOCK(w, {
        w->last_upload_servers_ok    = ok;
        w->last_upload_servers_total = total;
    });
}
void nh_syncd_status_set_last_upload_error_class(nh_syncd_status_writer *w,
                                                 const char *class_slug) {
    WITH_LOCK(w, {
        snprintf(w->last_upload_error_class,
                 sizeof w->last_upload_error_class,
                 "%s", class_slug ? class_slug : "");
    });
}

/* ─────────────── recent[] append ─────────────── */

static void recent_append_locked(nh_syncd_status_writer *w,
                                 int64_t ts,
                                 nh_notify_category cat,
                                 const char *key,
                                 const char *summary,
                                 bool delivered) {
    uint32_t hash = fnv1a32(key ? key : "");
    /* Head-dedup (bead nostrc-u40q): if the incoming record has the
     * SAME (cat, key_hash8) as the current head slot, bump its count
     * and refresh ts instead of pushing a new slot. Keeps a tight
     * throttled burst (e.g. 20 sweep-dropped repeats) from wiping the
     * ring of every other event. Summary + delivered on the head
     * slot are also refreshed to the newest values so the UI keeps
     * showing what happened most recently. */
    if (w->recent_count > 0) {
        recent_slot *head = &w->recent[w->recent_head];
        if (head->cat == cat && head->key_hash8 == hash) {
            head->ts        = ts;
            head->delivered = delivered;
            snprintf(head->summary, sizeof head->summary, "%s",
                     summary ? summary : "");
            /* Saturate at UINT32_MAX in the vanishingly unlikely
             * event of overflow — the ring is short-lived. */
            if (head->count < 0xffffffffu) head->count++;
            else                            head->count = 0xffffffffu;
            return;
        }
    }
    unsigned next = (w->recent_head + 1u) % NH_SYNCD_STATUS_RECENT_CAP;
    /* On the very first insertion, keep head at slot 0. */
    unsigned target = (w->recent_count == 0) ? 0u : next;
    recent_slot *s = &w->recent[target];
    s->ts        = ts;
    s->cat       = cat;
    s->key_hash8 = hash;
    snprintf(s->summary, sizeof s->summary, "%s",
             summary ? summary : "");
    s->delivered = delivered;
    s->count     = 1;
    w->recent_head = target;
    if (w->recent_count < NH_SYNCD_STATUS_RECENT_CAP) w->recent_count++;
}

void nh_syncd_status_notify_record(void *ud,
                                   int64_t epoch_secs,
                                   nh_notify_category cat,
                                   const char *key,
                                   const char *summary,
                                   const char *body,
                                   bool delivered) {
    (void)body;
    nh_syncd_status_writer *w = ud;
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    recent_append_locked(w, epoch_secs, cat, key, summary, delivered);
    pthread_mutex_unlock(&w->mu);
    /* Re-emit so the CLI sees the new entry within one poll tick. */
    (void)nh_syncd_status_emit(w, NULL);
}

/* ─────────────── body serialiser ─────────────── */

/* Append a formatted chunk into (buf, cap) starting at *off. Returns 0
 * on success, -1 on overflow (buffer stays intact for the caller). */
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

int nh_syncd_status_emit(nh_syncd_status_writer *w, const char *path) {
    if (!w) return -1;

    /* Snapshot under the lock so the render is consistent. */
    pthread_mutex_lock(&w->mu);
    nh_syncd_state_slug state = w->state;
    uint64_t             lpg  = w->last_push_gen;
    uint64_t             lpg2 = w->last_pull_gen;
    char                 err[64];
    memcpy(err, w->last_error, sizeof err);
    uint32_t pinned = w->pinned_count;
    uint64_t cb     = w->cache_bytes;
    uint64_t cq     = w->cache_quota;
    char qs[24];
    memcpy(qs, w->cache_quota_source, sizeof qs);
    uint32_t evr    = w->evict_rate_1h;
    uint32_t upok   = w->last_upload_servers_ok;
    uint32_t uptot  = w->last_upload_servers_total;
    char     upcls[48];
    memcpy(upcls, w->last_upload_error_class, sizeof upcls);
    unsigned rc     = w->recent_count;
    unsigned head   = w->recent_head;
    recent_slot recent[NH_SYNCD_STATUS_RECENT_CAP];
    memcpy(recent, w->recent, sizeof recent);
    pthread_mutex_unlock(&w->mu);

    char err_esc[128]  = {0};
    char qs_esc[64]    = {0};
    char upcls_esc[128] = {0};
    (void)nh_porthome_json_escape(err, err_esc, sizeof err_esc);
    (void)nh_porthome_json_escape(qs,  qs_esc,  sizeof qs_esc);
    (void)nh_porthome_json_escape(upcls, upcls_esc, sizeof upcls_esc);

    char body[2560];
    size_t off = 0;
    if (appendf(body, sizeof body, &off,
                "{\"state\":\"%s\","
                "\"last_push_gen\":%" PRIu64 ","
                "\"last_pull_gen\":%" PRIu64 ","
                "\"last_error_class\":\"%s\","
                "\"pinned_gen_count\":%u,"
                "\"cache_bytes\":%" PRIu64 ","
                "\"cache_quota_bytes\":%" PRIu64 ","
                "\"cache_quota_source\":\"%s\","
                "\"evict_rate_1h\":%u,"
                "\"last_upload_servers_ok\":%u,"
                "\"last_upload_servers_total\":%u,"
                "\"last_upload_error_class\":\"%s\","
                "\"recent\":[",
                nh_syncd_status_state_slug(state),
                lpg, lpg2, err_esc, pinned, cb, cq, qs_esc, evr,
                upok, uptot, upcls_esc) != 0)
        return -1;

    /* Emit recent[] most-recent-first. Walk backward from `head`. */
    for (unsigned i = 0; i < rc; ++i) {
        unsigned idx = (head + NH_SYNCD_STATUS_RECENT_CAP - i)
                       % NH_SYNCD_STATUS_RECENT_CAP;
        const recent_slot *s = &recent[idx];
        char sum_esc[128];
        (void)nh_porthome_json_escape(s->summary, sum_esc, sizeof sum_esc);
        if (s->count > 1u) {
            if (appendf(body, sizeof body, &off,
                        "%s{\"ts\":%" PRId64
                        ",\"cat\":\"%s\","
                        "\"key_hash8\":\"%08x\","
                        "\"summary\":\"%s\","
                        "\"delivered\":%s,"
                        "\"count\":%u}",
                        i ? "," : "",
                        (int64_t)s->ts,
                        nh_porthome_notify_category_slug(s->cat),
                        (unsigned)s->key_hash8,
                        sum_esc,
                        s->delivered ? "true" : "false",
                        (unsigned)s->count) != 0)
                return -1;
        } else {
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
    }
    if (appendf(body, sizeof body, &off, "]}") != 0) return -1;

    if (path)
        return nh_porthome_status_write_key(path, "syncd", body);
    return nh_porthome_status_write_key_default("syncd", body);
}
