#include "internal.h"
#ifdef FIPS_TEST_HOOKS
void fips_rekey_start_now(fips_rekey *m, fips_instant now, fips_rekey_env *env,
                          fips_rekey_outputs *out) {
    if (m->live && !out->count && m->cycle.kind == FIPS_REKEY_Idle)
        rk_start(m, now, env, out);
}
void fips_rekey_trigger(fips_rekey *m, fips_instant now, fips_rekey_env *env,
                        fips_rekey_outputs *out) {
    if (!m->live || out->count)
        return;
    fips_duration after = m->params.cfg.after;
    int32_t jitter = m->mem.jitter;
    m->params.cfg.after = rk_secs(0);
    m->mem.jitter = INT32_MIN;
    (void)fips_rekey_step(m, now, (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, env, out);
    m->params.cfg.after = after;
    if (m->mem.jitter == INT32_MIN)
        m->mem.jitter = jitter;
}
void fips_rekey_quiet(fips_rekey *m) {
    if (!m->live)
        return;
    m->params.cfg.after = rk_secs(FIPS_REKEY_MAX_AFTER);
    m->params.cfg.messages = m->params.cfg.soft;
}
static void backdate(fips_instant *at, fips_duration age) {
    if (!at->ops)
        return;
    fips_instant earlier;
    if (at->ops->checked_sub(*at, age, &earlier) == FIPS_OK)
        *at = earlier;
}
void fips_rekey_backdate_started(fips_rekey *m, fips_duration age) {
    if (!m->live || !m->cur.present)
        return;
    backdate(&m->cur.started, age);
    m->mem.link_since = rk_min(m->mem.link_since, m->cur.started);
}
void fips_rekey_backdate_base(fips_rekey *m, fips_duration age) {
    if (m->live && m->cur.present)
        backdate(&m->cur.base, age);
}
void fips_rekey_backdate_drain(fips_rekey *m, fips_duration age) {
    if (m->live && m->drain.ep.present)
        backdate(&m->drain.deadline, age);
}
void fips_rekey_backdate_pending(fips_rekey *m, fips_duration age) {
    if (!m->live || !m->cycle.ep.present)
        return;
    backdate(&m->cycle.ep.started, age);
    if (m->cycle.kind == FIPS_REKEY_PendR)
        backdate(&m->cycle.deadline, age);
}
void fips_rekey_backdate_dampening(fips_rekey *m, fips_duration age) {
    if (m->live)
        backdate(&m->mem.damp, age);
}
void fips_rekey_force_k(fips_rekey *m, bool k) {
    if (m->live && m->cur.present)
        m->cur.k = k;
}
void fips_rekey_force_peer_epoch(fips_rekey *m, const uint8_t *epoch) {
    if (!m->live)
        return;
    m->mem.has_peer_epoch = epoch != NULL;
    if (epoch)
        memcpy(m->mem.peer_epoch, epoch, 8);
    else
        memset(m->mem.peer_epoch, 0, 8);
}
bool fips_rekey_dampened(const fips_rekey *m, fips_instant now) {
    return m->live && m->mem.damp.ops && rk_before(now, m->mem.damp);
}
bool fips_rekey_reserve_slot(fips_rekey *m, fips_rekey_slot slot, uint64_t *counter,
                             fips_cipher_state *key) {
    fips_rekey_epoch *e = rk_epoch(m, slot);
    return e && m->profile->keys->reserve(e->session, counter, key);
}
void fips_rekey_offer_held(const fips_rekey *m, fips_rekey_outputs *out) {
    if (m->live && !out->count)
        rk_delegate(m, &m->cur, true, out);
}
bool fips_rekey_cleared_slots_are_zero(const fips_rekey *m) {
    for (size_t i = 0; i < 3; ++i)
        if (!m->used[i] &&
            !sodium_is_zero((const unsigned char *)&m->sessions[i], sizeof m->sessions[i]))
            return false;
    if (m->cycle.kind != FIPS_REKEY_Init &&
        !sodium_is_zero((const unsigned char *)&m->cycle.attempt.hs, sizeof m->cycle.attempt.hs))
        return false;
    for (size_t i = m->cands.count; i < FIPS_REKEY_CANDIDATES_MAX; ++i)
        if (!sodium_is_zero((const unsigned char *)&m->cands.list[i], sizeof m->cands.list[i]))
            return false;
    return true;
}
#endif
