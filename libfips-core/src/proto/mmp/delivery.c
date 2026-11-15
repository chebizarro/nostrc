#include <fips/proto/mmp/delivery.h>
#include "internal.h"
#include <string.h>
bool fips_mmp_link_evidence_usable_rr(const fips_mmp_link_evidence *l, fips_mmp_rr_counters *out)
{ if (!l->has_rr || l->rr.highest>=l->next_counter) return false; *out=l->rr; return true; }
static size_t find_peer(const fips_mmp_acks *s, const fips_node_addr *peer)
{ size_t i=0; while (i<s->len && memcmp(&s->peers[i].peer, peer, sizeof(*peer))) ++i; return i; }
static void refill(fips_mmp_ack_state *a, uint64_t epoch)
{ a->budget_epoch=epoch; a->unverified_left=FIPS_MMP_DELIVERY_UNVERIFIED_BUDGET; a->loss_left=FIPS_MMP_DELIVERY_LOSS_BUDGET; }
static bool whole(const fips_mmp_ack_state *a, uint64_t epoch, fips_mmp_rr_counters rr)
{ return epoch==a->first_epoch && rr.highest!=UINT64_MAX && rr.highest+1==rr.received; }
static uint64_t gap(uint8_t level)
{ if (!level) return 0; uint64_t g=UINT64_C(1000)<<(level-1); return g<FIPS_MMP_DELIVERY_MAXGAP_MS ? g : FIPS_MMP_DELIVERY_MAXGAP_MS; }
static bool backoff_allows(fips_mmp_ack_state *a, uint64_t now)
{
    if (!a->has_resent) return true;
    if (now>=mmp_add(a->resent_ms, FIPS_MMP_DELIVERY_QUIET_MS)) a->level=0;
    return now>=mmp_add(a->resent_ms, gap(a->level));
}
/* -1 ambiguous, 0 loss, 1 delivery; subtraction is checked, never saturating. */
static int delivered(const fips_mmp_sent_announce *s, fips_mmp_rr_counters rr)
{
    uint64_t r0=0, o0=0, span;
    bool complete=true;
    if (s->base_kind==FIPS_MMP_BaseCounted) {
        r0=s->base.received; o0=s->base.reordered; complete=s->complete;
        if (rr.highest<s->base.highest) return -1;
        span=rr.highest-s->base.highest;
    } else if (s->base_kind==FIPS_MMP_BaseZero) {
        if (rr.highest==UINT64_MAX) return -1;
        span=rr.highest+1;
    } else return -1;
    if (rr.received<r0 || rr.reordered<o0) return -1;
    uint64_t got=rr.received-r0, reorders=rr.reordered-o0;
    if (got<reorders) return -1;
    uint64_t sure=got-reorders;
    if (got<span) return 0;
    if (sure==span || (complete && got==span)) return 1;
    return -1;
}
void fips_mmp_acks_init(fips_mmp_acks *s, fips_mmp_ack_state *storage, size_t capacity)
{ *s=(fips_mmp_acks){FIPS_MMP_DELIVERY_FALLBACK_MS, storage, 0, capacity}; }
void fips_mmp_acks_set_fallback(fips_mmp_acks *s, uint64_t ms) { s->fallback_ms=ms; }
fips_status fips_mmp_acks_record(fips_mmp_acks *s, fips_node_addr peer, bool fresh, uint64_t counter, const fips_mmp_link_evidence *l, uint64_t now)
{
    size_t i=find_peer(s, &peer);
    if (i==s->len) {
        if (s->len==s->capacity) return FIPS_ERR_RANGE;
        s->peers[i]=(fips_mmp_ack_state){.peer=peer, .first_epoch=l->epoch}; refill(&s->peers[i], l->epoch); ++s->len;
    }
    fips_mmp_ack_state *a=&s->peers[i];
    if (fresh || l->epoch!=a->budget_epoch) refill(a, l->epoch);
    fips_mmp_sent_announce sent={.counter=counter, .epoch=l->epoch, .at_ms=now};
    fips_mmp_rr_counters rr;
    if (fips_mmp_link_evidence_usable_rr(l, &rr) && rr.highest<counter) {
        sent.base_kind=FIPS_MMP_BaseCounted; sent.base=rr; sent.complete=whole(a, l->epoch, rr);
    } else if (!l->has_rr && l->epoch==a->first_epoch) sent.base_kind=FIPS_MMP_BaseZero;
    a->sent=sent; a->has_sent=true; return FIPS_OK;
}
bool fips_mmp_acks_check(fips_mmp_acks *s, const fips_node_addr *peer, const fips_mmp_link_evidence *l, uint64_t now, fips_mmp_resend_reason *out)
{
    size_t i=find_peer(s, peer);
    if (i==s->len || !s->peers[i].has_sent) return false;
    fips_mmp_ack_state *a=&s->peers[i];
    fips_mmp_sent_announce *sent=&a->sent;
    if (l->epoch!=a->budget_epoch) refill(a, l->epoch);
    if (sent->epoch!=l->epoch) {
        sent->orphan=true; sent->epoch=l->epoch; sent->base_kind=FIPS_MMP_BaseUnknown; sent->at_ms=now;
    }
    fips_mmp_rr_counters rr;
    bool usable=fips_mmp_link_evidence_usable_rr(l, &rr);
    bool due=now>=mmp_add(sent->at_ms, s->fallback_ms);
    fips_mmp_resend_reason candidate;
    if (sent->base_kind==FIPS_MMP_BaseUnknown) {
        if (usable) {
            if (!sent->orphan && rr.highest<sent->counter) {
                sent->base_kind=FIPS_MMP_BaseCounted; sent->base=rr; sent->complete=whole(a, l->epoch, rr); return false;
            }
            candidate=sent->orphan ? FIPS_MMP_SessionChanged : FIPS_MMP_Unverified;
        } else { if (!due) return false; candidate=FIPS_MMP_Timeout; }
    } else {
        int delivery=usable && rr.highest>=sent->counter ? delivered(sent, rr) : -1;
        if (delivery==1) { a->has_sent=false; return false; }
        if (delivery==0 && a->loss_left) candidate=FIPS_MMP_Loss;
        else { if (!due) return false; candidate=FIPS_MMP_Timeout; }
    }
    bool loss=candidate==FIPS_MMP_Loss;
    uint8_t left=loss ? a->loss_left : a->unverified_left;
    if (!left || !backoff_allows(a, now)) return false;
    if (loss) --a->loss_left; else --a->unverified_left;
    if (a->level<7) ++a->level;
    a->resent_ms=now; a->has_resent=true; *out=candidate; return true;
}
bool fips_mmp_acks_outstanding(const fips_mmp_acks *s, const fips_node_addr *peer, uint64_t *counter)
{ size_t i=find_peer(s, peer); if (i==s->len || !s->peers[i].has_sent) return false; *counter=s->peers[i].sent.counter; return true; }
void fips_mmp_acks_remove(fips_mmp_acks *s, const fips_node_addr *peer)
{ size_t i=find_peer(s, peer); if (i==s->len) return; memmove(s->peers+i, s->peers+i+1, (s->len-i-1)*sizeof(*s->peers)); --s->len; }
