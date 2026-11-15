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
    rk_test_close(&a);
    rk_test_close(&b);
    rk_test_close(&c);
    rk_test_close(&d);
    assert(fips_rekey_leaked() == leaks);
    printf("rekey: %u tests passed\n", tests);
    return 0;
}
