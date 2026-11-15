#include "../src/proto/rekey/time.h"
#include "rekey_test.h"
static rk_test_packet manual_final(rk_test_local *from, const rk_test_local *to,
                                   rk_test_node *receiver, uint64_t at) {
    fips_rekey_hs hs = {0};
    fips_rekey_wire s = {0};
    assert(receiver->profile.initiate(from, &to->key, (fips_rekey_index){0}, &hs, &s));
    rk_test_packet setup = {.handshake = true, .kind = FIPS_REKEY_MSG_Setup, .len = s.len};
    memcpy(setup.bytes, s.bytes, s.len);
    rk_test_recv(receiver, at, &setup);
    rk_test_packet ack = rk_test_pop(receiver, true, FIPS_REKEY_MSG_Ack);
    fips_rekey_read read = {0};
    assert(receiver->profile.reply(&hs, from, ack.bytes, ack.len, &read) && read.has_fin);
    rk_test_packet fin = {.handshake = true, .kind = FIPS_REKEY_MSG_Msg3, .len = read.fin.len};
    memcpy(fin.bytes, read.fin.bytes, read.fin.len);
    fips_rekey_completed_clear(&read.done);
    fips_rekey_hs_clear(&hs);
    sodium_memzero(&read, sizeof read);
    return fin;
}
static void crossing(rk_test_packet *fa, rk_test_packet *fb) {
    pair(&rk_test_xk);
    rk_test_poll(&a, T);
    rk_test_poll(&b, T);
    rk_test_packet sa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup),
                   sb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, T + 10, &sa);
    rk_test_recv(&a, T + 10, &sb);
    rk_test_packet aa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Ack),
                   ab = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, T + 20, &ab);
    rk_test_recv(&b, T + 20, &aa);
    *fa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    *fb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg3);
}
static void stale_candidate_after_evidence(void) {
    rk_test_packet fa, fb;
    crossing(&fa, &fb);
    rk_test_recv(&b, T + 30, &fa);
    assert(VIEW(&b).current.tag == 2);
    flush(&b, &a, T + 40);
    assert(VIEW(&a).current.tag == 1 && !VIEW(&a).candidates);
    rk_test_recv(&a, T + 50, &fb);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_FinalNoAttempt));
}
static void late_final_keeps_new_candidate(void) {
    pair(&rk_test_xk);
    rk_test_packet older = manual_final(&a.local, &b.local, &b, T);
    rk_test_packet newer = manual_final(&a.local, &b.local, &b, T + 100);
    assert(VIEW(&b).candidates == 2);
    rk_test_recv(&b, T + 200, &older);
    assert(VIEW(&b).candidates == 1 && VIEW(&b).current.tag == 1);
    rk_test_recv(&b, T + 300, &newer);
    assert(VIEW(&b).current.tag == 2 && !VIEW(&b).candidates);
}
static void grace_cap(void) {
    pair(&rk_test_xk);
    rk_test_step(
        &a, 10,
        (fips_rekey_input){
            .kind = FIPS_REKEY_IN_PathFacts, .reachable = true, .has_srtt = true, .srtt = SEC(20)});
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 23);
    rk_test_packet f = data(&b, T + 23520);
    rk_test_recv(&a, T + 23520, &f);
    idle(&a, T + 20, 24, 31);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_PeerStayedOnOld));
}
static void lost_responder_prompt(void) {
    pair(&rk_test_xk);
    rk_test_packet fin = complete(&a, &b, T);
    rk_test_recv(&b, T + 30, &fin);
    b.wires = 0;
    rk_test_packet f = data(&a, T + 40);
    rk_test_recv(&b, T + 50, &f);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_Deliver) == 1 &&
           b.out.items[0].slot == FIPS_REKEY_Previous);
    rk_test_poll(&b, T + 1030);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_Prompt) == 1);
    flush(&b, &a, T + 1040);
    assert(VIEW(&a).current.tag == 1);
}
static void forged_setup_preserves_rotation(void) {
    pair(&rk_test_xk);
    rk_test_local forger;
    rk_test_local_init(&forger, 66, 3);
    rk_test_packet x = rk_test_fresh(&forger, &a.local, 0);
    rk_test_recv_on(&a, T - 5000, &x, true, true, 2);
    x = rk_test_fresh(&forger, &b.local, 0);
    rk_test_recv_on(&b, T - 5000, &x, true, true, 2);
    a.wires = b.wires = 0;
    rk_test_poll(&a, T);
    rk_test_poll(&b, T);
    assert(sends(&a, FIPS_REKEY_MSG_Setup) && sends(&b, FIPS_REKEY_MSG_Setup));
    x = rk_test_fresh(&forger, &b.local, 0);
    rk_test_recv_on(&b, T + 5, &x, true, true, 3);
    assert(VIEW(&b).cycle == FIPS_REKEY_Init);
    rk_test_poll(&b, T + 1000);
    assert(sends(&b, FIPS_REKEY_MSG_Setup));
}
static void first_crossing_heard_drops_candidate(void) {
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    rk_test_open(&b, &a.local, fips_rekey_params_fsp(), 0);
    rk_test_packet sa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup),
                   sb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&a, 7010, &sb);
    rk_test_recv(&b, 7010, &sa);
    rk_test_packet aa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Ack),
                   ab = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, 7020, &ab);
    rk_test_recv(&b, 7020, &aa);
    rk_test_packet fa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3),
                   fb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg3);
    assert(VIEW(&a).candidates == 1);
    rk_test_recv(&b, 7030, &fa);
    flush(&b, &a, 7040);
    assert(!VIEW(&a).candidates);
    rk_test_recv(&a, 7050, &fb);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_FinalNoAttempt) && VIEW(&a).current.tag == 0);
}
static void late_first_setup_refused(void) {
    ordered_open();
    rk_test_open(&b, &a.local, fips_rekey_params_fsp(), 0);
    rk_test_packet late = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 10);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, 20, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, 30, &ack);
    rk_test_packet fin = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&a, 40, &late);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_TieBreakKept) && !VIEW(&a).candidates);
    rk_test_recv(&b, 50, &fin);
    assert(rk_test_abandoned(&b, FIPS_REKEY_ABANDON_Yielded));
    flush(&b, &a, 60);
    assert(VIEW(&a).current.tag == 0);
}
static void assumed_larger_keeps_candidate(void) {
    pair(&rk_test_xk);
    (void)complete(&b, &a, T);
    b.wires = 0;
    rk_test_packet fin = manual_final(&a.local, &b.local, &b, T + 100);
    idle(&b, T + 20, 1, 31);
    assert(VIEW(&b).current.tag == 1 && VIEW(&b).candidates == 1);
    rk_test_recv(&b, T + 40000, &fin);
    assert(VIEW(&b).current.tag == 2 && rk_test_count(&b, FIPS_REKEY_OUT_Installed) == 1);
}
static void later_candidate_survives_first_confirmation(void) {
    ordered_open();
    rk_test_open(&b, &a.local, fips_rekey_params_fsp(), 0);
    rk_test_packet s = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_inbound(&a, &b.local, fips_rekey_params_fsp(), 10, &s);
    rk_test_packet ack = rk_test_pop(&a, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&b, 20, &ack);
    rk_test_packet fin = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&a, 30, &fin);
    a.wires = 0;
    fin = manual_final(&a.local, &b.local, &b, 1000);
    rk_test_packet f = data(&a, 1100);
    rk_test_recv(&b, 1200, &f);
    assert(VIEW(&b).candidates == 1);
    rk_test_recv(&b, 1300, &fin);
    assert(VIEW(&b).current.tag == 1 && rk_test_count(&b, FIPS_REKEY_OUT_Installed) == 1);
}
static void final_restart(void) {
    pair(&rk_test_xk);
    uint8_t seed = a.local.key.bytes[0];
    rk_test_close(&a);
    rk_test_node_init(&a, seed, &rk_test_xk);
    memset(a.local.epoch, 42, 8);
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), T - 1000);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, T - 990, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, T - 980, &ack);
    rk_test_packet fin = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&b, T - 970, &fin);
    assert(output_before(&b, FIPS_REKEY_OUT_Retired, FIPS_REKEY_OUT_Installed) &&
           output_before(&b, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_PeerRestarted));
    assert(!VIEW(&b).current.k && !VIEW(&b).previous.present);
    rk_test_packet f = data(&b, T - 960);
    rk_test_recv(&a, T - 960, &f);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
}
static void cutover_over_open_drain(void) {
    pair(&rk_test_ik);
    (void)complete(&a, &b, T);
    rk_test_poll(&a, T + 21);
    flush(&a, &b, T + 30);
    b.wires = 0;
    assert(VIEW(&a).previous.present);
    const fips_noise_session *old = fips_rekey_keys(&a.machine, FIPS_REKEY_Previous);
    rk_test_start(&a, T + 40000);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg1);
    rk_test_recv(&b, T + 40010, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg2);
    rk_test_recv(&a, T + 40020, &ack);
    rk_test_poll(&a, T + 40021);
    assert(VIEW(&a).current.tag == 2 && VIEW(&a).previous.tag == 1 && a.allocated == 2 &&
           rk_test_count(&a, FIPS_REKEY_OUT_Release) == 1);
    assert(sodium_is_zero((const uint8_t *)old, sizeof *old));
}
static void residual_msg1_does_not_retire_drain(void) {
    pair(&rk_test_ik);
    (void)complete(&a, &b, T);
    rk_test_poll(&a, T + 21);
    a.wires = 0;
    rk_test_packet s = rk_test_fresh(&b.local, &a.local, 999);
    rk_test_recv(&a, T + 10022, &s);
    assert(VIEW(&a).previous.present && !rk_test_count(&a, FIPS_REKEY_OUT_Retired));
}
static void previous_beside_responder_pending(void) {
    pair(&rk_test_xk_frame);
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 31);
    assert(VIEW(&a).current.tag == 1);
    b.wires = 0;
    rk_test_packet fin = manual_final(&b.local, &a.local, &a, T + 60000);
    rk_test_recv(&a, T + 60010, &fin);
    assert(VIEW(&a).cycle == FIPS_REKEY_PendR);
    rk_test_packet f = data(&b, T + 60020);
    rk_test_recv(&a, T + 60020, &f);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).cycle == FIPS_REKEY_PendR &&
           rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1);
}
static void drained_frame_during_repair_confirmation(void) {
    pair(&rk_test_xk);
    (void)complete(&a, &b, T);
    a.wires = 0;
    idle(&a, T + 20, 1, 31);
    uint64_t cut = T + 31020;
    rk_test_step(&a, cut + 1000, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent});
    idle(&a, cut, 2, 29);
    rk_test_poll(&a, cut + 30000);
    rk_test_packet setup = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    b.wires = 0;
    rk_test_recv(&b, cut + 30010, &setup);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, cut + 30020, &ack);
    a.wires = 0;
    uint64_t done = cut + 30020;
    rk_test_packet f = data(&b, done + 500);
    rk_test_recv(&a, done + 500, &f);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).pending.tag == 2);
    idle(&a, done, 1, 16);
    f = data(&b, done + 16500);
    rk_test_recv(&a, done + 16500, &f);
    assert(VIEW(&a).current.tag == 1 && VIEW(&a).pending.tag == 2);
    idle(&a, done, 17, 31);
    assert(rk_test_abandoned(&a, FIPS_REKEY_ABANDON_PeerStayedOnOld));
    f = data(&b, done + 31500);
    rk_test_recv(&a, done + 31500, &f);
    assert(VIEW(&a).current.tag == 0 && rk_test_count(&a, FIPS_REKEY_OUT_Reverted) == 1);
}
static void restart_crossing_converges(void) {
    pair(&rk_test_xk);
    rk_test_poll(&b, T);
    rk_test_packet sb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_poll(&a, T);
    rk_test_packet sa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, T + 5, &sa);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, T + 8, &ack);
    rk_test_packet oldfin = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    uint8_t seed = a.local.key.bytes[0];
    rk_test_close(&a);
    rk_test_node_init(&a, seed, &rk_test_xk);
    memset(a.local.epoch, 42, 8);
    rk_test_inbound(&a, &b.local, fips_rekey_params_fsp(), T + 10, &sb);
    a.wires = 0;
    rk_test_step(&a, T + 20, (fips_rekey_input){.kind = FIPS_REKEY_IN_Open, .peer = b.local.key});
    sa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&b, T + 30, &sa);
    ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, T + 40, &ack);
    a.wires = 0;
    rk_test_poll(&b, T + 1000);
    sb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Setup);
    rk_test_recv(&a, T + 1010, &sb);
    ack = rk_test_pop(&a, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&b, T + 1020, &ack);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_PeerRestarted) == 1 && VIEW(&b).candidates == 2);
    rk_test_packet fb = rk_test_pop(&b, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&b, T + 1030, &oldfin);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_StaleIncarnation));
    rk_test_recv(&a, T + 1040, &fb);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_TieBreakKept));
    rk_test_poll(&a, T + 1040);
    rk_test_packet fa = rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
    rk_test_recv(&b, T + 1050, &fa);
    assert(rk_test_count(&b, FIPS_REKEY_OUT_Installed) == 1);
    rk_test_packet f = data(&b, T + 1060);
    rk_test_recv(&a, T + 1060, &f);
    assert(rk_test_count(&a, FIPS_REKEY_OUT_Deliver) == 1 &&
           a.out.items[0].slot == FIPS_REKEY_Current);
}
void rekey_crossing_tests(void) {
    RUN(previous_beside_responder_pending);
    RUN(drained_frame_during_repair_confirmation);
    RUN(restart_crossing_converges);
    RUN(stale_candidate_after_evidence);
    RUN(late_final_keeps_new_candidate);
    RUN(grace_cap);
    RUN(lost_responder_prompt);
    RUN(forged_setup_preserves_rotation);
    RUN(first_crossing_heard_drops_candidate);
    RUN(late_first_setup_refused);
    RUN(assumed_larger_keeps_candidate);
    RUN(later_candidate_survives_first_confirmation);
    RUN(final_restart);
    RUN(cutover_over_open_drain);
    RUN(residual_msg1_does_not_retire_drain);
}
