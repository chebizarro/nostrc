/* DNS-SD responder: probing, announcing, answering and goodbyes. */
#include "mdns/responder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { G_PTR = 1, G_SRV = 2, G_TXT = 4, G_A = 8, G_AAAA = 16, G_META = 32 };
#define NGROUPS 6
#define UNIQUE_GROUPS (G_SRV | G_TXT | G_A | G_AAAA)
#define GROUP_RECORDS (FIPSD_MDNS_IFACE_ADDRS * 2)
#define UNICAST_QUEUE 4
#define CONFLICT_WINDOW 15u
typedef enum { R_IDLE, R_PROBING, R_ANNOUNCING, R_ANNOUNCED } rstate;
typedef struct riface {
    bool used, announced_once;
    fipsd_mdns_iface_addrs a;
    rstate state;
    unsigned step;
    uint64_t next_ms;
    uint32_t pending;
    uint64_t pending_ms;
    bool pending_defense;
    bool mcast_ever[NGROUPS];
    uint64_t last_mcast[NGROUPS];
    size_t gone4, gone6;
    uint8_t gone_v4[FIPSD_MDNS_IFACE_ADDRS][4], gone_v6[FIPSD_MDNS_IFACE_ADDRS][16];
} riface;
struct fipsd_mdns_responder {
    fipsd_mdns_name service, meta, local, instance, host;
    char base[64], label[80];
    unsigned rename;
    uint16_t txt_len;
    uint8_t txt[FIPSD_MDNS_MAX_RDATA];
    uint16_t port;
    fipsd_mdns_rng rng;
    riface ifs[FIPSD_MDNS_MAX_INTERFACES];
    uint64_t conflicts[CONFLICT_WINDOW];
    size_t conflict_at;
    uint64_t hold_until_ms;
    size_t queued;
    fipsd_mdns_out queue[UNICAST_QUEUE];
};

static uint32_t group_ttl(uint32_t g)
{ return g == G_SRV || g == G_A || g == G_AAAA ? FIPSD_MDNS_HOST_TTL : FIPSD_MDNS_OTHER_TTL; }
static uint32_t uniform(fipsd_mdns_responder *r, uint32_t bound) { return bound ? r->rng.uniform(r->rng.user, bound) : 0; }

static bool set_names(fipsd_mdns_responder *r)
{
    if (r->rename) snprintf(r->label, sizeof r->label, "%s-%u", r->base, r->rename);
    else snprintf(r->label, sizeof r->label, "%s", r->base);
    return fipsd_mdns_name_prepend(r->label, &r->service, &r->instance) &&
        fipsd_mdns_name_prepend(r->label, &r->local, &r->host);
}
fipsd_mdns_responder *fipsd_mdns_responder_create(const fipsd_mdns_responder_config *cfg)
{
    fipsd_mdns_responder *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    fipsd_mdns_rr txt;
    r->rng = cfg->rng;
    snprintf(r->base, sizeof r->base, "%s", cfg->label);
    if (!cfg->rng.uniform || strlen(cfg->label) >= sizeof r->base - 8 ||
        !fipsd_mdns_name_from_text(cfg->service_type, &r->service) ||
        !fipsd_mdns_name_from_text("_services._dns-sd._udp.local.", &r->meta) ||
        !fipsd_mdns_name_from_text("local.", &r->local) || !set_names(r) ||
        !fipsd_mdns_rr_txt(&txt, &r->instance, cfg->txt, cfg->txt_count, FIPSD_MDNS_OTHER_TTL, true)) {
        free(r); return NULL;
    }
    r->txt_len = txt.rdlen; memcpy(r->txt, txt.rdata, txt.rdlen);
    return r;
}
void fipsd_mdns_responder_destroy(fipsd_mdns_responder *r) { free(r); }
const char *fipsd_mdns_responder_label(const fipsd_mdns_responder *r) { return r->label; }

/* The records of one group on an interface; ttl0 writes goodbyes. */
static size_t group_records(const fipsd_mdns_responder *r, const riface *f, uint32_t g, bool ttl0, fipsd_mdns_rr *out)
{
    uint32_t ttl = ttl0 ? 0 : group_ttl(g);
    size_t n = 0;
    switch (g) {
    case G_PTR: fipsd_mdns_rr_ptr(&out[n++], &r->service, &r->instance, ttl); break;
    case G_META: fipsd_mdns_rr_ptr(&out[n++], &r->meta, &r->service, ttl); break;
    case G_SRV: fipsd_mdns_rr_srv(&out[n++], &r->instance, r->port, &r->host, ttl, true); break;
    case G_TXT: {
        fipsd_mdns_rr *t = &out[n++];
        t->name = r->instance; t->rtype = FIPSD_MDNS_TYPE_TXT; t->rclass = FIPSD_MDNS_CLASS_IN;
        t->cache_flush = true; t->ttl = ttl; t->rdlen = r->txt_len; memcpy(t->rdata, r->txt, r->txt_len);
        break;
    }
    case G_A: for (size_t i = 0; i < f->a.v4_count; ++i) fipsd_mdns_rr_a(&out[n++], &r->host, f->a.v4[i], ttl, true); break;
    case G_AAAA: for (size_t i = 0; i < f->a.v6_count; ++i) fipsd_mdns_rr_aaaa(&out[n++], &r->host, f->a.v6[i], ttl, true); break;
    default: break;
    }
    return n;
}
static void put_groups(const fipsd_mdns_responder *r, const riface *f, fipsd_mdns_writer *w, fipsd_mdns_section s,
                       uint32_t groups, bool ttl0, uint32_t ttl_cap, bool no_flush, fipsd_mdns_rr *scratch)
{
    static const uint32_t order[NGROUPS] = {G_PTR, G_META, G_SRV, G_TXT, G_A, G_AAAA};
    for (int i = 0; i < NGROUPS; ++i) {
        if (!(groups & order[i])) continue;
        size_t n = group_records(r, f, order[i], ttl0, scratch);
        for (size_t k = 0; k < n; ++k) (void)fipsd_mdns_put_rr(w, s, &scratch[k], ttl_cap, no_flush);
    }
}
static uint32_t additional_for(uint32_t answers)
{
    uint32_t extra = 0;
    if (answers & G_PTR) extra |= G_SRV | G_TXT | G_A | G_AAAA;
    if (answers & G_SRV) extra |= G_A | G_AAAA;
    return extra & ~answers;
}
static fipsd_mdns_out *slot(fipsd_mdns_out *out, size_t cap, size_t *n, uint32_t ifindex)
{
    if (*n >= cap) return NULL;
    fipsd_mdns_out *o = &out[(*n)++];
    o->ifindex = ifindex; o->unicast = false; memset(&o->dst, 0, sizeof o->dst); o->len = 0;
    return o;
}
static void mark_sent(riface *f, uint32_t groups, uint64_t now)
{
    for (int i = 0; i < NGROUPS; ++i) if (groups & (1u << i)) { f->mcast_ever[i] = true; f->last_mcast[i] = now; }
}
static size_t build_probe(const fipsd_mdns_responder *r, const riface *f, bool first, uint8_t *buf, fipsd_mdns_rr *scratch)
{
    fipsd_mdns_writer w;
    fipsd_mdns_writer_init(&w, buf, FIPSD_MDNS_MAX_PACKET, 0, 0);
    (void)fipsd_mdns_put_question(&w, &r->instance, FIPSD_MDNS_TYPE_ANY, first);
    (void)fipsd_mdns_put_question(&w, &r->host, FIPSD_MDNS_TYPE_ANY, first);
    put_groups(r, f, &w, FIPSD_MDNS_AUTHORITY, UNIQUE_GROUPS, false, 0, false, scratch);
    return fipsd_mdns_writer_finish(&w);
}
static size_t build_announce(const fipsd_mdns_responder *r, riface *f, bool with_gone, uint8_t *buf, fipsd_mdns_rr *scratch)
{
    fipsd_mdns_writer w;
    fipsd_mdns_writer_init(&w, buf, FIPSD_MDNS_MAX_PACKET, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    put_groups(r, f, &w, FIPSD_MDNS_ANSWER, G_PTR | UNIQUE_GROUPS, false, 0, false, scratch);
    if (with_gone) {
        for (size_t i = 0; i < f->gone4; ++i) { fipsd_mdns_rr_a(scratch, &r->host, f->gone_v4[i], 0, false); (void)fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, scratch, 0, false); }
        for (size_t i = 0; i < f->gone6; ++i) { fipsd_mdns_rr_aaaa(scratch, &r->host, f->gone_v6[i], 0, false); (void)fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, scratch, 0, false); }
    }
    return fipsd_mdns_writer_finish(&w);
}
static size_t build_response(const fipsd_mdns_responder *r, const riface *f, uint32_t groups, const fipsd_mdns_message *legacy,
                             uint8_t *buf, fipsd_mdns_rr *scratch)
{
    fipsd_mdns_writer w;
    fipsd_mdns_writer_init(&w, buf, FIPSD_MDNS_MAX_PACKET, legacy ? legacy->id : 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    if (legacy)
        for (size_t i = 0; i < legacy->qdcount; ++i)
            (void)fipsd_mdns_put_question(&w, &legacy->questions[i].name, legacy->questions[i].qtype, false);
    uint32_t cap = legacy ? FIPSD_MDNS_LEGACY_TTL : 0;
    put_groups(r, f, &w, FIPSD_MDNS_ANSWER, groups, false, cap, legacy != NULL, scratch);
    put_groups(r, f, &w, FIPSD_MDNS_ADDITIONAL, additional_for(groups), false, cap, legacy != NULL, scratch);
    return fipsd_mdns_writer_finish(&w);
}

static riface *find(fipsd_mdns_responder *r, uint32_t index)
{
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) if (r->ifs[i].used && r->ifs[i].a.index == index) return &r->ifs[i];
    return NULL;
}
static void start_probing(fipsd_mdns_responder *r, riface *f, uint64_t at)
{
    f->state = r->port ? R_PROBING : R_IDLE;
    f->step = 0; f->next_ms = at; f->pending = 0;
}
static void restart_all(fipsd_mdns_responder *r, uint64_t now, uint32_t delay_bound)
{
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i)
        if (r->ifs[i].used) start_probing(r, &r->ifs[i], now + uniform(r, delay_bound));
}
void fipsd_mdns_responder_set_port(fipsd_mdns_responder *r, uint16_t port, uint64_t now_ms)
{
    if (!port || port == r->port) return;
    uint16_t old = r->port;
    r->port = port;
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        riface *f = &r->ifs[i];
        if (!f->used) continue;
        if (!old || f->state == R_IDLE) start_probing(r, f, now_ms + uniform(r, FIPSD_MDNS_PROBE_INTERVAL_MS + 1));
        else if (f->state == R_ANNOUNCING || f->state == R_ANNOUNCED) { f->state = R_ANNOUNCING; f->step = 0; f->next_ms = now_ms; }
    }
}
static bool has4(const fipsd_mdns_iface_addrs *a, const uint8_t v[4])
{ for (size_t i = 0; i < a->v4_count; ++i) if (!memcmp(a->v4[i], v, 4)) return true; return false; }
static bool has6(const fipsd_mdns_iface_addrs *a, const uint8_t v[16])
{ for (size_t i = 0; i < a->v6_count; ++i) if (!memcmp(a->v6[i], v, 16)) return true; return false; }
void fipsd_mdns_responder_set_interfaces(fipsd_mdns_responder *r, const fipsd_mdns_iface_addrs *ifaces, size_t n,
                                         uint64_t now_ms)
{
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        riface *f = &r->ifs[i];
        if (!f->used) continue;
        bool kept = false;
        for (size_t k = 0; k < n && !kept; ++k) kept = ifaces[k].index == f->a.index;
        if (!kept) memset(f, 0, sizeof *f); /* gone: nothing can be sent on it */
    }
    for (size_t k = 0; k < n; ++k) {
        const fipsd_mdns_iface_addrs *a = &ifaces[k];
        riface *f = find(r, a->index);
        if (f) {
            bool changed = a->v4_count != f->a.v4_count || a->v6_count != f->a.v6_count;
            for (size_t i = 0; i < f->a.v4_count; ++i)
                if (!has4(a, f->a.v4[i])) { changed = true; if (f->gone4 < FIPSD_MDNS_IFACE_ADDRS) memcpy(f->gone_v4[f->gone4++], f->a.v4[i], 4); }
            for (size_t i = 0; i < f->a.v6_count; ++i)
                if (!has6(a, f->a.v6[i])) { changed = true; if (f->gone6 < FIPSD_MDNS_IFACE_ADDRS) memcpy(f->gone_v6[f->gone6++], f->a.v6[i], 16); }
            for (size_t i = 0; i < a->v4_count; ++i) if (!has4(&f->a, a->v4[i])) changed = true;
            for (size_t i = 0; i < a->v6_count; ++i) if (!has6(&f->a, a->v6[i])) changed = true;
            f->a = *a;
            if (changed && (f->state == R_ANNOUNCING || f->state == R_ANNOUNCED)) { f->state = R_ANNOUNCING; f->step = 0; f->next_ms = now_ms; }
            continue;
        }
        for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES && !f; ++i) if (!r->ifs[i].used) f = &r->ifs[i];
        if (!f) break;
        memset(f, 0, sizeof *f);
        f->used = true; f->a = *a;
        start_probing(r, f, now_ms + uniform(r, FIPSD_MDNS_PROBE_INTERVAL_MS + 1));
    }
}

/* Whether rr, received from another host, claims one of our unique names
 * with data that is not ours on any interface. Goodbyes claim nothing. */
static bool conflicting(const fipsd_mdns_responder *r, const fipsd_mdns_rr *rr, fipsd_mdns_rr *scratch)
{
    if (rr->rclass != FIPSD_MDNS_CLASS_IN || !rr->ttl) return false;
    if (fipsd_mdns_name_equal(&rr->name, &r->instance)) {
        if (rr->rtype == FIPSD_MDNS_TYPE_SRV) { (void)group_records(r, NULL, G_SRV, false, scratch); return !fipsd_mdns_rr_same(rr, scratch); }
        if (rr->rtype == FIPSD_MDNS_TYPE_TXT) return rr->rdlen != r->txt_len || memcmp(rr->rdata, r->txt, r->txt_len);
        return false;
    }
    if (!fipsd_mdns_name_equal(&rr->name, &r->host)) return false;
    if (rr->rtype != FIPSD_MDNS_TYPE_A && rr->rtype != FIPSD_MDNS_TYPE_AAAA) return false;
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        const riface *f = &r->ifs[i];
        if (!f->used) continue;
        if (rr->rtype == FIPSD_MDNS_TYPE_A && rr->rdlen == 4 && has4(&f->a, rr->rdata)) return false;
        if (rr->rtype == FIPSD_MDNS_TYPE_AAAA && rr->rdlen == 16 && has6(&f->a, rr->rdata)) return false;
    }
    return true;
}
static void on_conflict(fipsd_mdns_responder *r, bool probing, uint64_t now)
{
    if (!probing) { restart_all(r, now, FIPSD_MDNS_PROBE_INTERVAL_MS + 1); return; }
    /* Fifteen conflicts within ten seconds: wait five before probing again. */
    uint64_t oldest = r->conflicts[r->conflict_at];
    r->conflicts[r->conflict_at] = now; r->conflict_at = (r->conflict_at + 1) % CONFLICT_WINDOW;
    if (oldest && now - oldest < 10000u) r->hold_until_ms = now + 5000u;
    ++r->rename;
    if (r->rename == 1) r->rename = 2;
    if (!set_names(r)) { --r->rename; (void)set_names(r); }
    restart_all(r, now, FIPSD_MDNS_PROBE_INTERVAL_MS + 1);
}
static int cmp_rr(const void *a, const void *b) { return fipsd_mdns_rr_compare(a, b); }
/* Section 8.2: true when the other host's proposed records for name sort
 * after ours, so we defer. */
static bool lose_tiebreak(const fipsd_mdns_responder *r, const riface *f, const fipsd_mdns_message *msg,
                          const fipsd_mdns_name *name, uint32_t groups, fipsd_mdns_rr *ours, fipsd_mdns_rr *theirs)
{
    size_t nt = 0, no = 0;
    const fipsd_mdns_rr *auth = fipsd_mdns_authority(msg);
    for (size_t i = 0; i < msg->nscount && nt < GROUP_RECORDS; ++i)
        if (fipsd_mdns_name_equal(&auth[i].name, name)) theirs[nt++] = auth[i];
    if (!nt) return false;
    for (int i = 0; i < NGROUPS; ++i)
        if (groups & (1u << i)) no += group_records(r, f, 1u << i, false, ours + no);
    qsort(ours, no, sizeof *ours, cmp_rr);
    qsort(theirs, nt, sizeof *theirs, cmp_rr);
    for (size_t i = 0; i < no && i < nt; ++i) {
        int c = fipsd_mdns_rr_compare(&ours[i], &theirs[i]);
        if (c) return c < 0;
    }
    return nt > no;
}
static uint32_t question_groups(const fipsd_mdns_responder *r, const fipsd_mdns_question *q)
{
    if (q->qclass != FIPSD_MDNS_CLASS_IN && q->qclass != FIPSD_DNS_CLASS_ANY) return 0;
    bool any = q->qtype == FIPSD_MDNS_TYPE_ANY;
    if (fipsd_mdns_name_equal(&q->name, &r->service)) return any || q->qtype == FIPSD_MDNS_TYPE_PTR ? G_PTR : 0;
    if (fipsd_mdns_name_equal(&q->name, &r->meta)) return any || q->qtype == FIPSD_MDNS_TYPE_PTR ? G_META : 0;
    if (fipsd_mdns_name_equal(&q->name, &r->instance))
        return (any || q->qtype == FIPSD_MDNS_TYPE_SRV ? G_SRV : 0) | (any || q->qtype == FIPSD_MDNS_TYPE_TXT ? G_TXT : 0);
    if (fipsd_mdns_name_equal(&q->name, &r->host))
        return (any || q->qtype == FIPSD_MDNS_TYPE_A ? G_A : 0) | (any || q->qtype == FIPSD_MDNS_TYPE_AAAA ? G_AAAA : 0);
    return 0;
}
/* Section 7.1: the query lists every record of the group with at least half
 * our TTL. A group with no records (no addresses of a family) is dropped. */
static bool suppressed(const fipsd_mdns_responder *r, const riface *f, uint32_t g, const fipsd_mdns_message *msg,
                       fipsd_mdns_rr *scratch)
{
    size_t n = group_records(r, f, g, false, scratch);
    if (!n) return true;
    const fipsd_mdns_rr *known = fipsd_mdns_answers(msg);
    for (size_t i = 0; i < n; ++i) {
        bool listed = false;
        for (size_t k = 0; k < msg->ancount && !listed; ++k)
            listed = fipsd_mdns_rr_same(&known[k], &scratch[i]) && known[k].ttl * 2 >= scratch[i].ttl;
        if (!listed) return false;
    }
    return true;
}
static void queue_unicast(fipsd_mdns_responder *r, const riface *f, uint32_t groups, const fipsd_mdns_message *legacy,
                          const fipsd_mdns_sockaddr *dst, fipsd_mdns_rr *scratch)
{
    if (r->queued >= UNICAST_QUEUE) return;
    fipsd_mdns_out *o = &r->queue[r->queued];
    o->ifindex = f->a.index; o->unicast = true; o->dst = *dst;
    o->len = build_response(r, f, groups, legacy, o->bytes, scratch);
    if (o->len) ++r->queued;
}
void fipsd_mdns_responder_on_message(fipsd_mdns_responder *r, const fipsd_mdns_message *msg, uint32_t ifindex,
                                     const fipsd_mdns_sockaddr *src, bool own, uint64_t now_ms)
{
    riface *f = find(r, ifindex);
    if (own || !f || !r->port) return;
    fipsd_mdns_rr *scratch = malloc(sizeof *scratch * GROUP_RECORDS * 2);
    if (!scratch) return;
    if (fipsd_mdns_is_response(msg)) {
        bool conflict = false;
        for (size_t i = 0; i < fipsd_mdns_record_count(msg) && !conflict; ++i)
            conflict = conflicting(r, &msg->records[i], scratch);
        if (conflict) on_conflict(r, f->state == R_PROBING, now_ms);
        free(scratch);
        return;
    }
    if (f->state == R_PROBING &&
        (lose_tiebreak(r, f, msg, &r->instance, G_SRV | G_TXT, scratch, scratch + GROUP_RECORDS) ||
         lose_tiebreak(r, f, msg, &r->host, G_A | G_AAAA, scratch, scratch + GROUP_RECORDS))) {
        start_probing(r, f, now_ms + 1000u);
        free(scratch);
        return;
    }
    if (f->state != R_ANNOUNCING && f->state != R_ANNOUNCED) { free(scratch); return; }
    uint32_t groups = 0; bool qu = false;
    for (size_t i = 0; i < msg->qdcount; ++i) {
        uint32_t g = question_groups(r, &msg->questions[i]);
        groups |= g;
        if (g && msg->questions[i].unicast) qu = true;
    }
    bool defense = false;
    const fipsd_mdns_rr *auth = fipsd_mdns_authority(msg);
    for (size_t i = 0; i < msg->nscount; ++i)
        if (fipsd_mdns_name_equal(&auth[i].name, &r->instance) || fipsd_mdns_name_equal(&auth[i].name, &r->host)) defense = true;
    for (int i = 0; i < NGROUPS; ++i)
        if ((groups & (1u << i)) && suppressed(r, f, 1u << i, msg, scratch)) groups &= ~(1u << i);
    if (!groups) { free(scratch); return; }
    if (src->port != FIPSD_MDNS_PORT) {
        queue_unicast(r, f, groups, msg, src, scratch);
        free(scratch);
        return;
    }
    if (qu) {
        uint32_t direct = 0;
        for (int i = 0; i < NGROUPS; ++i) {
            uint32_t g = 1u << i;
            if ((groups & g) && f->mcast_ever[i] && now_ms - f->last_mcast[i] < (uint64_t)group_ttl(g) * 250u) direct |= g;
        }
        if (direct) queue_unicast(r, f, direct, NULL, src, scratch);
        groups &= ~direct;
    }
    if (groups) {
        uint64_t due = defense || !(groups & (G_PTR | G_META)) ? now_ms : now_ms + 20u + uniform(r, 101);
        if (!f->pending || due < f->pending_ms) f->pending_ms = due;
        f->pending |= groups;
        f->pending_defense |= defense;
    }
    free(scratch);
}
size_t fipsd_mdns_responder_poll(fipsd_mdns_responder *r, uint64_t now_ms, fipsd_mdns_out *out, size_t cap)
{
    size_t n = 0;
    while (r->queued && n < cap) {
        out[n++] = r->queue[0];
        memmove(r->queue, r->queue + 1, sizeof r->queue[0] * --r->queued);
    }
    fipsd_mdns_rr *scratch = malloc(sizeof *scratch * GROUP_RECORDS);
    if (!scratch) return n;
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        riface *f = &r->ifs[i];
        if (!f->used) continue;
        if (f->state == R_PROBING && now_ms >= f->next_ms && now_ms >= r->hold_until_ms) {
            if (f->step < FIPSD_MDNS_PROBES) {
                fipsd_mdns_out *o = slot(out, cap, &n, f->a.index);
                if (!o) break;
                o->len = build_probe(r, f, f->step == 0, o->bytes, scratch);
                ++f->step; f->next_ms = now_ms + FIPSD_MDNS_PROBE_INTERVAL_MS;
            } else { f->state = R_ANNOUNCING; f->step = 0; f->next_ms = now_ms; }
        }
        if (f->state == R_ANNOUNCING && now_ms >= f->next_ms) {
            fipsd_mdns_out *o = slot(out, cap, &n, f->a.index);
            if (!o) break;
            o->len = build_announce(r, f, f->step == 0, o->bytes, scratch);
            if (f->step == 0) f->gone4 = f->gone6 = 0;
            mark_sent(f, G_PTR | UNIQUE_GROUPS, now_ms);
            f->announced_once = true;
            f->pending &= ~(G_PTR | UNIQUE_GROUPS);
            if (++f->step >= FIPSD_MDNS_ANNOUNCEMENTS) f->state = R_ANNOUNCED;
            else f->next_ms = now_ms + FIPSD_MDNS_ANNOUNCE_INTERVAL_MS;
        }
        if (f->pending && now_ms >= f->pending_ms && (f->state == R_ANNOUNCING || f->state == R_ANNOUNCED)) {
            uint64_t limit = f->pending_defense ? 250u : 1000u;
            uint32_t groups = 0;
            for (int g = 0; g < NGROUPS; ++g)
                if ((f->pending & (1u << g)) && (!f->mcast_ever[g] || now_ms - f->last_mcast[g] >= limit)) groups |= 1u << g;
            f->pending = 0; f->pending_defense = false;
            if (groups) {
                fipsd_mdns_out *o = slot(out, cap, &n, f->a.index);
                if (!o) break;
                o->len = build_response(r, f, groups, NULL, o->bytes, scratch);
                mark_sent(f, groups | additional_for(groups), now_ms);
            }
        }
    }
    free(scratch);
    /* A packet that did not fit is dropped rather than sent short. */
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) if (out[i].len) { if (kept != i) out[kept] = out[i]; ++kept; }
    return kept;
}
size_t fipsd_mdns_responder_goodbye(fipsd_mdns_responder *r, fipsd_mdns_out *out, size_t cap)
{
    size_t n = 0;
    fipsd_mdns_rr *scratch = malloc(sizeof *scratch * GROUP_RECORDS);
    if (!scratch) return 0;
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES && n < cap; ++i) {
        riface *f = &r->ifs[i];
        if (!f->used || !f->announced_once) continue;
        fipsd_mdns_out *o = slot(out, cap, &n, f->a.index);
        fipsd_mdns_writer w;
        fipsd_mdns_writer_init(&w, o->bytes, FIPSD_MDNS_MAX_PACKET, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
        put_groups(r, f, &w, FIPSD_MDNS_ANSWER, G_PTR | UNIQUE_GROUPS, true, 0, false, scratch);
        o->len = fipsd_mdns_writer_finish(&w);
        f->announced_once = false; f->state = R_IDLE;
        if (!o->len) --n;
    }
    free(scratch);
    return n;
}
uint64_t fipsd_mdns_responder_deadline(const fipsd_mdns_responder *r)
{
    uint64_t at = r->queued ? 0 : UINT64_MAX;
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        const riface *f = &r->ifs[i];
        if (!f->used) continue;
        if (f->state == R_PROBING) { uint64_t t = f->next_ms > r->hold_until_ms ? f->next_ms : r->hold_until_ms; if (t < at) at = t; }
        if (f->state == R_ANNOUNCING && f->next_ms < at) at = f->next_ms;
        if (f->pending && f->pending_ms < at) at = f->pending_ms;
    }
    return at;
}
bool fipsd_mdns_responder_announced(const fipsd_mdns_responder *r, uint32_t ifindex)
{
    for (size_t i = 0; i < FIPSD_MDNS_MAX_INTERFACES; ++i)
        if (r->ifs[i].used && r->ifs[i].a.index == ifindex) return r->ifs[i].state == R_ANNOUNCED || r->ifs[i].state == R_ANNOUNCING;
    return false;
}
