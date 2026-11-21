#include <fips/proto/settle/settle.h>
#include <fips/proto/settle/faults.h>
#include <string.h>

static fips_settle_leg *find(const fips_settle_legs *s, uint64_t link)
{
    for (size_t i=0; i<s->capacity; ++i)
        if (s->slots[i].active && s->slots[i].link==link) return &s->slots[i];
    return NULL;
}
static int cmp(fips_instant a, fips_instant b) { return a.ops->compare(a,b); }
void fips_settle_legs_init(fips_settle_legs *s, fips_settle_leg *slots, size_t capacity)
{
    if (capacity) memset(slots,0,capacity*sizeof(*slots));
    *s=(fips_settle_legs){.slots=slots,.capacity=capacity};
}
fips_status fips_settle_leg_open(fips_settle_legs *s, const fips_settle_start *start,
    bool answer, const uint8_t *msg2, size_t msg2_len, fips_instant now,
    const fips_settle_params *p, fips_settle_leg *old)
{
    if (!start->lease->live || start->msg1_len>FIPS_SETTLE_MESSAGE_CAPACITY ||
        msg2_len>FIPS_SETTLE_MESSAGE_CAPACITY) return FIPS_ERR_RANGE;
    fips_settle_leg *slot=find(s,start->link);
    if (!slot) for (size_t i=0; i<s->capacity; ++i) if (!s->slots[i].active) { slot=&s->slots[i]; break; }
    if (!slot) return FIPS_ERR_RANGE;
    uint32_t cap=answer ? 0 : start->has_cap && !SETTLE_FAULT(OriginIgnored) && start->cap<p->resends ? start->cap : p->resends;
    fips_settle_leg leg={.active=true,.answer=answer,.has_peer=start->has_peer,
        .has_held=start->has_held,.link=start->link,.held=start->held,.peer=start->peer,
        .msg1_len=start->msg1_len,.msg2_len=answer ? msg2_len : 0,.resends=cap,
        .has_next=cap>0,.deadline=settle_deadline(now,p->handshake),.lease=*start->lease};
    if (cap) leg.next=settle_deadline(now,SETTLE_FAULT(FirstResendAtInterval) ? p->resend : p->first);
    if (leg.msg1_len) memcpy(leg.msg1,start->msg1,leg.msg1_len);
    if (leg.msg2_len) memcpy(leg.msg2,msg2,leg.msg2_len);
    *old=*slot;
    *slot=leg;
    *start->lease=(fips_rekey_index_lease){0};
    return FIPS_OK;
}
const fips_settle_leg *fips_settle_leg_get(const fips_settle_legs *s, uint64_t link) { return find(s,link); }
bool fips_settle_leg_take(fips_settle_legs *s, uint64_t link, fips_settle_leg *out)
{
    fips_settle_leg *leg=find(s,link);
    if (!leg) return false;
    *out=*leg;
    memset(leg,0,sizeof(*leg));
    return true;
}
const uint8_t *fips_settle_leg_stored(const fips_settle_legs *s, uint64_t link,
    const uint8_t *msg1, size_t msg1_len, size_t *msg2_len)
{
    const fips_settle_leg *leg=find(s,link);
    if (!leg || !leg->answer || SETTLE_FAULT(AnswerFresh) || leg->msg1_len!=msg1_len ||
        (msg1_len && memcmp(leg->msg1,msg1,msg1_len))) return NULL;
    *msg2_len=leg->msg2_len;
    return leg->msg2;
}
void fips_settle_leg_resent(fips_settle_legs *s, uint64_t link, fips_instant now, const fips_settle_params *p)
{
    fips_settle_leg *leg=find(s,link);
    if (!leg) return;
    leg->retry=false;
    if (leg->sent<UINT32_MAX) ++leg->sent;
    leg->has_next=leg->sent<leg->resends;
    if (leg->has_next) leg->next=settle_deadline(now,fips_settle_wait(p,leg->sent));
}
void fips_settle_leg_unsent(fips_settle_legs *s, uint64_t link, fips_instant now, const fips_settle_params *p)
{
    fips_settle_leg *leg=find(s,link);
    if (!leg || leg->answer || (!leg->has_next && !SETTLE_FAULT(UnsentPastCap))) return;
    if (SETTLE_FAULT(UnsentStaysDue)) return;
    leg->retry=!SETTLE_FAULT(UnsentDueAtWake); leg->has_next=true;
    leg->next=settle_deadline(now,p->retry);
}
size_t fips_settle_legs_answering(const fips_settle_legs *s)
{
    size_t n=0;
    for (size_t i=0; i<s->capacity; ++i) if (s->slots[i].active && s->slots[i].answer) ++n;
    return n;
}
bool fips_settle_legs_next_wake(const fips_settle_legs *s, fips_instant *out)
{
    bool found=false;
    fips_instant best={0};
    for (size_t i=0; i<s->capacity; ++i) {
        const fips_settle_leg *leg=&s->slots[i];
        if (!leg->active) continue;
        fips_instant at=leg->has_next && cmp(leg->next,leg->deadline)<0 ? leg->next : leg->deadline;
        if (!found || cmp(at,best)<0) { best=at; found=true; }
    }
    if (found) *out=best;
    return found;
}
static bool arrived(fips_instant at,fips_instant now)
{
    if (SETTLE_FAULT(PollAhead)) { fips_instant ahead; if (now.ops->checked_add(now,(fips_duration){0,1000000},&ahead)!=FIPS_OK) return true; now=ahead; }
    return cmp(at,now)<=0;
}
static bool reaped(const fips_settle_leg *leg, fips_instant now, bool all)
{
    return all || (!SETTLE_FAULT(LegNeverReaped) && arrived(leg->deadline,now));
}
static bool due(const fips_settle_leg *leg, fips_instant now)
{
    return leg->retry || (leg->has_next && arrived(leg->next,now));
}
/* Iterate by key rather than slot so reusing holes preserves BTreeMap order. */
static fips_settle_leg *after(fips_settle_legs *s, bool first, uint64_t previous)
{
    fips_settle_leg *next=NULL;
    for (size_t i=0; i<s->capacity; ++i) {
        fips_settle_leg *leg=&s->slots[i];
        if (leg->active && (first || leg->link>previous) && (!next || leg->link<next->link)) next=leg;
    }
    return next;
}
fips_status fips_settle_legs_poll(fips_settle_legs *s, fips_settle_links *links,
    fips_instant now, bool close_all, fips_settle_leg_out *out, size_t capacity, size_t *count,
    fips_settle_pair *closes, size_t close_capacity, size_t *close_count)
{
    fips_settle_fault_resends_begin();
    size_t need=0, close_need=0;
    for (size_t i=0; i<s->capacity; ++i) {
        const fips_settle_leg *leg=&s->slots[i];
        if (!leg->active) continue;
        bool reap=reaped(leg,now,close_all);
        if (reap || due(leg,now)) ++need;
        if (reap) {
            const fips_settle_link *r=fips_settle_link_get(links,leg->link);
            if (r) {
                if (r->path_count==SIZE_MAX || close_need>SIZE_MAX-r->path_count-1) return FIPS_ERR_RANGE;
                close_need+=r->path_count+1;
            }
        }
    }
    if (need>capacity || close_need>close_capacity) return FIPS_ERR_RANGE;
    size_t n=0, cn=0;
    for (unsigned pass=0; pass<2; ++pass) {
        bool first=true;
        uint64_t previous=0;
        fips_settle_leg *leg;
        while ((leg=after(s,first,previous))!=NULL) {
            previous=leg->link;
            first=false;
            if (pass==0 && reaped(leg,now,close_all)) {
                fips_settle_leg_out o={.reaped=true,.dialed=!leg->answer,
                    .has_peer=leg->has_peer,.peer=leg->peer,.link=leg->link,
                    .lease=leg->lease,.close_offset=cn};
                /* Preflight proves this cannot fail, even with shared paths. */
                size_t written=0;
                (void)fips_settle_link_retire(links,leg->link,closes ? closes+cn : NULL,
                    close_capacity-cn,&written);
                o.close_count=written;
                cn+=written;
                if (close_all && n==0 && SETTLE_FAULT(LeaseKept)) { fips_rekey_index_lease_drop(&o.lease); }
                out[n++]=o;
                memset(leg,0,sizeof(*leg));
            } else if (pass==1 && !close_all && due(leg,now)) {
                fips_settle_leg_out o={.link=leg->link,.bytes=leg->msg1,.length=leg->msg1_len};
                if (SETTLE_FAULT(ResendFresh)) o.bytes=fips_settle_fault_resend(o.bytes,o.length,&o.length);
                out[n++]=o;
            }
        }
    }
    *count=n;
    *close_count=cn;
    return FIPS_OK;
}
