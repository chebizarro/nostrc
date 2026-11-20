#include "../src/proto/rekey/time.h"
#include "rekey_test.h"
static void ik_cycle(void) {
    pair(&rk_test_ik);
    assert(rk_cmp(fips_rekey_next_wake(&a.machine), fips_test_instant(T)) == 0);
    rk_test_poll(&a, T - 1);
    assert(!a.out.count);
    (void)complete(&a, &b, T);
    assert(VIEW(&a).cycle == FIPS_REKEY_PendI && VIEW(&a).wait == FIPS_REKEY_WAIT_Held &&
           VIEW(&b).cycle == FIPS_REKEY_PendR);
    assert(output_before(&a, FIPS_REKEY_OUT_Unbind, FIPS_REKEY_OUT_Bind));
    const fips_noise_session *pending = fips_rekey_keys(&a.machine, FIPS_REKEY_Pending),
                             *old = fips_rekey_keys(&a.machine, FIPS_REKEY_Current);
    rk_test_poll(&a, T + 21);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).current.k && VIEW(&a).previous.tag == 0);
    assert(fips_rekey_keys(&a.machine, FIPS_REKEY_Current) == pending &&
           fips_rekey_keys(&a.machine, FIPS_REKEY_Previous) == old);
    assert(output_before(&a, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_Prompt));
    fips_rekey_sealed s = {0};
    assert(fips_rekey_seal(&a.machine, fips_test_instant(T + 26), &s) == FIPS_REKEY_SEAL_OK);
    assert(s.elapsed.secs == 0 && s.elapsed.nanos == 5000000 && s.theirs.value == 101);
    fips_rekey_sealed_clear(&s);
    assert(sodium_is_zero((const uint8_t *)&s, sizeof s));
    flush(&a, &b, T + 30);
    assert(VIEW(&b).current.tag == 1 && VIEW(&b).current.k);
    flush(&b, &a, T + 40);
    rk_test_poll(&a, T + 2000);
    assert(!a.out.count);
    rk_test_poll(&a, T + 50000);
    assert(!VIEW(&a).previous.present && rk_test_count(&a, FIPS_REKEY_OUT_Retired) == 1);
    assert(sodium_is_zero((const uint8_t *)old, sizeof *old));
    rk_test_poll(&b, T + 50000);
    (void)complete(&a, &b, T * 2 + 21);
    rk_test_poll(&a, T * 2 + 42);
    flush(&a, &b, T * 2 + 50);
    assert(!VIEW(&a).current.k && !VIEW(&b).current.k);
}
static void msg1_resends_and_backoff(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_packet orig = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    unsigned at[] = {1, 3, 7, 15}, n = 0;
    for (unsigned i = 1; i <= 30; ++i) {
        rk_test_poll(&a, T + i * 1000);
        if (sends(&a, FIPS_REKEY_MSG_Msg1)) {
            assert(n < 4 && at[n++] == i);
            rk_test_packet x = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
            assert(x.len == orig.len && !memcmp(x.bytes, orig.bytes, x.len));
        }
    }
    assert(n == 4 && rk_test_abandoned(&a, FIPS_REKEY_ABANDON_Unanswered) && a.allocated == 1);
    assert(rk_cmp(fips_rekey_next_wake(&a.machine), fips_test_instant(T + 38000)) == 0);
    rk_test_poll(&a, T + 37999);
    assert(!a.out.count);
    rk_test_poll(&a, T + 38000);
    assert(sends(&a, FIPS_REKEY_MSG_Msg1));
    rk_test_packet next = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    assert(memcmp(orig.bytes, next.bytes, orig.len));
    idle(&a, T + 38000, 1, 30);
    assert(rk_cmp(fips_rekey_next_wake(&a.machine), fips_test_instant(T + 77000)) == 0);
}
static void trigger_floors(void) {
    pair(&rk_test_ik);
    rk_test_clear(&a);
    fips_rekey_trigger(&a.machine, fips_test_instant(30999), &a.env, &a.out);
    rk_test_execute(&a, 30999);
    assert(!a.out.count);
    fips_rekey_trigger(&a.machine, fips_test_instant(31000), &a.env, &a.out);
    rk_test_execute(&a, 31000);
    assert(sends(&a, FIPS_REKEY_MSG_Msg1));
    rk_test_clear(&a);
    fips_rekey_trigger(&a.machine, fips_test_instant(31100), &a.env, &a.out);
    assert(!a.out.count);
}
static void responder_hold_expires(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    rk_test_recv(&b, T + 10, &s);
    const fips_noise_session *pending = fips_rekey_keys(&b.machine, FIPS_REKEY_Pending);
    rk_test_poll(&b, T + 120009);
    assert(VIEW(&b).cycle == FIPS_REKEY_PendR && !rk_test_count(&b, FIPS_REKEY_OUT_Installed));
    rk_test_poll(&b, T + 120010);
    assert(rk_test_abandoned(&b, FIPS_REKEY_ABANDON_Unadopted));
    assert(sodium_is_zero((const uint8_t *)pending, sizeof *pending));
}
static void replay_rewritten_index(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    rk_test_recv(&b, T + 10, &s);
    rk_test_recv(&b, T + 11, &s);
    assert(sends(&b, FIPS_REKEY_MSG_Msg2));
    s.bytes[1] ^= 1;
    rk_test_recv(&b, T + 12, &s);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_PendingHeld));
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg2);
    rk_test_recv(&a, T + 20, &ack);
    rk_test_poll(&a, T + 21);
    flush(&a, &b, T + 30);
    rk_test_recv(&b, T + 10030, &s);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_AnsweredBefore));
}
static void answer_routes(void) {
    for (unsigned arrival = 0; arrival < 2; ++arrival) {
        pair(&rk_test_ik);
        a.machine.params.cfg.arrival = arrival != 0;
        b.machine.params.cfg.arrival = arrival != 0;
        rk_test_poll(&a, T);
        rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
        rk_test_recv_on(&b, T + 10, &s, true, true, 1);
        fips_rekey_route route = b.out.items[b.out.count - 1].route;
        rk_test_recv_on(&b, T + 20, &s, true, true, 2);
        assert(sends(&b, FIPS_REKEY_MSG_Msg2));
        assert(!memcmp(&route, &b.out.items[b.out.count - 1].route, sizeof route));
    }
}
static void off_link_arrival_unreachable(void) {
    pair(&rk_test_ik);
    b.machine.params.cfg.arrival = true;
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 999);
    rk_test_recv_on(&b, T, &s, false, false, 2);
    assert(sends(&b, FIPS_REKEY_MSG_Msg2));
    assert(b.out.items[b.out.count - 1].route == FIPS_REKEY_ROUTE_EstablishedFallback);
}
static void crossing_msg1(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_poll(&b, T);
    rk_test_packet x = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1),
                   y = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg1);
    rk_test_recv(&a, T + 10, &y);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_DualInitWon) &&
           VIEW(&a).cycle == FIPS_REKEY_Init);
    rk_test_recv(&b, T + 10, &x);
    assert(rk_test_abandoned(&b, FIPS_REKEY_ABANDON_Yielded) && VIEW(&b).cycle == FIPS_REKEY_PendR);
}
static void refused_msg1_never_arms_pending(void) {
    pair(&rk_test_ik);
    b.local.fail = true;
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 999);
    rk_test_recv(&b, T, &s);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_HandshakeWrite) && !VIEW(&b).pending.present &&
           b.allocated == 1);
}
static void pending_frame_before_poll(void) {
    pair(&rk_test_ik);
    (void)complete(&a, &b, T);
    rk_test_packet f = rk_test_slot_frame(&b, FIPS_REKEY_Pending, false, DATA, sizeof DATA);
    rk_test_recv(&a, T + 25, &f);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).current.k &&
           rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
}
static void peer_k_not_own_k(void) {
    pair(&rk_test_ik);
    fips_rekey_force_k(&a.machine, true);
    rk_test_packet f = data(&b, 10);
    rk_test_recv(&a, 20, &f);
    (void)complete(&a, &b, T);
    rk_test_poll(&a, T + 21);
    assert(VIEW(&a).current.k);
}
static void restart_order(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    memset(b.local.epoch, 42, 8);
    rk_test_packet ack = rk_test_answer(&b.local, &s, 101);
    rk_test_recv(&a, T + 10, &ack);
    rk_test_poll(&a, T + 11);
    assert(output_before(&a, FIPS_REKEY_OUT_Retired, FIPS_REKEY_OUT_Installed) &&
           output_before(&a, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_PeerRestarted));
    assert(!VIEW(&a).previous.present && !VIEW(&a).current.k && VIEW(&a).peer_epoch[0] == 42);
}
static void forged_reply_unchanged(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1),
                   ack = rk_test_answer(&b.local, &s, 101);
    uint8_t before[sizeof a.machine];
    memcpy(before, &a.machine, sizeof before);
    ack.bytes[100] ^= 1;
    rk_test_recv(&a, T + 10, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_ReplyUnauthenticated) &&
           !memcmp(before, &a.machine, sizeof before));
    ack.bytes[100] ^= 1;
    ack.bytes[5] ^= 4;
    rk_test_recv(&a, T + 11, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_UnknownIndex));
}
static void frame_replay_and_short(void) {
    pair(&rk_test_ik);
    rk_test_packet f = data(&b, 10);
    rk_test_recv(&a, 20, &f);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
    rk_test_recv(&a, 21, &f);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_Replay));
    f = rk_test_data(&b, 30, DATA, 4);
    rk_test_recv(&a, 31, &f);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_ShortPlaintext));
    f = data(&b, 40);
    f.bytes[f.len - 1] ^= 1;
    uint8_t before[sizeof a.machine];
    memcpy(before, &a.machine, sizeof before);
    rk_test_recv(&a, 41, &f);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_FrameUnauthenticated) &&
           !memcmp(before, &a.machine, sizeof before));
}
static void counter_floors_soft_hard(void) {
    pair(&rk_test_ik);
    rk_test_close(&a);
    rk_test_close(&b);
    fips_rekey_config cfg = fips_rekey_config_fmp();
    cfg.messages = 4;
    cfg.soft = 5;
    cfg.hard = 8;
    rk_test_pair(&a, &b, &rk_test_ik, cfg);
    for (unsigned i = 0; i < 4; ++i)
        (void)data(&a, 10);
    rk_test_poll(&a, 30999);
    assert(!sends(&a, FIPS_REKEY_MSG_Msg1));
    rk_test_poll(&a, 31000);
    assert(sends(&a, FIPS_REKEY_MSG_Msg1));
    (void)data(&a, 31001);
    rk_test_poll(&a, 61000);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_Unanswered) && sends(&a, FIPS_REKEY_MSG_Msg1));
    for (unsigned i = 5; i < 8; ++i)
        (void)data(&a, 61001);
    fips_rekey_sealed s = {0};
    assert(fips_rekey_seal(&a.machine, fips_test_instant(62000), &s) == FIPS_REKEY_SEAL_Exhausted);
    rk_test_poll(&a, 62000);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Teardown) == 1);
}
static void carry_preserves_anchors(void) {
    cycle(&rk_test_ik);
    rk_test_packet f = data(&b, T + 100);
    rk_test_recv(&a, T + 101, &f);
    fips_instant started = VIEW(&a).current.started, link = VIEW(&a).link_since,
                 heard = VIEW(&a).last_rx;
    fips_rekey_carry carry = {0};
    rk_test_clear(&a);
    assert(
        fips_rekey_close_machine(&a.machine, FIPS_REKEY_CLOSE_CrossConnectionMove, &a.out, &carry));
    rk_test_execute(&a, 0);
    assert(rk_cmp(carry.started, started) == 0 && rk_cmp(carry.link_since, link) == 0 &&
           rk_cmp(carry.last_rx, heard) == 0);
    rk_test_establish(&a, &b.local, fips_rekey_params_fmp(), &carry);
    assert(rk_cmp(VIEW(&a).current.started, started) == 0 &&
           rk_cmp(VIEW(&a).link_since, link) == 0 && rk_cmp(VIEW(&a).last_rx, heard) == 0);
}
static void close_every_state(void) {
    for (unsigned phase = 0; phase < 5; ++phase) {
        pair(&rk_test_ik);
        if (phase == 1)
            rk_test_poll(&a, T);
        if (phase >= 2) {
            (void)complete(&a, &b, T);
            if (phase >= 3)
                rk_test_poll(&a, T + 21);
            if (phase == 4)
                flush(&a, &b, T + 30);
        }
        rk_test_close(&a);
        rk_test_close(&b);
        assert(!a.allocated && !b.allocated);
    }
}
static void previous_drain_retention(void) {
    pair(&rk_test_ik);
    (void)complete(&a, &b, T);
    rk_test_poll(&a, T + 21);
    a.wires = 0;
    rk_test_packet f = data(&b, T + 10000);
    rk_test_recv(&a, T + 10001, &f);
    assert(VIEW(&a).previous.present && VIEW(&a).current.tag == 1);
    rk_test_poll(&a, T + 119000);
    assert(VIEW(&a).previous.present);
    rk_test_poll(&a, T + 120021);
    assert(!VIEW(&a).previous.present);
}
static void transport_recorded_at_bind(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    a.env.transport = 99;
    rk_test_poll(&a, T + 30000);
    for (size_t i = 0; i < a.out.count; ++i)
        if (a.out.items[i].kind == FIPS_REKEY_OUT_Unbind)
            assert(a.out.items[i].transport == 1);
}
static void worker_report_rows(void) {
    pair(&rk_test_ik);
    a.env.workers = true;
    rk_test_poll(&a, 10);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Delegate) == 1);
    rk_test_step(&a, 10, (fips_rekey_input){.kind = FIPS_REKEY_IN_Delegated, .tag = 0});
    assert(VIEW(&a).current.recv == FIPS_REKEY_Delegated);
    fips_rekey_input_kind kinds[] = {FIPS_REKEY_IN_WorkerFrame, FIPS_REKEY_IN_Delegated,
                                     FIPS_REKEY_IN_Undelegated};
    for (unsigned i = 0; i < 3; ++i) {
        rk_test_step(&a, 20, (fips_rekey_input){.kind = kinds[i], .tag = 99});
        assert(rk_test_rejected(&a, FIPS_REKEY_REASON_StaleTag));
    }
    rk_test_step(&a, 20, (fips_rekey_input){.kind = FIPS_REKEY_IN_Delegated, .tag = 0});
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_AlreadyDelegated));
    rk_test_packet f = data(&b, 30);
    rk_test_recv(&a, 30, &f);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_DelegatedEpoch));
    rk_test_poll(&b, T);
    rk_test_packet s = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg1);
    rk_test_recv(&a, T + 10, &s);
    for (unsigned i = 0; i < 3; ++i) {
        rk_test_step(&a, T + 11, (fips_rekey_input){.kind = kinds[i], .tag = 1});
        assert(rk_test_rejected(&a, i == 1 ? FIPS_REKEY_REASON_NotOffered
                                           : FIPS_REKEY_REASON_NotDelegated));
    }
    rk_test_packet ack = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg2);
    rk_test_recv(&b, T + 20, &ack);
    rk_test_poll(&b, T + 21);
    flush(&b, &a, T + 30);
    assert(VIEW(&a).current.tag == 1 && rk_test_count(&a, FIPS_REKEY_OUT_Delegate) == 1);
    rk_test_step(&a, T + 40,
                 (fips_rekey_input){.kind = FIPS_REKEY_IN_WorkerFrame, .tag = 0, .counter = 9});
    assert(!a.out.count && rk_cmp(VIEW(&a).last_rx, fips_test_instant(T + 40)) == 0);
    f = rk_test_slot_frame(&b, FIPS_REKEY_Previous, false, DATA, sizeof DATA);
    rk_test_recv(&a, T + 41, &f);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_DelegatedEpoch));
    rk_test_step(&a, T + 50, (fips_rekey_input){.kind = FIPS_REKEY_IN_Undelegated, .tag = 1});
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_NotDelegated));
}
static void reoffered_worker_floor(void) {
    pair(&rk_test_ik);
    a.env.workers = true;
    rk_test_poll(&a, 10);
    rk_test_step(&a, 10, (fips_rekey_input){.kind = FIPS_REKEY_IN_Delegated, .tag = 0});
    rk_test_packet frames[8];
    for (unsigned i = 0; i < 8; ++i)
        frames[i] = data(&b, 20 + i);
    for (unsigned i = 0; i <= 5; ++i)
        rk_test_step(&a, 30,
                     (fips_rekey_input){.kind = FIPS_REKEY_IN_WorkerFrame, .tag = 0, .counter = i});
    rk_test_step(&a, 40, (fips_rekey_input){.kind = FIPS_REKEY_IN_Undelegated, .tag = 0});
    rk_test_recv(&a, 50, &frames[3]);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_Replay));
    rk_test_recv(&a, 50, &frames[6]);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
    rk_test_poll(&a, 60);
    assert(a.out.count == 1 && a.out.items[0].kind == FIPS_REKEY_OUT_Delegate &&
           a.out.items[0].data.recv.has_floor && a.out.items[0].data.recv.floor == 5);
    rk_test_clear(&a);
    assert(sodium_is_zero((const uint8_t *)&a.out, sizeof a.out));
}
static void young_and_off_link_msg1_rows(void) {
    pair(&rk_test_ik);
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 100),
                   answer = rk_test_answer(&b.local, &s, 100);
    fips_rekey_msg1_read tok;
    assert(rk_test_token(&s, &tok));
    fips_rekey_digest digest, ephemeral;
    memcpy(digest.bytes, tok.digest, 32);
    memcpy(ephemeral.bytes, tok.ephemeral, 32);
    fips_rekey_wire msg = {.len = answer.len};
    memcpy(msg.bytes, answer.bytes, answer.len);
    fips_rekey_record record;
    fips_rekey_record_setup(&record, digest, ephemeral, &msg);
    fips_rekey_note_setup(&b.machine, &record);
    rk_test_recv(&b, 5000, &s);
    assert(sends(&b, FIPS_REKEY_MSG_Msg2));
    s.bytes[1] ^= 4;
    rk_test_recv(&b, 5000, &s);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_AnsweredBefore));
    rk_test_packet young = rk_test_fresh(&a.local, &b.local, 101);
    rk_test_recv(&b, 5000, &young);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_ReplaceRequested) == 1 && !VIEW(&b).pending.present);
    rk_test_recv(&b, 5001, &young);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_AnsweredBefore));
    rk_test_packet changed = rk_test_fresh(&a.local, &b.local, 102);
    changed.bytes[81] = 9;
    rk_test_auth(&changed);
    rk_test_recv(&b, 5002, &changed);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_EpochChanged));
    rk_test_packet off = rk_test_fresh(&a.local, &b.local, 103);
    rk_test_recv_on(&b, 40000, &off, false, true, 2);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_OffLink));
    rk_test_recv(&b, 40001, &off);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_AnsweredBefore));
    rk_test_packet fresh = rk_test_fresh(&a.local, &b.local, 104);
    rk_test_recv(&b, 41000, &fresh);
    assert(VIEW(&b).cycle == FIPS_REKEY_PendR);
    rk_test_packet later = rk_test_fresh(&a.local, &b.local, 105);
    rk_test_recv(&b, 42000, &later);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_PendingHeld));
}
static void index_exhaustion_and_start_failure(void) {
    pair(&rk_test_ik);
    a.next_index = 1024;
    rk_test_poll(&a, T);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_IndexExhausted) &&
           VIEW(&a).cycle == FIPS_REKEY_Idle);
    a.next_index = 101;
    a.local.fail = true;
    rk_test_poll(&a, T);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_HandshakeWrite) && a.allocated == 1 &&
           VIEW(&a).cycle == FIPS_REKEY_Idle);
}
void rekey_fmp_tests(void) {
    RUN(worker_report_rows);
    RUN(reoffered_worker_floor);
    RUN(young_and_off_link_msg1_rows);
    RUN(index_exhaustion_and_start_failure);
    RUN(ik_cycle);
    RUN(msg1_resends_and_backoff);
    RUN(trigger_floors);
    RUN(responder_hold_expires);
    RUN(replay_rewritten_index);
    RUN(answer_routes);
    RUN(off_link_arrival_unreachable);
    RUN(crossing_msg1);
    RUN(refused_msg1_never_arms_pending);
    RUN(pending_frame_before_poll);
    RUN(peer_k_not_own_k);
    RUN(restart_order);
    RUN(forged_reply_unchanged);
    RUN(frame_replay_and_short);
    RUN(counter_floors_soft_hard);
    RUN(carry_preserves_anchors);
    RUN(close_every_state);
    RUN(previous_drain_retention);
    RUN(transport_recorded_at_bind);
}
