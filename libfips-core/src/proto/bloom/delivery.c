/* Bloom's bounded instance of proto::mmp::delivery::Acks. No MMP codec or IO. */
#include <fips/proto/bloom/state.h>
#include <string.h>

static uint64_t sat_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
static bool usable(const fips_bloom_link_evidence *ev) { return ev->has_rr && ev->rr.highest < ev->next_counter; }
static bool whole(const fips_bloom_ack *ack, uint64_t epoch, fips_bloom_rr rr)
{
    return epoch == ack->first_epoch && rr.highest != UINT64_MAX && rr.highest + 1 == rr.received;
}
static void refill(fips_bloom_ack *ack, uint64_t epoch)
{
    ack->budget_epoch = epoch;
    ack->unverified_left = FIPS_BLOOM_UNVERIFIED_BUDGET;
    ack->loss_left = FIPS_BLOOM_LOSS_BUDGET;
}
void fips_bloom_peer_record_announce(fips_bloom_peer *p, const fips_bloom_filter *f, uint64_t counter, const fips_bloom_link_evidence *ev, uint64_t now)
{
    fips_bloom_ack *ack = &p->ack;
    if (!ack->initialized) {
        memset(ack, 0, sizeof(*ack)); ack->initialized = true;
        ack->first_epoch = ev->epoch; refill(ack, ev->epoch);
    }
    if (!p->sent_len || !fips_bloom_equal(f, &p->sent[0]) || ev->epoch != ack->budget_epoch) refill(ack, ev->epoch);
    ack->base = 0; /* unknown */
    if (usable(ev) && ev->rr.highest < counter) {
        ack->base = 1; ack->base_rr = ev->rr; ack->base_complete = whole(ack, ev->epoch, ev->rr);
    } else if (!ev->has_rr && ev->epoch == ack->first_epoch) ack->base = 2; /* zero */
    ack->counter = counter; ack->epoch = ev->epoch; ack->at_ms = now;
    ack->outstanding = true; ack->orphan = false;
}
/* -1 ambiguous, 0 loss, 1 delivered. Checked arithmetic is essential: an
 * inconsistent or cross-session pair is not delivery (nor proof of loss). */
static int delivered(const fips_bloom_ack *ack, fips_bloom_rr rr)
{
    uint64_t r0 = 0, span;
    uint32_t o0 = 0;
    bool complete = true;
    if (ack->base == 1) {
        if (rr.highest < ack->base_rr.highest) return -1;
        span = rr.highest - ack->base_rr.highest;
        r0 = ack->base_rr.received; o0 = ack->base_rr.reordered;
        complete = ack->base_complete;
    } else if (ack->base == 2) {
        if (rr.highest == UINT64_MAX) return -1;
        span = rr.highest + 1;
    } else return -1;
    if (rr.received < r0 || rr.reordered < o0) return -1;
    uint64_t got = rr.received - r0, reordered = rr.reordered - o0;
    if (got < reordered) return -1;
    uint64_t sure = got - reordered;
    if (got < span) return 0;
    if (sure == span || (complete && got == span)) return 1;
    return -1;
}
fips_bloom_resend fips_bloom_peer_check_announce(fips_bloom_state *s, fips_bloom_peer *p, const fips_bloom_link_evidence *ev, uint64_t now)
{
    fips_bloom_ack *ack = &p->ack;
    if (!ack->outstanding) return FIPS_BLOOM_RESEND_NONE;
    if (ev->epoch != ack->budget_epoch) refill(ack, ev->epoch);
    if (ack->epoch != ev->epoch) {
        ack->orphan = true; ack->epoch = ev->epoch; ack->base = 0; ack->at_ms = now;
    }
    bool has_rr = usable(ev), due = now >= sat_add(ack->at_ms, s->fallback_ms);
    fips_bloom_resend candidate = FIPS_BLOOM_RESEND_NONE;
    if (ack->base == 0 && has_rr) {
        if (!ack->orphan && ev->rr.highest < ack->counter) {
            ack->base = 1; ack->base_rr = ev->rr; ack->base_complete = whole(ack, ev->epoch, ev->rr);
            return FIPS_BLOOM_RESEND_NONE;
        }
        candidate = ack->orphan ? FIPS_BLOOM_RESEND_SESSION_CHANGED : FIPS_BLOOM_RESEND_UNVERIFIED;
    } else if (ack->base != 0 && has_rr && ev->rr.highest >= ack->counter) {
        int d = delivered(ack, ev->rr);
        if (d == 1) { ack->outstanding = false; return FIPS_BLOOM_RESEND_NONE; }
        if (d == 0 && ack->loss_left) candidate = FIPS_BLOOM_RESEND_LOSS;
    }
    if (candidate == FIPS_BLOOM_RESEND_NONE && due) candidate = FIPS_BLOOM_RESEND_TIMEOUT;
    if (candidate == FIPS_BLOOM_RESEND_NONE) return candidate;
    /* A pending send already carries this retry (or fresher content). Keep
     * processing confirmations above, but do not spend another retry credit
     * while debounce or a transport refusal holds that send. */
    if (p->pending) return FIPS_BLOOM_RESEND_NONE;
    bool loss = candidate == FIPS_BLOOM_RESEND_LOSS;
    if (!(loss ? ack->loss_left : ack->unverified_left)) return FIPS_BLOOM_RESEND_NONE;
    if (ack->has_resent) {
        if (now >= sat_add(ack->resent_ms, FIPS_BLOOM_QUIET_MS)) ack->level = 0;
        uint64_t gap = ack->level ? (UINT64_C(1000) << (ack->level - 1)) : 0;
        if (gap > 60000) gap = 60000;
        if (now < sat_add(ack->resent_ms, gap)) return FIPS_BLOOM_RESEND_NONE;
    }
    if (loss) --ack->loss_left; else --ack->unverified_left;
    if (ack->level < 7) ++ack->level;
    ack->resent_ms = now; ack->has_resent = true;
    p->pending = true; p->pending_resend = candidate;
    return candidate;
}
