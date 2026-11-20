#include "internal.h"

/* Each network row is judged against const state. Only apply (and its transition
 * helpers) consumes the resulting proof. Temporary crypto is cleared in step. */
typedef enum change_kind {
    CH_REJECT,
    CH_RESEND,
    CH_REFUSE,
    CH_REPLACE,
    CH_RESPOND,
    CH_REPLY,
    CH_CANDIDATE,
    CH_DROP,
    CH_ACCEPT,
    CH_DONE,
    CH_RELEASE,
    CH_FRAME,
    CH_WORKER,
    CH_DELEGATED,
    CH_UNDELEGATED,
    CH_OPEN,
    CH_DATA,
    CH_PATH,
    CH_POLL
} change_kind;
typedef struct change {
    change_kind kind;
    fips_rekey_reason reason;
    fips_rekey_input input;
    fips_rekey_read read;
    fips_rekey_candidate candidate;
    fips_rekey_wire msg;
    fips_rekey_msg_kind msg_kind;
    fips_rekey_route route;
    size_t candidate_index;
    bool restart, yield_own, record;
    rk_frame_proof frame;
} change;

static bool smaller(const fips_rekey *m) {
    fips_node_addr peer = fips_rekey_bond_addr(&m->bond);
    return memcmp(&m->me, &peer, sizeof peer) < 0;
}
static fips_rekey_own own(const fips_rekey *m) {
    switch (m->cycle.kind) {
    case FIPS_REKEY_Idle:
        return m->cur.present && m->cur.fin.present ? FIPS_REKEY_OWN_Completed
                                                    : FIPS_REKEY_OWN_None;
    case FIPS_REKEY_Init:
        return FIPS_REKEY_OWN_Unanswered;
    case FIPS_REKEY_PendI:
        return FIPS_REKEY_OWN_Completed;
    case FIPS_REKEY_PendR:
        return FIPS_REKEY_OWN_None;
    }
    return FIPS_REKEY_OWN_None;
}
static fips_rekey_verdict resolve(const fips_rekey *m, fips_rekey_commit commit, bool restart) {
    fips_node_addr peer = fips_rekey_bond_addr(&m->bond);
    return fips_rekey_resolve(commit, own(m), &m->me, &peer, restart);
}
static fips_duration grace(const fips_rekey *m) {
    fips_duration rtt = rk_dmax(m->mem.hs_rtt, m->mem.srtt);
    return rk_dmin(rk_dmax(rk_add(rtt, rtt), rk_secs(1)), m->params.grace);
}
static bool slot_of(const fips_rekey *m, fips_rekey_epoch_tag tag, fips_rekey_slot *slot) {
    const fips_rekey_slot slots[] = {FIPS_REKEY_Current, FIPS_REKEY_Previous, FIPS_REKEY_Pending};
    for (size_t i = 0; i < 3; ++i) {
        const fips_rekey_epoch *e = rk_epoch_const(m, slots[i]);
        if (e && e->tag == tag) {
            *slot = slots[i];
            return true;
        }
    }
    return false;
}
static bool sealable(const fips_rekey *m, fips_rekey_epoch_tag tag) {
    return (m->cur.present && m->cur.tag == tag) ||
           (m->cycle.kind == FIPS_REKEY_PendI && m->cycle.wait == FIPS_REKEY_WAIT_Evidence &&
            m->cycle.ep.tag == tag);
}
static void opening_check(const fips_rekey *m, fips_rekey_teardown why, fips_rekey_outputs *out) {
    if (!m->cur.present && m->cycle.kind == FIPS_REKEY_Idle && !m->cands.count)
        rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown = why;
}
static void bind(const fips_rekey_epoch *ep, fips_rekey_purpose purpose, fips_rekey_outputs *out) {
    if (!ep->ids.lease.live)
        return;
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Bind);
    o->transport = ep->ids.transport;
    o->index = ep->ids.lease.index;
    o->tag = ep->tag;
    o->has_tag = true;
    o->purpose = purpose;
}
static void rebind(const fips_rekey_epoch *ep, fips_rekey_transport was, fips_rekey_purpose purpose,
                   fips_rekey_outputs *out) {
    if (!ep->ids.lease.live)
        return;
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Unbind);
    o->transport = was;
    o->index = ep->ids.lease.index;
    bind(ep, purpose, out);
}
static void release(fips_rekey_index_lease *lease, fips_rekey_outputs *out) {
    if (lease->live) {
        rk_output(out, FIPS_REKEY_OUT_Release)->lease = *lease;
        *lease = (fips_rekey_index_lease){0};
    }
}
int32_t rk_jitter(fips_rekey_env *env, uint16_t j) {
    uint16_t bound = j > UINT16_MAX / 2 ? UINT16_MAX : (uint16_t)(j * 2u);
    return (int32_t)env->jitter_draw(env->jitter_ctx, bound) - (int32_t)j;
}

/* Setup order: validation, identical cached answer, young/completed refusal,
 * quota, then first-handshake-only eviction. SIZE_MAX means free, count means
 * refused. No unauthenticated setup changes cadence or the own attempt. */
static size_t setup_room(const fips_rekey *m, fips_instant now, const uint8_t *bytes, size_t len,
                         fips_node_addr carrier, fips_rekey_reason *reason, size_t *copy) {
    *copy = SIZE_MAX;
    if (len > FIPS_REKEY_WIRE_MAX || !m->profile->valid(bytes, len)) {
        *reason = FIPS_REKEY_REASON_SetupMalformed;
        return m->cands.count;
    }
    *copy = rk_cand_find(&m->cands, bytes, len);
    if (*copy != SIZE_MAX)
        return SIZE_MAX;
    bool young = m->cycle.kind == FIPS_REKEY_Init && !m->cur.present &&
                 rk_dcmp(rk_since(now, m->cycle.attempt.first), m->params.cfg.young) < 0;
    if (smaller(m) && (own(m) == FIPS_REKEY_OWN_Completed || young)) {
        *reason = FIPS_REKEY_REASON_TieBreakKept;
        return m->cands.count;
    }
    if (rk_cand_room(&m->cands, m->profile, carrier))
        return SIZE_MAX;
    if (!m->cur.present) {
        size_t victim = rk_cand_victim(&m->cands, m->profile, carrier, now, m->params.cfg.young);
        if (victim != SIZE_MAX)
            return victim;
    }
    *reason = FIPS_REKEY_REASON_CandidatesFull;
    return m->cands.count;
}
bool fips_rekey_grows(const fips_rekey *m, fips_instant now, const uint8_t *msg, size_t len,
                      fips_node_addr carrier) {
    if (!m->live || !m->profile->valid)
        return false;
    fips_rekey_reason reason = FIPS_REKEY_REASON_SetupMalformed;
    size_t copy;
    size_t room = setup_room(m, now, msg, len, carrier, &reason, &copy);
    return room == SIZE_MAX && copy == SIZE_MAX;
}
static void judge_reply(const fips_rekey *m, change *c, fips_rekey_env *env) {
    const fips_rekey_input *in = &c->input;
    bool attempting = m->cycle.kind == FIPS_REKEY_Init;
    if (m->profile->indexed) {
        fips_rekey_index index = m->profile->index(in->bytes, in->len);
        if (!index.some || !attempting || !m->cycle.attempt.lease.live ||
            index.value != m->cycle.attempt.lease.index) {
            c->reason = FIPS_REKEY_REASON_UnknownIndex;
            const fips_rekey_slot slots[] = {FIPS_REKEY_Pending, FIPS_REKEY_Current,
                                             FIPS_REKEY_Previous};
            for (size_t i = 0; i < 3; ++i) {
                const fips_rekey_epoch *e = rk_epoch_const(m, slots[i]);
                if (e && index.some && e->ids.lease.live && e->ids.lease.index == index.value)
                    c->reason = FIPS_REKEY_REASON_LateReply;
            }
            return;
        }
    }
    if (!attempting) {
        c->reason = m->cycle.kind == FIPS_REKEY_PendI ? FIPS_REKEY_REASON_LateReply
                                                      : FIPS_REKEY_REASON_UnknownAttempt;
        return;
    }
    if (!m->profile->reply(&m->cycle.attempt.hs, env->local, in->bytes, in->len, &c->read) ||
        c->read.fin.len > FIPS_REKEY_WIRE_MAX || !fips_rekey_bond_check(&m->bond, &c->read.done)) {
        c->reason = FIPS_REKEY_REASON_ReplyUnauthenticated;
        return;
    }
    c->kind = CH_REPLY;
}
static void judge_setup(const fips_rekey *m, fips_instant now, change *c, fips_rekey_env *env) {
    const fips_rekey_input *in = &c->input;
    size_t copy;
    c->candidate_index = setup_room(m, now, in->bytes, in->len, in->carrier, &c->reason, &copy);
    if (copy != SIZE_MAX) {
        c->kind = CH_RESEND;
        c->msg = m->cands.list[copy].ack;
        c->msg_kind = FIPS_REKEY_MSG_Ack;
        c->route = FIPS_REKEY_ROUTE_Established;
        return;
    }
    if (c->candidate_index == m->cands.count)
        return;
    if (!m->profile->answer(env->local, in->bytes, in->len, &c->candidate.hs, &c->candidate.ack) ||
        c->candidate.ack.len > FIPS_REKEY_WIRE_MAX) {
        c->reason = FIPS_REKEY_REASON_SetupMalformed;
        return;
    }
    memcpy(c->candidate.msg1.bytes, in->bytes, in->len);
    c->candidate.msg1.len = in->len;
    c->candidate.carrier = in->carrier;
    c->candidate.route = in->route;
    c->candidate.born = now;
    c->candidate.deadline = rk_later(now, m->params.cfg.ttl);
    c->kind = CH_CANDIDATE;
}
static void judge_final(const fips_rekey *m, change *c) {
    if (!m->cands.count) {
        c->reason = FIPS_REKEY_REASON_FinalNoAttempt;
        return;
    }
    for (size_t n = m->cands.count; n > 0; --n) {
        const fips_rekey_candidate *cand = &m->cands.list[n - 1];
        if (!m->profile->complete(&cand->hs, c->input.bytes, c->input.len, &c->read.done)) {
            fips_rekey_completed_clear(&c->read.done);
            continue;
        }
        c->candidate_index = n - 1;
        c->kind = CH_DROP;
        if (!fips_rekey_bond_check(&m->bond, &c->read.done)) {
            c->reason = FIPS_REKEY_REASON_KeyMismatch;
            return;
        }
        if (cand->has_bound && memcmp(cand->bound, c->read.done.epoch, 8)) {
            c->reason = FIPS_REKEY_REASON_StaleIncarnation;
            return;
        }
        c->restart = m->cur.present && m->mem.has_peer_epoch &&
                     memcmp(m->mem.peer_epoch, c->read.done.epoch, 8);
        fips_rekey_verdict verdict = resolve(m, FIPS_REKEY_COMMIT_AtFinal, c->restart);
        if (verdict == FIPS_REKEY_Accept || verdict == FIPS_REKEY_YieldOwn)
            c->kind = CH_ACCEPT;
        else
            c->reason = FIPS_REKEY_REASON_TieBreakKept;
        return;
    }
    c->reason = FIPS_REKEY_REASON_FinalUnauthenticated;
}
static fips_rekey_digest digest(const uint8_t bytes[32]) {
    fips_rekey_digest d;
    memcpy(d.bytes, bytes, 32);
    return d;
}
static void judge_msg1(const fips_rekey *m, fips_instant now, change *c) {
    const fips_rekey_msg1_read *tok = c->input.tok;
    if (!m->cur.present) {
        c->reason = FIPS_REKEY_REASON_NotEstablished;
        return;
    }
    if (!m->mem.has_peer_epoch || memcmp(m->mem.peer_epoch, tok->epoch, 8)) {
        c->reason = FIPS_REKEY_REASON_EpochChanged;
        return;
    }
    if (m->bond.kind != FIPS_REKEY_BOND_Key || memcmp(m->bond.key.bytes, tok->remote.bytes, 32)) {
        c->reason = FIPS_REKEY_REASON_KeyMismatch;
        return;
    }
    const fips_rekey_held_answer *held =
        m->cycle.kind == FIPS_REKEY_PendR && m->cycle.answer.present ? &m->cycle.answer : NULL;
    bool setup = m->record.has_setup && !memcmp(m->record.setup.bytes, tok->digest, 32);
    bool armed = !setup && held && !memcmp(held->msg1.bytes, tok->digest, 32);
    bool seen = fips_rekey_record_holds(&m->record, digest(tok->ephemeral)) ||
                (held && !memcmp(held->ephemeral.bytes, tok->ephemeral, 32));
    bool fresh = !setup && !armed && !seen;
    bool eligible = rk_dcmp(rk_since(now, m->mem.link_since), m->params.cfg.link_age) >= 0 &&
                    rk_dcmp(rk_since(now, m->cur.started), m->params.cfg.session_age) >= 0;
    if (!eligible) {
        if (setup && m->record.has_answer) {
            c->kind = CH_RESEND;
            c->msg = m->record.answer;
            c->msg_kind = FIPS_REKEY_MSG_Msg2;
            c->route = FIPS_REKEY_ROUTE_EstablishedFallback;
        } else if (fresh) {
            c->kind = CH_REPLACE;
            c->record = true;
        } else
            c->reason = FIPS_REKEY_REASON_AnsweredBefore;
        return;
    }
    if (!c->input.on_link && c->input.reachable) {
        c->kind = CH_REFUSE;
        c->reason = FIPS_REKEY_REASON_OffLink;
        c->record = fresh;
        return;
    }
    if (m->cycle.kind == FIPS_REKEY_PendR && armed) {
        c->kind = CH_RESEND;
        c->msg = held->msg2;
        c->msg_kind = FIPS_REKEY_MSG_Msg2;
        c->route = held->route;
        return;
    }
    if (m->cycle.kind == FIPS_REKEY_PendR || m->cycle.kind == FIPS_REKEY_PendI) {
        c->kind = CH_REFUSE;
        c->reason = FIPS_REKEY_REASON_PendingHeld;
        c->record = fresh;
        return;
    }
    if (!fresh) {
        c->reason = FIPS_REKEY_REASON_AnsweredBefore;
        return;
    }
    if (m->cycle.kind == FIPS_REKEY_Init) {
        if (resolve(m, FIPS_REKEY_COMMIT_AtAnswer, false) != FIPS_REKEY_YieldOwn) {
            c->kind = CH_REFUSE;
            c->reason = FIPS_REKEY_REASON_DualInitWon;
            c->record = true;
            return;
        }
        c->yield_own = true;
    }
    c->kind = CH_RESPOND;
}
static void judge_frame(const fips_rekey *m, change *c, fips_rekey_env *env) {
    fips_rekey_frame_hint h = c->input.hint;
    c->reason = FIPS_REKEY_REASON_FrameUnauthenticated;
    if (!m->cur.present) {
        c->reason = FIPS_REKEY_REASON_NotEstablished;
        return;
    }
    if (h.body > c->input.len)
        return;
    fips_rekey_slot order[] = {FIPS_REKEY_Current, FIPS_REKEY_Pending, FIPS_REKEY_Previous};
    size_t count = 3;
    if (m->profile->select == FIPS_REKEY_ByIndex) {
        bool found = false;
        for (size_t i = 0; i < 3; ++i) {
            const fips_rekey_epoch *ep = rk_epoch_const(m, order[i]);
            if (ep && ep->ids.lease.live && ep->ids.lease.index == h.index) {
                order[0] = order[i];
                count = 1;
                found = true;
                if (ep->recv == FIPS_REKEY_Delegated) {
                    c->reason = FIPS_REKEY_REASON_DelegatedEpoch;
                    return;
                }
                break;
            }
        }
        if (!found)
            return;
    } else if (h.k != m->cur.k) {
        order[0] = FIPS_REKEY_Pending;
        order[1] = FIPS_REKEY_Current;
    }
    bool refused = false;
    for (size_t i = 0; i < count; ++i) {
        const fips_rekey_epoch *ep = rk_epoch_const(m, order[i]);
        if (!ep || ep->recv == FIPS_REKEY_Delegated)
            continue;
        size_t len = 0;
        switch (rk_epoch_trial(m, ep, h, c->input.buf, c->input.len, env, &len)) {
        case RK_REFUSED:
            refused = true;
            break;
        case RK_FAILED:
            break;
        case RK_OPENED:
            if (len > env->scratch_capacity)
                return;
            c->frame = (rk_frame_proof){
                order[i], ep->tag, h.counter, h.k, m->profile->is_probe(env->scratch, len), len};
            c->kind = CH_FRAME;
            return;
        }
    }
    if (refused)
        c->reason = FIPS_REKEY_REASON_Replay;
}
static void judge(const fips_rekey *m, fips_instant now, fips_rekey_input input,
                  fips_rekey_env *env, change *c) {
    c->kind = CH_REJECT;
    c->input = input;
    c->reason = FIPS_REKEY_REASON_NotRekey;
    switch (input.kind) {
    case FIPS_REKEY_IN_Reply:
        judge_reply(m, c, env);
        break;
    case FIPS_REKEY_IN_Final:
        if (m->profile->complete)
            judge_final(m, c);
        break;
    case FIPS_REKEY_IN_Setup:
        if (m->profile->valid && m->profile->answer)
            judge_setup(m, now, c, env);
        break;
    case FIPS_REKEY_IN_PeerMsg1:
        if (m->profile->msg1_answer && input.tok)
            judge_msg1(m, now, c);
        break;
    case FIPS_REKEY_IN_PeerDone:
        if (!input.leg)
            break;
        c->kind = CH_RELEASE;
        if (input.intent != FIPS_REKEY_INTENT_Rekey)
            break;
        if (!m->cur.present) {
            c->reason = FIPS_REKEY_REASON_NotEstablished;
            break;
        }
        if (!fips_rekey_bond_check(&m->bond, &input.leg->done)) {
            c->reason = FIPS_REKEY_REASON_KeyMismatch;
            break;
        }
        c->restart = m->mem.has_peer_epoch && memcmp(m->mem.peer_epoch, input.leg->done.epoch, 8);
        if (resolve(m, m->profile->commit, c->restart) == FIPS_REKEY_KeepOwn ||
            resolve(m, m->profile->commit, c->restart) == FIPS_REKEY_DiscardCompeting) {
            c->reason = FIPS_REKEY_REASON_TieBreakKept;
            break;
        }
        c->kind = CH_DONE;
        break;
    case FIPS_REKEY_IN_Frame:
        judge_frame(m, c, env);
        break;
    case FIPS_REKEY_IN_AuthenticatedFrame: {
        fips_rekey_slot slot;
        c->reason = FIPS_REKEY_REASON_StaleTag;
        if (!slot_of(m, input.tag, &slot)) break;
        const fips_rekey_epoch *ep = rk_epoch_const(m, slot);
        if (m->profile->indexed && (!ep->ids.lease.live || ep->ids.lease.index != input.hint.index)) break;
        c->reason = FIPS_REKEY_REASON_Replay;
        if (!m->profile->keys->fresh(ep->session, input.hint.counter)) break;
        c->reason = FIPS_REKEY_REASON_ShortPlaintext;
        if (!input.buf || input.hint.body > input.len) break;
        size_t n = input.len - input.hint.body;
        if (n < m->profile->floor || n > env->scratch_capacity) break;
        /* Host-owned completion bytes, never a second AEAD or a worker's window. */
        memcpy(env->scratch, input.buf + input.hint.body, n);
        c->frame = (rk_frame_proof){slot, input.tag, input.hint.counter,
            input.hint.k, m->profile->is_probe(env->scratch, n), n};
        c->kind = CH_FRAME;
        break;
    }
    case FIPS_REKEY_IN_WorkerFrame:
    case FIPS_REKEY_IN_Delegated:
    case FIPS_REKEY_IN_Undelegated: {
        if (m->profile->layer != FIPS_REKEY_LAYER_Fmp)
            break;
        if (!slot_of(m, input.tag, &c->frame.slot)) {
            c->reason = FIPS_REKEY_REASON_StaleTag;
            break;
        }
        if (c->frame.slot == FIPS_REKEY_Pending) {
            c->reason = input.kind == FIPS_REKEY_IN_Delegated ? FIPS_REKEY_REASON_NotOffered
                                                              : FIPS_REKEY_REASON_NotDelegated;
            break;
        }
        const fips_rekey_epoch *ep = rk_epoch_const(m, c->frame.slot);
        if (input.kind == FIPS_REKEY_IN_WorkerFrame)
            c->kind = CH_WORKER;
        else if (input.kind == FIPS_REKEY_IN_Delegated) {
            if (ep->recv == FIPS_REKEY_Delegated)
                c->reason = FIPS_REKEY_REASON_AlreadyDelegated;
            else
                c->kind = CH_DELEGATED;
        } else {
            if (ep->recv == FIPS_REKEY_Here)
                c->reason = FIPS_REKEY_REASON_NotDelegated;
            else
                c->kind = CH_UNDELEGATED;
        }
        break;
    }
    case FIPS_REKEY_IN_Open: {
        if (m->profile->layer != FIPS_REKEY_LAYER_Fsp)
            break;
        if (m->cur.present) {
            c->reason = FIPS_REKEY_REASON_AlreadyEstablished;
            break;
        }
        if (m->cycle.kind != FIPS_REKEY_Idle) {
            c->reason = FIPS_REKEY_REASON_AttemptInFlight;
            break;
        }
        fips_rekey_completed identity = {0};
        identity.remote = input.peer;
        if (fips_rekey_bond_check(&m->bond, &identity))
            c->kind = CH_OPEN;
        else
            c->reason = FIPS_REKEY_REASON_KeyMismatch;
        fips_rekey_completed_clear(&identity);
        break;
    }
    case FIPS_REKEY_IN_DataSent:
        if (m->profile->layer == FIPS_REKEY_LAYER_Fsp)
            c->kind = CH_DATA;
        break;
    case FIPS_REKEY_IN_PathFacts:
        if (m->profile->layer == FIPS_REKEY_LAYER_Fsp)
            c->kind = CH_PATH;
        break;
    case FIPS_REKEY_IN_Poll:
        c->kind = CH_POLL;
        break;
    }
}

void rk_start(fips_rekey *m, fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out) {
    if (!m->live || m->bond.kind != FIPS_REKEY_BOND_Key || m->cycle.kind != FIPS_REKEY_Idle)
        return;
    fips_rekey_attempt a = {0};
    if (m->profile->indexed && !fips_rekey_allocate(env->indices, &a.lease)) {
        rk_reject(out, FIPS_REKEY_REASON_IndexExhausted);
        return;
    }
    fips_rekey_index ours = {a.lease.index, a.lease.live};
    if (!m->profile->initiate(env->local, &m->bond.key, ours, &a.hs, &a.out) ||
        a.out.len > FIPS_REKEY_WIRE_MAX) {
        release(&a.lease, out);
        fips_rekey_hs_clear(&a.hs);
        sodium_memzero(&a, sizeof a);
        rk_reject(out, FIPS_REKEY_REASON_HandshakeWrite);
        return;
    }
    fips_rekey_schedule sched = m->params.first;
    fips_duration budget = m->params.budget;
    if (m->cur.present && m->profile->setup_rtt) {
        fips_duration rtt = rk_dmax(m->mem.hs_rtt, m->mem.srtt);
        fips_duration first = rk_dmin(
            rk_dmax(rk_dmax(rk_add(rtt, rtt), rk_secs(1)), m->mem.setup_rto), m->params.rung);
        uint8_t ignored;
        sched = fips_rekey_schedule_doubling(first, UINT8_MAX, &m->params.cfg.handshake, &ignored);
        budget = m->params.cfg.handshake;
    }
    if (a.lease.live) {
        fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Bind);
        o->transport = env->transport;
        o->index = a.lease.index;
        o->purpose = FIPS_REKEY_PURPOSE_Attempt;
    }
    rk_send(out, &a.out, m->profile->messages == 2 ? FIPS_REKEY_MSG_Msg1 : FIPS_REKEY_MSG_Setup,
            FIPS_REKEY_ROUTE_Established);
    a.transport = env->transport;
    a.first = now;
    a.deadline = rk_later(now, budget);
    a.ladder = fips_rekey_ladder_new(now, sched);
    m->cycle.kind = FIPS_REKEY_Init;
    m->cycle.attempt = a;
    sodium_memzero(&a, sizeof a);
    m->mem.due = false;
}
static void respond(fips_rekey *m, fips_instant now, const change *c, fips_rekey_env *env,
                    fips_rekey_outputs *out) {
    fips_rekey_ids ids = {0};
    ids.transport = env->transport;
    if (m->profile->indexed && !fips_rekey_allocate(env->indices, &ids.lease)) {
        rk_reject(out, FIPS_REKEY_REASON_IndexExhausted);
        return;
    }
    fips_rekey_completed done = {0};
    fips_rekey_wire msg = {0};
    fips_rekey_index ours = {ids.lease.index, ids.lease.live};
    bool ok = m->profile->msg1_answer(c->input.tok->token, env->local, ours, &done, &msg);
    if (!ok || msg.len > FIPS_REKEY_WIRE_MAX || !fips_rekey_bond_check(&m->bond, &done)) {
        release(&ids.lease, out);
        fips_rekey_completed_clear(&done);
        rk_reject(out, FIPS_REKEY_REASON_HandshakeWrite);
        return;
    }
    ids.theirs = done.theirs;
    if (c->yield_own)
        rk_cycle_end(m, FIPS_REKEY_ABANDON_Yielded, FIPS_REKEY_CLOSE_Shutdown, out);
    rk_epoch_new(m, &m->cycle.ep, &done, &ids, false, FIPS_REKEY_Responder, now, env);
    bind(&m->cycle.ep, FIPS_REKEY_PURPOSE_Pending, out);
    fips_rekey_route route = m->params.cfg.arrival && c->input.on_link
                                 ? FIPS_REKEY_ROUTE_Arrival
                                 : FIPS_REKEY_ROUTE_EstablishedFallback;
    rk_send(out, &msg, FIPS_REKEY_MSG_Msg2, route);
    m->cycle.kind = FIPS_REKEY_PendR;
    m->cycle.deadline = rk_later(now, m->params.cfg.hold);
    m->cycle.answer = (fips_rekey_held_answer){digest(c->input.tok->digest),
                                               digest(c->input.tok->ephemeral), msg, route, true};
    if (m->profile->dampen_at == FIPS_REKEY_DAMPEN_Answer)
        m->mem.damp = rk_later(now, m->params.cfg.dampening);
}
static void sample(fips_rekey *m, fips_duration rtt, bool resent) {
    if (!resent) {
        m->mem.hs_rtt = rtt;
        m->mem.has_hs_rtt = true;
        m->mem.setup_rto = rk_secs(0);
        m->mem.has_setup_rto = false;
    } else if (m->profile->setup_rtt) {
        m->mem.setup_rto = rk_dmin(rk_add(rtt, rtt), m->params.rung);
        m->mem.has_setup_rto = true;
    }
}
static void install(fips_rekey *m, fips_instant now, fips_rekey_epoch *ep, fips_rekey_env *env,
                    fips_rekey_outputs *out) {
    ep->started = ep->base = now;
    m->mem.jitter = rk_jitter(env, m->params.cfg.jitter);
    bind(ep, FIPS_REKEY_PURPOSE_Current, out);
    rk_delegate(m, ep, env->workers, out);
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Installed);
    o->tag = ep->tag;
    o->role = ep->role;
    o->evidence = FIPS_REKEY_EVIDENCE_First;
    if (ep->role == FIPS_REKEY_Responder) {
        rk_prompt(out, ep->tag);
        ep->prompted = now;
    }
    rk_epoch_move(ep, &m->cur);
    rk_cycle_clear(&m->cycle);
}
/* Consume pending metadata, never the stable arena key bytes. */
static void cutover(fips_rekey *m, fips_instant now, fips_rekey_evidence_kind evidence, bool has_k,
                    bool k, bool pruned, fips_rekey_env *env, fips_rekey_outputs *out) {
    assert(m->cur.present && m->cycle.ep.present);
    if (!pruned) {
        switch (evidence) {
        case FIPS_REKEY_EVIDENCE_Frame:
        case FIPS_REKEY_EVIDENCE_Proof:
            rk_cand_clear(&m->cands);
            break;
        case FIPS_REKEY_EVIDENCE_Assumed:
        case FIPS_REKEY_EVIDENCE_Delay:
        case FIPS_REKEY_EVIDENCE_Restart:
            if (smaller(m))
                rk_cand_clear(&m->cands);
            else if (evidence == FIPS_REKEY_EVIDENCE_Restart && m->mem.has_peer_epoch)
                for (size_t i = 0; i < m->cands.count; ++i) {
                    m->cands.list[i].has_bound = true;
                    memcpy(m->cands.list[i].bound, m->mem.peer_epoch, 8);
                }
            break;
        case FIPS_REKEY_EVIDENCE_First:
            break;
        }
    }
    rk_retire(m, &m->drain.ep, out);
    memset(&m->cur.fin, 0, sizeof m->cur.fin);
    m->cur.has_elicit = false;
    if (m->cycle.restart)
        rk_retire(m, &m->cur, out);
    else {
        rk_epoch_move(&m->cur, &m->drain.ep);
        m->drain.evidenced = evidence == FIPS_REKEY_EVIDENCE_Frame;
        m->drain.deadline =
            rk_later(now, m->drain.evidenced ? m->params.cfg.drain : m->params.cfg.retention);
    }
    rk_pending_record(m);
    fips_rekey_epoch *ep = &m->cycle.ep;
    ep->started = now;
    if (!ep->base.ops)
        ep->base = now;
    m->mem.jitter = rk_jitter(env, m->params.cfg.jitter);
    if (evidence == FIPS_REKEY_EVIDENCE_Frame)
        ep->heard = ep->moved = now;
    if (evidence == FIPS_REKEY_EVIDENCE_Restart || evidence == FIPS_REKEY_EVIDENCE_Delay)
        ep->fin = m->cycle.fin;
    if (ep->fin.present && !fips_rekey_ladder_next_at(&ep->fin.ladder).ops) {
        memset(&ep->fin, 0, sizeof ep->fin);
        if (evidence != FIPS_REKEY_EVIDENCE_Frame)
            m->mem.due = true;
    }
    if (m->cycle.restart)
        m->mem.link_since = now;
    rk_delegate(m, ep, env->workers, out);
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Installed);
    o->tag = ep->tag;
    o->role = ep->role;
    o->evidence = evidence;
    if (m->cycle.restart && m->mem.has_peer_epoch) {
        o = rk_output(out, FIPS_REKEY_OUT_PeerRestarted);
        o->layer = m->profile->layer;
        memcpy(o->epoch, m->mem.peer_epoch, 8);
    }
    rk_prompt(out, ep->tag);
    ep->prompted = now;
    if (!ep->moved.ops) {
        ep->elicit = fips_rekey_ladder_new(now, m->params.elicit);
        ep->has_elicit = true;
    }
    rk_output(out, FIPS_REKEY_OUT_Rekeyed)->role = ep->role;
    m->mem.last_cutover = now;
    m->mem.abandons = 0;
    if (evidence == FIPS_REKEY_EVIDENCE_Frame) {
        m->mem.last_rx = m->mem.rx_cur = now;
        m->mem.has_peer_k = has_k;
        m->mem.peer_k = k;
        m->mem.sent_data = false;
        m->mem.silence = 0;
    }
    rk_epoch_move(ep, &m->cur);
    rk_cycle_clear(&m->cycle);
}
static void reply(fips_rekey *m, fips_instant now, change *c, fips_rekey_env *env,
                  fips_rekey_outputs *out) {
    sample(m, rk_since(now, m->cycle.attempt.first), m->cycle.attempt.resent);
    fips_rekey_ids ids = {m->cycle.attempt.lease, c->read.done.theirs, env->transport};
    fips_rekey_transport was = m->cycle.attempt.transport;
    m->cycle.attempt.lease = (fips_rekey_index_lease){0};
    rk_cycle_clear(&m->cycle);
    uint8_t epoch[8];
    memcpy(epoch, c->read.done.epoch, 8);
    if (c->read.has_fin)
        rk_send(out, &c->read.fin, FIPS_REKEY_MSG_Msg3, FIPS_REKEY_ROUTE_Established);
    bool restart =
        m->cur.present && (!m->mem.has_peer_epoch || memcmp(m->mem.peer_epoch, epoch, 8));
    bool k = m->cur.present && !restart && !(m->mem.has_peer_k ? m->mem.peer_k : m->cur.k);
    rk_epoch_new(m, &m->cycle.ep, &c->read.done, &ids, k, FIPS_REKEY_Initiator, now, env);
    fips_rekey_epoch *ep = &m->cycle.ep;
    m->cycle.kind = FIPS_REKEY_PendI;
    m->cycle.restart = restart;
    if (!m->cur.present) {
        ep->has_route = true;
        ep->route = c->input.route;
        if (c->read.has_fin)
            ep->fin =
                (fips_rekey_final){true, c->read.fin, fips_rekey_ladder_new(now, m->params.fin)};
        m->mem.has_peer_epoch = true;
        memcpy(m->mem.peer_epoch, epoch, 8);
        install(m, now, ep, env, out);
        return;
    }
    if (restart) {
        m->mem.has_peer_epoch = true;
        memcpy(m->mem.peer_epoch, epoch, 8);
    }
    if (restart && c->read.has_fin) {
        rebind(ep, was, FIPS_REKEY_PURPOSE_Current, out);
        ep->base = now;
        ep->has_route = true;
        ep->route = c->input.route;
        m->cycle.fin =
            (fips_rekey_final){true, c->read.fin, fips_rekey_ladder_new(now, m->params.fin)};
        cutover(m, now, FIPS_REKEY_EVIDENCE_Restart, false, false, false, env, out);
        return;
    }
    rebind(ep, was, FIPS_REKEY_PURPOSE_Pending, out);
    switch (m->profile->initiator_cutover) {
    case FIPS_REKEY_OnProof:
        m->cycle.wait = FIPS_REKEY_WAIT_Held;
        m->cycle.deadline = now;
        break;
    case FIPS_REKEY_OnEvidence:
        m->cycle.wait = FIPS_REKEY_WAIT_Evidence;
        m->cycle.deadline = rk_later(now, m->params.confirm);
        ep->base = now;
        ep->has_route = true;
        ep->route = c->input.route;
        rk_prompt(out, ep->tag);
        rk_prompt(out, m->cur.tag);
        m->cycle.fin.present = c->read.has_fin;
        m->cycle.fin.msg = c->read.fin;
        m->cycle.has_probes = true;
        m->cycle.probes = fips_rekey_ladder_new(now, m->params.probes);
        m->cycle.probed = now;
        break;
    case FIPS_REKEY_AfterDelay:
        m->cycle.wait = FIPS_REKEY_WAIT_Delay;
        m->cycle.deadline = rk_later(now, rk_dmax(m->params.cfg.delay, m->mem.hs_rtt));
        if (c->read.has_fin)
            m->cycle.fin = (fips_rekey_final){true, c->read.fin,
                                              fips_rekey_ladder_new(now, m->params.delayed)};
        break;
    }
}
static void yield_cycle(fips_rekey *m, fips_rekey_outputs *out) {
    if (m->cycle.kind == FIPS_REKEY_Idle)
        memset(&m->cur.fin, 0, sizeof m->cur.fin);
    rk_cycle_end(m,
                 m->cycle.kind == FIPS_REKEY_PendR ? FIPS_REKEY_ABANDON_Superseded
                                                   : FIPS_REKEY_ABANDON_Yielded,
                 FIPS_REKEY_CLOSE_Shutdown, out);
}
static void complete(fips_rekey *m, fips_instant now, change *c, fips_rekey_env *env,
                     fips_rekey_outputs *out) {
    fips_rekey_route_handle route = m->cands.list[c->candidate_index].route;
    fips_instant born = m->cands.list[c->candidate_index].born;
    /* The list is in sequence order, including equal-instant arrivals. */
    for (size_t n = c->candidate_index + 1; n > 0; --n)
        rk_cand_remove(&m->cands, 0);
    yield_cycle(m, out);
    fips_duration rtt = rk_since(now, born);
    if (rk_dcmp(rtt, rk_secs(1)) < 0)
        sample(m, rtt, false);
    if (m->profile->dampen_at == FIPS_REKEY_DAMPEN_Completion)
        m->mem.damp = rk_later(now, m->params.cfg.dampening);
    m->mem.has_peer_epoch = true;
    memcpy(m->mem.peer_epoch, c->read.done.epoch, 8);
    m->bond.kind = FIPS_REKEY_BOND_Key;
    m->bond.key = c->read.done.remote;
    fips_rekey_ids ids = {0};
    ids.transport = env->transport;
    bool k = m->cur.present && !c->restart && !m->cur.k;
    rk_epoch_new(m, &m->cycle.ep, &c->read.done, &ids, k, FIPS_REKEY_Responder, now, env);
    m->cycle.ep.has_route = true;
    m->cycle.ep.route = route;
    if (!m->cur.present) {
        install(m, now, &m->cycle.ep, env, out);
        return;
    }
    m->cycle.kind = FIPS_REKEY_PendR;
    m->cycle.restart = c->restart;
    if (m->profile->responder_cutover == FIPS_REKEY_AtProof) {
        m->cycle.ep.base = now;
        cutover(m, now, FIPS_REKEY_EVIDENCE_Proof, false, false, true, env, out);
    } else
        m->cycle.deadline = rk_later(now, m->params.cfg.hold);
}
static void adopt(fips_rekey *m, fips_instant now, change *c, fips_rekey_env *env,
                  fips_rekey_outputs *out) {
    fips_rekey_leg *leg = c->input.leg;
    yield_cycle(m, out);
    if (c->restart) {
        m->mem.has_peer_epoch = true;
        memcpy(m->mem.peer_epoch, leg->done.epoch, 8);
    }
    fips_rekey_ids ids = {leg->lease, leg->done.theirs, env->transport};
    leg->lease = (fips_rekey_index_lease){0};
    rk_epoch_new(m, &m->cycle.ep, &leg->done, &ids, false, FIPS_REKEY_Responder, now, env);
    bind(&m->cycle.ep, FIPS_REKEY_PURPOSE_Pending, out);
    m->cycle.kind = FIPS_REKEY_PendR;
    m->cycle.deadline = rk_later(now, m->params.cfg.hold);
    m->cycle.restart = c->restart;
    if (m->profile->dampen_at == FIPS_REKEY_DAMPEN_Completion)
        m->mem.damp = rk_later(now, m->params.cfg.dampening);
}
static void heard_current(fips_rekey *m, fips_instant now, bool k, bool probe,
                          fips_rekey_outputs *out) {
    if (!m->cur.present)
        return;
    fips_rekey_epoch *cur = &m->cur;
    m->mem.last_rx = m->mem.rx_cur = now;
    m->mem.sent_data = false;
    m->mem.silence = 0;
    m->mem.has_peer_k = true;
    m->mem.peer_k = k;
    if (!cur->heard.ops) {
        cur->heard = now;
        cur->assumed = false;
        if (cur->fin.present) {
            memset(&cur->fin, 0, sizeof cur->fin);
            for (size_t i = m->cands.count; i > 0; --i)
                if (smaller(m) || !rk_before(cur->started, m->cands.list[i - 1].born))
                    rk_cand_remove(&m->cands, i - 1);
        }
    }
    bool blind = probe && cur->role == FIPS_REKEY_Responder &&
                 m->profile->responder_cutover == FIPS_REKEY_AtProof;
    if (!blind && !cur->moved.ops) {
        cur->moved = now;
        cur->has_elicit = false;
        if (m->drain.ep.present && !m->drain.evidenced) {
            m->drain.deadline = rk_min(m->drain.deadline, rk_later(now, m->params.cfg.drain));
            m->drain.evidenced = true;
        }
    }
    if (blind && (!cur->prompted.ops || rk_due(rk_later(cur->prompted, rk_secs(1)), now))) {
        rk_prompt(out, cur->tag);
        cur->prompted = now;
    }
    if (m->cycle.kind == FIPS_REKEY_PendI && m->cycle.wait == FIPS_REKEY_WAIT_Evidence &&
        m->cycle.probed.ops && rk_due(rk_later(m->cycle.probed, grace(m)), now))
        m->cycle.stayed = true;
}
static void revert(fips_rekey *m, fips_instant now, fips_rekey_outputs *out) {
    assert(m->cur.present && m->drain.ep.present && m->cycle.kind == FIPS_REKEY_Idle);
    rk_epoch_move(&m->cur, &m->cycle.ep);
    rk_epoch_move(&m->drain.ep, &m->cur);
    memset(&m->drain, 0, sizeof m->drain);
    m->cycle.ep.assumed = false;
    m->cycle.ep.has_elicit = false;
    memset(&m->cycle.ep.fin, 0, sizeof m->cycle.ep.fin);
    m->cycle.kind = FIPS_REKEY_PendI;
    m->cycle.wait = FIPS_REKEY_WAIT_Evidence;
    m->cycle.deadline = rk_later(now, m->params.cfg.revert);
    m->cycle.reverted = true;
    m->cycle.has_probes = true;
    m->cycle.probes = fips_rekey_ladder_new(now, m->params.revert);
    m->cycle.probed = now;
    rk_output(out, FIPS_REKEY_OUT_Reverted)->tag = m->cycle.ep.tag;
    rk_prompt(out, m->cycle.ep.tag);
    rk_prompt(out, m->cur.tag);
}
static void heard_previous(fips_rekey *m, fips_instant now, fips_rekey_outputs *out) {
    m->mem.last_rx = now;
    if (!m->cur.present)
        return;
    if (m->cur.assumed && !m->cur.heard.ops) {
        switch (m->cycle.kind) {
        case FIPS_REKEY_Init:
            rk_cycle_end(m, FIPS_REKEY_ABANDON_Reverted, FIPS_REKEY_CLOSE_Shutdown, out);
            revert(m, now, out);
            break;
        case FIPS_REKEY_Idle:
            revert(m, now, out);
            break;
        case FIPS_REKEY_PendI:
            if (m->cycle.wait == FIPS_REKEY_WAIT_Evidence && m->cycle.probed.ops &&
                rk_due(rk_later(m->cycle.probed, grace(m)), now))
                m->cycle.stayed = true;
            break;
        case FIPS_REKEY_PendR:
            break;
        }
        return;
    }
    bool confirming = m->profile->responder_cutover == FIPS_REKEY_AtProof &&
                      m->cur.role == FIPS_REKEY_Responder && !m->cur.moved.ops &&
                      rk_before(now, rk_later(m->cur.started, m->params.span));
    if (confirming &&
        (!m->cur.prompted.ops || rk_due(rk_later(m->cur.prompted, rk_secs(1)), now))) {
        rk_prompt(out, m->cur.tag);
        m->cur.prompted = now;
    }
}
static void deliver(const fips_rekey *m, const change *c, fips_rekey_slot slot, fips_rekey_env *env,
                    fips_rekey_outputs *out) {
    size_t body = c->input.hint.body, len = c->frame.len;
    if (len < m->profile->floor || body > c->input.len || len > c->input.len - body ||
        len > env->scratch_capacity) {
        rk_reject(out, FIPS_REKEY_REASON_ShortPlaintext);
        return;
    }
    memcpy(c->input.buf + body, env->scratch, len);
    fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Deliver);
    o->slot = slot;
    o->tag = c->frame.tag;
    o->begin = body;
    o->end = body + len;
}
static void frame(fips_rekey *m, fips_instant now, const change *c, fips_rekey_env *env,
                  fips_rekey_outputs *out) {
    fips_rekey_epoch *ep = rk_epoch(m, c->frame.slot);
    assert(ep);
    if (env->replay_mark_observer) env->replay_mark_observer(env->replay_mark_ctx);
    m->profile->keys->mark(ep->session, c->frame.counter);
    switch (c->frame.slot) {
    case FIPS_REKEY_Pending:
        if (m->cycle.kind == FIPS_REKEY_PendR)
            ep->k = c->frame.k;
        cutover(m, now, FIPS_REKEY_EVIDENCE_Frame, true, c->frame.k, false, env, out);
        deliver(m, c, FIPS_REKEY_Current, env, out);
        break;
    case FIPS_REKEY_Current:
        deliver(m, c, FIPS_REKEY_Current, env, out);
        heard_current(m, now, c->frame.k, c->frame.probe, out);
        break;
    case FIPS_REKEY_Previous:
        deliver(m, c, FIPS_REKEY_Previous, env, out);
        heard_previous(m, now, out);
        break;
    }
}

/* At equal instants, enum order is the README's expiry order. */
typedef enum due_kind {
    D_DRAIN,
    D_CANDIDATE,
    D_UNADOPTED,
    D_HELD,
    D_PROBE,
    D_DELAYED,
    D_FIN,
    D_ELICIT,
    D_CONFIRM,
    D_DELAY_END,
    D_RESEND,
    D_EXPIRE
} due_kind;
typedef struct due {
    fips_instant at;
    due_kind kind;
    uint64_t id;
} due;
#define DUE_MAX 12u
static void due_add(due list[DUE_MAX], size_t *n, fips_instant at, due_kind kind, uint64_t id) {
    if (!at.ops)
        return;
    assert(*n < DUE_MAX);
    list[(*n)++] = (due){at, kind, id};
}
static size_t deadlines(const fips_rekey *m, due list[DUE_MAX]) {
    size_t n = 0;
    if (m->drain.ep.present)
        due_add(list, &n, m->drain.deadline, D_DRAIN, m->drain.ep.tag);
    for (size_t i = 0; i < m->cands.count; ++i)
        due_add(list, &n, m->cands.list[i].deadline, D_CANDIDATE, m->cands.list[i].seq);
    const fips_rekey_cycle *c = &m->cycle;
    switch (c->kind) {
    case FIPS_REKEY_Idle:
        break;
    case FIPS_REKEY_Init:
        due_add(list, &n, fips_rekey_ladder_next_at(&c->attempt.ladder), D_RESEND, 0);
        due_add(list, &n, c->attempt.deadline, D_EXPIRE, 0);
        break;
    case FIPS_REKEY_PendI:
        switch (c->wait) {
        case FIPS_REKEY_WAIT_Held:
            due_add(list, &n, c->deadline, D_HELD, c->ep.tag);
            break;
        case FIPS_REKEY_WAIT_Evidence:
            if (c->has_probes)
                due_add(list, &n, fips_rekey_ladder_next_at(&c->probes), D_PROBE, c->ep.tag);
            due_add(list, &n, c->deadline, D_CONFIRM, c->ep.tag);
            break;
        case FIPS_REKEY_WAIT_Delay:
            if (c->fin.present)
                due_add(list, &n, fips_rekey_ladder_next_at(&c->fin.ladder), D_DELAYED, c->ep.tag);
            due_add(list, &n, c->deadline, D_DELAY_END, c->ep.tag);
            break;
        }
        break;
    case FIPS_REKEY_PendR:
        due_add(list, &n, c->deadline, D_UNADOPTED, c->ep.tag);
        break;
    }
    if (m->cur.present) {
        if (m->cur.fin.present)
            due_add(list, &n, fips_rekey_ladder_next_at(&m->cur.fin.ladder), D_FIN, m->cur.tag);
        if (!m->cur.moved.ops && m->cur.has_elicit)
            due_add(list, &n, fips_rekey_ladder_next_at(&m->cur.elicit), D_ELICIT, m->cur.tag);
    }
    return n;
}
static fips_instant trigger_at(const fips_rekey *m) {
    uint64_t ms = rk_millis(m->params.cfg.after);
    int64_t j = (int64_t)m->mem.jitter * 1000;
    ms = j >= 0 ? rk_uadd(ms, (uint64_t)j) : (ms > (uint64_t)-j ? ms - (uint64_t)-j : 0);
    return rk_later(m->cur.started, fips_duration_from_millis(ms));
}
static fips_duration silence_interval(const fips_rekey *m) {
    return fips_backoff(m->mem.silence, m->params.cfg.silence, m->params.cfg.after);
}
static fips_instant silence_at(const fips_rekey *m) {
    return rk_later(rk_max(m->mem.rx_cur, m->cur.started), silence_interval(m));
}
static fips_instant start_at(const fips_rekey *m) {
    if (!m->cur.present || m->cycle.kind != FIPS_REKEY_Idle || m->cur.fin.present ||
        !m->mem.reachable)
        return (fips_instant){0};
    const fips_rekey_config *cfg = &m->params.cfg;
    uint64_t counter = m->profile->keys->sent(m->cur.session);
    fips_instant gate = rk_max(rk_later(m->mem.link_since, cfg->floor_link),
                               rk_later(m->cur.started, cfg->floor_session));
    if (m->mem.last_cutover.ops)
        gate = rk_max(gate, rk_later(m->mem.last_cutover, cfg->floor_cutover));
    if (counter >= cfg->soft)
        return gate;
    gate = rk_max(gate, m->mem.damp);
    fips_instant backed = rk_max(gate, m->mem.backoff);
    fips_instant scheduled = m->mem.due                 ? gate
                             : counter >= cfg->messages ? backed
                                                        : rk_max(trigger_at(m), backed);
    return m->profile->silence && m->mem.sent_data
               ? rk_min(scheduled, rk_max(silence_at(m), backed))
               : scheduled;
}
static bool silence_start(const fips_rekey *m, fips_instant now) {
    uint64_t counter = m->profile->keys->sent(m->cur.session);
    return !rk_due(trigger_at(m), now) && counter < m->params.cfg.messages &&
           counter < m->params.cfg.soft && !m->mem.due;
}
fips_instant fips_rekey_next_wake(const fips_rekey *m) {
    if (!m->live)
        return (fips_instant){0};
    due list[DUE_MAX];
    size_t n = deadlines(m, list);
    fips_instant best = {0};
    for (size_t i = 0; i < n; ++i)
        best = rk_min(best, list[i].at);
    best = rk_min(best, start_at(m));
    if (m->cur.present && m->profile->keys->sent(m->cur.session) >= m->params.cfg.hard)
        best = rk_min(best, m->cur.started);
    return best;
}
static void probe(fips_rekey *m, fips_instant now, uint64_t tag, fips_rekey_outputs *out) {
    fips_rekey_cycle *c = &m->cycle;
    if (!m->cur.present || c->kind != FIPS_REKEY_PendI || c->ep.tag != tag || !c->has_probes)
        return;
    fips_rekey_taken taken;
    if (!fips_rekey_ladder_take(&c->probes, now, &taken))
        return;
    bool late = !c->reverted && rk_before(c->deadline, rk_later(now, grace(m)));
    if (taken.send && !late) {
        if (c->fin.present)
            rk_send(out, &c->fin.msg, FIPS_REKEY_MSG_Msg3, FIPS_REKEY_ROUTE_Established);
        rk_prompt(out, c->ep.tag);
        c->probed = now;
        c->stayed = false;
    }
    rk_prompt(out, m->cur.tag);
}
static bool expire_one(fips_rekey *m, fips_instant now, due item, fips_rekey_env *env,
                       fips_rekey_outputs *out) {
    fips_rekey_taken taken;
    fips_rekey_cycle *c = &m->cycle;
    bool pendi = c->kind == FIPS_REKEY_PendI && c->ep.tag == item.id;
    switch (item.kind) {
    case D_DRAIN:
        if (m->drain.ep.present && m->drain.ep.tag == item.id) {
            bool evidenced = m->drain.evidenced;
            rk_retire(m, &m->drain.ep, out);
            memset(&m->drain, 0, sizeof m->drain);
            if (!evidenced)
                rk_abandoned(out, FIPS_REKEY_ABANDON_AdoptionNotSeen);
        }
        break;
    case D_CANDIDATE:
        for (size_t i = 0; i < m->cands.count; ++i)
            if (m->cands.list[i].seq == item.id && rk_due(m->cands.list[i].deadline, now)) {
                rk_cand_remove(&m->cands, i);
                rk_reject(out, FIPS_REKEY_REASON_CandidateExpired);
                break;
            }
        break;
    case D_UNADOPTED:
        if (c->kind == FIPS_REKEY_PendR && c->ep.tag == item.id)
            rk_cycle_end(m, FIPS_REKEY_ABANDON_Unadopted, FIPS_REKEY_CLOSE_Shutdown, out);
        break;
    case D_HELD:
        if (pendi && c->wait == FIPS_REKEY_WAIT_Held)
            cutover(m, now, FIPS_REKEY_EVIDENCE_Proof, false, false, false, env, out);
        break;
    case D_PROBE:
        probe(m, now, item.id, out);
        break;
    case D_DELAYED:
        if (pendi && c->fin.present && fips_rekey_ladder_take(&c->fin.ladder, now, &taken))
            rk_send(out, &c->fin.msg, FIPS_REKEY_MSG_Msg3, FIPS_REKEY_ROUTE_Established);
        break;
    case D_FIN:
        if (m->cur.present && m->cur.tag == item.id && m->cur.fin.present &&
            fips_rekey_ladder_take(&m->cur.fin.ladder, now, &taken)) {
            rk_send(out, &m->cur.fin.msg, FIPS_REKEY_MSG_Msg3, FIPS_REKEY_ROUTE_Established);
            if (taken.last) {
                memset(&m->cur.fin, 0, sizeof m->cur.fin);
                if (!m->cur.heard.ops)
                    m->mem.due = true;
            }
        }
        break;
    case D_ELICIT:
        if (m->cur.present && m->cur.tag == item.id && !m->cur.moved.ops && m->cur.has_elicit &&
            fips_rekey_ladder_take(&m->cur.elicit, now, &taken)) {
            rk_prompt(out, m->cur.tag);
            m->cur.prompted = now;
            if (taken.last)
                m->cur.has_elicit = false;
        }
        break;
    case D_CONFIRM:
        if (pendi && c->wait == FIPS_REKEY_WAIT_Evidence && rk_due(c->deadline, now)) {
            if (c->stayed || c->reverted) {
                fips_rekey_abandon cause = c->reverted ? FIPS_REKEY_ABANDON_RevertUnconfirmed
                                                       : FIPS_REKEY_ABANDON_PeerStayedOnOld;
                rk_retire(m, &c->ep, out);
                rk_cycle_clear(c);
                rk_abandoned(out, cause);
                m->mem.due = true;
            } else {
                cutover(m, now, FIPS_REKEY_EVIDENCE_Assumed, false, false, false, env, out);
                m->cur.assumed = true;
            }
        }
        break;
    case D_DELAY_END:
        if (pendi && c->wait == FIPS_REKEY_WAIT_Delay)
            cutover(m, now, FIPS_REKEY_EVIDENCE_Delay, false, false, false, env, out);
        break;
    case D_RESEND:
        if (c->kind == FIPS_REKEY_Init && rk_before(now, c->attempt.deadline) &&
            fips_rekey_ladder_take(&c->attempt.ladder, now, &taken)) {
            bool held = !m->cur.present && !smaller(m) && m->cands.count;
            if (!(held && taken.latest < rk_millis(m->params.cfg.young))) {
                rk_send(out, &c->attempt.out,
                        m->profile->messages == 2 ? FIPS_REKEY_MSG_Msg1 : FIPS_REKEY_MSG_Setup,
                        FIPS_REKEY_ROUTE_Established);
                c->attempt.resent = true;
                c->attempt.resend_count = rk_u8add(c->attempt.resend_count, 1);
            }
        }
        break;
    case D_EXPIRE:
        if (c->kind == FIPS_REKEY_Init && rk_due(c->attempt.deadline, now)) {
            rk_cycle_end(m, FIPS_REKEY_ABANDON_Unanswered, FIPS_REKEY_CLOSE_Shutdown, out);
            m->mem.abandons = rk_u8add(m->mem.abandons, 1);
            fips_duration random = rk_secs(env->jitter_draw(env->jitter_ctx, m->params.cfg.jitter));
            m->mem.backoff = rk_later(
                now, rk_add(fips_rekey_params_backoff(&m->params, m->mem.abandons), random));
            if (!m->cur.present && !m->cands.count) {
                rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown =
                    FIPS_REKEY_TEARDOWN_OpeningFailed;
                return true;
            }
        }
        break;
    }
    return false;
}
static bool due_before(due a, due b) {
    int cmp = a.at.ops->compare(a.at, b.at);
    return cmp < 0 || (cmp == 0 && (a.kind < b.kind || (a.kind == b.kind && a.id < b.id)));
}
static void poll(fips_rekey *m, fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out) {
    due done[3 * DUE_MAX];
    size_t ndone = 0;
    bool ended = false;
    for (unsigned pass = 0; pass < 3 && !ended; ++pass) {
        due all[DUE_MAX], ready[DUE_MAX];
        size_t n = deadlines(m, all), nr = 0;
        for (size_t i = 0; i < n; ++i) {
            bool seen = false;
            for (size_t j = 0; j < ndone; ++j)
                if (all[i].kind == done[j].kind && all[i].id == done[j].id)
                    seen = true;
            if (!seen && rk_due(all[i].at, now))
                ready[nr++] = all[i];
        }
        if (!nr)
            break;
        for (size_t i = 1; i < nr; ++i) {
            due d = ready[i];
            size_t j = i;
            while (j && due_before(d, ready[j - 1])) {
                ready[j] = ready[j - 1];
                --j;
            }
            ready[j] = d;
        }
        for (size_t i = 0; i < nr; ++i) {
            done[ndone++] = ready[i];
            ended = expire_one(m, now, ready[i], env, out) || ended;
        }
    }
    if (!ended && !m->cur.present && m->cycle.kind == FIPS_REKEY_Idle && !m->cands.count) {
        rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown = FIPS_REKEY_TEARDOWN_OpeningIdle;
        ended = true;
    }
    if (m->cur.present && m->profile->keys->sent(m->cur.session) >= m->params.cfg.hard) {
        rk_output(out, FIPS_REKEY_OUT_Teardown)->teardown = FIPS_REKEY_TEARDOWN_CounterExhausted;
        ended = true;
    }
    if (ended)
        return;
    if (rk_due(start_at(m), now)) {
        bool silent = silence_start(m, now);
        rk_start(m, now, env, out);
        if (m->cycle.kind == FIPS_REKEY_Init && silent &&
            rk_dcmp(silence_interval(m), m->params.cfg.after) < 0)
            m->mem.silence = rk_u8add(m->mem.silence, 1);
    }
    if (env->workers) {
        const fips_rekey_slot slots[] = {FIPS_REKEY_Current, FIPS_REKEY_Previous};
        for (size_t i = 0; i < 2; ++i) {
            const fips_rekey_epoch *ep = rk_epoch_const(m, slots[i]);
            if (!ep)
                continue;
            bool offered = false;
            for (size_t j = 0; j < out->count; ++j)
                if (out->items[j].kind == FIPS_REKEY_OUT_Delegate && out->items[j].tag == ep->tag)
                    offered = true;
            if (!offered)
                rk_delegate(m, ep, true, out);
        }
    }
}
static void apply(fips_rekey *m, fips_instant now, change *c, fips_rekey_env *env,
                  fips_rekey_outputs *out) {
    switch (c->kind) {
    case CH_REJECT:
        rk_reject(out, c->reason);
        break;
    case CH_RESEND:
        rk_send(out, &c->msg, c->msg_kind, c->route);
        break;
    case CH_REFUSE:
    case CH_REPLACE:
        if (c->record)
            fips_rekey_record_push(&m->record, digest(c->input.tok->ephemeral));
        if (c->kind == CH_REPLACE)
            rk_output(out, FIPS_REKEY_OUT_ReplaceRequested)->data.tok = *c->input.tok;
        else
            rk_reject(out, c->reason);
        break;
    case CH_RESPOND:
        respond(m, now, c, env, out);
        break;
    case CH_REPLY:
        reply(m, now, c, env, out);
        break;
    case CH_CANDIDATE:
        if (c->candidate_index != SIZE_MAX)
            rk_cand_remove(&m->cands, c->candidate_index);
        rk_send(out, &c->candidate.ack, FIPS_REKEY_MSG_Ack, FIPS_REKEY_ROUTE_Established);
        rk_cand_add(&m->cands, &c->candidate);
        break;
    case CH_DROP:
        rk_cand_remove(&m->cands, c->candidate_index);
        rk_reject(out, c->reason);
        opening_check(m, FIPS_REKEY_TEARDOWN_OpeningIdle, out);
        break;
    case CH_ACCEPT:
        complete(m, now, c, env, out);
        break;
    case CH_DONE:
        adopt(m, now, c, env, out);
        break;
    case CH_RELEASE:
        fips_rekey_completed_clear(&c->input.leg->done);
        if (c->input.leg->lease.live) {
            fips_rekey_output *o = rk_output(out, FIPS_REKEY_OUT_Unbind);
            o->transport = env->transport;
            o->index = c->input.leg->lease.index;
            release(&c->input.leg->lease, out);
        }
        rk_reject(out, c->reason);
        break;
    case CH_FRAME:
        frame(m, now, c, env, out);
        break;
    case CH_WORKER: {
        fips_rekey_epoch *ep = rk_epoch(m, c->frame.slot);
        assert(ep);
        if (!ep->has_floor || c->input.counter > ep->floor)
            ep->floor = c->input.counter;
        ep->has_floor = true;
        if (c->frame.slot == FIPS_REKEY_Current)
            heard_current(m, now, c->input.k, false, out);
        else
            heard_previous(m, now, out);
        break;
    }
    case CH_DELEGATED:
        rk_epoch(m, c->frame.slot)->recv = FIPS_REKEY_Delegated;
        break;
    case CH_UNDELEGATED:
        rk_epoch(m, c->frame.slot)->recv = FIPS_REKEY_Here;
        break;
    case CH_OPEN:
        m->bond.kind = FIPS_REKEY_BOND_Key;
        m->bond.key = c->input.peer;
        rk_start(m, now, env, out);
        break;
    case CH_DATA:
        m->mem.sent_data = true;
        break;
    case CH_PATH:
        m->mem.reachable = c->input.reachable;
        m->mem.has_srtt = c->input.has_srtt;
        m->mem.srtt = c->input.has_srtt ? c->input.srtt : rk_secs(0);
        break;
    case CH_POLL:
        poll(m, now, env, out);
        break;
    }
}
fips_status fips_rekey_step(fips_rekey *m, fips_instant now, fips_rekey_input input,
                            fips_rekey_env *env, fips_rekey_outputs *out) {
    if (!m->live || out->count)
        return FIPS_ERR_RANGE;
    change c = {0};
    judge(m, now, input, env, &c);
    apply(m, now, &c, env, out);
    /* Prompt handles outlive this call, so withdraw any demoted later in it. */
    size_t write = 0;
    for (size_t i = 0; i < out->count; ++i) {
        if (out->items[i].kind == FIPS_REKEY_OUT_Prompt && !sealable(m, out->items[i].tag))
            continue;
        if (write != i)
            out->items[write] = out->items[i];
        ++write;
    }
    sodium_memzero(out->items + write, (out->count - write) * sizeof *out->items);
    out->count = write;
    fips_rekey_completed_clear(&c.read.done);
    fips_rekey_hs_clear(&c.candidate.hs);
    sodium_memzero(&c, sizeof c);
    return FIPS_OK;
}
