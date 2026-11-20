#include "../src/proto/rekey/time.h"
#include "rekey_test.h"
static void xk_cycle(void) {
    cycle(&rk_test_xk);
    assert(VIEW(&a).current.k && VIEW(&b).current.k);
    assert(VIEW(&a).previous.present && VIEW(&b).previous.present);
    rk_test_packet f = data(&a, T + 70);
    rk_test_recv(&b, T + 71, &f);
    rk_test_poll(&a, T + 11000);
    rk_test_poll(&b, T + 11000);
    assert(!VIEW(&a).previous.present && !VIEW(&b).previous.present);
}
static void setup_rows(void) {
    pair(&rk_test_xk);
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 0), bad = s;
    bad.len = 1;
    rk_test_recv(&b, T, &bad);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_SetupMalformed));
    rk_test_recv(&b, T, &s);
    assert(VIEW(&b).candidates == 1 && sends(&b, FIPS_REKEY_MSG_Ack));
    rk_test_recv(&b, T + 1, &s);
    assert(VIEW(&b).candidates == 1 && sends(&b, FIPS_REKEY_MSG_Ack));
    s = rk_test_fresh(&a.local, &b.local, 0);
    rk_test_recv(&b, T + 2, &s);
    assert(VIEW(&b).candidates == 2);
    s = rk_test_fresh(&a.local, &b.local, 0);
    rk_test_recv(&b, T + 3, &s);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_CandidatesFull) && VIEW(&b).candidates == 2);
    rk_test_poll(&b, T + 40002);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_CandidateExpired) && !VIEW(&b).candidates);
}
static void ack_rows(void) {
    pair(&rk_test_xk);
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 0), ack = rk_test_answer(&b.local, &s, 0);
    rk_test_recv(&a, T, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_UnknownAttempt));
    rk_test_poll(&a, T);
    s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    ack = rk_test_answer(&b.local, &s, 0);
    ack.bytes[100] ^= 1;
    rk_test_recv(&a, T + 10, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_ReplyUnauthenticated));
    ack.bytes[100] ^= 1;
    rk_test_recv(&a, T + 11, &ack);
    assert(VIEW(&a).pending.present);
    rk_test_recv(&a, T + 12, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_LateReply));
}
static void final_rows(void) {
    pair(&rk_test_xk);
    rk_test_packet fin = complete(&a, &b, T), bad = fin;
    bad.bytes[100] ^= 1;
    rk_test_recv(&b, T + 30, &bad);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_FinalUnauthenticated) &&
           VIEW(&b).candidates == 1);
    rk_test_recv(&b, T + 31, &fin);
    assert(VIEW(&b).current.tag == 1 && !VIEW(&b).candidates);
    rk_test_recv(&b, T + 32, &fin);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_FinalNoAttempt));
}
static void other_static_no_install(void) {
    pair(&rk_test_xk);
    rk_test_packet fin = complete(&a, &b, T);
    fips_peer_key other = {{66}};
    uint8_t epoch[8] = {9};
    fin = rk_test_forge3(&fin, other, epoch);
    rk_test_recv(&b, T + 30, &fin);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_KeyMismatch) && VIEW(&b).current.tag == 0 &&
           !VIEW(&b).candidates);
}
static void no_pending_data_seal(void) {
    pair(&rk_test_xk);
    (void)complete(&a, &b, T);
    fips_rekey_sealed sealed = {0};
    assert(fips_rekey_seal(&a.machine, fips_test_instant(T + 21), &sealed) == FIPS_REKEY_SEAL_OK &&
           sealed.tag == 0);
    fips_rekey_sealed_clear(&sealed);
    fips_rekey_sealed_prompt prompt = {0};
    assert(fips_rekey_seal_prompt(&a.machine, fips_test_instant(T + 21), 1, &prompt) ==
               FIPS_REKEY_SEAL_OK &&
           prompt.sealed.tag == 1);
    fips_rekey_sealed_prompt_clear(&prompt);
    assert(sodium_is_zero((const uint8_t *)&prompt, sizeof prompt));
}
static uint64_t assumed(void) {
    pair(&rk_test_xk);
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 31);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).previous.tag == 0 &&
           VIEW(&a).cycle == FIPS_REKEY_Idle);
    return T + 31020;
}
static void assumed_revert_and_expiry(void) {
    uint64_t cut = assumed();
    const fips_noise_session *now = fips_rekey_keys(&a.machine, FIPS_REKEY_Current),
                             *old = fips_rekey_keys(&a.machine, FIPS_REKEY_Previous);
    rk_test_packet f = data(&b, cut + 500);
    rk_test_recv(&a, cut + 500, &f);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Reverted) == 1 && VIEW(&a).current.tag == 0 &&
           VIEW(&a).pending.tag == 1);
    assert(fips_rekey_keys(&a.machine, FIPS_REKEY_Pending) == now &&
           fips_rekey_keys(&a.machine, FIPS_REKEY_Current) == old);
    idle(&a, cut + 500, 1, 10);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_RevertUnconfirmed) &&
           sends(&a, FIPS_REKEY_MSG_Setup));
    assert(sodium_is_zero((const uint8_t *)now, sizeof *now));
}
static void peer_stays_after_probe(void) {
    pair(&rk_test_xk);
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 16);
    rk_test_packet f = data(&b, T + 17020);
    rk_test_recv(&a, T + 17020, &f);
    idle(&a, T + 20, 18, 31);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_PeerStayedOnOld) && VIEW(&a).current.tag == 0 &&
           sends(&a, FIPS_REKEY_MSG_Setup));
}
static void repair_revert(void) {
    uint64_t cut = assumed();
    rk_test_step(&a, cut + 1000, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent});
    idle(&a, cut, 2, 30);
    assert(VIEW(&a).cycle == FIPS_REKEY_Init);
    rk_test_packet f = data(&b, cut + 30500);
    rk_test_recv(&a, cut + 30500, &f);
    assert(VIEW(&a).current.tag == 0 && rk_test_abandoned(&a, FIPS_REKEY_ABANDON_Reverted) &&
           rk_test_count(&a, FIPS_REKEY_OUT_Reverted) == 1);
}
static void late_poll_no_unjudgeable_probe(void) {
    pair(&rk_test_xk);
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 7);
    rk_test_packet f = data(&b, T + 9020);
    rk_test_recv(&a, T + 9020, &f);
    rk_test_poll(&a, T + 32020);
    for (size_t i = 0; i < a.out.count; ++i)
        if (a.out.items[i].kind == FIPS_REKEY_OUT_Prompt)
            assert(a.out.items[i].tag == 0);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_PeerStayedOnOld) &&
           sends(&a, FIPS_REKEY_MSG_Setup) && VIEW(&a).current.tag == 0);
}
static void silence_repair(void) {
    pair(&rk_test_xk);
    rk_test_step(&a, 40000, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent});
    rk_test_poll(&a, 40000);
    assert(sends(&a, FIPS_REKEY_MSG_Setup));
    idle(&a, 40000, 1, 30);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_Unanswered));
    idle(&a, 0, 71, 77);
    assert(!sends(&a, FIPS_REKEY_MSG_Setup));
    rk_test_poll(&a, 78000);
    assert(sends(&a, FIPS_REKEY_MSG_Setup));
}
static void silence_frame_resets(void) {
    pair(&rk_test_xk);
    rk_test_step(&a, 30000, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent});
    rk_test_poll(&a, 30000);
    assert(a.machine.mem.silence == 1);
    rk_test_packet f = data(&b, 30001);
    rk_test_recv(&a, 30002, &f);
    assert(!a.machine.mem.silence && !a.machine.mem.sent_data);
    assert(rk_cmp(a.machine.mem.rx_cur, fips_test_instant(30002)) == 0);
}
static void restart_at_ack(void) {
    pair(&rk_test_xk);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    memset(b.local.epoch, 42, 8);
    rk_test_packet ack = rk_test_answer(&b.local, &s, 0);
    rk_test_recv(&a, T + 20, &ack);
    assert(output_before(&a, FIPS_REKEY_OUT_Retired, FIPS_REKEY_OUT_Installed) &&
           output_before(&a, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_PeerRestarted));
    assert(VIEW(&a).current.tag == 1 && !VIEW(&a).previous.present && !VIEW(&a).current.k);
    unsigned expected[] = {1, 3, 7, 15, 31}, n = 0;
    a.wires = 0;
    for (unsigned i = 1; i <= 31; ++i) {
        rk_test_poll(&a, T + 20 + i * 1000);
        if (sends(&a, FIPS_REKEY_MSG_Msg3)) {
            assert(n < 5 && expected[n++] == i);
        }
        if (i < 31)
            assert(!sends(&a, FIPS_REKEY_MSG_Setup));
        a.wires = 0;
    }
    assert(n == 5 && sends(&a, FIPS_REKEY_MSG_Setup));
}
static void previous_counter_overlap(void) {
    pair(&rk_test_xk);
    rk_test_packet fin = complete(&a, &b, T), old[5];
    for (unsigned i = 0; i < 5; ++i)
        old[i] = data(&a, T + 21);
    rk_test_recv(&b, T + 30, &fin);
    flush(&b, &a, T + 40);
    for (unsigned i = 0; i < 3; ++i) {
        rk_test_packet f = data(&a, T + 41);
        rk_test_recv(&b, T + 42, &f);
    }
    for (unsigned i = 0; i < 5; ++i) {
        rk_test_recv(&b, T + 50, &old[i]);
        assert(rk_test_count(&b, FIPS_REKEY_OUT_Deliver) == 1 &&
               b.out.items[0].slot == FIPS_REKEY_Previous);
    }
    rk_test_recv(&b, T + 51, &old[0]);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_Replay));
}
static void forged_frames_unchanged(void) {
    pair(&rk_test_xk);
    rk_test_packet f = data(&b, 10);
    uint8_t before[sizeof a.machine];
    memcpy(before, &a.machine, sizeof before);
    for (unsigned i = 0; i < 30; ++i) {
        rk_test_packet bad = f;
        bad.bytes[bad.len - 1] ^= (uint8_t)(i | 1);
        rk_test_recv(&a, 20, &bad);
        assert(rk_test_rejected(&a, FIPS_REKEY_REASON_FrameUnauthenticated));
        assert(!memcmp(before, &a.machine, sizeof before));
    }
    rk_test_recv(&a, 30, &f);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
}
static void first_handshake(void) {
    rk_test_packet fin = first();
    assert(VIEW(&a).proven && !VIEW(&b).proven && VIEW(&a).current.tag == 0);
    rk_test_recv(&b, 30, &fin);
    assert(VIEW(&b).proven && VIEW(&b).current.tag == 0);
    flush(&b, &a, 40);
    idle(&a, 0, 1, 60);
    assert(!a.out.count);
    rk_test_poll(&a, T + 20);
    assert(sends(&a, FIPS_REKEY_MSG_Setup));
}
static void first_retained_msg3_resends(void) {
    (void)first();
    unsigned expected[] = {1, 3, 7, 15, 31}, n = 0;
    a.wires = 0;
    for (unsigned i = 1; i <= 31; ++i) {
        rk_test_poll(&a, 20 + i * 1000);
        if (sends(&a, FIPS_REKEY_MSG_Msg3)) {
            assert(n < 5 && i == expected[n++]);
        }
        a.wires = 0;
    }
    assert(n == 5 && sends(&a, FIPS_REKEY_MSG_Setup));
}
static void opening_expiry(void) {
    (void)first();
    rk_test_poll(&b, 40010);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_CandidateExpired) &&
           rk_test_count(&b, FIPS_REKEY_OUT_Teardown) == 1);
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    idle(&a, 0, 1, 30);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_Unanswered) &&
           rk_test_count(&a, FIPS_REKEY_OUT_Teardown) == 1);
}
static void open_not_blocked_by_candidate(void) {
    ordered_open();
    rk_test_packet s = rk_test_fresh(&a.local, &b.local, 0);
    rk_test_inbound(&b, &a.local, fips_rekey_params_fsp(), 0, &s);
    fips_peer_key wrong = {{66}};
    rk_test_step(&b, 100, (fips_rekey_input){.kind = FIPS_REKEY_IN_Open, .peer = wrong});
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_KeyMismatch));
    rk_test_step(&b, 101, (fips_rekey_input){.kind = FIPS_REKEY_IN_Open, .peer = a.local.key});
    assert(sends(&b, FIPS_REKEY_MSG_Setup) && VIEW(&b).candidates == 1);
    rk_test_step(&b, 102, (fips_rekey_input){.kind = FIPS_REKEY_IN_Open, .peer = a.local.key});
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_AttemptInFlight));
    fips_rekey_sealed sealed = {0};
    assert(fips_rekey_seal(&b.machine, fips_test_instant(103), &sealed) ==
           FIPS_REKEY_SEAL_NotEstablished);
}
static void carrier_eviction_preserves_other(void) {
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_inbound(&b, &a.local, fips_rekey_params_fsp(), 0, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_local forger;
    rk_test_local_init(&forger, 66, 3);
    for (unsigned i = 0; i < 30; ++i) {
        rk_test_packet x = rk_test_fresh(&forger, &b.local, 0);
        rk_test_recv_on(&b, 1000 + i * 1000, &x, true, true, 2);
        b.wires = 0;
    }
    rk_test_recv(&a, 35000, &ack);
    rk_test_packet fin = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&b, 35010, &fin);
    assert(VIEW(&b).proven);
}
static void young_crossing_resends(void) {
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    rk_test_open(&b, &a.local, fips_rekey_params_fsp(), 0);
    rk_test_packet sa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup),
                   sb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&a, 100, &sb);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_TieBreakKept));
    rk_test_recv(&b, 100, &sa);
    assert(sends(&b, FIPS_REKEY_MSG_Ack));
    b.wires = 0;
    for (unsigned i = 1; i <= 7; ++i) {
        rk_test_poll(&b, i * 1000);
        assert(sends(&b, FIPS_REKEY_MSG_Setup) == (i == 7));
        b.wires = 0;
    }
    rk_test_recv(&a, 5100, &sb);
    assert(sends(&a, FIPS_REKEY_MSG_Ack));
}
static void previous_does_not_confirm(void) {
    pair(&rk_test_xk);
    rk_test_packet fin = complete(&a, &b, T);
    rk_test_recv(&b, T + 30, &fin);
    b.wires = 0;
    rk_test_packet f = data(&a, T + 500);
    rk_test_recv(&b, T + 500, &f);
    assert(b.out.items[0].slot == FIPS_REKEY_Previous);
    rk_test_poll(&b, T + 1030);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_Prompt) == 1);
    rk_test_poll(&b, T + 3030);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_Prompt) == 1 && VIEW(&b).previous.present);
    assert(rk_cmp(fips_rekey_next_wake(&b.machine), fips_test_instant(T + 120030)) == 0);
}
static void late_deadlines_ordered(void) {
    pair(&rk_test_xk);
    (void)complete(&b, &a, T);
    rk_test_local forger;
    rk_test_local_init(&forger, 33, 3);
    rk_test_packet s = rk_test_fresh(&forger, &b.local, 0);
    rk_test_recv_on(&b, T + 21, &s, true, true, 2);
    b.wires = 0;
    rk_test_poll(&b, T + 50000);
    assert(output_before(&b, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_Reject) &&
           rk_test_rejected(&b, FIPS_REKEY_REASON_CandidateExpired));
    assert(VIEW(&b).current.tag == 1 && VIEW(&b).previous.tag == 0);
}
static void unreachable_gate(void) {
    pair(&rk_test_xk);
    rk_test_step(&a, 10, (fips_rekey_input){.kind = FIPS_REKEY_IN_PathFacts, .reachable = false});
    rk_test_poll(&a, T);
    assert(!a.out.count && !fips_rekey_next_wake(&a.machine).ops);
    rk_test_step(&a, T + 1, (fips_rekey_input){.kind = FIPS_REKEY_IN_PathFacts, .reachable = true});
    rk_test_poll(&a, T + 2);
    assert(sends(&a, FIPS_REKEY_MSG_Setup));
}
static void measured_responder_rtt(void) {
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_inbound(&b, &a.local, fips_rekey_params_fsp(), 10, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, 400, &ack);
    rk_test_packet fin = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&b, 810, &fin);
    rk_test_poll(&b, T + 810);
    assert(sends(&b, FIPS_REKEY_MSG_Setup) &&
           rk_cmp(fips_rekey_next_wake(&b.machine), fips_test_instant(T + 810 + 1600)) == 0);
}
static void srtt_first_rung(void) {
    unsigned srtt[] = {3, 20}, first_ms[] = {6000, 9000};
    for (unsigned i = 0; i < 2; ++i) {
        pair(&rk_test_xk);
        rk_test_step(&a, 10,
                     (fips_rekey_input){.kind = FIPS_REKEY_IN_PathFacts,
                                        .reachable = true,
                                        .has_srtt = true,
                                        .srtt = SEC(srtt[i])});
        rk_test_poll(&a, T);
        assert(rk_cmp(fips_rekey_next_wake(&a.machine), fips_test_instant(T + first_ms[i])) == 0);
    }
}
static void resent_reply_not_rtt_sample(void) {
    pair(&rk_test_xk);
    rk_test_poll(&a, T);
    a.wires = 0;
    rk_test_poll(&a, T + 3000);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, T + 3100, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, T + 3500, &ack);
    assert(!a.machine.mem.has_hs_rtt);
}
static void dampening_and_cutover_floors(void) {
    cycle(&rk_test_xk);
    a.machine.params.cfg.messages = 5;
    b.machine.params.cfg.messages = 5;
    for (unsigned i = 0; i < 5; ++i) {
        (void)data(&a, T + 61);
        (void)data(&b, T + 61);
    }
    a.wires = 0;
    b.wires = 0;
    idle(&a, T + 40, 1, 9);
    assert(!sends(&a, FIPS_REKEY_MSG_Setup));
    rk_test_poll(&a, T + 10040);
    assert(sends(&a, FIPS_REKEY_MSG_Setup));
    idle(&b, T + 30, 1, 29);
    assert(!sends(&b, FIPS_REKEY_MSG_Setup));
    assert(fips_rekey_dampened(&b.machine, fips_test_instant(T + 30029)));
    rk_test_poll(&b, T + 30030);
    assert(sends(&b, FIPS_REKEY_MSG_Setup) &&
           !fips_rekey_dampened(&b.machine, fips_test_instant(T + 30030)));
}
void rekey_fsp_tests(void) {
    RUN(xk_cycle);
    RUN(setup_rows);
    RUN(ack_rows);
    RUN(final_rows);
    RUN(other_static_no_install);
    RUN(no_pending_data_seal);
    RUN(assumed_revert_and_expiry);
    RUN(peer_stays_after_probe);
    RUN(repair_revert);
    RUN(late_poll_no_unjudgeable_probe);
    RUN(silence_repair);
    RUN(silence_frame_resets);
    RUN(restart_at_ack);
    RUN(previous_counter_overlap);
    RUN(forged_frames_unchanged);
    RUN(first_handshake);
    RUN(first_retained_msg3_resends);
    RUN(opening_expiry);
    RUN(open_not_blocked_by_candidate);
    RUN(carrier_eviction_preserves_other);
    RUN(young_crossing_resends);
    RUN(previous_does_not_confirm);
    RUN(late_deadlines_ordered);
    RUN(unreachable_gate);
    RUN(measured_responder_rtt);
    RUN(srtt_first_rung);
    RUN(resent_reply_not_rtt_sample);
    RUN(dampening_and_cutover_floors);
}
