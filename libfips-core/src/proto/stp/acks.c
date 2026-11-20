/* STP adapter of the pinned mmp::delivery rule. Kept private to this subsystem
 * until the shared MMP port lands; no MMP frame/transport implementation here. */
#include <fips/proto/stp/state.h>
#include "internal.h"
fips_stp_peer *fips_stp_ensure_peer(fips_tree_state *s, fips_node_addr id);
static bool usable(const fips_stp_link_evidence *l) {
    return l->has_rr && l->rr.highest < l->next_counter;
}
static bool whole(const fips_stp_ack_state *a, const fips_stp_link_evidence *l) {
    return l->epoch == a->first_epoch && l->rr.highest != UINT64_MAX &&
           l->rr.highest + 1 == l->rr.received;
}
static void refill(fips_stp_ack_state *a, uint64_t epoch) {
    a->budget_epoch = epoch;
    a->loss_left = 3;
    a->unverified_left = 1;
}
fips_status fips_tree_state_record_announce(fips_tree_state *s, fips_node_addr id, uint64_t seq,
                                            uint64_t counter, const fips_stp_link_evidence *l,
                                            fips_instant now) {
    fips_stp_peer *p = fips_stp_ensure_peer(s, id);
    if (!p)
        return FIPS_ERR_RANGE;
    fips_stp_ack_state *a = &p->ack;
    bool fresh = !a->initialized || a->sequence != seq;
    if (!a->initialized) {
        a->initialized = true;
        a->first_epoch = l->epoch;
        refill(a, l->epoch);
    }
    if (fresh || a->budget_epoch != l->epoch)
        refill(a, l->epoch);
    a->sequence = seq;
    a->counter = counter;
    a->epoch = l->epoch;
    a->orphan = false;
    a->at_ms = stp_ms(s->flap.origin, now);
    a->sent = true;
    if (usable(l) && l->rr.highest < counter) {
        a->base_kind = 1;
        a->base = l->rr;
        a->complete = whole(a, l);
    } else if (!l->has_rr && l->epoch == a->first_epoch)
        a->base_kind = 2;
    else
        a->base_kind = 0;
    return FIPS_OK;
}
/* -1 ambiguous, 0 loss, 1 delivered. Checked subtraction rejects session resets. */
static int delivered(const fips_stp_ack_state *a, fips_stp_rr_counters rr) {
    uint64_t r0 = 0, o0 = 0, span;
    bool complete = true;
    if (a->base_kind == 1) {
        r0 = a->base.received;
        o0 = a->base.reordered;
        complete = a->complete;
        if (rr.highest < a->base.highest)
            return -1;
        span = rr.highest - a->base.highest;
    } else if (a->base_kind == 2) {
        if (rr.highest == UINT64_MAX)
            return -1;
        span = rr.highest + 1;
    } else
        return -1;
    if (rr.received < r0 || rr.reordered < o0)
        return -1;
    uint64_t got = rr.received - r0, reordered = rr.reordered - o0;
    if (got < reordered)
        return -1;
    uint64_t sure = got - reordered;
    if (got < span)
        return 0;
    return sure == span || (complete && got == span) ? 1 : -1;
}
bool fips_tree_state_check_announce(fips_tree_state *s, fips_node_addr id,
                                    const fips_stp_link_evidence *l, fips_instant now,
                                    fips_stp_resend_reason *out) {
    fips_stp_peer *p = (fips_stp_peer *)fips_tree_state_peer(s, id);
    if (!p || !p->ack.sent)
        return false;
    fips_stp_ack_state *a = &p->ack;
    uint64_t ms = stp_ms(s->flap.origin, now);
    if (l->epoch != a->budget_epoch)
        refill(a, l->epoch);
    if (a->epoch != l->epoch) {
        a->orphan = true;
        a->epoch = l->epoch;
        a->base_kind = 0;
        a->at_ms = ms;
    }
    bool has = usable(l), due = ms >= sat_add(a->at_ms, s->fallback_ms);
    fips_stp_resend_reason reason;
    if (!a->base_kind && has) {
        if (!a->orphan && l->rr.highest < a->counter) {
            a->base_kind = 1;
            a->base = l->rr;
            a->complete = whole(a, l);
            return false;
        }
        reason = a->orphan ? FIPS_STP_SessionChanged : FIPS_STP_Unverified;
    } else {
        int delivery = has && l->rr.highest >= a->counter ? delivered(a, l->rr) : -1;
        if (delivery == 1) {
            a->sent = false;
            return false;
        }
        if (delivery == 0 && a->loss_left)
            reason = FIPS_STP_Loss;
        else if (due)
            reason = FIPS_STP_Timeout;
        else
            return false;
    }
    bool loss = reason == FIPS_STP_Loss;
    if (!(loss ? a->loss_left : a->unverified_left))
        return false;
    if (a->resent) {
        if (ms >= sat_add(a->resent_ms, 120000))
            a->level = 0;
        uint64_t gap = a->level ? fips_backoff_ms(a->level - 1, 1000, 60000) : 0;
        if (ms < sat_add(a->resent_ms, gap))
            return false;
    }
    if (loss)
        a->loss_left--;
    else
        a->unverified_left--;
    if (a->level < 7)
        a->level++;
    a->resent = true;
    a->resent_ms = ms;
    *out = reason;
    return true;
}
bool fips_tree_state_outstanding_counter(const fips_tree_state *s, fips_node_addr id,
                                         uint64_t *out) {
    const fips_stp_peer *p = fips_tree_state_peer(s, id);
    if (!p || !p->ack.sent)
        return false;
    *out = p->ack.counter;
    return true;
}
bool fips_tree_state_outstanding_seq(const fips_tree_state *s, fips_node_addr id, uint64_t *out) {
    const fips_stp_peer *p = fips_tree_state_peer(s, id);
    if (!p || !p->ack.sent)
        return false;
    *out = p->ack.sequence;
    return true;
}
void fips_tree_state_set_fallback(fips_tree_state *s, uint64_t ms) { s->fallback_ms = ms; }
