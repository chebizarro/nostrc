#include "../src/proto/rekey/internal.h"
#include "rekey_test.h"
#include <jansson.h>
static void after_jitter(void) {
    fips_rekey_config x = fips_rekey_config_fmp();
    fips_rekey_params p;
    x.after = SEC(15);
    x.link_age = SEC(0);
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_AfterNotAboveJitter);
    x.after = SEC(16);
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_OK);
}
static void zero_messages(void) {
    fips_rekey_config x = fips_rekey_config_fmp();
    fips_rekey_params p;
    x.messages = 0;
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_ZeroMessages);
    x.messages = 1;
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_OK);
}
static void after_link_floor(void) {
    fips_rekey_config x = fips_rekey_config_fmp();
    fips_rekey_params p;
    x.after = SEC(44);
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_AfterBelowLinkFloor);
    x.after = SEC(45);
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_OK);
}
static void invalid_settings(void) {
    fips_rekey_config x;
    fips_rekey_params p;
#define BAD(field, value, error)                                                                   \
    do {                                                                                           \
        x = fips_rekey_config_fmp();                                                               \
        x.field = value;                                                                           \
        assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_##error);                         \
    } while (0)
    BAD(messages, x.soft + 1, MessagesPastSoft);
    BAD(after, SEC((uint64_t)UINT32_MAX + 1), AfterTooLong);
    BAD(check, SEC(0), Zero);
    BAD(silence, SEC(0), Zero);
    BAD(handshake, SEC(3), HandshakeTooShort);
    BAD(drain, SEC(2), DrainTooShort);
    BAD(retention, SEC(9), RetentionBelowDrain);
    BAD(hold, SEC(31), HoldTooShort);
    BAD(soft, x.hard, SoftNotBelowHard);
    BAD(after, ((fips_duration){120, 1000000000}), InvalidDuration);
#undef BAD
}
static void derived_defaults(void) {
    fips_rekey_params p = fips_rekey_params_fsp();
    assert(p.confirm.secs == 31 && p.span.secs == 41 && p.grace.secs == 8 && p.rung.secs == 9 &&
           p.gap.secs == 4 && p.budget.secs == 30);
    assert(p.inert.first == 1 && p.inert.probes == 1);
    assert(fips_rekey_params_backoff(&p, 1).secs == 1);
    assert(fips_rekey_params_backoff(&p, 3).secs == 4 &&
           fips_rekey_params_backoff(&p, 200).secs == 120);
    fips_rekey_config x = fips_rekey_config_fmp();
    x.resends = 3;
    assert(fips_rekey_params_new(x, &p) == FIPS_REKEY_PARAMS_OK);
    assert(p.budget.secs == 8 && !p.inert.first);
}
static void doubling(void) {
    uint8_t inert;
    fips_duration end = SEC(30);
    fips_rekey_schedule s = fips_rekey_schedule_doubling(SEC(1), 5, &end, &inert);
    uint64_t expected[] = {1000, 3000, 7000, 15000, 31000};
    assert(s.len == 4 && inert == 1 && !memcmp(s.at, expected, 4 * sizeof(uint64_t)));
    s = fips_rekey_schedule_doubling(SEC(9), 16, &end, &inert);
    assert(s.len == 2 && s.at[0] == 9000 && s.at[1] == 27000 && inert == 14);
    s = fips_rekey_schedule_doubling(SEC(1), 5, NULL, &inert);
    assert(s.len == 5 && !memcmp(s.at, expected, sizeof expected));
}
static void probes(void) {
    uint8_t inert;
    fips_rekey_schedule s = fips_rekey_schedule_probes(SEC(1), 5, SEC(31), SEC(4), &inert);
    uint64_t expected[] = {1000, 3000, 7000, 11000, 15000, 19000, 23000, 27000}, last;
    assert(s.len == 8 && !memcmp(s.at, expected, sizeof expected));
    assert(s.keepalive == 0xe8 && inert == 1);
    assert(fips_rekey_schedule_last_send(&s, &last) && last == 15000);
    s = fips_rekey_schedule_probes(SEC(1), 5, SEC(31), (fips_duration){0, 500000000}, &inert);
    assert(s.len == 16 && inert == 46);
}
static void late_ladder(void) {
    fips_rekey_params p = fips_rekey_params_fsp();
    fips_rekey_ladder l = fips_rekey_ladder_new(fips_test_instant(0), p.probes);
    fips_rekey_taken got;
    assert(rk_cmp(fips_rekey_ladder_next_at(&l), fips_test_instant(1000)) == 0);
    assert(!fips_rekey_ladder_take(&l, fips_test_instant(999), &got));
    assert(fips_rekey_ladder_take(&l, fips_test_instant(12000), &got) && got.send && !got.last &&
           got.latest == 11000);
    assert(rk_cmp(fips_rekey_ladder_next_at(&l), fips_test_instant(15000)) == 0);
    assert(fips_rekey_ladder_take(&l, fips_test_instant(20000), &got) && got.send);
    assert(fips_rekey_ladder_take(&l, fips_test_instant(28000), &got) && !got.send && got.last);
    assert(!fips_rekey_ladder_next_at(&l).ops);
}
static fips_rekey_digest digest(unsigned n) {
    fips_rekey_digest x = {{0}};
    for (unsigned i = 0; i < 4; ++i)
        x.bytes[i] = (uint8_t)(n >> (i * 8));
    return x;
}
static void record_bound(void) {
    fips_rekey_record r;
    fips_rekey_record_setup(&r, digest(0), digest(1), NULL);
    for (unsigned i = 2; i < 770; ++i) {
        fips_rekey_record_push(&r, digest(i));
        assert(fips_rekey_record_len(&r) <= 257);
    }
    assert(fips_rekey_record_len(&r) == 257 && fips_rekey_record_holds(&r, digest(1)) &&
           !fips_rekey_record_holds(&r, digest(2)) && fips_rekey_record_holds(&r, digest(769)));
}
static void record_absorb(void) {
    fips_rekey_record old, r;
    fips_rekey_wire msg = {.len = 1, .bytes = {9}};
    fips_rekey_record_setup(&old, digest(10), digest(11), NULL);
    fips_rekey_record_push(&old, digest(12));
    fips_rekey_record_setup(&r, digest(20), digest(21), &msg);
    fips_rekey_record_push(&r, digest(22));
    fips_rekey_record_absorb(&r, &old);
    unsigned expected[] = {11, 12, 21, 22};
    for (unsigned i = 0; i < 4; ++i)
        assert(fips_rekey_record_holds(&r, digest(expected[i])));
    assert(r.setup.bytes[0] == 20 && r.has_answer && r.answer.bytes[0] == 9 &&
           sodium_is_zero((const uint8_t *)&old, sizeof old));
    for (unsigned i = 0; i < 256; ++i)
        fips_rekey_record_push(&old, digest(100 + i));
    fips_rekey_record_absorb(&r, &old);
    assert(fips_rekey_record_len(&r) <= 257);
}
static void record_store(void) {
    fips_rekey_record_entry storage[4];
    fips_rekey_record_store store;
    fips_rekey_record r;
    fips_rekey_record_store_new(&store, storage, 2);
    uint8_t epoch[8] = {1}, other[8] = {2};
    fips_node_addr peer[3] = {{{1}}, {{2}}, {{3}}};
    fips_rekey_record_new(&r);
    fips_rekey_record_store_file(&store, peer[0], epoch, &r);
    assert(!store.count);
    for (unsigned i = 0; i < 4; ++i) {
        fips_rekey_record_new(&r);
        fips_rekey_record_push(&r, digest(i + 1));
        fips_rekey_record_store_file(&store, peer[i == 2 ? 0 : i == 3 ? 2 : i], epoch, &r);
    }
    assert(store.count == 2);
    fips_rekey_record_store_take(&store, &peer[0], other, &r);
    assert(!fips_rekey_record_len(&r));
    fips_rekey_record_store_take(&store, &peer[1], epoch, &r);
    assert(!fips_rekey_record_len(&r));
    fips_rekey_record_store_take(&store, &peer[0], epoch, &r);
    assert(fips_rekey_record_holds(&r, digest(3)) && !fips_rekey_record_holds(&r, digest(1)));
    fips_rekey_record_store_take(&store, &peer[2], epoch, &r);
    assert(fips_rekey_record_holds(&r, digest(4)) && !store.count);
}
static bool one(void *ctx, uint32_t *out) {
    uint32_t *n = ctx;
    if (!*n)
        return false;
    *out = *n;
    *n = 0;
    return true;
}
static void lease_drop(void) {
    size_t before = fips_rekey_leaked();
    uint32_t n = 5, index;
    fips_rekey_index_lease l = {0};
    fips_rekey_index_source src = {one, &n};
    assert(fips_rekey_allocate(src, &l));
    assert(fips_rekey_index_lease_release(&l, &index) && index == 5);
    assert(fips_rekey_leaked() == before);
    n = 6;
    assert(fips_rekey_allocate(src, &l));
    fips_rekey_index_lease_drop(&l);
    assert(fips_rekey_leaked() == before + 1);
    assert(!fips_rekey_allocate(src, &l));
}
static void attribution(void) {
    fips_rekey_reason local[] = {FIPS_REKEY_REASON_IndexExhausted, FIPS_REKEY_REASON_HandshakeWrite,
                                 FIPS_REKEY_REASON_NotRekey, FIPS_REKEY_REASON_StaleTag};
    for (size_t i = 0; i < 4; ++i) {
        assert(fips_rekey_reason_attribution(local[i]) == FIPS_REKEY_ATTRIBUTION_Local);
        assert(!fips_rekey_reason_unauthenticated_reachable(local[i]));
    }
    fips_rekey_reason remote[] = {FIPS_REKEY_REASON_FrameUnauthenticated,
                                  FIPS_REKEY_REASON_Replay,
                                  FIPS_REKEY_REASON_ReplyUnauthenticated,
                                  FIPS_REKEY_REASON_FinalUnauthenticated,
                                  FIPS_REKEY_REASON_SetupMalformed,
                                  FIPS_REKEY_REASON_CandidatesFull,
                                  FIPS_REKEY_REASON_TieBreakKept,
                                  FIPS_REKEY_REASON_KeyMismatch,
                                  FIPS_REKEY_REASON_OffLink};
    for (size_t i = 0; i < 9; ++i)
        assert(fips_rekey_reason_unauthenticated_reachable(remote[i]));
    assert(fips_rekey_reason_attribution(FIPS_REKEY_REASON_KeyMismatch) !=
           FIPS_REKEY_ATTRIBUTION_Peer);
    assert(fips_rekey_reason_attribution(FIPS_REKEY_REASON_OffLink) != FIPS_REKEY_ATTRIBUTION_Peer);
}
static void agreement(void) {
    fips_node_addr x = {{1}}, y = {{2}};
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtAnswer, FIPS_REKEY_OWN_Unanswered, &x, &y,
                              false) == FIPS_REKEY_KeepOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtAnswer, FIPS_REKEY_OWN_Unanswered, &y, &x,
                              false) == FIPS_REKEY_YieldOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtAnswer, FIPS_REKEY_OWN_Completed, &y, &x,
                              false) == FIPS_REKEY_DiscardCompeting);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtFinal, FIPS_REKEY_OWN_Unanswered, &x, &y,
                              false) == FIPS_REKEY_YieldOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtFinal, FIPS_REKEY_OWN_Completed, &x, &y, false) ==
           FIPS_REKEY_KeepOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtFinal, FIPS_REKEY_OWN_Completed, &y, &x, false) ==
           FIPS_REKEY_YieldOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtFinal, FIPS_REKEY_OWN_Completed, &x, &y, true) ==
           FIPS_REKEY_YieldOwn);
    assert(fips_rekey_resolve(FIPS_REKEY_COMMIT_AtFinal, FIPS_REKEY_OWN_None, &x, &y, true) ==
           FIPS_REKEY_Accept);
}
static void candidate_eviction(void) {
    fips_rekey_candidates cs = {0};
    for (unsigned i = 0; i < 4; ++i) {
        fips_rekey_candidate e = {.born = fips_test_instant(i == 3 ? 1000 : 0)};
        e.carrier.bytes[0] = (uint8_t)(i < 2 ? i + 1 : 3);
        rk_cand_add(&cs, &e);
    }
    fips_node_addr four = {{4}};
    assert(!rk_cand_room(&cs, &rk_test_xk, four));
    assert(rk_cand_victim(&cs, &rk_test_xk, four, fips_test_instant(4000), SEC(5)) == SIZE_MAX);
    assert(rk_cand_victim(&cs, &rk_test_xk, four, fips_test_instant(10000), SEC(5)) == 2);
    cs.list[3].carrier = four;
    assert(rk_cand_victim(&cs, &rk_test_xk, (fips_node_addr){{5}}, fips_test_instant(10000),
                          SEC(5)) == SIZE_MAX);
    rk_cand_clear(&cs);
    assert(!cs.count && cs.next == 4 && sodium_is_zero((const uint8_t *)cs.list, sizeof cs.list));
}
static void vector_defaults(void) {
    json_error_t error;
    json_t *root = json_load_file(FIPS_REKEY_VECTOR, 0, &error);
    assert(root);
    const char *actual =
        json_string_value(json_object_get(json_object_get(root, "cases"), "params_debug"));
    assert(actual);
    fips_rekey_config x = fips_rekey_config_fsp();
    char expected[2048];
    size_t n = 0;
#define APP(...)                                                                                   \
    do {                                                                                           \
        int written = snprintf(expected + n, sizeof expected - n, __VA_ARGS__);                    \
        assert(written >= 0 && (size_t)written < sizeof expected - n);                             \
        n += (size_t)written;                                                                      \
    } while (0)
#define D(field)                                                                                   \
    do {                                                                                           \
        assert(!x.field.nanos);                                                                    \
        APP(", " #field ": %llus", (unsigned long long)x.field.secs);                              \
    } while (0)
    APP("Config { after: %llus, jitter: %u, messages: %llu", (unsigned long long)x.after.secs,
        x.jitter, (unsigned long long)x.messages);
    D(handshake);
    D(check);
    D(drain);
    D(retention);
    D(hold);
    D(revert);
    D(dampening);
    D(ttl);
    D(young);
    D(link_age);
    D(session_age);
    assert(!x.floor_link.secs && !x.floor_link.nanos && !x.floor_session.secs &&
           !x.floor_session.nanos);
    APP(", floor_link: 0ns, floor_session: 0ns");
    D(floor_cutover);
    D(silence);
    APP(", resends: %u, soft: %llu, hard: %llu", x.resends, (unsigned long long)x.soft,
        (unsigned long long)x.hard);
    D(delay);
    APP(", arrival: %s }", x.arrival ? "true" : "false");
    assert(!strcmp(actual, expected));
    json_decref(root);
    x = fips_rekey_config_fmp();
    assert(x.floor_link.secs == 31 && x.floor_session.secs == 31 && !x.floor_cutover.secs);
#undef APP
#undef D
}
void rekey_params_tests(void) {
    RUN(after_jitter);
    RUN(zero_messages);
    RUN(after_link_floor);
    RUN(invalid_settings);
    RUN(derived_defaults);
    RUN(doubling);
    RUN(probes);
    RUN(late_ladder);
    RUN(record_bound);
    RUN(record_absorb);
    RUN(record_store);
    RUN(lease_drop);
    RUN(attribution);
    RUN(agreement);
    RUN(candidate_eviction);
    RUN(vector_defaults);
}
