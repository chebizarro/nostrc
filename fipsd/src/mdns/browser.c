/* DNS-SD browser: record cache, resolution and continuous querying. */
#include "mdns/browser.h"
#include <stdlib.h>
#include <string.h>

#define TTL_CEILING_SECS 86400u   /* a longer TTL is held for a day */
#define GOODBYE_GRACE_MS 1000u
#define FLUSH_GRACE_MS 1000u
#define MISSING_ATTEMPTS 3u

typedef struct entry {
    bool used;
    bool stale;               /* goodbye or flushed: out of resolution, expires soon */
    fipsd_mdns_rr rr;         /* rr.ttl: the TTL received, in seconds */
    uint64_t received_ms, expires_ms;
    uint8_t refresh_stage;    /* refresh queries sent at 80/85/90/95% */
    uint8_t jitter_pm;        /* 0-20 per mille added to each refresh point */
    uint32_t scope;           /* AAAA: the arrival interface, part of the key */
    uint32_t ifaces[FIPSD_MDNS_MAX_IFACES];
    size_t iface_count;
} entry;
typedef struct instance {
    bool used;
    fipsd_mdns_name name;
    bool reported, changed, removed;
    uint64_t signature;
    uint8_t missing_attempts;
    uint64_t missing_next_ms; /* 0: no missing-record query scheduled */
} instance;
struct fipsd_mdns_browser {
    fipsd_mdns_name service;
    fipsd_mdns_rng rng;
    uint64_t now_ms, next_query_ms, interval_ms;
    entry cache[FIPSD_MDNS_CACHE_MAX];
    instance inst[FIPSD_MDNS_INSTANCES_MAX];
};

static uint32_t uniform(fipsd_mdns_browser *b, uint32_t bound) { return bound ? b->rng.uniform(b->rng.user, bound) : 0; }
static uint64_t ttl_ms(const entry *e) { return (uint64_t)e->rr.ttl * 1000u; }

fipsd_mdns_browser *fipsd_mdns_browser_create(const char *service_type, fipsd_mdns_rng rng, uint64_t now_ms)
{
    fipsd_mdns_browser *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    if (!fipsd_mdns_name_from_text(service_type, &b->service) || !rng.uniform) { free(b); return NULL; }
    b->rng = rng;
    fipsd_mdns_browser_restart(b, now_ms);
    return b;
}
void fipsd_mdns_browser_destroy(fipsd_mdns_browser *b) { free(b); }
void fipsd_mdns_browser_restart(fipsd_mdns_browser *b, uint64_t now_ms)
{
    b->now_ms = now_ms;
    b->next_query_ms = now_ms + 20 + uniform(b, 101);
    b->interval_ms = 1000;
}

static bool live(const entry *e) { return e->used && !e->stale; }
static const entry *latest(const fipsd_mdns_browser *b, const fipsd_mdns_name *name, uint16_t type)
{
    const entry *best = NULL;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        const entry *e = &b->cache[i];
        if (live(e) && e->rr.rtype == type && fipsd_mdns_name_equal(&e->rr.name, name) &&
            (!best || e->received_ms > best->received_ms)) best = e;
    }
    return best;
}
static bool has_live_ptr_to(const fipsd_mdns_browser *b, const fipsd_mdns_name *instance_name)
{
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        const entry *e = &b->cache[i]; fipsd_mdns_name t;
        if (live(e) && e->rr.rtype == FIPSD_MDNS_TYPE_PTR && fipsd_mdns_ptr_target(&e->rr, &t) &&
            fipsd_mdns_name_equal(&t, instance_name)) return true;
    }
    return false;
}
/* Whether name is the target of a live SRV of an instance with a live PTR. */
static bool wanted_host(const fipsd_mdns_browser *b, const fipsd_mdns_name *name)
{
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        const entry *e = &b->cache[i]; uint16_t port; fipsd_mdns_name target;
        if (live(e) && e->rr.rtype == FIPSD_MDNS_TYPE_SRV && fipsd_mdns_srv_fields(&e->rr, &port, &target) &&
            fipsd_mdns_name_equal(&target, name)) return true;
    }
    return false;
}
static bool wanted(const fipsd_mdns_browser *b, const entry *e)
{
    switch (e->rr.rtype) {
    case FIPSD_MDNS_TYPE_PTR: return fipsd_mdns_name_equal(&e->rr.name, &b->service);
    case FIPSD_MDNS_TYPE_SRV: case FIPSD_MDNS_TYPE_TXT: return has_live_ptr_to(b, &e->rr.name);
    case FIPSD_MDNS_TYPE_A: case FIPSD_MDNS_TYPE_AAAA: return wanted_host(b, &e->rr.name);
    default: return false;
    }
}
static void add_iface(entry *e, uint32_t ifindex)
{
    if (!ifindex) return;
    for (size_t i = 0; i < e->iface_count; ++i) if (e->ifaces[i] == ifindex) return;
    if (e->iface_count < FIPSD_MDNS_MAX_IFACES) e->ifaces[e->iface_count++] = ifindex;
}
static bool same_key(const entry *e, const fipsd_mdns_rr *rr, uint32_t ifindex)
{
    return e->used && fipsd_mdns_rr_same(&e->rr, rr) && (rr->rtype != FIPSD_MDNS_TYPE_AAAA || e->scope == ifindex);
}
static entry *slot(fipsd_mdns_browser *b)
{
    entry *victim = NULL;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        entry *e = &b->cache[i];
        if (!e->used) return e;
        if (!victim || e->expires_ms < victim->expires_ms) victim = e;
    }
    return victim; /* full: the record closest to expiry gives way */
}
static void store(fipsd_mdns_browser *b, const fipsd_mdns_rr *rr, uint32_t ifindex, uint64_t now)
{
    uint32_t ttl = rr->ttl > TTL_CEILING_SECS ? TTL_CEILING_SECS : rr->ttl;
    if (rr->cache_flush && ttl) {
        for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
            entry *e = &b->cache[i];
            if (!e->used || e->rr.rtype != rr->rtype || !fipsd_mdns_name_equal(&e->rr.name, &rr->name)) continue;
            if (same_key(e, rr, ifindex) || e->received_ms + FLUSH_GRACE_MS > now) continue;
            e->stale = true;
            if (e->expires_ms > now + FLUSH_GRACE_MS) e->expires_ms = now + FLUSH_GRACE_MS;
        }
    }
    entry *found = NULL;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX && !found; ++i) if (same_key(&b->cache[i], rr, ifindex)) found = &b->cache[i];
    if (!ttl) {
        if (found) {
            found->stale = true;
            if (found->expires_ms > now + GOODBYE_GRACE_MS) found->expires_ms = now + GOODBYE_GRACE_MS;
        }
        return;
    }
    if (!found) {
        found = slot(b);
        memset(found, 0, sizeof *found);
        found->used = true;
        found->rr = *rr;
        found->rr.cache_flush = false;
        found->scope = rr->rtype == FIPSD_MDNS_TYPE_AAAA ? ifindex : 0;
        found->jitter_pm = (uint8_t)uniform(b, 21);
    }
    found->stale = false;
    found->rr.ttl = ttl;
    found->received_ms = now;
    found->expires_ms = now + (uint64_t)ttl * 1000u;
    found->refresh_stage = 0;
    add_iface(found, ifindex);
}

/* FNV-1a over the resolution's fields. */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *s = p;
    for (size_t i = 0; i < n; ++i) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}
static int addr_order(const void *pa, const void *pb)
{
    const fipsd_mdns_scoped_ip *a = pa, *b = pb;
    if (a->ip.width != b->ip.width) return a->ip.width < b->ip.width ? -1 : 1;
    int c = memcmp(a->ip.bytes, b->ip.bytes, a->ip.width);
    if (c) return c;
    return a->scope == b->scope ? 0 : a->scope < b->scope ? -1 : 1;
}
static bool resolve(const fipsd_mdns_browser *b, const instance *in, fipsd_mdns_resolved *out, uint64_t *sig,
                    bool *missing_srv, bool *missing_txt, fipsd_mdns_name *host)
{
    const entry *srv = latest(b, &in->name, FIPSD_MDNS_TYPE_SRV), *txt = latest(b, &in->name, FIPSD_MDNS_TYPE_TXT);
    if (missing_srv) *missing_srv = !srv;
    if (missing_txt) *missing_txt = !txt;
    fipsd_mdns_name target = {0};
    uint16_t port = 0;
    if (srv && !fipsd_mdns_srv_fields(&srv->rr, &port, &target)) srv = NULL;
    if (host) *host = target;
    if (!srv || !txt) return false;
    memset(out, 0, offsetof(fipsd_mdns_resolved, addrs));
    out->addr_count = 0;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX && out->addr_count < FIPSD_MDNS_MAX_ADDRS; ++i) {
        const entry *e = &b->cache[i];
        if (!live(e) || (e->rr.rtype != FIPSD_MDNS_TYPE_A && e->rr.rtype != FIPSD_MDNS_TYPE_AAAA) ||
            !fipsd_mdns_name_equal(&e->rr.name, &target)) continue;
        fipsd_mdns_scoped_ip *a = &out->addrs[out->addr_count++];
        memset(a, 0, sizeof *a);
        a->ip.width = (uint8_t)e->rr.rdlen; memcpy(a->ip.bytes, e->rr.rdata, e->rr.rdlen);
        a->scope = e->scope;
        memcpy(a->ifaces, e->ifaces, sizeof a->ifaces); a->iface_count = e->iface_count;
    }
    if (!out->addr_count) return false;
    qsort(out->addrs, out->addr_count, sizeof out->addrs[0], addr_order);
    (void)fipsd_mdns_name_to_text(&in->name, out->fullname, sizeof out->fullname);
    (void)fipsd_mdns_name_to_text(&target, out->host, sizeof out->host);
    out->port = port;
    out->txt_len = txt->rr.rdlen; memcpy(out->txt, txt->rr.rdata, txt->rr.rdlen);
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &port, sizeof port);
    for (size_t i = 0; i < target.len; ++i) { uint8_t c = target.wire[i]; c = c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; h = fnv(h, &c, 1); }
    h = fnv(h, out->txt, out->txt_len);
    for (size_t i = 0; i < out->addr_count; ++i) {
        const fipsd_mdns_scoped_ip *a = &out->addrs[i];
        h = fnv(h, &a->ip, sizeof a->ip); h = fnv(h, &a->scope, sizeof a->scope);
        h = fnv(h, a->ifaces, a->iface_count * sizeof a->ifaces[0]);
    }
    *sig = h;
    return true;
}
static instance *find_instance(fipsd_mdns_browser *b, const fipsd_mdns_name *name, bool create)
{
    instance *free_slot = NULL;
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i) {
        instance *in = &b->inst[i];
        if (in->used && fipsd_mdns_name_equal(&in->name, name)) return in;
        if (!in->used && !free_slot) free_slot = in;
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true; free_slot->name = *name;
    return free_slot;
}
static void recompute(fipsd_mdns_browser *b, uint64_t now)
{
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        const entry *e = &b->cache[i]; fipsd_mdns_name t;
        if (live(e) && e->rr.rtype == FIPSD_MDNS_TYPE_PTR && fipsd_mdns_ptr_target(&e->rr, &t)) (void)find_instance(b, &t, true);
    }
    fipsd_mdns_resolved *scratch = malloc(sizeof *scratch);
    if (!scratch) return;
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i) {
        instance *in = &b->inst[i];
        if (!in->used) continue;
        uint64_t sig = 0;
        if (!has_live_ptr_to(b, &in->name)) {
            in->changed = false;
            if (in->reported) { in->removed = true; in->reported = false; }
            else if (!in->removed) in->used = false;
            continue;
        }
        if (resolve(b, in, scratch, &sig, NULL, NULL, NULL)) {
            in->missing_attempts = 0; in->missing_next_ms = 0;
            if (!in->reported || sig != in->signature) in->changed = true;
        } else {
            in->changed = false;
            if (in->reported) { in->reported = false; in->removed = true; }
            if (!in->missing_attempts && !in->missing_next_ms) in->missing_next_ms = now;
        }
    }
    free(scratch);
}
void fipsd_mdns_browser_ingest(fipsd_mdns_browser *b, const fipsd_mdns_message *msg, uint32_t ifindex, uint64_t now_ms)
{
    b->now_ms = now_ms;
    if (!fipsd_mdns_is_response(msg)) return;
    const fipsd_mdns_rr *sets[2] = {fipsd_mdns_answers(msg), fipsd_mdns_additional(msg)};
    size_t counts[2] = {msg->ancount, msg->arcount};
    char label[64];
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 0; s < 2; ++s)
            for (size_t i = 0; i < counts[s]; ++i) {
                const fipsd_mdns_rr *rr = &sets[s][i];
                if (rr->rclass != FIPSD_MDNS_CLASS_IN) continue;
                bool keep = false;
                if (!pass) {
                    if (rr->rtype == FIPSD_MDNS_TYPE_PTR) keep = fipsd_mdns_name_equal(&rr->name, &b->service);
                    else if (rr->rtype == FIPSD_MDNS_TYPE_SRV || rr->rtype == FIPSD_MDNS_TYPE_TXT)
                        keep = fipsd_mdns_name_child_of(&rr->name, &b->service, label);
                } else if (rr->rtype == FIPSD_MDNS_TYPE_A || rr->rtype == FIPSD_MDNS_TYPE_AAAA) {
                    keep = wanted_host(b, &rr->name);
                }
                if (keep) store(b, rr, ifindex, now_ms);
            }
    recompute(b, now_ms);
}
void fipsd_mdns_browser_expire(fipsd_mdns_browser *b, uint64_t now_ms)
{
    b->now_ms = now_ms;
    bool any = false;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i)
        if (b->cache[i].used && b->cache[i].expires_ms <= now_ms) { b->cache[i].used = false; any = true; }
    if (any) recompute(b, now_ms);
}
void fipsd_mdns_browser_forget_interface(fipsd_mdns_browser *b, uint32_t ifindex)
{
    if (!ifindex) return;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        entry *e = &b->cache[i];
        if (!e->used) continue;
        if (e->rr.rtype == FIPSD_MDNS_TYPE_AAAA && e->scope == ifindex) { e->used = false; continue; }
        size_t n = 0;
        for (size_t k = 0; k < e->iface_count; ++k) if (e->ifaces[k] != ifindex) e->ifaces[n++] = e->ifaces[k];
        if (e->iface_count && !n) e->used = false;
        e->iface_count = n;
    }
    recompute(b, b->now_ms);
}
bool fipsd_mdns_browser_next_resolved(fipsd_mdns_browser *b, fipsd_mdns_resolved *out)
{
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i) {
        instance *in = &b->inst[i];
        if (!in->used || !in->changed) continue;
        in->changed = false;
        uint64_t sig;
        if (!resolve(b, in, out, &sig, NULL, NULL, NULL)) continue;
        in->signature = sig; in->reported = true;
        return true;
    }
    return false;
}
bool fipsd_mdns_browser_next_removed(fipsd_mdns_browser *b, char *fullname, size_t cap)
{
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i) {
        instance *in = &b->inst[i];
        if (!in->used || !in->removed) continue;
        (void)fipsd_mdns_name_to_text(&in->name, fullname, cap);
        in->removed = false;
        if (!has_live_ptr_to(b, &in->name)) in->used = false;
        return true;
    }
    return false;
}
static uint64_t refresh_at(const entry *e)
{
    uint64_t t = ttl_ms(e);
    return e->received_ms + t * (80u + 5u * e->refresh_stage) / 100u + t * e->jitter_pm / 1000u;
}
typedef struct qlist { fipsd_mdns_name names[FIPSD_MDNS_MAX_QUESTIONS]; uint16_t types[FIPSD_MDNS_MAX_QUESTIONS]; size_t n; } qlist;
static void ask(qlist *q, const fipsd_mdns_name *name, uint16_t type)
{
    for (size_t i = 0; i < q->n; ++i) if (q->types[i] == type && fipsd_mdns_name_equal(&q->names[i], name)) return;
    if (q->n < FIPSD_MDNS_MAX_QUESTIONS) { q->names[q->n] = *name; q->types[q->n++] = type; }
}
size_t fipsd_mdns_browser_query(fipsd_mdns_browser *b, uint64_t now_ms, uint8_t *buf, size_t cap)
{
    b->now_ms = now_ms;
    qlist q = {.n = 0};
    bool browse = now_ms >= b->next_query_ms;
    if (browse) {
        ask(&q, &b->service, FIPSD_MDNS_TYPE_PTR);
        b->next_query_ms = now_ms + b->interval_ms;
        b->interval_ms = b->interval_ms * 2 > FIPSD_MDNS_QUERY_MAX_INTERVAL_MS ? FIPSD_MDNS_QUERY_MAX_INTERVAL_MS : b->interval_ms * 2;
    }
    fipsd_mdns_resolved *scratch = malloc(sizeof *scratch);
    if (!scratch) return 0;
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i) {
        instance *in = &b->inst[i];
        if (!in->used || !in->missing_next_ms || now_ms < in->missing_next_ms) continue;
        bool no_srv, no_txt; fipsd_mdns_name host; uint64_t sig;
        if (!resolve(b, in, scratch, &sig, &no_srv, &no_txt, &host)) {
            if (no_srv) ask(&q, &in->name, FIPSD_MDNS_TYPE_SRV);
            if (no_txt) ask(&q, &in->name, FIPSD_MDNS_TYPE_TXT);
            if (!no_srv) { ask(&q, &host, FIPSD_MDNS_TYPE_A); ask(&q, &host, FIPSD_MDNS_TYPE_AAAA); }
        }
        ++in->missing_attempts;
        in->missing_next_ms = in->missing_attempts >= MISSING_ATTEMPTS ? 0 : now_ms + 1000u * in->missing_attempts;
    }
    free(scratch);
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        entry *e = &b->cache[i];
        if (!live(e) || e->refresh_stage >= 4 || now_ms < refresh_at(e) || !wanted(b, e)) continue;
        ask(&q, &e->rr.name, e->rr.rtype);
        while (e->refresh_stage < 4 && now_ms >= refresh_at(e)) ++e->refresh_stage;
    }
    if (!q.n) return 0;
    fipsd_mdns_writer w;
    fipsd_mdns_writer_init(&w, buf, cap, 0, 0);
    for (size_t i = 0; i < q.n; ++i) (void)fipsd_mdns_put_question(&w, &q.names[i], q.types[i], false);
    bool asked_ptr = false;
    for (size_t i = 0; i < q.n; ++i) if (q.types[i] == FIPSD_MDNS_TYPE_PTR && fipsd_mdns_name_equal(&q.names[i], &b->service)) asked_ptr = true;
    if (asked_ptr) {
        /* Known answers (RFC 6762 Section 7.1): only those with over half
         * their TTL left, written with the TTL that remains; the list stops
         * where the packet would overflow. */
        for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
            const entry *e = &b->cache[i];
            if (!live(e) || e->rr.rtype != FIPSD_MDNS_TYPE_PTR || !fipsd_mdns_name_equal(&e->rr.name, &b->service)) continue;
            uint64_t left = e->expires_ms > now_ms ? e->expires_ms - now_ms : 0;
            if (left * 2 <= ttl_ms(e)) continue;
            fipsd_mdns_rr known = e->rr; known.ttl = (uint32_t)(left / 1000u);
            size_t before = w.len;
            if (!fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &known, 0, true)) {
                w.overflow = false; w.len = before; break;
            }
        }
    }
    return fipsd_mdns_writer_finish(&w);
}
uint64_t fipsd_mdns_browser_deadline(const fipsd_mdns_browser *b)
{
    uint64_t at = b->next_query_ms;
    for (size_t i = 0; i < FIPSD_MDNS_INSTANCES_MAX; ++i)
        if (b->inst[i].used && b->inst[i].missing_next_ms && b->inst[i].missing_next_ms < at) at = b->inst[i].missing_next_ms;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) {
        const entry *e = &b->cache[i];
        if (!e->used) continue;
        if (e->expires_ms < at) at = e->expires_ms;
        if (live(e) && e->refresh_stage < 4 && wanted(b, e) && refresh_at(e) < at) at = refresh_at(e);
    }
    return at;
}
size_t fipsd_mdns_browser_cached(const fipsd_mdns_browser *b)
{
    size_t n = 0;
    for (size_t i = 0; i < FIPSD_MDNS_CACHE_MAX; ++i) n += b->cache[i].used;
    return n;
}
