#include "internal.h"

fips_rekey_output *rk_output(fips_rekey_outputs *out, fips_rekey_output_kind kind) {
    assert(out->count < FIPS_REKEY_OUTPUTS_MAX);
    fips_rekey_output *o = &out->items[out->count++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    return o;
}
void rk_send(fips_rekey_outputs *out, const fips_rekey_wire *msg, fips_rekey_msg_kind kind,
             fips_rekey_route route) {
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Send);
    o->data.msg = *msg;
    o->message_kind = kind;
    o->route = route;
}
void rk_reject(fips_rekey_outputs *out, fips_rekey_reason r) {
    rk_output(out, FIPS_REKEY_OUT_Reject)->reason = r;
}
void rk_abandoned(fips_rekey_outputs *out, fips_rekey_abandon why) {
    rk_output(out, FIPS_REKEY_OUT_Abandoned)->abandon = why;
}
void rk_prompt(fips_rekey_outputs *out, fips_rekey_epoch_tag tag) {
    rk_output(out, FIPS_REKEY_OUT_Prompt)->tag = tag;
}
/* Only items[0, count) ever hold data: rk_output is the sole appender and
 * writes items[count++]; in-place edits stay below count; step compaction wipes
 * the tail it drops. So wiping that prefix and count leaves the list exactly as
 * {0} (nostrc-02p6s; Noise audit addendum), and an empty list costs nothing
 * instead of the 58 KB full-struct wipe per executed step. A corrupt count
 * falls back to the full wipe. Debug builds check the slot after count is
 * still zero (a write past count); test_rekey.c guards the whole struct. */
void fips_rekey_outputs_clear(fips_rekey_outputs *out) {
    assert(out->count <= FIPS_REKEY_OUTPUTS_MAX);
    if (out->count > FIPS_REKEY_OUTPUTS_MAX) {
        sodium_memzero(out, sizeof *out);
        return;
    }
    size_t n = out->count;
    assert(n == FIPS_REKEY_OUTPUTS_MAX ||
           sodium_is_zero((const unsigned char *)&out->items[n], sizeof out->items[n]));
    for (size_t i = 0; i < n; ++i) {
        if (out->items[i].kind == FIPS_REKEY_OUT_Delegate)
            fips_rekey_recv_copy_clear(&out->items[i].data.recv);
        if (out->items[i].kind == FIPS_REKEY_OUT_Release)
            fips_rekey_index_lease_drop(&out->items[i].lease);
    }
    sodium_memzero(out->items, n * sizeof *out->items);
    sodium_memzero(&out->count, sizeof out->count);
}
void fips_rekey_sealed_clear(fips_rekey_sealed *s) {
    fips_cipher_state_destroy(&s->key);
    sodium_memzero(s, sizeof *s);
}
void fips_rekey_sealed_prompt_clear(fips_rekey_sealed_prompt *s) {
    fips_rekey_sealed_clear(&s->sealed);
    sodium_memzero(s, sizeof *s);
}
static void init(fips_rekey *m, const fips_rekey_profile *p, fips_rekey_params params,
                 fips_rekey_bond bond, fips_node_addr me, fips_instant now) {
    memset(m, 0, sizeof *m);
    m->live = true;
    m->profile = p;
    m->params = params;
    m->bond = bond;
    m->me = me;
    m->mem.link_since = now;
    m->mem.reachable = true;
}
fips_status fips_rekey_established(fips_rekey *m, const fips_rekey_profile *p,
                                   fips_rekey_params params, fips_rekey_bond bond,
                                   fips_node_addr me, fips_rekey_completed *first,
                                   fips_rekey_ids *ids, fips_rekey_setup_facts *facts,
                                   fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out) {
    if (!fips_rekey_profile_valid(p) || out->count || !fips_rekey_bond_check(&bond, first) ||
        p->indexed != ids->lease.live || p->indexed != ids->theirs.some)
        return FIPS_ERR_RANGE;
    init(m, p, params, bond, me, now);
    rk_epoch_new(m, &m->cur, first, ids, facts->k, FIPS_REKEY_Initiator, facts->started, env);
    m->cur.base = facts->started;
    m->mem.has_peer_epoch = facts->has_peer_epoch;
    memcpy(m->mem.peer_epoch, facts->peer_epoch, 8);
    m->mem.has_peer_k = facts->has_peer_k;
    m->mem.peer_k = facts->peer_k;
    m->mem.last_rx = facts->last_rx;
    m->mem.link_since = facts->link_since;
    m->mem.jitter = rk_jitter(env, params.cfg.jitter);
    m->record = facts->record;
    sodium_memzero(facts, sizeof *facts);
    if (m->cur.ids.lease.live) {
        fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Bind);
        o->transport = m->cur.ids.transport;
        o->index = m->cur.ids.lease.index;
        o->tag = m->cur.tag;
        o->has_tag = true;
        o->purpose = FIPS_REKEY_PURPOSE_Current;
    }
    rk_delegate(m, &m->cur, env->workers, out);
    return FIPS_OK;
}
fips_status fips_rekey_opening_outbound(fips_rekey *m, const fips_rekey_profile *p,
                                        fips_rekey_params params, fips_peer_key peer,
                                        fips_node_addr me, fips_instant now, fips_rekey_env *env,
                                        fips_rekey_outputs *out) {
    if (!fips_rekey_profile_valid(p) || p->layer != FIPS_REKEY_LAYER_Fsp || out->count)
        return FIPS_ERR_RANGE;
    fips_rekey_bond bond = {.kind = FIPS_REKEY_BOND_Key, .key = peer};
    init(m, p, params, bond, me, now);
    rk_start(m, now, env, out);
    if (m->cycle.kind == FIPS_REKEY_Idle)
        rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown = FIPS_REKEY_TEARDOWN_OpeningFailed;
    return FIPS_OK;
}
fips_status fips_rekey_opening_inbound(fips_rekey *m, const fips_rekey_profile *p,
                                       fips_rekey_params params, fips_node_addr claimed,
                                       fips_node_addr me, fips_instant now, fips_rekey_input setup,
                                       fips_rekey_env *env, fips_rekey_outputs *out) {
    if (!fips_rekey_profile_valid(p) || p->layer != FIPS_REKEY_LAYER_Fsp || out->count ||
        setup.kind != FIPS_REKEY_IN_Setup)
        return FIPS_ERR_RANGE;
    fips_rekey_bond bond = {.kind = FIPS_REKEY_BOND_Claimed, .claimed = claimed};
    init(m, p, params, bond, me, now);
    fips_status status = fips_rekey_step(m, now, setup, env, out);
    if (!m->cands.count)
        rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown = FIPS_REKEY_TEARDOWN_OpeningIdle;
    return status;
}
bool fips_rekey_close_machine(fips_rekey *m, fips_rekey_close why, fips_rekey_outputs *out,
                              fips_rekey_carry *carry) {
    if (!m->live || out->count)
        return false;
    rk_cycle_end(m, FIPS_REKEY_ABANDON_Closed, why, out);
    rk_retire(m, &m->drain.ep, out);
    rk_cand_clear(&m->cands);
    bool have = m->cur.present;
    if (have) {
        memset(carry, 0, sizeof *carry);
        carry->has_peer_epoch = m->mem.has_peer_epoch;
        memcpy(carry->peer_epoch, m->mem.peer_epoch, 8);
        carry->k = m->cur.k;
        carry->started = m->cur.started;
        carry->link_since = m->mem.link_since;
        carry->has_peer_k = m->mem.has_peer_k;
        carry->peer_k = m->mem.peer_k;
        carry->last_rx = m->mem.last_rx;
        carry->record = m->record;
        rk_retire(m, &m->cur, out);
    }
    fips_rekey_destroy(m);
    return have;
}
void fips_rekey_destroy(fips_rekey *m) {
    if (!m->live)
        return;
    fips_rekey_index_lease_drop(&m->cur.ids.lease);
    fips_rekey_index_lease_drop(&m->drain.ep.ids.lease);
    fips_rekey_index_lease_drop(&m->cycle.ep.ids.lease);
    fips_rekey_index_lease_drop(&m->cycle.attempt.lease);
    for (size_t i = 0; i < 3; ++i)
        fips_noise_session_clear_slot(&m->sessions[i]);
    rk_cand_clear(&m->cands);
    rk_cycle_clear(&m->cycle);
    sodium_memzero(m, sizeof *m);
}
