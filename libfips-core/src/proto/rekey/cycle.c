#include "internal.h"
void rk_pending_record(fips_rekey *m) {
    if (m->cycle.kind == FIPS_REKEY_PendR && m->cycle.answer.present)
        fips_rekey_record_push(&m->record, m->cycle.answer.ephemeral);
}
void rk_cycle_clear(fips_rekey_cycle *c) {
    fips_rekey_hs_clear(&c->attempt.hs);
    sodium_memzero(c, sizeof *c);
}
void rk_cycle_end(fips_rekey *m, fips_rekey_abandon why, fips_rekey_close closed,
                  fips_rekey_outputs *out) {
    switch (m->cycle.kind) {
    case FIPS_REKEY_Idle:
        break;
    case FIPS_REKEY_Init:
        if (m->cycle.attempt.lease.live) {
            fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Unbind);
            o->transport = m->cycle.attempt.transport;
            o->index = m->cycle.attempt.lease.index;
            o = rk_output(out, FIPS_REKEY_OUT_Release);
            o->lease = m->cycle.attempt.lease;
        }
        rk_abandoned(out, why);
        out->items[out->count - 1].close = closed;
        break;
    case FIPS_REKEY_PendI:
        rk_retire(m, &m->cycle.ep, out);
        break;
    case FIPS_REKEY_PendR:
        rk_pending_record(m);
        rk_retire(m, &m->cycle.ep, out);
        if (why != FIPS_REKEY_ABANDON_Closed)
            rk_abandoned(out, why);
        break;
    }
    rk_cycle_clear(&m->cycle);
}
