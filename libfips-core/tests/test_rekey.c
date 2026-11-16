#include "rekey_test.h"
rk_test_node a, b, c, d;
unsigned tests;
static void destroyed_machine_is_inert(void) {
    pair(&rk_test_ik);
    rk_test_poll(&a, T);
    rk_test_close(&a);
    fips_rekey_outputs out = {0};
    fips_rekey_sealed sealed = {0};
    for (int i = FIPS_REKEY_IN_Reply; i <= FIPS_REKEY_IN_Poll; ++i) {
        assert(fips_rekey_step(&a.machine, fips_test_instant(T),
                               (fips_rekey_input){.kind = (fips_rekey_input_kind)i}, &a.env,
                               &out) == FIPS_ERR_RANGE);
        assert(sodium_is_zero((const uint8_t *)&a.machine, sizeof a.machine));
        assert(!out.count);
    }
    fips_rekey_start_now(&a.machine, fips_test_instant(T), &a.env, &out);
    assert(fips_rekey_seal(&a.machine, fips_test_instant(T), &sealed) ==
           FIPS_REKEY_SEAL_NotEstablished);
    assert(!out.count && !fips_rekey_next_wake(&a.machine).ops);
}
static void pending_prompt_hard_limit(void) {
    pair(&rk_test_xk);
    rk_test_close(&a);
    rk_test_close(&b);
    fips_rekey_config cfg = fips_rekey_config_fsp();
    cfg.messages = 8;
    cfg.soft = 10;
    cfg.hard = 12;
    rk_test_pair(&a, &b, &rk_test_xk, cfg);
    (void)complete(&a, &b, T);
    fips_rekey_sealed_prompt prompt = {0};
    const fips_noise_session *pending = fips_rekey_keys(&a.machine, FIPS_REKEY_Pending);
    while (fips_noise_session_current_send_counter(pending) < 12) {
        assert(fips_rekey_seal_prompt(&a.machine, fips_test_instant(T + 21), 1, &prompt) ==
               FIPS_REKEY_SEAL_OK);
        fips_rekey_sealed_prompt_clear(&prompt);
    }
    uint8_t before[sizeof a.machine];
    memcpy(before, &a.machine, sizeof before);
    assert(fips_rekey_seal_prompt(&a.machine, fips_test_instant(T + 22), 1, &prompt) ==
           FIPS_REKEY_SEAL_Exhausted);
    assert(!memcmp(before, &a.machine, sizeof before));
    assert(sodium_is_zero((const uint8_t *)&prompt, sizeof prompt));
    assert(fips_rekey_sent(&a.machine) < 12);
}
static void authenticated_worker_completion(void) {
    const fips_rekey_profile *profiles[] = {&rk_test_ik, &rk_test_xk};
    for (size_t i = 0; i < 2; ++i) {
        pair(profiles[i]);
        fips_rekey_epoch_tag tag = VIEW(&a).current.tag;
        uint8_t plain[64] = {0x52};
        size_t len = a.profile.floor > 6 ? a.profile.floor : 6;
        fips_rekey_input in = {.kind = FIPS_REKEY_IN_AuthenticatedFrame, .tag = tag,
            .buf = plain, .len = len, .hint = {.counter = 42,
                .index = fips_rekey_ours(&a.machine, FIPS_REKEY_Current).value}};
        fips_rekey_outputs out = {0};
        assert(fips_rekey_step(&a.machine, fips_test_instant(10), in, &a.env, &out) == FIPS_OK);
        bool delivered = false;
        for (size_t j = 0; j < out.count; ++j) delivered |= out.items[j].kind == FIPS_REKEY_OUT_Deliver;
        assert(delivered);
        fips_rekey_outputs_clear(&out);
        assert(fips_rekey_step(&a.machine, fips_test_instant(11), in, &a.env, &out) == FIPS_OK);
        assert(out.count && out.items[0].kind == FIPS_REKEY_OUT_Reject && out.items[0].reason == FIPS_REKEY_REASON_Replay);
        fips_rekey_outputs_clear(&out);
        in.tag = UINT64_MAX; in.hint.counter = 43;
        assert(fips_rekey_step(&a.machine, fips_test_instant(12), in, &a.env, &out) == FIPS_OK);
        assert(out.count && out.items[0].kind == FIPS_REKEY_OUT_Reject && out.items[0].reason == FIPS_REKEY_REASON_StaleTag);
        fips_rekey_outputs_clear(&out);
        const fips_noise_session *keys = fips_rekey_keys(&a.machine, FIPS_REKEY_Current);
        assert(fips_noise_session_check_replay(keys, 43) == FIPS_NOISE_OK);
        /* Both queued completions authenticated the pending key before the
         * dispatcher saw either. First cuts over; duplicate must still replay. */
        pair(profiles[i]); (void)complete(&a, &b, T);
        assert(VIEW(&a).pending.present);
        in.tag = VIEW(&a).pending.tag; in.hint.counter = 900;
        in.hint.index = fips_rekey_ours(&a.machine, FIPS_REKEY_Pending).value;
        in.hint.k = VIEW(&a).pending.k;
        assert(fips_rekey_step(&a.machine, fips_test_instant(T + 21), in, &a.env, &out) == FIPS_OK);
        assert(VIEW(&a).current.tag == in.tag);
        fips_rekey_outputs_clear(&out);
        assert(fips_rekey_step(&a.machine, fips_test_instant(T + 22), in, &a.env, &out) == FIPS_OK);
        assert(out.count && out.items[0].kind == FIPS_REKEY_OUT_Reject && out.items[0].reason == FIPS_REKEY_REASON_Replay);
        fips_rekey_outputs_clear(&out);
        assert(VIEW(&a).previous.present);
        in.tag = VIEW(&a).previous.tag; in.hint.counter = 901;
        in.hint.index = fips_rekey_ours(&a.machine, FIPS_REKEY_Previous).value;
        in.hint.k = VIEW(&a).previous.k;
        assert(fips_rekey_step(&a.machine, fips_test_instant(T + 23), in, &a.env, &out) == FIPS_OK);
        delivered = false;
        for (size_t j = 0; j < out.count; ++j) delivered |= out.items[j].kind == FIPS_REKEY_OUT_Deliver;
        assert(delivered); fips_rekey_outputs_clear(&out);
    }
}
int main(void) {
    assert(sodium_init() >= 0);
    rekey_params_tests();
    size_t leaks = fips_rekey_leaked();
    rekey_fmp_tests();
    rekey_fsp_tests();
    rekey_next_tests();
    rekey_crossing_tests();
    RUN(destroyed_machine_is_inert);
    RUN(pending_prompt_hard_limit);
    RUN(authenticated_worker_completion);
    rk_test_close(&a);
    rk_test_close(&b);
    rk_test_close(&c);
    rk_test_close(&d);
    assert(fips_rekey_leaked() == leaks);
    printf("rekey: %u tests passed\n", tests);
    return 0;
}
