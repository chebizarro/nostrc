#include "internal.h"
const fips_rekey_epoch *rk_epoch_const(const fips_rekey *m, fips_rekey_slot slot) {
    const fips_rekey_epoch *e = NULL;
    if (!m->live)
        return NULL;
    switch (slot) {
    case FIPS_REKEY_Current:
        e = &m->cur;
        break;
    case FIPS_REKEY_Previous:
        e = &m->drain.ep;
        break;
    case FIPS_REKEY_Pending:
        e = &m->cycle.ep;
        break;
    }
    return e && e->present ? e : NULL;
}
fips_rekey_epoch *rk_epoch(fips_rekey *m, fips_rekey_slot slot) {
    /* Same selection without discarding constness from a borrowed accessor. */
    if (!m->live)
        return NULL;
    switch (slot) {
    case FIPS_REKEY_Current:
        return m->cur.present ? &m->cur : NULL;
    case FIPS_REKEY_Previous:
        return m->drain.ep.present ? &m->drain.ep : NULL;
    case FIPS_REKEY_Pending:
        return m->cycle.ep.present ? &m->cycle.ep : NULL;
    }
    return NULL;
}
void rk_epoch_new(fips_rekey *m, fips_rekey_epoch *ep, fips_rekey_completed *done,
                  fips_rekey_ids *ids, bool k, fips_rekey_role role, fips_instant now,
                  fips_rekey_env *env) {
    size_t i = 0;
    while (i < 3 && m->used[i])
        ++i;
    assert(i < 3);
    memset(ep, 0, sizeof *ep);
    ep->present = true;
    ep->tag = fips_rekey_tag_source_draw(env->tags);
    ep->session = &m->sessions[i];
    fips_noise_status status = fips_noise_session_take_cleared(&done->session, ep->session);
    assert(status == FIPS_NOISE_OK);
    (void)status;
    m->used[i] = true;
    ep->ids = *ids;
    memset(ids, 0, sizeof *ids);
    ep->k = k;
    ep->role = role;
    ep->started = now;
    ep->recv = FIPS_REKEY_Here;
    fips_rekey_completed_clear(done);
}
void rk_epoch_move(fips_rekey_epoch *from, fips_rekey_epoch *to) {
    assert(!to->present);
    *to = *from;
    sodium_memzero(from, sizeof *from);
}
void rk_retire(fips_rekey *m, fips_rekey_epoch *e, fips_rekey_outputs *out) {
    if (!e->present)
        return;
    if (e->ids.lease.live) {
        fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Unbind);
        o->transport = e->ids.transport;
        o->index = e->ids.lease.index;
        if (e->recv == FIPS_REKEY_Delegated) {
            o = rk_output(out, FIPS_REKEY_OUT_Revoke);
            o->tag = e->tag;
            o->transport = e->ids.transport;
            o->index = e->ids.lease.index;
        }
        o = rk_output(out, FIPS_REKEY_OUT_Release);
        o->lease = e->ids.lease;
        e->ids.lease = (fips_rekey_index_lease){0};
    }
    size_t i = (size_t)(e->session - m->sessions);
    assert(i < 3);
    fips_noise_session_clear_slot(e->session);
    m->used[i] = false;
    rk_output(out, FIPS_REKEY_OUT_Retired)->tag = e->tag;
    sodium_memzero(e, sizeof *e);
}
rk_trial rk_epoch_trial(const fips_rekey *m, const fips_rekey_epoch *ep, fips_rekey_frame_hint h,
                        const uint8_t *buf, size_t len, fips_rekey_env *env, size_t *opened) {
    if ((ep->has_floor && h.counter <= ep->floor) ||
        !m->profile->keys->fresh(ep->session, h.counter))
        return RK_REFUSED;
    return m->profile->keys->open(ep->session, m->profile->aad, h, buf, len, env->scratch,
                                  env->scratch_capacity, opened)
               ? RK_OPENED
               : RK_FAILED;
}
void rk_delegate(const fips_rekey *m, const fips_rekey_epoch *ep, bool workers,
                 fips_rekey_outputs *out) {
    if (!ep->present || !workers || ep->recv == FIPS_REKEY_Delegated || !ep->ids.lease.live)
        return;
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Delegate);
    o->tag = ep->tag;
    o->transport = ep->ids.transport;
    o->index = ep->ids.lease.index;
    m->profile->keys->copy(ep->session, ep->has_floor, ep->floor, &o->data.recv);
}
