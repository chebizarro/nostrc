#include <fips/proto/settle/gates.h>
#include <fips/proto/settle/faults.h>
#include <string.h>

_Static_assert(FIPS_SETTLE_GATE_EPHEMERALS_MAX >= FIPS_SETTLE_DEFAULT_REPLAYS,
               "gate arena holds upstream default replay memory");
_Static_assert(FIPS_SETTLE_GATE_EPHEMERALS_MAX >= FIPS_SETTLE_DEFAULT_COUNTED,
               "gate arena holds upstream default counted sessions");
static bool same(const fips_node_addr *a, const fips_node_addr *b) { return memcmp(a->bytes,b->bytes,16)==0; }
static int cmp(fips_instant a, fips_instant b) { return a.ops->compare(a,b); }
static int dcmp(fips_duration a, fips_duration b)
{ return a.secs!=b.secs ? (a.secs>b.secs ? 1 : -1) : a.nanos!=b.nanos ? (a.nanos>b.nanos ? 1 : -1) : 0; }
static bool recent(fips_instant at, fips_instant now, const fips_settle_params *p)
{ return dcmp(now.ops->saturating_duration_since(now,at),p->interval)<0; }
static fips_settle_memory *find(const fips_settle_gates *g, const fips_node_addr *peer)
{
    for (size_t i=0;i<g->capacity;++i) if (g->slots[i].active && same(&g->slots[i].peer,peer)) return &g->slots[i];
    return NULL;
}
const fips_settle_memory *fips_settle_gate_get(const fips_settle_gates *g, const fips_node_addr *peer) { return find(g,peer); }
fips_status fips_settle_gates_init(fips_settle_gates *g, fips_settle_memory *slots, size_t capacity)
{
    if (capacity>SIZE_MAX/sizeof(*slots)) return FIPS_ERR_RANGE;
    if (capacity) memset(slots,0,capacity*sizeof(*slots));
    *g=(fips_settle_gates){.slots=slots,.capacity=capacity}; return FIPS_OK;
}
static bool valid(const fips_settle_params *p)
{ return p->replays<=FIPS_SETTLE_GATE_EPHEMERALS_MAX && p->counted>0 && p->counted<=FIPS_SETTLE_GATE_EPHEMERALS_MAX; }
static bool expired(const fips_settle_silence *r, fips_instant now, const fips_settle_params *p)
{ return cmp(now,settle_deadline(r->has_until ? r->until : r->last,p->horizon))>=0; }
static bool live(const fips_settle_memory *m, fips_instant now, fips_instant memory, const fips_settle_params *p)
{
    if (SETTLE_FAULT(MemoryNeverDropped)) return true;
    if (m->has_stamp && recent(m->stamp,now,p)) return true;
    for (size_t i=0;i<m->replay_count;++i) if (recent(m->replays[i].at,memory,p)) return true;
    for (size_t i=0;i<m->redial_count;++i) if (!SETTLE_FAULT(RedialMemoryLapses) && recent(m->redials[i].at,memory,p)) return true;
    return m->silence.present && !expired(&m->silence,now,p);
}
static size_t prune(fips_settle_remembered *entries, size_t count, fips_instant now, const fips_settle_params *p)
{
    size_t n=0;
    for (size_t i=0;i<count;++i) if (recent(entries[i].at,now,p)) entries[n++]=entries[i];
    return n;
}
/* Capacity is checked against the post-lapse view before any state is changed. */
static bool room(const fips_settle_gates *g, const fips_node_addr *peer,
    fips_instant now, fips_instant memory, const fips_settle_params *p)
{
    if (find(g,peer)) return true;
    for (size_t i=0;i<g->capacity;++i)
        if (!g->slots[i].active || !live(&g->slots[i],now,memory,p)) return true;
    return false;
}
static void lapse(fips_settle_gates *g, fips_instant now, fips_instant memory, const fips_settle_params *p)
{
    if (SETTLE_FAULT(MemoryNeverDropped)) return;
    for (size_t i=0;i<g->capacity;++i) {
        fips_settle_memory *m=&g->slots[i]; if (!m->active) continue;
        m->replay_count=prune(m->replays,m->replay_count,memory,p);
        if (!live(m,now,memory,p)) memset(m,0,sizeof(*m));
    }
}
static fips_settle_memory *entry(fips_settle_gates *g, fips_node_addr peer)
{
    fips_settle_memory *m=find(g,&peer); if (m) return m;
    for (size_t i=0;i<g->capacity;++i) if (!g->slots[i].active) {
        m=&g->slots[i]; *m=(fips_settle_memory){.active=true,.peer=peer}; return m;
    }
    return NULL;
}
static bool silencing(const fips_settle_memory *m, const uint8_t *epoch, fips_instant now)
{
    const fips_settle_silence *r=&m->silence;
    return r->present && r->has_until && cmp(r->until,now)>0 &&
        !(epoch && r->has_epoch && memcmp(epoch,r->epoch,8));
}
fips_status fips_settle_gate(fips_settle_gates *g, fips_node_addr peer,
    const fips_settle_ask *a, fips_instant now, const fips_settle_params *p, fips_settle_refusal *out)
{
    if (!valid(p) || (unsigned)a->source>FIPS_SETTLE_SOURCE_REKEY_MSG2) return FIPS_ERR_RANGE;
    fips_instant memory=a->has_memory ? a->memory : now;
    const fips_settle_memory *m=find(g,&peer);
    if (m && !live(m,now,memory,p)) m=NULL;
    fips_settle_refusal r=FIPS_SETTLE_GATE_ACCEPT;
    bool stamp=false;
    if (m && (a->source==FIPS_SETTLE_SOURCE_NOT_HELD || a->source==FIPS_SETTLE_SOURCE_REPLACEMENT ||
        a->source==FIPS_SETTLE_SOURCE_REDIAL) && silencing(m,a->has_epoch ? a->epoch : NULL,now)) r=FIPS_SETTLE_GATE_SILENT;
    else if (a->source!=FIPS_SETTLE_SOURCE_NOT_HELD && a->has_held && (a->source<=FIPS_SETTLE_SOURCE_REDIAL || (a->source==FIPS_SETTLE_SOURCE_OWN_MSG2 && SETTLE_FAULT(OutboundGated)))) {
        if (a->source==FIPS_SETTLE_SOURCE_REDIAL) {
            size_t n=0;
            if (!SETTLE_FAULT(RedialUngated) && a->has_ephemeral && m) for (size_t i=0;i<m->redial_count;++i) {
                if (!recent(m->redials[i].at,memory,p)) continue;
                ++n;
                if (!memcmp(m->redials[i].ephemeral,a->ephemeral,32)) r=FIPS_SETTLE_GATE_REDIALED;
            }
            if (!SETTLE_FAULT(RedialUngated) && a->has_ephemeral && n>=p->replays) r=FIPS_SETTLE_GATE_REDIALED;
            if (r==FIPS_SETTLE_GATE_ACCEPT && !SETTLE_FAULT(RedialUngated) && SETTLE_FAULT(RedialHeardFloored) && a->held.heard && recent(a->held.has_last ? a->held.last : a->held.installed,now,p)) r=FIPS_SETTLE_GATE_HEARD;
        } else {
            if (!a->held.heard && a->has_ephemeral) {
                size_t n=0;
                if (m) for (size_t i=0;i<m->replay_count;++i) {
                    if (!recent(m->replays[i].at,memory,p)) continue;
                    ++n;
                    if (!SETTLE_FAULT(ReplaySeenIgnored) && !memcmp(m->replays[i].ephemeral,a->ephemeral,32)) r=FIPS_SETTLE_GATE_REPLAYED;
                }
                if (r==FIPS_SETTLE_GATE_ACCEPT && !SETTLE_FAULT(ReplayUnbounded) && n>=p->replays) r=FIPS_SETTLE_GATE_REPLAY_FULL;
            }
            fips_instant from=a->held.heard && a->held.has_last && !SETTLE_FAULT(HeardFromInstall) ? a->held.last : a->held.installed;
            fips_duration floor=a->held.heard ? p->interval : p->unheard;
            int age=dcmp(now.ops->saturating_duration_since(now,from),floor);
            bool young=age<0 || (!a->held.heard && SETTLE_FAULT(UnheardInclusive) && age==0);
            if (a->held.heard && !a->held.has_last && SETTLE_FAULT(HeardNeedsFrame)) young=false;
            if (r==FIPS_SETTLE_GATE_ACCEPT && young)
                r=a->held.heard ? FIPS_SETTLE_GATE_HEARD : FIPS_SETTLE_GATE_UNHEARD;
        }
        if (r==FIPS_SETTLE_GATE_ACCEPT && !(a->source==FIPS_SETTLE_SOURCE_REDIAL && SETTLE_FAULT(RedialUngated)) && m && m->has_stamp && recent(m->stamp,now,p)) r=FIPS_SETTLE_GATE_DAMPENED;
        stamp=a->source!=FIPS_SETTLE_SOURCE_REDIAL && ((r==FIPS_SETTLE_GATE_ACCEPT && a->held.heard) || (r!=FIPS_SETTLE_GATE_ACCEPT && SETTLE_FAULT(DampenerFromRefusal)));
    }
    if (a->has_held && a->source!=FIPS_SETTLE_SOURCE_NOT_HELD && a->source!=FIPS_SETTLE_SOURCE_REDIAL && a->source>=FIPS_SETTLE_SOURCE_OWN_MSG2 && !(a->source==FIPS_SETTLE_SOURCE_OWN_MSG2 && SETTLE_FAULT(OutboundGated)) && SETTLE_FAULT(DampenerAtMsg2)) stamp=true;
    if ((stamp || SETTLE_FAULT(MemoryNeverDropped)) && !room(g,&peer,now,memory,p)) return FIPS_ERR_RANGE;
    lapse(g,now,memory,p);
    if (SETTLE_FAULT(MemoryNeverDropped)) (void)entry(g,peer);
    if (stamp) { fips_settle_memory *w=entry(g,peer); w->has_stamp=true; w->stamp=now; }
    *out=r; return FIPS_OK;
}
static void remember(fips_settle_remembered *entries, size_t *count,
    const uint8_t ephemeral[32], fips_instant at, size_t limit)
{
    bool seen=false, trimmed=false;
    for (size_t i=0;i<*count;++i) if (!memcmp(entries[i].ephemeral,ephemeral,32)) seen=true;
    if (!seen) {
        if (*count==FIPS_SETTLE_GATE_EPHEMERALS_MAX) {
            memmove(entries,entries+1,(*count-1)*sizeof(*entries)); --*count; trimmed=true;
        }
        memcpy(entries[*count].ephemeral,ephemeral,32); entries[(*count)++].at=at;
    }
    /* Rust appends if absent, then removes exactly one oldest entry if over
     * the configured limit, including when the parameters were reduced. */
    if (*count>limit && !trimmed) {
        memmove(entries,entries+1,(*count-1)*sizeof(*entries)); --*count;
    }
}
fips_status fips_settle_gate_promoted(fips_settle_gates *g, fips_node_addr peer,
    const uint8_t ephemeral[32], fips_instant now, const fips_instant *memory, const fips_settle_params *p)
{
    fips_instant at=memory ? *memory : now;
    if (!valid(p) || !room(g,&peer,now,at,p)) return FIPS_ERR_RANGE;
    lapse(g,now,at,p); fips_settle_memory *m=entry(g,peer);
    remember(m->replays,&m->replay_count,ephemeral,at,SETTLE_FAULT(ReplayUnbounded) ? FIPS_SETTLE_GATE_EPHEMERALS_MAX : p->replays); return FIPS_OK;
}
static void forget(fips_settle_gates *g, const fips_node_addr *peer, bool replays, bool silence)
{
    fips_settle_memory *m=find(g,peer); if (!m) return;
    if (replays) m->replay_count=0;
    if (silence) memset(&m->silence,0,sizeof(m->silence));
    if (!m->has_stamp && !m->replay_count && !m->silence.present && (!m->redial_count || SETTLE_FAULT(RedialMemoryLapses)) && !SETTLE_FAULT(MemoryNeverDropped)) memset(m,0,sizeof(*m));
}
void fips_settle_gate_heard(fips_settle_gates *g, const fips_node_addr *peer) { if (!SETTLE_FAULT(HeardKeepsMemory)) forget(g,peer,true,true); }
void fips_settle_gate_framed(fips_settle_gates *g, const fips_node_addr *peer) { if (!SETTLE_FAULT(HeardKeepsMemory)) forget(g,peer,false,true); }
void fips_settle_gate_forget(fips_settle_gates *g, const fips_node_addr *peer) { forget(g,peer,true,false); }
fips_status fips_settle_gate_ended(fips_settle_gates *g, fips_node_addr peer,
    const uint8_t *epoch, const uint8_t *setup, fips_instant now, const fips_instant *memory,
    const fips_settle_params *p, bool *started, fips_duration *length)
{
    fips_instant at=memory ? *memory : now;
    if (!valid(p) || !room(g,&peer,now,at,p)) return FIPS_ERR_RANGE;
    const fips_settle_memory *prior=find(g,&peer);
    if (setup && prior && live(prior,now,at,p)) {
        const fips_settle_silence *s=&prior->silence;
        bool reset=!s->present || expired(s,now,p) || (s->has_epoch && epoch && memcmp(s->epoch,epoch,8));
        bool seen=false;
        for (size_t i=0;i<s->counted_count;++i) if (!memcmp(s->counted[i],setup,32)) seen=true;
        if (!reset && !seen && s->counted_count==FIPS_SETTLE_GATE_EPHEMERALS_MAX && s->counted_count!=p->counted)
            return FIPS_ERR_RANGE;
    }
    lapse(g,now,at,p); fips_settle_memory *m=entry(g,peer); fips_settle_silence *r=&m->silence;
    if (!r->present || expired(r,now,p) || (r->has_epoch && epoch && memcmp(r->epoch,epoch,8)))
        *r=(fips_settle_silence){.present=true,.last=now};
    if (!r->has_epoch && epoch) { r->has_epoch=true; memcpy(r->epoch,epoch,8); }
    *started=false; *length=(fips_duration){0};
    if (setup) {
        for (size_t i=0;i<r->counted_count;++i) if (!memcmp(r->counted[i],setup,32)) return FIPS_OK;
        if (r->counted_count==p->counted) {
            memmove(r->counted,r->counted+1,(r->counted_count-1)*32); --r->counted_count;
        }
        memcpy(r->counted[r->counted_count++],setup,32);
    }
    if (r->count<UINT32_MAX) ++r->count;
    r->last=now;
    uint32_t limit=SETTLE_FAULT(SilentEarly) && p->silent ? p->silent-1 : p->silent;
    if (r->count<limit) return FIPS_OK;
    *length=fips_backoff(SETTLE_FAULT(SilentFlat) ? 0 : r->count-limit,p->silentbase,p->silentcap); *started=true;
    r->until=settle_deadline(now,*length); r->has_until=true; return FIPS_OK;
}
fips_status fips_settle_gate_redialed(fips_settle_gates *g, fips_node_addr peer,
    const uint8_t ephemeral[32], fips_instant now, const fips_settle_params *p)
{
    if (!valid(p)) return FIPS_ERR_RANGE;
    if (SETTLE_FAULT(RedialUngated)) return FIPS_OK;
    fips_settle_memory *m=find(g,&peer);
    if (!m) { m=entry(g,peer); if (!m) return FIPS_ERR_RANGE; }
    m->redial_count=prune(m->redials,m->redial_count,now,p);
    remember(m->redials,&m->redial_count,ephemeral,now,p->replays); return FIPS_OK;
}
fips_status fips_settle_gate_swapped(fips_settle_gates *g, fips_node_addr peer, bool heard, fips_instant now)
{
    fips_settle_memory *m=find(g,&peer);
    if (heard) { if (!m) m=entry(g,peer); if (!m) return FIPS_ERR_RANGE; m->has_stamp=true; m->stamp=now; }
    if (m) m->redial_count=0;
    return FIPS_OK;
}
bool fips_settle_gate_silenced(const fips_settle_gates *g, const fips_node_addr *peer, const uint8_t *epoch, fips_instant now)
{ const fips_settle_memory *m=find(g,peer); return m && silencing(m,epoch,now); }
bool fips_settle_gate_refusing(const fips_settle_gates *g, const fips_node_addr *peer, fips_instant now, fips_settle_silencing *out)
{
    const fips_settle_memory *m=find(g,peer);
    if (!m || !m->silence.present || !m->silence.has_until || cmp(m->silence.until,now)<=0) return false;
    const fips_settle_silence *r=&m->silence;
    *out=(fips_settle_silencing){.has_epoch=r->has_epoch,.silent=r->count,
        .remaining=now.ops->saturating_duration_since(r->until,now)};
    if (r->has_epoch) memcpy(out->epoch,r->epoch,8);
    return true;
}
bool fips_settle_gate_count_refused(fips_settle_gates *g, const fips_node_addr *peer, uint32_t *out)
{
    fips_settle_memory *m=find(g,peer); if (!m || !m->silence.present) return false;
    if (m->silence.refused<UINT32_MAX) ++m->silence.refused;
    *out=m->silence.refused; return true;
}
uint32_t fips_settle_gate_take_refused(fips_settle_gates *g, const fips_node_addr *peer)
{
    fips_settle_memory *m=find(g,peer); if (!m || !m->silence.present) return 0;
    uint32_t n=m->silence.refused; m->silence.refused=0; return n;
}
