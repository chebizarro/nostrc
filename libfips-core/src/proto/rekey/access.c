#include "internal.h"

void fips_rekey_note_setup(fips_rekey *m, fips_rekey_record *setup) {
    if (!m->live)
        return;
    fips_rekey_record_absorb(setup, &m->record);
    m->record = *setup;
    fips_rekey_record_new(setup);
}
const fips_rekey_held_answer *fips_rekey_answer(const fips_rekey *m) {
    return m->live && m->cycle.kind == FIPS_REKEY_PendR && m->cycle.answer.present
               ? &m->cycle.answer
               : NULL;
}
const fips_noise_session *fips_rekey_keys(const fips_rekey *m, fips_rekey_slot s) {
    const fips_rekey_epoch *e = rk_epoch_const(m, s);
    return e ? e->session : NULL;
}
bool fips_rekey_tag(const fips_rekey *m, fips_rekey_slot s, fips_rekey_epoch_tag *tag) {
    const fips_rekey_epoch *e = rk_epoch_const(m, s);
    if (!e)
        return false;
    *tag = e->tag;
    return true;
}
fips_rekey_index fips_rekey_ours(const fips_rekey *m, fips_rekey_slot s) {
    const fips_rekey_epoch *e = rk_epoch_const(m, s);
    return e ? (fips_rekey_index){e->ids.lease.index, e->ids.lease.live} : (fips_rekey_index){0};
}
fips_rekey_index fips_rekey_theirs(const fips_rekey *m, fips_rekey_slot s) {
    const fips_rekey_epoch *e = rk_epoch_const(m, s);
    return e ? e->ids.theirs : (fips_rekey_index){0};
}
fips_instant fips_rekey_started(const fips_rekey *m, fips_rekey_slot s) {
    const fips_rekey_epoch *e = rk_epoch_const(m, s);
    return e ? e->started : (fips_instant){0};
}
fips_instant fips_rekey_base(const fips_rekey *m) {
    return m->live && m->cur.present ? m->cur.base : (fips_instant){0};
}
bool fips_rekey_k(const fips_rekey *m) { return m->live && m->cur.present && m->cur.k; }
uint64_t fips_rekey_sent(const fips_rekey *m) {
    return m->live && m->cur.present ? m->profile->keys->sent(m->cur.session) : 0;
}
int32_t fips_rekey_jitter_secs(const fips_rekey *m) { return m->live ? m->mem.jitter : 0; }
fips_instant fips_rekey_drain_deadline(const fips_rekey *m) {
    return m->live && m->drain.ep.present ? m->drain.deadline : (fips_instant){0};
}
fips_instant fips_rekey_last_cutover(const fips_rekey *m) {
    return m->live ? m->mem.last_cutover : (fips_instant){0};
}
static fips_rekey_epoch_view show(const fips_rekey_epoch *e) {
    if (!e || !e->present)
        return (fips_rekey_epoch_view){0};
    return (fips_rekey_epoch_view){true,       e->tag,  e->k,    e->role,
                                   e->started, e->base, e->recv, e->ext};
}
fips_rekey_view fips_rekey_get_view(const fips_rekey *m) {
    fips_rekey_view v = {0};
    if (!m->live)
        return v;
    v.identity = m->bond;
    v.proven = m->cur.present && m->bond.kind == FIPS_REKEY_BOND_Key;
    if (!v.proven) {
        v.identity.claimed = fips_rekey_bond_addr(&m->bond);
        v.identity.kind = FIPS_REKEY_BOND_Claimed;
    }
    v.has_peer_epoch = m->mem.has_peer_epoch;
    memcpy(v.peer_epoch, m->mem.peer_epoch, 8);
    v.last_rx = m->mem.last_rx;
    v.link_since = m->mem.link_since;
    v.current = show(&m->cur);
    v.previous = show(&m->drain.ep);
    v.pending = show(&m->cycle.ep);
    v.cycle = m->cycle.kind;
    v.wait = m->cycle.wait;
    v.candidates = m->cands.count;
    return v;
}
size_t fips_rekey_bindings(const fips_rekey *m, fips_rekey_binding out[3]) {
    size_t n = 0;
    if (!m->live)
        return 0;
    if (m->cycle.kind == FIPS_REKEY_Init && m->cycle.attempt.lease.live)
        out[n++] = (fips_rekey_binding){m->cycle.attempt.transport, m->cycle.attempt.lease.index,
                                        FIPS_REKEY_PURPOSE_Attempt};
    const fips_rekey_epoch *epochs[] = {&m->cycle.ep, &m->cur, &m->drain.ep};
    const fips_rekey_purpose purposes[] = {FIPS_REKEY_PURPOSE_Pending, FIPS_REKEY_PURPOSE_Current,
                                           FIPS_REKEY_PURPOSE_Previous};
    for (size_t i = 0; i < 3; ++i)
        if (epochs[i]->present && epochs[i]->ids.lease.live) {
            assert(n < 3);
            out[n++] = (fips_rekey_binding){epochs[i]->ids.transport, epochs[i]->ids.lease.index,
                                            purposes[i]};
        }
    return n;
}
static fips_rekey_seal_refusal reserve(fips_rekey *m, fips_rekey_epoch *ep, fips_instant now,
                                       fips_rekey_sealed *out) {
    if (!ep->base.ops)
        return FIPS_REKEY_SEAL_NotSealable;
    if (m->profile->keys->sent(ep->session) >= m->params.cfg.hard)
        return FIPS_REKEY_SEAL_Exhausted;
    fips_rekey_sealed s = {0};
    if (!m->profile->keys->reserve(ep->session, &s.counter, &s.key)) {
        fips_rekey_sealed_clear(&s);
        return FIPS_REKEY_SEAL_Exhausted;
    }
    s.tag = ep->tag;
    s.k = ep->k;
    s.elapsed = rk_since(now, ep->base);
    s.theirs = ep->ids.theirs;
    *out = s;
    fips_rekey_sealed_clear(&s);
    return FIPS_REKEY_SEAL_OK;
}
fips_rekey_seal_refusal fips_rekey_seal(fips_rekey *m, fips_instant now, fips_rekey_sealed *out) {
    if (!m->live || !m->cur.present)
        return FIPS_REKEY_SEAL_NotEstablished;
    return reserve(m, &m->cur, now, out);
}
fips_rekey_seal_refusal fips_rekey_seal_prompt(fips_rekey *m, fips_instant now,
                                               fips_rekey_epoch_tag tag,
                                               fips_rekey_sealed_prompt *out) {
    if (!m->live || !m->cur.present)
        return FIPS_REKEY_SEAL_NotEstablished;
    fips_rekey_epoch *ep = NULL;
    if (m->cur.tag == tag)
        ep = &m->cur;
    else if (m->cycle.kind == FIPS_REKEY_PendI && m->cycle.wait == FIPS_REKEY_WAIT_Evidence &&
             m->cycle.ep.tag == tag)
        ep = &m->cycle.ep;
    if (!ep || !ep->base.ops)
        return FIPS_REKEY_SEAL_NotSealable;
    fips_rekey_sealed_prompt p = {0};
    /* Build the bounded prompt before reserving a nonce: a bad local profile
     * cannot burn a counter and then ask the caller to retry the same prompt. */
    if (!m->profile->prompt(rk_since(now, ep->base), &p.plaintext) ||
        p.plaintext.len > FIPS_REKEY_WIRE_MAX)
        return FIPS_REKEY_SEAL_NotSealable;
    fips_rekey_seal_refusal status = reserve(m, ep, now, &p.sealed);
    if (status != FIPS_REKEY_SEAL_OK) {
        fips_rekey_sealed_prompt_clear(&p);
        return status;
    }
    p.has_route = ep->has_route;
    p.route = ep->route;
    *out = p;
    fips_rekey_sealed_prompt_clear(&p);
    return FIPS_REKEY_SEAL_OK;
}
