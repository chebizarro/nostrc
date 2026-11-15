#include "rekey_test.h"
/* The next-shaped indexed three-message profile takes authenticated PeerDone
 * from a leg owner, not the candidate pool used by the unindexed XK shape. */
static void attempt(rk_test_node *from, rk_test_node *to, uint64_t at, fips_rekey_leg *leg) {
    rk_test_poll(from, at);
    rk_test_packet setup = rk_test_pop(from, true, FIPS_REKEY_MSG_Setup);
    memset(leg, 0, sizeof *leg);
    assert(fips_rekey_allocate(to->env.indices, &leg->lease));
    ++to->external_leases;
    rk_test_complete_leg(&to->local, &setup, leg->lease.index, leg);
    rk_test_packet ack = rk_test_answer(&to->local, &setup, leg->lease.index);
    rk_test_recv(from, at + 10, &ack);
}
static void accept(rk_test_node *to, uint64_t at, fips_rekey_leg *leg, fips_rekey_intent intent) {
    assert(to->external_leases);
    --to->external_leases;
    rk_test_step(to, at,
                 (fips_rekey_input){.kind = FIPS_REKEY_IN_PeerDone, .leg = leg, .intent = intent});
    assert(sodium_is_zero((const uint8_t *)&leg->done.session, sizeof leg->done.session) &&
           !leg->lease.live);
}
static void delayed_cutover(void) {
    pair(&rk_test_xx);
    fips_rekey_leg leg = {0};
    attempt(&a, &b, T, &leg);
    accept(&b, T + 20, &leg, FIPS_REKEY_INTENT_Rekey);
    assert(VIEW(&a).cycle == FIPS_REKEY_PendI && VIEW(&a).wait == FIPS_REKEY_WAIT_Delay &&
           VIEW(&b).cycle == FIPS_REKEY_PendR);
    rk_test_poll(&a, T + 1010);
    assert(sends(&a, FIPS_REKEY_MSG_Msg3));
    rk_test_poll(&a, T + 2010);
    assert(VIEW(&a).current.tag == 1 && rk_test_count(&a, FIPS_REKEY_OUT_Installed) == 1);
    flush(&a, &b, T + 2020);
    assert(VIEW(&b).current.tag == 1);
    flush(&b, &a, T + 2030);
    rk_test_poll(&a, T + 15010);
    assert(!sends(&a, FIPS_REKEY_MSG_Msg3));
}
static void leg_refusal_and_release(void) {
    pair(&rk_test_xx);
    fips_rekey_leg leg = {0};
    attempt(&a, &b, T, &leg);
    accept(&b, T + 20, &leg, FIPS_REKEY_INTENT_Fresh);
    assert(rk_test_rejected(&b, FIPS_REKEY_REASON_NotRekey) && b.allocated == 1 &&
           VIEW(&b).current.tag == 0);
}
static void leg_tie_break(void) {
    pair(&rk_test_xx);
    fips_rekey_leg at_b = {0}, at_a = {0};
    attempt(&a, &b, T, &at_b);
    /* A leg lease is owned by the external table, not machine bindings. */
    attempt(&b, &a, T, &at_a);
    accept(&a, T + 20, &at_a, FIPS_REKEY_INTENT_Rekey);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_TieBreakKept));
    accept(&b, T + 20, &at_b, FIPS_REKEY_INTENT_Rekey);
    assert(VIEW(&b).cycle == FIPS_REKEY_PendR && rk_test_count(&b, FIPS_REKEY_OUT_Retired) == 1);
}
static void impostor_reply_no_restart(void) {
    pair(&rk_test_xx);
    rk_test_poll(&a, T);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_local impostor;
    rk_test_local_init(&impostor, 66, 3);
    memset(impostor.epoch, 99, 8);
    rk_test_packet ack = rk_test_answer(&impostor, &s, 300);
    uint8_t before[sizeof a.machine];
    memcpy(before, &a.machine, sizeof before);
    rk_test_recv(&a, T + 10, &ack);
    assert(rk_test_rejected(&a, FIPS_REKEY_REASON_ReplyUnauthenticated) &&
           !memcmp(before, &a.machine, sizeof before));
}
static void frame_driven_responder(void) {
    pair(&rk_test_xk_frame);
    rk_test_packet fin = complete(&a, &b, T);
    rk_test_recv(&b, T + 30, &fin);
    assert(VIEW(&b).cycle == FIPS_REKEY_PendR && !rk_test_count(&b, FIPS_REKEY_OUT_Installed));
    rk_test_packet probe = rk_test_pop(&a, false, 0);
    rk_test_recv(&b, T + 40, &probe);
    assert(VIEW(&b).current.tag == 1 && rk_test_count(&b, FIPS_REKEY_OUT_Installed) == 1);
}
static void retained_delayed_ladder(void) {
    pair(&rk_test_xx);
    fips_rekey_leg leg = {0};
    attempt(&a, &b, T, &leg);
    accept(&b, T + 20, &leg, FIPS_REKEY_INTENT_Fresh);
    unsigned expected[] = {1, 3, 7, 15}, count = 0;
    bool repair = false;
    a.wires = 0;
    for (unsigned i = 1; i <= 40; ++i) {
        rk_test_poll(&a, T + 10 + i * 1000);
        if (sends(&a, FIPS_REKEY_MSG_Msg3)) {
            assert(count < 4 && expected[count++] == i);
        }
        repair |= sends(&a, FIPS_REKEY_MSG_Setup);
        a.wires = 0;
    }
    assert(count == 4 && repair);
}
static void delayed_completion_tie(void) {
    for (unsigned smaller = 0; smaller < 2; ++smaller) {
        pair(&rk_test_xx);
        rk_test_node *me = smaller ? &a : &b, *peer = smaller ? &b : &a;
        fips_rekey_leg old = {0}, leg = {0};
        attempt(me, peer, T, &old);
        accept(peer, T + 20, &old, FIPS_REKEY_INTENT_Fresh);
        rk_test_poll(me, T + 2010);
        assert(VIEW(me).current.tag == 1);
        attempt(peer, me, T + 3000, &leg);
        accept(me, T + 3020, &leg, FIPS_REKEY_INTENT_Rekey);
        if (smaller)
            assert(rk_test_rejected(me, FIPS_REKEY_REASON_TieBreakKept) &&
                   VIEW(me).cycle == FIPS_REKEY_Idle);
        else
            assert(VIEW(me).cycle == FIPS_REKEY_PendR && VIEW(me).current.tag == 1 &&
                   !me->machine.cur.fin.present);
    }
}
void rekey_next_tests(void) {
    RUN(retained_delayed_ladder);
    RUN(delayed_completion_tie);
    RUN(delayed_cutover);
    RUN(leg_refusal_and_release);
    RUN(leg_tie_break);
    RUN(impostor_reply_no_restart);
    RUN(frame_driven_responder);
}
