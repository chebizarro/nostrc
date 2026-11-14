#include <fips/proto/fmp/mod.h>
#include <fips/noise/aead.h>
#include "fakes/host.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); return 1; } } while (0)
static fips_node_addr addr(uint8_t n) { fips_node_addr a = {{0}}; a.bytes[0] = n; return a; }
static fips_fmp_epoch epoch(uint8_t n) { fips_fmp_epoch e = {.present = true}; memset(e.bytes, n, 8); return e; }
static fips_fmp_leg_stamp stamp(uint64_t n) { return (fips_fmp_leg_stamp){n, fips_test_instant(n)}; }
static fips_fmp_ephemeral_digest setup(uint32_t n)
{
    uint8_t bytes[33] = {0};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(n >> (8 * i));
    fips_fmp_ephemeral_digest d;
    fips_fmp_ephemeral_digest_of_noise_msg1(bytes, sizeof(bytes), &d);
    return d;
}
static fips_fmp_wire_outcome outcome(uint8_t e)
{
    fips_fmp_wire_outcome w = {.remote_epoch = epoch(e)};
    w.peer_identity.node_addr = addr(42);
    return w;
}
static fips_fmp_connection_state inbound(uint64_t ms)
{
    fips_fmp_connection_state s;
    fips_fmp_connection_inbound(&s, (fips_link_id){1}, stamp(ms));
    return s;
}
/* Each upstream #[test] has a same-named case below. Short local fixtures keep
 * all mutable state caller-owned and clock readings synthetic. */
#define ACTIONS fips_fmp_conn_action storage[8] = {0}; fips_fmp_action_list a = {storage, 0, 8}
#define SILENT fips_fmp_silent_record storage[4]; fips_fmp_silent_sessions s; \
    fips_fmp_silent_sessions_init(&s, storage, 4, fips_test_instant(0)); \
    fips_node_addr peer = addr(1); bool started = false; fips_fmp_refusal_start r = {0}
#define END(n, ms) do { fips_fmp_ephemeral_digest d = setup(n); \
    CHECK(fips_fmp_silent_sessions_ended(&s, peer, epoch(7), &d, fips_test_instant(ms), &started, &r) == FIPS_OK); } while (0)
#define THREE do { END(0, 0); END(1, 1000); END(2, 2000); CHECK(started); } while (0)
#define COUNT_IS(n) do { uint32_t count; CHECK(fips_fmp_silent_sessions_count(&s, &peer, &count) && count == (n)); } while (0)
#define MSG1 uint8_t noise[106] = {0}, bytes[115]; fips_writer w; \
    fips_writer_init(&w, bytes, sizeof(bytes)); \
    CHECK(fips_fmp_build_msg1((fips_session_index){0xabcdef01}, noise, sizeof(noise), &w, NULL) == FIPS_OK)
#define MSG2 uint8_t noise[57] = {0}, bytes[70]; fips_writer w; \
    fips_writer_init(&w, bytes, sizeof(bytes)); \
    CHECK(fips_fmp_build_msg2((fips_session_index){0x11223344}, (fips_session_index){0x55667788}, noise, sizeof(noise), &w, NULL) == FIPS_OK)

static int empty_stale_set_yields_no_actions(void) { ACTIONS; CHECK(fips_fmp_poll_timeouts(NULL, 0, &a) == FIPS_OK && a.len == 0); return 0; }
static int inbound_stale_connection_is_torn_down_without_retry(void)
{
    ACTIONS; fips_fmp_conn_snapshot s = {.link = {7}, .has_retry_addr = true, .retry_addr = addr(34)};
    CHECK(fips_fmp_poll_timeouts(&s, 1, &a) == FIPS_OK && a.len == 1);
    CHECK(storage[0].kind == FIPS_FMP_Teardown && storage[0].link.value == 7); return 0;
}
static int outbound_stale_with_identity_schedules_retry_then_tears_down(void)
{
    ACTIONS; fips_fmp_conn_snapshot s = {.link = {9}, .is_outbound = true, .has_retry_addr = true, .retry_addr = addr(51)};
    CHECK(fips_fmp_poll_timeouts(&s, 1, &a) == FIPS_OK && a.len == 2);
    CHECK(storage[0].kind == FIPS_FMP_ScheduleRetry && storage[0].peer.bytes[0] == 51);
    CHECK(storage[1].kind == FIPS_FMP_Teardown && storage[1].link.value == 9); return 0;
}
static int outbound_stale_without_identity_only_tears_down(void)
{
    ACTIONS; fips_fmp_conn_snapshot s = {.link = {11}, .is_outbound = true};
    CHECK(fips_fmp_poll_timeouts(&s, 1, &a) == FIPS_OK && a.len == 1);
    CHECK(storage[0].kind == FIPS_FMP_Teardown && storage[0].link.value == 11); return 0;
}
static int no_resend_candidates_yields_no_actions(void)
{ ACTIONS; CHECK(fips_fmp_poll_resends(NULL, 0, fips_test_instant(1000), 500, 2, &a) == FIPS_OK && a.len == 0); return 0; }
static int resend_emits_bytes_and_backoff_schedule(void)
{
    ACTIONS; const uint8_t bytes[] = {0xde, 0xad, 0xbe, 0xef};
    fips_fmp_conn_snapshot s = {.link = {5}, .msg1 = {bytes, sizeof(bytes)}};
    CHECK(fips_fmp_poll_resends(&s, 1, fips_test_instant(1000), 500, 2, &a) == FIPS_OK && a.len == 1);
    CHECK(storage[0].kind == FIPS_FMP_ResendMsg1 && storage[0].link.value == 5);
    CHECK(storage[0].bytes.data == bytes && storage[0].bytes.len == 4 && storage[0].next_resend_at.opaque[0] == 2000);
    CHECK(s.resend_count == 0); return 0;
}
static int resend_backoff_exponent_uses_count_plus_one(void)
{
    ACTIONS; const uint8_t b = 1; fips_fmp_conn_snapshot s = {.link = {6}, .resend_count = 2, .msg1 = {&b, 1}};
    CHECK(fips_fmp_poll_resends(&s, 1, fips_test_instant(0), 100, 2, &a) == FIPS_OK);
    CHECK(storage[0].next_resend_at.opaque[0] == 800); return 0;
}
static int establish_inbound_net_new_promotes(void)
{
    fips_fmp_establish_snapshot s = {0}; fips_fmp_wire_outcome w = outcome(1);
    CHECK(fips_fmp_establish_inbound(&s, &w).kind == FIPS_FMP_IN_Promote); return 0;
}
static int establish_inbound_at_cap_net_new_rejects(void)
{
    fips_fmp_establish_snapshot s = {.at_max_peers = true}; fips_fmp_wire_outcome w = outcome(1);
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Reject && d.reason == FIPS_FMP_AtMaxPeers); return 0;
}
static int establish_inbound_at_cap_existing_peer_not_capped(void)
{
    fips_fmp_establish_snapshot s = {.at_max_peers = true, .has_existing_peer = true, .existing_peer_epoch = epoch(9)};
    fips_fmp_wire_outcome w = outcome(9);
    CHECK(fips_fmp_establish_inbound(&s, &w).kind == FIPS_FMP_IN_Rekey); return 0;
}
static int a_held_peers_same_epoch_msg1_goes_to_its_link_machine_until_that_machine_asks_for_a_replacement(void)
{
    fips_fmp_establish_snapshot s = {.has_existing_peer = true, .existing_peer_epoch = epoch(9)};
    fips_fmp_wire_outcome w = outcome(9); fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Rekey && !memcmp(&d.peer, &w.peer_identity.node_addr, sizeof(d.peer)));
    s.replace = true; d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_ReplaceThenPromote && !memcmp(&d.peer, &w.peer_identity.node_addr, sizeof(d.peer))); return 0;
}
static int establish_inbound_epoch_mismatch_restarts(void)
{
    fips_fmp_establish_snapshot s = {.has_existing_peer = true, .existing_peer_epoch = epoch(1)};
    fips_fmp_wire_outcome w = outcome(2); fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_RestartThenPromote && d.peer.bytes[0] == 42); return 0;
}
static int establish_outbound_no_existing_peer_promotes(void)
{
    fips_fmp_outbound_snapshot s = {.our_outbound_wins = true};
    CHECK(fips_fmp_establish_outbound(&s, (fips_fmp_epoch){0}) == FIPS_FMP_OUT_Promote); return 0;
}
static int establish_outbound_cross_connection_win_swaps(void)
{
    fips_fmp_outbound_snapshot s = {.has_existing_peer = true, .our_outbound_wins = true};
    CHECK(fips_fmp_establish_outbound(&s, (fips_fmp_epoch){0}) == FIPS_FMP_OUT_CrossConnectionSwap); return 0;
}
static int establish_outbound_cross_connection_loss_keeps(void)
{
    fips_fmp_outbound_snapshot s = {.has_existing_peer = true};
    CHECK(fips_fmp_establish_outbound(&s, (fips_fmp_epoch){0}) == FIPS_FMP_OUT_CrossConnectionKeep); return 0;
}
static int test_cross_connection_smaller_node_wins_outbound(void)
{
    fips_node_addr a = addr(1), b = addr(2);
    CHECK(fips_fmp_cross_connection_winner(&a, &b, true)); CHECK(!fips_fmp_cross_connection_winner(&a, &b, false));
    CHECK(!fips_fmp_cross_connection_winner(&b, &a, true)); CHECK(fips_fmp_cross_connection_winner(&b, &a, false)); return 0;
}
static int test_cross_connection_symmetric(void)
{
    fips_node_addr a = addr(1), b = addr(2);
    bool ao = fips_fmp_cross_connection_winner(&a, &b, true), bi = fips_fmp_cross_connection_winner(&b, &a, false);
    bool ai = fips_fmp_cross_connection_winner(&a, &b, false), bo = fips_fmp_cross_connection_winner(&b, &a, true);
    CHECK(ao == bi && ai == bo && ao != ai); return 0;
}

static int outbound_initializes_pure_fields(void)
{
    fips_fmp_connection_state s; fips_peer_identity id = {.node_addr = addr(4)};
    fips_fmp_connection_outbound(&s, (fips_link_id){1}, id, stamp(1000));
    CHECK(s.direction == FIPS_LINK_OUTBOUND && s.has_expected_identity && s.link_id.value == 1);
    CHECK(s.started_at == 1000 && s.last_activity == 1000 && s.active_at.opaque[0] == 1000);
    CHECK(!s.has_transport_id && !s.has_source_addr && !s.remote_epoch.present && s.resend_count == 0); return 0;
}
static int inbound_initializes_pure_fields(void)
{ fips_fmp_connection_state s = inbound(2000); CHECK(s.direction == FIPS_LINK_INBOUND && !s.has_expected_identity && s.started_at == 2000); return 0; }
static int inbound_with_transport_sets_transport_and_addr(void)
{
    fips_fmp_connection_state s; fips_transport_addr a;
    CHECK(fips_transport_addr_from_string("192.0.2.1:5000", 14, &a) == FIPS_OK);
    fips_fmp_connection_inbound_with_transport(&s, (fips_link_id){3}, (fips_transport_id){7}, a, stamp(3000));
    CHECK(s.direction == FIPS_LINK_INBOUND && s.has_transport_id && s.transport_id.value == 7 && s.has_source_addr);
    CHECK(fips_transport_addr_equal(&s.source_addr, &a)); return 0;
}
static int index_setters_round_trip(void)
{
    fips_fmp_connection_state s = inbound(0); fips_session_index prev = {123};
    CHECK(!s.has_our_index && !s.has_their_index);
    CHECK(!fips_fmp_connection_set_our_index(&s, (fips_session_index){0x1111}, &prev) && prev.value == 123);
    s.their_index.value = 0x2222; s.has_their_index = true;
    CHECK(s.our_index.value == 0x1111 && s.their_index.value == 0x2222);
    CHECK(fips_fmp_connection_take_our_index(&s, &prev) && prev.value == 0x1111 && !s.has_our_index); return 0;
}
static int transport_and_source_setters_round_trip(void)
{
    fips_fmp_connection_state s = inbound(0); s.transport_id.value = 9; s.has_transport_id = true;
    CHECK(fips_transport_addr_from_string("peer", 4, &s.source_addr) == FIPS_OK); s.has_source_addr = true;
    CHECK(s.has_transport_id && s.transport_id.value == 9 && s.has_source_addr && s.source_addr.len == 4);
    CHECK(!memcmp(s.source_addr.bytes, "peer", 4)); return 0;
}
static int identity_and_epoch_setters(void)
{
    fips_fmp_connection_state s = inbound(0); CHECK(!s.has_expected_identity && !s.remote_epoch.present);
    s.expected_identity.node_addr = addr(4); s.has_expected_identity = true; s.remote_epoch = epoch(9);
    CHECK(s.has_expected_identity && s.expected_identity.node_addr.bytes[0] == 4 && s.remote_epoch.present && s.remote_epoch.bytes[0] == 9); return 0;
}
static int resend_bookkeeping(void)
{
    fips_fmp_connection_state s = inbound(0); const uint8_t one[] = {1,2,3}, two[] = {4,5}, three[] = {6,7,8};
    CHECK(!s.has_handshake_msg1 && !s.has_handshake_msg2);
    CHECK(fips_fmp_connection_set_handshake_msg1(&s, one, 3) == FIPS_OK && s.resend_count == 0);
    CHECK(s.has_handshake_msg1 && s.handshake_msg1_len == 3 && !memcmp(s.handshake_msg1, one, 3));
    fips_fmp_connection_record_resend(&s); CHECK(s.resend_count == 1);
    fips_fmp_connection_record_resend(&s); CHECK(s.resend_count == 2);
    CHECK(fips_fmp_connection_set_handshake_msg1(&s, two, 2) == FIPS_OK && s.resend_count == 0);
    CHECK(s.handshake_msg1_len == 2 && !memcmp(s.handshake_msg1, two, 2));
    CHECK(fips_fmp_connection_set_handshake_msg2(&s, three, 3) == FIPS_OK && s.has_handshake_msg2);
    CHECK(s.handshake_msg2_len == 3 && !memcmp(s.handshake_msg2, three, 3)); return 0;
}
static int timing_and_touch(void)
{
    fips_fmp_connection_state s = inbound(1000);
    CHECK(fips_fmp_connection_duration(&s, 1500) == 500 && fips_fmp_connection_idle_time(&s, fips_test_instant(1500)) == 500);
    CHECK(!fips_fmp_connection_is_timed_out(&s, fips_test_instant(1500), 1000));
    CHECK(fips_fmp_connection_is_timed_out(&s, fips_test_instant(2500), 1000));
    fips_fmp_connection_touch(&s, stamp(2000));
    CHECK(s.last_activity == 2000 && fips_fmp_connection_idle_time(&s, fips_test_instant(2500)) == 500);
    CHECK(fips_fmp_connection_duration(&s, 2500) == 1500 && !fips_fmp_connection_is_timed_out(&s, fips_test_instant(2500), 1000)); return 0;
}
static int a_legs_timeout_reads_its_interval_stamp_and_its_shown_times_the_wall_stamp(void)
{
    fips_fmp_connection_state s; fips_fmp_leg_stamp at = {UINT64_C(1700000000000), fips_test_instant(1000)};
    fips_fmp_connection_inbound(&s, (fips_link_id){1}, at);
    CHECK(s.started_at == at.wall_ms && s.last_activity == at.wall_ms && s.active_at.opaque[0] == 1000);
    CHECK(fips_fmp_connection_idle_time(&s, fips_test_instant(1500)) == 500);
    CHECK(!fips_fmp_connection_is_timed_out(&s, fips_test_instant(1500), 1000));
    CHECK(fips_fmp_connection_is_timed_out(&s, fips_test_instant(2500), 1000));
    fips_fmp_connection_touch(&s, (fips_fmp_leg_stamp){UINT64_C(1600000000000), fips_test_instant(2000)});
    CHECK(s.started_at == at.wall_ms && s.last_activity == UINT64_C(1600000000000) && s.active_at.opaque[0] == 2000);
    CHECK(!fips_fmp_connection_is_timed_out(&s, fips_test_instant(2500), 1000)); return 0;
}

static int two_silent_sessions_refuse_nothing_and_the_third_refuses_for_the_base(void)
{
    SILENT; END(0, 0); END(1, 1000); CHECK(!started);
    fips_fmp_active_backoff b; CHECK(!fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(2000), &b));
    END(2, 2000); CHECK(started && r.backoff.silent == 3 && r.backoff.remaining_ms == 30000);
    CHECK(fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(2000), &b) && b.remaining_ms == 30000); return 0;
}
static int each_further_silent_session_doubles_the_refusal_up_to_the_ten_minute_cap(void)
{
    SILENT; THREE; const uint64_t lengths[] = {60000,120000,240000,480000,600000,600000,600000};
    for (unsigned i = 0; i < 7; ++i) { END(i + 3, (i + 3) * 1000); CHECK(started && r.backoff.remaining_ms == lengths[i]); } return 0;
}
static int the_refusal_ends_when_its_time_has_run(void)
{
    SILENT; THREE; fips_fmp_active_backoff b;
    CHECK(fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(31999), &b) && b.remaining_ms == 1);
    CHECK(!fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(32000), &b)); return 0;
}
static int an_authenticated_frame_drops_the_record(void)
{
    SILENT; THREE; fips_fmp_silent_sessions_heard(&s, &peer); uint32_t n; fips_fmp_active_backoff b;
    CHECK(!fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(2000), &b));
    CHECK(!fips_fmp_silent_sessions_count(&s, &peer, &n)); END(10, 3000); END(11, 4000); CHECK(!started); return 0;
}
static int a_silent_session_at_another_epoch_restarts_the_count_at_one(void)
{
    SILENT; THREE; fips_fmp_ephemeral_digest d = setup(3); fips_fmp_active_backoff b;
    CHECK(fips_fmp_silent_sessions_ended(&s, peer, epoch(8), &d, fips_test_instant(3000), &started, &r) == FIPS_OK && !started);
    COUNT_IS(1); CHECK(!fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(3000), &b)); return 0;
}
static int a_record_without_an_epoch_adopts_the_first_epoch_it_sees(void)
{
    SILENT;
    for (unsigned i = 0; i < 3; ++i) {
        fips_fmp_epoch e = i == 1 ? epoch(7) : (fips_fmp_epoch){0}; fips_fmp_ephemeral_digest d = setup(i);
        CHECK(fips_fmp_silent_sessions_ended(&s, peer, e, &d, fips_test_instant(i * 1000), &started, &r) == FIPS_OK);
    }
    CHECK(started && r.backoff.epoch.present && r.backoff.epoch.bytes[0] == 7 && r.backoff.silent == 3); return 0;
}
static int a_session_promoted_from_an_already_counted_msg1_is_not_counted_again(void)
{
    SILENT; for (unsigned t = 0; t < 5; ++t) { END(0, t * 1000); CHECK(!started); }
    COUNT_IS(1); fips_fmp_active_backoff b; CHECK(!fips_fmp_silent_sessions_refusing(&s, &peer, fips_test_instant(5000), &b)); return 0;
}
static int sessions_promoted_from_msg1s_carrying_one_ephemeral_count_once_whatever_follows_it(void)
{
    SILENT; uint8_t bytes[49] = {0};
    for (unsigned t = 0; t < 3; ++t) {
        memset(bytes + 33, (int)t, 16); fips_fmp_ephemeral_digest d;
        CHECK(fips_fmp_ephemeral_digest_of_noise_msg1(bytes, sizeof(bytes), &d));
        CHECK(fips_fmp_silent_sessions_ended(&s, peer, epoch(7), &d, fips_test_instant(t * 1000), &started, &r) == FIPS_OK && !started);
    }
    COUNT_IS(1); return 0;
}
static int a_msg1_too_short_to_carry_an_ephemeral_has_no_ephemeral_digest(void)
{ const uint8_t b[32] = {0}; fips_fmp_ephemeral_digest d; CHECK(!fips_fmp_ephemeral_digest_of_noise_msg1(b, sizeof(b), &d)); return 0; }
static int sessions_this_node_dialled_count_without_a_setup_digest(void)
{
    SILENT; for (unsigned i = 0; i < 3; ++i)
        CHECK(fips_fmp_silent_sessions_ended(&s, peer, epoch(7), NULL, fips_test_instant(i * 1000), &started, &r) == FIPS_OK);
    CHECK(started); return 0;
}
static int a_new_refusal_reports_how_many_msg1s_the_previous_refusal_refused(void)
{
    SILENT; THREE; CHECK(r.prior_refused == 0);
    uint32_t n; for (unsigned i = 1; i <= 8; ++i) CHECK(fips_fmp_silent_sessions_note_refused(&s, &peer, &n) && n == i);
    END(3, 33000); CHECK(started && r.prior_refused == 8);
    CHECK(fips_fmp_silent_sessions_note_refused(&s, &peer, &n) && n == 1); return 0;
}
static int clearing_a_record_returns_its_refused_count(void)
{
    SILENT; uint32_t n; CHECK(!fips_fmp_silent_sessions_note_refused(&s, &peer, &n)); THREE;
    for (unsigned i = 0; i < 5; ++i) CHECK(fips_fmp_silent_sessions_note_refused(&s, &peer, &n));
    CHECK(fips_fmp_silent_sessions_heard(&s, &peer) == 5 && fips_fmp_silent_sessions_heard(&s, &peer) == 0); return 0;
}
static int a_record_is_pruned_a_cap_after_its_refusal_ends_and_kept_before(void)
{
    SILENT; THREE; fips_node_addr other = addr(2); uint32_t n;
    CHECK(fips_fmp_silent_sessions_ended(&s, other, epoch(7), NULL, fips_test_instant(72000), &started, &r) == FIPS_OK);
    COUNT_IS(3);
    CHECK(fips_fmp_silent_sessions_ended(&s, other, epoch(7), NULL, fips_test_instant(632000), &started, &r) == FIPS_OK);
    CHECK(!fips_fmp_silent_sessions_count(&s, &peer, &n) && s.len == 1); return 0;
}
static int a_record_below_the_limit_is_pruned_a_cap_after_its_last_silent_session(void)
{
    SILENT; END(0, 1000); fips_node_addr other = addr(2); uint32_t n;
    CHECK(fips_fmp_silent_sessions_ended(&s, other, epoch(7), NULL, fips_test_instant(600999), &started, &r) == FIPS_OK);
    COUNT_IS(1);
    CHECK(fips_fmp_silent_sessions_ended(&s, other, epoch(7), NULL, fips_test_instant(601000), &started, &r) == FIPS_OK);
    CHECK(!fips_fmp_silent_sessions_count(&s, &peer, &n)); return 0;
}
static fips_fmp_establish_snapshot refusing_snapshot(bool held, bool replace)
{
    return (fips_fmp_establish_snapshot){.has_existing_peer = held, .replace = replace,
        .existing_peer_epoch = epoch(7), .has_silent_backoff = true, .silent_backoff = {epoch(7), 3, 30000}};
}
static int a_same_epoch_msg1_from_an_identity_with_no_peer_is_refused_during_the_back_off(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(false, false); fips_fmp_wire_outcome w = outcome(7);
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Reject && d.reason == FIPS_FMP_SilentBackoff); return 0;
}
static int a_same_epoch_msg1_that_would_replace_a_young_session_is_refused_during_the_back_off(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(true, true); fips_fmp_wire_outcome w = outcome(7);
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Reject && d.reason == FIPS_FMP_SilentBackoff); return 0;
}
static int a_msg1_with_no_epoch_counts_as_the_same_epoch(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(false, false); fips_fmp_wire_outcome w = {0};
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Reject && d.reason == FIPS_FMP_SilentBackoff); return 0;
}
static int a_new_epoch_msg1_is_promoted_during_the_back_off(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(false, false); fips_fmp_wire_outcome w = outcome(8);
    CHECK(fips_fmp_establish_inbound(&s, &w).kind == FIPS_FMP_IN_Promote);
    s = refusing_snapshot(true, true); CHECK(fips_fmp_establish_inbound(&s, &w).kind == FIPS_FMP_IN_RestartThenPromote); return 0;
}
static int a_held_peers_same_epoch_msg1_goes_to_its_link_machine_whatever_the_back_off(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(true, false); fips_fmp_wire_outcome w = outcome(7);
    CHECK(fips_fmp_establish_inbound(&s, &w).kind == FIPS_FMP_IN_Rekey); return 0;
}
static int the_max_peers_gate_still_comes_first(void)
{
    fips_fmp_establish_snapshot s = refusing_snapshot(false, false); s.at_max_peers = true; fips_fmp_wire_outcome w = outcome(7);
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&s, &w);
    CHECK(d.kind == FIPS_FMP_IN_Reject && d.reason == FIPS_FMP_AtMaxPeers); return 0;
}

static int test_handshake_message_type_roundtrip(void)
{
    for (unsigned i = 1; i <= 2; ++i) { fips_fmp_handshake_message_type t; CHECK(fips_fmp_handshake_message_type_from_byte((uint8_t)i, &t) && (unsigned)t == i); }
    CHECK(!strcmp(fips_fmp_handshake_message_type_name(FIPS_FMP_NoiseIKMsg1), "NoiseIKMsg1"));
    CHECK(!strcmp(fips_fmp_handshake_message_type_name(FIPS_FMP_NoiseIKMsg2), "NoiseIKMsg2")); return 0;
}
static int test_handshake_message_type_invalid(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        fips_fmp_handshake_message_type t = FIPS_FMP_NoiseIKMsg1;
        CHECK(fips_fmp_handshake_message_type_from_byte((uint8_t)i, &t) == (i == 1 || i == 2));
        if (i != 1 && i != 2) CHECK(t == FIPS_FMP_NoiseIKMsg1);
    } return 0;
}
static int test_handshake_message_type_is_handshake(void)
{ CHECK(fips_fmp_is_handshake(1) && fips_fmp_is_handshake(2) && !fips_fmp_is_handshake(0) && !fips_fmp_is_handshake(16)); return 0; }
static int test_disconnect_reason_roundtrip(void)
{
    const uint8_t reasons[] = {0,1,2,3,4,5,6,7,255};
    for (size_t i = 0; i < sizeof(reasons); ++i) {
        fips_fmp_disconnect_reason r; CHECK(fips_fmp_disconnect_reason_from_byte(reasons[i], &r) && (unsigned)r == reasons[i]);
        CHECK(strcmp(fips_fmp_disconnect_reason_name(r), "Unknown"));
    } return 0;
}
static int test_disconnect_reason_unknown_byte(void)
{
    for (unsigned i = 8; i < 255; ++i) { fips_fmp_disconnect_reason r = FIPS_FMP_Shutdown; CHECK(!fips_fmp_disconnect_reason_from_byte((uint8_t)i, &r) && r == FIPS_FMP_Shutdown); } return 0;
}
static int test_disconnect_encode_decode(void)
{
    fips_fmp_disconnect d = {FIPS_FMP_Shutdown}, out; uint8_t b[2]; fips_writer w; fips_reader r;
    fips_writer_init(&w, b, sizeof(b)); CHECK(fips_fmp_disconnect_encode(&d, &w, NULL) == FIPS_OK && w.pos == 2 && b[0] == 0x50 && b[1] == 0);
    fips_reader_init(&r, b + 1, 1); CHECK(fips_fmp_disconnect_decode(&r, &out, NULL) == FIPS_OK && out.reason == d.reason); return 0;
}
static int test_disconnect_all_reasons(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        fips_fmp_disconnect d, out; if (!fips_fmp_disconnect_reason_from_byte((uint8_t)i, &d.reason)) continue;
        uint8_t b[2]; fips_writer w; fips_reader r; fips_writer_init(&w, b, sizeof(b));
        CHECK(fips_fmp_disconnect_encode(&d, &w, NULL) == FIPS_OK);
        fips_reader_init(&r, b + 1, 1); CHECK(fips_fmp_disconnect_decode(&r, &out, NULL) == FIPS_OK && out.reason == d.reason);
    } return 0;
}
static int test_disconnect_decode_empty_payload(void)
{
    fips_reader r; fips_fmp_disconnect d = {FIPS_FMP_Restart}; fips_proto_error e;
    fips_reader_init(&r, NULL, 0); CHECK(fips_fmp_disconnect_decode(&r, &d, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.expected == 1 && e.as.message_too_short.got == 0 && r.pos == 0 && d.reason == FIPS_FMP_Restart); return 0;
}
static int test_disconnect_decode_unknown_reason(void)
{
    const uint8_t b = 0x80; fips_reader r; fips_fmp_disconnect d; fips_reader_init(&r, &b, 1);
    CHECK(fips_fmp_disconnect_decode(&r, &d, NULL) == FIPS_OK && d.reason == FIPS_FMP_Other); return 0;
}
static int the_frame_overhead_is_the_header_the_timestamp_and_the_tag(void)
{
    CHECK(FIPS_FRAMING_FMP_FRAME_OVERHEAD == 36);
    CHECK(FIPS_FRAMING_FMP_FRAME_OVERHEAD == FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE + FIPS_FRAMING_FMP_INNER_HEADER_SIZE - 1);
    CHECK(FIPS_FRAMING_FMP_FRAME_OVERHEAD == FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + 4 + FIPS_NOISE_TAG_SIZE); return 0;
}
static int test_common_prefix_parse(void)
{
    const uint8_t b[] = {0,4,32,0}; fips_fmp_common_prefix p;
    CHECK(fips_fmp_common_prefix_parse(b, sizeof(b), &p)); CHECK(p.version == 0 && p.phase == 0 && p.flags == 4 && p.payload_len == 32); return 0;
}
static int test_common_prefix_too_short(void)
{ const uint8_t b[3] = {0}; fips_fmp_common_prefix p; CHECK(!fips_fmp_common_prefix_parse(b, sizeof(b), &p)); return 0; }
static int test_encrypted_header_parse(void)
{
    uint8_t h[16], cipher[48], bytes[64]; memset(cipher, 0xaa, sizeof(cipher));
    fips_fmp_build_established_header((fips_session_index){0x12345678}, 42, 0, 32, h);
    fips_writer w; fips_writer_init(&w, bytes, sizeof(bytes));
    CHECK(fips_fmp_build_encrypted(h, cipher, sizeof(cipher), &w, NULL) == FIPS_OK && w.pos == 64 && bytes[0] == 0);
    fips_fmp_encrypted_header p; CHECK(fips_fmp_encrypted_header_parse(bytes, w.pos, &p));
    CHECK(p.receiver_idx.value == 0x12345678 && p.counter == 42 && p.flags == 0 && p.payload_len == 32);
    CHECK(!memcmp(h, p.header_bytes, 16) && !memcmp(bytes + 16, cipher, 48)); return 0;
}
static int test_encrypted_header_too_short(void)
{ uint8_t b[31] = {0}; fips_fmp_encrypted_header p; CHECK(!fips_fmp_encrypted_header_parse(b, sizeof(b), &p)); return 0; }
static int test_encrypted_header_wrong_phase(void)
{ uint8_t b[32] = {1}; fips_fmp_encrypted_header p; CHECK(!fips_fmp_encrypted_header_parse(b, sizeof(b), &p)); return 0; }
static int test_encrypted_header_wrong_version(void)
{ uint8_t b[32] = {16}; fips_fmp_encrypted_header p; CHECK(!fips_fmp_encrypted_header_parse(b, sizeof(b), &p)); return 0; }
static int test_msg1_header_parse(void)
{
    MSG1; fips_fmp_msg1_header h; CHECK(w.pos == 114 && bytes[0] == 1 && fips_fmp_msg1_header_parse(bytes, w.pos, &h));
    CHECK(h.sender_idx.value == 0xabcdef01 && h.noise_msg1_offset == 8 && !memcmp(bytes + h.noise_msg1_offset, noise, sizeof(noise))); return 0;
}
static int test_msg1_header_wrong_size(void)
{ MSG1; fips_fmp_msg1_header h; CHECK(!fips_fmp_msg1_header_parse(bytes, 113, &h) && !fips_fmp_msg1_header_parse(bytes, 115, &h)); return 0; }
static int test_msg1_header_wrong_phase(void)
{ MSG1; bytes[0] = 2; fips_fmp_msg1_header h; CHECK(!fips_fmp_msg1_header_parse(bytes, w.pos, &h)); return 0; }
static int test_msg1_header_nonzero_flags(void)
{ MSG1; bytes[1] = 1; fips_fmp_msg1_header h; CHECK(!fips_fmp_msg1_header_parse(bytes, w.pos, &h)); return 0; }
static int test_msg2_header_parse(void)
{
    MSG2; fips_fmp_msg2_header h; CHECK(w.pos == 69 && bytes[0] == 2 && fips_fmp_msg2_header_parse(bytes, w.pos, &h));
    CHECK(h.sender_idx.value == 0x11223344 && h.receiver_idx.value == 0x55667788 && h.noise_msg2_offset == 12);
    CHECK(!memcmp(bytes + h.noise_msg2_offset, noise, sizeof(noise))); return 0;
}
static int test_msg2_header_wrong_size(void)
{ MSG2; fips_fmp_msg2_header h; CHECK(!fips_fmp_msg2_header_parse(bytes, 68, &h) && !fips_fmp_msg2_header_parse(bytes, 70, &h)); return 0; }
static int test_msg2_header_wrong_phase(void)
{ MSG2; bytes[0] = 0; fips_fmp_msg2_header h; CHECK(!fips_fmp_msg2_header_parse(bytes, w.pos, &h)); return 0; }
static int test_wire_sizes(void)
{
    CHECK(FIPS_FRAMING_FMP_MSG1_WIRE_SIZE == 114 && FIPS_FRAMING_FMP_MSG2_WIRE_SIZE == 69 && FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE == 32);
    CHECK(FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE == 4 && FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE == 16 && FIPS_FRAMING_FMP_INNER_HEADER_SIZE == 5); return 0;
}
static int test_roundtrip_indices(void)
{
    MSG1; fips_writer_init(&w, bytes, sizeof(bytes));
    CHECK(fips_fmp_build_msg1((fips_session_index){0xdeadbeef}, noise, sizeof(noise), &w, NULL) == FIPS_OK);
    fips_fmp_msg1_header h; CHECK(fips_fmp_msg1_header_parse(bytes, w.pos, &h) && h.sender_idx.value == 0xdeadbeef);
    CHECK(bytes[4] == 0xef && bytes[5] == 0xbe && bytes[6] == 0xad && bytes[7] == 0xde); return 0;
}
static int test_inner_header_prepend_strip(void)
{
    const uint8_t b[] = {0x10,0xaa,0xbb}; uint8_t out[7]; fips_writer w; fips_writer_init(&w, out, sizeof(out));
    CHECK(fips_fmp_prepend_inner_header(12345, b, sizeof(b), &w, NULL) == FIPS_OK && w.pos == 7);
    uint32_t ts; const uint8_t *rest; size_t len; CHECK(fips_fmp_strip_inner_header(out, w.pos, &ts, &rest, &len));
    CHECK(ts == 12345 && len == sizeof(b) && !memcmp(rest, b, len)); return 0;
}
static int test_inner_header_too_short(void)
{ uint8_t b[4] = {0}; uint32_t ts; const uint8_t *p; size_t n; CHECK(!fips_fmp_strip_inner_header(b, 4, &ts, &p, &n)); return 0; }
static int test_flags_byte(void)
{
    uint8_t bytes[32] = {0}; fips_fmp_build_established_header((fips_session_index){1}, 0, FIPS_FMP_FLAG_KEY_EPOCH | FIPS_FMP_FLAG_SP, 100, bytes);
    fips_fmp_encrypted_header h; CHECK(bytes[1] == 5 && fips_fmp_encrypted_header_parse(bytes, sizeof(bytes), &h));
    CHECK((h.flags & FIPS_FMP_FLAG_KEY_EPOCH) && !(h.flags & FIPS_FMP_FLAG_CE) && (h.flags & FIPS_FMP_FLAG_SP));
    /* Rust parser is structural; ingress validation separately rejects this mismatch. */
    CHECK(fips_fmp_validate_frame(bytes, sizeof(bytes), NULL) == FIPS_ERR_RANGE); return 0;
}
static int test_payload_len_in_msg1(void)
{ MSG1; fips_fmp_common_prefix p; CHECK(fips_fmp_common_prefix_parse(bytes, w.pos, &p) && p.payload_len == 110); return 0; }
static int test_payload_len_in_msg2(void)
{ MSG2; fips_fmp_common_prefix p; CHECK(fips_fmp_common_prefix_parse(bytes, w.pos, &p) && p.payload_len == 65); return 0; }
static int expected_payload_len_for_an_established_frame_excludes_header_and_tag(void)
{
    uint8_t h[16], c[56] = {0}, bytes[72]; fips_fmp_build_established_header((fips_session_index){7}, 0, 0, 40, h);
    fips_writer w; fips_writer_init(&w, bytes, sizeof(bytes)); CHECK(fips_fmp_build_encrypted(h, c, sizeof(c), &w, NULL) == FIPS_OK && w.pos == 72);
    uint16_t n; CHECK(fips_fmp_expected_payload_len(0, w.pos, &n) && n == 40); return 0;
}
static int expected_payload_len_matches_what_build_msg1_and_build_msg2_actually_emit(void)
{
    uint8_t noise1[106] = {0}, noise2[57] = {0}, bytes[114]; fips_writer w; fips_fmp_common_prefix p; uint16_t n;
    fips_writer_init(&w, bytes, sizeof(bytes)); CHECK(fips_fmp_build_msg1((fips_session_index){1}, noise1, 106, &w, NULL) == FIPS_OK);
    CHECK(fips_fmp_common_prefix_parse(bytes, w.pos, &p) && fips_fmp_expected_payload_len(1, w.pos, &n) && n == p.payload_len);
    fips_writer_init(&w, bytes, sizeof(bytes)); CHECK(fips_fmp_build_msg2((fips_session_index){1}, (fips_session_index){2}, noise2, 57, &w, NULL) == FIPS_OK);
    CHECK(fips_fmp_common_prefix_parse(bytes, w.pos, &p) && fips_fmp_expected_payload_len(2, w.pos, &n) && n == p.payload_len); return 0;
}
static int test_backoff_exponential(void)
{ for (uint32_t n = 0; n < 5; ++n) CHECK(fips_fmp_backoff_ms(n, 5000, 300000) == (UINT64_C(5000) << n)); return 0; }
static int test_backoff_cap(void) { CHECK(fips_fmp_backoff_ms(20, 5000, 300000) == 300000); return 0; }
static int test_backoff_zero_base(void) { CHECK(fips_fmp_backoff_ms(3, 0, 300000) == 0); return 0; }

static int malformed_at_every_byte_and_pending_short_drop(void)
{
    /* November 12 pending-session regression: authenticated inner plaintext
     * of length 0..4 is dropped, even though its outer frame/tag are complete. */
    const uint8_t inner[5] = {1,2,3,4,0x51};
    for (size_t len = 0; len <= sizeof(inner); ++len) {
        ACTIONS;
        CHECK(fips_fmp_receive_plaintext((fips_link_id){7}, len ? inner : NULL, len, &a) == FIPS_OK && a.len == 1);
        if (len < 5) {
            CHECK(storage[0].kind == FIPS_FMP_DropFrame && storage[0].bytes.len == 0);
            CHECK(storage[0].error.variant == FIPS_PROTO_MessageTooShort);
            CHECK(storage[0].error.as.message_too_short.expected == 5 && storage[0].error.as.message_too_short.got == len);
        } else {
            CHECK(storage[0].kind == FIPS_FMP_DeliverLinkMessage && storage[0].timestamp_ms == 0x04030201);
            CHECK(storage[0].bytes.data == inner + 4 && storage[0].bytes.len == 1);
        }
    }
    uint8_t noise[106] = {0}, valid[3][115] = {{0}}; size_t sizes[] = {32,114,69};
    fips_fmp_build_established_header((fips_session_index){7}, UINT64_MAX, 7, 0, valid[0]);
    fips_writer w; fips_writer_init(&w, valid[1], sizeof(valid[1]));
    CHECK(fips_fmp_build_msg1((fips_session_index){7}, noise, 106, &w, NULL) == FIPS_OK);
    fips_writer_init(&w, valid[2], sizeof(valid[2]));
    CHECK(fips_fmp_build_msg2((fips_session_index){7}, (fips_session_index){9}, noise, 57, &w, NULL) == FIPS_OK);
    for (size_t kind = 0; kind < 3; ++kind) {
        for (size_t n = 0; n < sizes[kind]; ++n) {
            ACTIONS; fips_proto_error e;
            CHECK(fips_fmp_validate_frame(n ? valid[kind] : NULL, n, &e) == FIPS_ERR_RANGE);
            CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.got == n);
            CHECK(e.as.message_too_short.expected == (n < 4 ? 4 : sizes[kind]));
            CHECK(fips_fmp_receive((fips_link_id){3}, n ? valid[kind] : NULL, n, &a) == FIPS_OK && a.len == 1);
            CHECK(storage[0].kind == FIPS_FMP_DropFrame && storage[0].bytes.len == 0);
        }
        for (size_t offset = 0; offset < sizes[kind]; ++offset) {
            uint8_t b[115]; memcpy(b, valid[kind], sizes[kind]); b[offset] ^= 0x80;
            fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
            fips_status st = fips_fmp_validate_frame(b, sizes[kind], &e);
            bool bad = offset == 0 || offset == 2 || offset == 3 || (kind && offset == 1);
            CHECK((st != FIPS_OK) == bad);
            if (offset == 0) CHECK(e.variant == FIPS_PROTO_UnsupportedVersion && e.as.unsupported_version == 8);
            else if (bad) CHECK(e.variant == FIPS_PROTO_Malformed);
            else CHECK(e.variant == FIPS_PROTO_InvalidSignature); /* opaque bytes, not authenticated */
        }
        ACTIONS; CHECK(fips_fmp_receive((fips_link_id){3}, valid[kind], sizes[kind], &a) == FIPS_OK);
        CHECK(storage[0].kind == (kind == 0 ? FIPS_FMP_ReceiveEncrypted : kind == 1 ? FIPS_FMP_ReceiveMsg1 : FIPS_FMP_ReceiveMsg2));
    }
    for (unsigned phase = 3; phase < 16; ++phase) {
        valid[0][0] = (uint8_t)phase; fips_proto_error e;
        CHECK(fips_fmp_validate_frame(valid[0], 32, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_InvalidMessageType && e.as.invalid_message_type == phase);
    }
    for (size_t i = 1; i < 3; ++i) { fips_proto_error e;
        CHECK(fips_fmp_validate_frame(valid[i], sizes[i] + 1, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooLong);
    }
    uint16_t n = 77;
    CHECK(!fips_fmp_expected_payload_len(0, 31, &n) && n == 77);
    CHECK(fips_fmp_expected_payload_len(0, (size_t)UINT16_MAX + 32, &n) && n == UINT16_MAX);
    CHECK(!fips_fmp_expected_payload_len(0, (size_t)UINT16_MAX + 33, &n));
    CHECK(!fips_fmp_expected_payload_len(15, 32, &n)); return 0;
}
static int writer_bounds_and_atomic_outputs(void)
{
    uint8_t noise[107] = {0}, buf[120], header[16], tag[16] = {0};
    fips_fmp_build_established_header((fips_session_index){0}, 0, 0, 0, header);
    for (size_t kind = 0; kind < 5; ++kind) {
        size_t total = kind == 0 ? 114 : kind == 1 ? 69 : kind == 2 ? 32 : kind == 3 ? 5 : 2;
        for (size_t cap = 0; cap < total; ++cap) {
            memset(buf, 0xcc, sizeof(buf)); fips_writer w; fips_writer_init(&w, buf, cap);
            fips_proto_error e; fips_status st; fips_fmp_disconnect d = {FIPS_FMP_Restart};
            if (kind == 0) st = fips_fmp_build_msg1((fips_session_index){1}, noise, 106, &w, &e);
            else if (kind == 1) st = fips_fmp_build_msg2((fips_session_index){1}, (fips_session_index){2}, noise, 57, &w, &e);
            else if (kind == 2) st = fips_fmp_build_encrypted(header, tag, sizeof(tag), &w, &e);
            else if (kind == 3) st = fips_fmp_prepend_inner_header(1, noise, 1, &w, &e);
            else st = fips_fmp_disconnect_encode(&d, &w, &e);
            CHECK(st == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooLong && w.pos == 0);
            CHECK(e.as.message_too_long.max == cap && e.as.message_too_long.got == total);
            for (size_t i = 0; i < sizeof(buf); ++i) CHECK(buf[i] == 0xcc);
        }
    }
    fips_writer w; fips_writer_init(&w, buf, sizeof(buf)); fips_proto_error e;
    CHECK(fips_fmp_build_msg1((fips_session_index){0}, noise, 105, &w, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooShort);
    CHECK(fips_fmp_build_msg1((fips_session_index){0}, noise, 107, &w, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooLong);
    CHECK(fips_fmp_build_msg2((fips_session_index){0}, (fips_session_index){0}, noise, 56, &w, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooShort);
    CHECK(fips_fmp_build_msg2((fips_session_index){0}, (fips_session_index){0}, noise, 58, &w, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooLong);
    CHECK(fips_fmp_build_encrypted(header, tag, 15, &w, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_MessageTooShort);
    CHECK(fips_fmp_build_encrypted(header, tag, SIZE_MAX, &w, &e) == FIPS_ERR_RANGE);
    CHECK(fips_fmp_prepend_inner_header(0, noise, SIZE_MAX, &w, &e) == FIPS_ERR_RANGE && w.pos == 0);
    fips_fmp_msg1_header one = {{7}, 99}, before = one;
    CHECK(!fips_fmp_msg1_header_parse(NULL, 0, &one) && !memcmp(&one, &before, sizeof(one)));
    fips_fmp_msg2_header two = {{7}, {8}, 99}, before2 = two;
    CHECK(!fips_fmp_msg2_header_parse(NULL, 0, &two) && !memcmp(&two, &before2, sizeof(two)));
    fips_fmp_encrypted_header enc = {.counter = 99}, before3 = enc;
    CHECK(!fips_fmp_encrypted_header_parse(NULL, 0, &enc) && !memcmp(&enc, &before3, sizeof(enc)));
    return 0;
}
static int bounded_lifecycle_and_extra_transitions(void)
{
    ACTIONS; a.capacity = 2; a.len = 1;
    fips_fmp_conn_snapshot snap = {.is_outbound = true, .has_retry_addr = true};
    CHECK(fips_fmp_poll_timeouts(&snap, 1, &a) == FIPS_ERR_RANGE && a.len == 1 && storage[1].kind == FIPS_FMP_Teardown);
    a.capacity = a.len;
    CHECK(fips_fmp_poll_resends(&snap, 1, fips_test_instant(0), 1, 2, &a) == FIPS_ERR_RANGE && a.len == 1);
    CHECK(fips_fmp_receive((fips_link_id){0}, NULL, 0, &a) == FIPS_ERR_RANGE && a.len == 1);
    a.len = 0; a.capacity = 8; snap.resend_count = UINT32_MAX;
    CHECK(fips_fmp_poll_resends(&snap, 1, fips_test_instant(0), UINT64_MAX, 2, &a) == FIPS_OK);
    CHECK(storage[0].next_resend_at.opaque[0] == FIPS_DEADLINE_CEILING_SECS * 1000);
    CHECK(fips_fmp_poll_resends(&snap, 1, fips_test_instant(123), 1, NAN, &a) == FIPS_OK && storage[1].next_resend_at.opaque[0] == 123);
    fips_fmp_outbound_snapshot o = {.has_existing_peer = true, .own_dial = true, .existing_peer_epoch = epoch(1)};
    CHECK(fips_fmp_establish_outbound(&o, epoch(1)) == FIPS_FMP_OUT_CrossConnectionSwap);
    CHECK(fips_fmp_establish_outbound(&o, epoch(2)) == FIPS_FMP_OUT_RestartThenPromote);
    fips_fmp_connection_state s = inbound(1000), original = s; uint8_t bytes[115] = {0};
    CHECK(fips_fmp_connection_set_handshake_msg1(&s, bytes, 115) == FIPS_ERR_RANGE && !memcmp(&s, &original, sizeof(s)));
    CHECK(fips_fmp_connection_set_handshake_msg2(&s, bytes, 70) == FIPS_ERR_RANGE && !memcmp(&s, &original, sizeof(s)));
    CHECK(!fips_fmp_connection_is_timed_out(&s, fips_test_instant(2000), 1000));
    CHECK(fips_fmp_connection_idle_time(&s, fips_test_instant(999)) == 0);
    s.resend_count = UINT32_MAX; fips_fmp_connection_record_resend(&s); CHECK(s.resend_count == UINT32_MAX);
    fips_session_index old; CHECK(!fips_fmp_connection_set_our_index(&s, (fips_session_index){1}, &old));
    CHECK(fips_fmp_connection_set_our_index(&s, (fips_session_index){2}, &old) && old.value == 1);
    CHECK(fips_fmp_connection_take_our_index(&s, &old) && old.value == 2); CHECK(!fips_fmp_connection_take_our_index(&s, &old));
    for (unsigned i = 0; i < 3; ++i) {
        fips_fmp_promotion_result p = {.kind = (fips_fmp_promotion_kind)i, .node_addr = addr(9), .loser_link_id = {3}};
        fips_node_addr node; fips_link_id link;
        CHECK(fips_fmp_promotion_node_addr(&p, &node) == (i != 1));
        CHECK(fips_fmp_promotion_should_close_this_connection(&p) == (i == 1));
        CHECK(fips_fmp_promotion_link_to_close(&p, &link) == (i == 2));
        if (i == 2) CHECK(link.value == 3);
    } return 0;
}
static int bounded_silent_storage_and_digest(void)
{
    SILENT; s.capacity = 1; THREE;
    fips_fmp_refusal_start before = r; bool before_started = started;
    CHECK(fips_fmp_silent_sessions_ended(&s, addr(2), epoch(7), NULL, fips_test_instant(3000), &started, &r) == FIPS_ERR_RANGE);
    CHECK(started == before_started && !memcmp(&r, &before, sizeof(r)) && s.len == 1); COUNT_IS(3);
    CHECK(fips_fmp_silent_sessions_ended(&s, addr(2), epoch(7), NULL, fips_test_instant(632000), &started, &r) == FIPS_OK && !started);
    peer = addr(2); for (unsigned i = 0; i < 9; ++i) END(i, 633000 + i);
    COUNT_IS(10); END(0, 634000); COUNT_IS(11); /* oldest of the eight digests was evicted */
    s.records[0].silent = UINT32_MAX; s.records[0].refused = UINT32_MAX;
    uint32_t n; CHECK(fips_fmp_silent_sessions_note_refused(&s, &peer, &n) && n == UINT32_MAX);
    END(99, 635000); CHECK(r.prior_refused == UINT32_MAX && r.backoff.silent == UINT32_MAX);
    fips_fmp_msg1_digest d; fips_fmp_msg1_digest_of((const uint8_t *)"abc", 3, &d);
    const uint8_t sha[] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CHECK(!memcmp(d.bytes, sha, sizeof(sha))); uint8_t prefix[4]; fips_fmp_msg1_digest_prefix(&d, prefix); CHECK(!memcmp(prefix, sha, 4));
    fips_fmp_silent_sessions_init(&s, NULL, 0, fips_test_instant(0));
    CHECK(fips_fmp_silent_sessions_ended(&s, peer, epoch(7), NULL, fips_test_instant(0), &started, &r) == FIPS_ERR_RANGE); return 0;
}
static int encrypted_aad_authentication_boundary(void)
{
    uint8_t key_bytes[32] = {0}, header[16], inner[5] = {1,2,3,4,0x51}, cipher[21], wire[37], opened[5];
    fips_aead_key key; CHECK(fips_aead_key_init(&key, key_bytes) == FIPS_AEAD_OK);
    fips_fmp_build_established_header((fips_session_index){42}, 7, 5, sizeof(inner), header);
    size_t n; CHECK(fips_aead_seal(&key, 7, header, sizeof(header), inner, sizeof(inner), cipher, sizeof(cipher), &n) == FIPS_AEAD_OK);
    fips_writer w; fips_writer_init(&w, wire, sizeof(wire));
    CHECK(fips_fmp_build_encrypted(header, cipher, n, &w, NULL) == FIPS_OK);
    fips_fmp_encrypted_header parsed; CHECK(fips_fmp_encrypted_header_parse(wire, w.pos, &parsed));
    CHECK(fips_aead_open(&key, parsed.counter, parsed.header_bytes, 16, wire + 16, w.pos - 16, opened, sizeof(opened), &n) == FIPS_AEAD_OK);
    CHECK(n == sizeof(inner) && !memcmp(opened, inner, n));
    for (size_t i = 0; i < sizeof(wire); ++i) {
        wire[i] ^= 0x80;
        CHECK(fips_aead_open(&key, 7, wire, 16, wire + 16, sizeof(wire) - 16, opened, sizeof(opened), &n) == FIPS_AEAD_ERR_AUTH);
        wire[i] ^= 0x80;
    }
    /* A valid tag does not imply enough plaintext for the inner header. */
    for (size_t len = 0; len < FIPS_FRAMING_FMP_INNER_HEADER_SIZE; ++len) {
        uint64_t counter = 8 + len; /* Never reuse the earlier nonce. */
        fips_fmp_build_established_header((fips_session_index){42}, counter, 5, (uint16_t)len, header);
        CHECK(fips_aead_seal(&key, counter, header, 16, inner, len, cipher, sizeof(cipher), &n) == FIPS_AEAD_OK);
        fips_writer_init(&w, wire, sizeof(wire));
        CHECK(fips_fmp_build_encrypted(header, cipher, n, &w, NULL) == FIPS_OK);
        CHECK(fips_fmp_validate_frame(wire, w.pos, NULL) == FIPS_OK);
        CHECK(fips_aead_open(&key, counter, wire, 16, wire + 16, w.pos - 16, opened, sizeof(opened), &n) == FIPS_AEAD_OK);
        ACTIONS; CHECK(fips_fmp_receive_plaintext((fips_link_id){42}, opened, n, &a) == FIPS_OK);
        CHECK(storage[0].kind == FIPS_FMP_DropFrame && storage[0].error.as.message_too_short.got == len);
    }
    fips_aead_key_destroy(&key); return 0;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"empty_stale_set_yields_no_actions", empty_stale_set_yields_no_actions},
        {"inbound_stale_connection_is_torn_down_without_retry", inbound_stale_connection_is_torn_down_without_retry},
        {"outbound_stale_with_identity_schedules_retry_then_tears_down", outbound_stale_with_identity_schedules_retry_then_tears_down},
        {"outbound_stale_without_identity_only_tears_down", outbound_stale_without_identity_only_tears_down},
        {"no_resend_candidates_yields_no_actions", no_resend_candidates_yields_no_actions},
        {"resend_emits_bytes_and_backoff_schedule", resend_emits_bytes_and_backoff_schedule},
        {"resend_backoff_exponent_uses_count_plus_one", resend_backoff_exponent_uses_count_plus_one},
        {"establish_inbound_net_new_promotes", establish_inbound_net_new_promotes},
        {"establish_inbound_at_cap_net_new_rejects", establish_inbound_at_cap_net_new_rejects},
        {"establish_inbound_at_cap_existing_peer_not_capped", establish_inbound_at_cap_existing_peer_not_capped},
        {"a_held_peers_same_epoch_msg1_goes_to_its_link_machine_until_that_machine_asks_for_a_replacement", a_held_peers_same_epoch_msg1_goes_to_its_link_machine_until_that_machine_asks_for_a_replacement},
        {"establish_inbound_epoch_mismatch_restarts", establish_inbound_epoch_mismatch_restarts},
        {"establish_outbound_no_existing_peer_promotes", establish_outbound_no_existing_peer_promotes},
        {"establish_outbound_cross_connection_win_swaps", establish_outbound_cross_connection_win_swaps},
        {"establish_outbound_cross_connection_loss_keeps", establish_outbound_cross_connection_loss_keeps},
        {"test_cross_connection_smaller_node_wins_outbound", test_cross_connection_smaller_node_wins_outbound},
        {"test_cross_connection_symmetric", test_cross_connection_symmetric},
        {"outbound_initializes_pure_fields", outbound_initializes_pure_fields},
        {"inbound_initializes_pure_fields", inbound_initializes_pure_fields},
        {"inbound_with_transport_sets_transport_and_addr", inbound_with_transport_sets_transport_and_addr},
        {"index_setters_round_trip", index_setters_round_trip},
        {"transport_and_source_setters_round_trip", transport_and_source_setters_round_trip},
        {"identity_and_epoch_setters", identity_and_epoch_setters},
        {"resend_bookkeeping", resend_bookkeeping},
        {"timing_and_touch", timing_and_touch},
        {"a_legs_timeout_reads_its_interval_stamp_and_its_shown_times_the_wall_stamp", a_legs_timeout_reads_its_interval_stamp_and_its_shown_times_the_wall_stamp},
        {"two_silent_sessions_refuse_nothing_and_the_third_refuses_for_the_base", two_silent_sessions_refuse_nothing_and_the_third_refuses_for_the_base},
        {"each_further_silent_session_doubles_the_refusal_up_to_the_ten_minute_cap", each_further_silent_session_doubles_the_refusal_up_to_the_ten_minute_cap},
        {"the_refusal_ends_when_its_time_has_run", the_refusal_ends_when_its_time_has_run},
        {"an_authenticated_frame_drops_the_record", an_authenticated_frame_drops_the_record},
        {"a_silent_session_at_another_epoch_restarts_the_count_at_one", a_silent_session_at_another_epoch_restarts_the_count_at_one},
        {"a_record_without_an_epoch_adopts_the_first_epoch_it_sees", a_record_without_an_epoch_adopts_the_first_epoch_it_sees},
        {"a_session_promoted_from_an_already_counted_msg1_is_not_counted_again", a_session_promoted_from_an_already_counted_msg1_is_not_counted_again},
        {"sessions_promoted_from_msg1s_carrying_one_ephemeral_count_once_whatever_follows_it", sessions_promoted_from_msg1s_carrying_one_ephemeral_count_once_whatever_follows_it},
        {"a_msg1_too_short_to_carry_an_ephemeral_has_no_ephemeral_digest", a_msg1_too_short_to_carry_an_ephemeral_has_no_ephemeral_digest},
        {"sessions_this_node_dialled_count_without_a_setup_digest", sessions_this_node_dialled_count_without_a_setup_digest},
        {"a_new_refusal_reports_how_many_msg1s_the_previous_refusal_refused", a_new_refusal_reports_how_many_msg1s_the_previous_refusal_refused},
        {"clearing_a_record_returns_its_refused_count", clearing_a_record_returns_its_refused_count},
        {"a_record_is_pruned_a_cap_after_its_refusal_ends_and_kept_before", a_record_is_pruned_a_cap_after_its_refusal_ends_and_kept_before},
        {"a_record_below_the_limit_is_pruned_a_cap_after_its_last_silent_session", a_record_below_the_limit_is_pruned_a_cap_after_its_last_silent_session},
        {"a_same_epoch_msg1_from_an_identity_with_no_peer_is_refused_during_the_back_off", a_same_epoch_msg1_from_an_identity_with_no_peer_is_refused_during_the_back_off},
        {"a_same_epoch_msg1_that_would_replace_a_young_session_is_refused_during_the_back_off", a_same_epoch_msg1_that_would_replace_a_young_session_is_refused_during_the_back_off},
        {"a_msg1_with_no_epoch_counts_as_the_same_epoch", a_msg1_with_no_epoch_counts_as_the_same_epoch},
        {"a_new_epoch_msg1_is_promoted_during_the_back_off", a_new_epoch_msg1_is_promoted_during_the_back_off},
        {"a_held_peers_same_epoch_msg1_goes_to_its_link_machine_whatever_the_back_off", a_held_peers_same_epoch_msg1_goes_to_its_link_machine_whatever_the_back_off},
        {"the_max_peers_gate_still_comes_first", the_max_peers_gate_still_comes_first},
        {"test_handshake_message_type_roundtrip", test_handshake_message_type_roundtrip},
        {"test_handshake_message_type_invalid", test_handshake_message_type_invalid},
        {"test_handshake_message_type_is_handshake", test_handshake_message_type_is_handshake},
        {"test_disconnect_reason_roundtrip", test_disconnect_reason_roundtrip},
        {"test_disconnect_reason_unknown_byte", test_disconnect_reason_unknown_byte},
        {"test_disconnect_encode_decode", test_disconnect_encode_decode},
        {"test_disconnect_all_reasons", test_disconnect_all_reasons},
        {"test_disconnect_decode_empty_payload", test_disconnect_decode_empty_payload},
        {"test_disconnect_decode_unknown_reason", test_disconnect_decode_unknown_reason},
        {"the_frame_overhead_is_the_header_the_timestamp_and_the_tag", the_frame_overhead_is_the_header_the_timestamp_and_the_tag},
        {"test_common_prefix_parse", test_common_prefix_parse},
        {"test_common_prefix_too_short", test_common_prefix_too_short},
        {"test_encrypted_header_parse", test_encrypted_header_parse},
        {"test_encrypted_header_too_short", test_encrypted_header_too_short},
        {"test_encrypted_header_wrong_phase", test_encrypted_header_wrong_phase},
        {"test_encrypted_header_wrong_version", test_encrypted_header_wrong_version},
        {"test_msg1_header_parse", test_msg1_header_parse},
        {"test_msg1_header_wrong_size", test_msg1_header_wrong_size},
        {"test_msg1_header_wrong_phase", test_msg1_header_wrong_phase},
        {"test_msg1_header_nonzero_flags", test_msg1_header_nonzero_flags},
        {"test_msg2_header_parse", test_msg2_header_parse},
        {"test_msg2_header_wrong_size", test_msg2_header_wrong_size},
        {"test_msg2_header_wrong_phase", test_msg2_header_wrong_phase},
        {"test_wire_sizes", test_wire_sizes},
        {"test_roundtrip_indices", test_roundtrip_indices},
        {"test_inner_header_prepend_strip", test_inner_header_prepend_strip},
        {"test_inner_header_too_short", test_inner_header_too_short},
        {"test_flags_byte", test_flags_byte},
        {"test_payload_len_in_msg1", test_payload_len_in_msg1},
        {"test_payload_len_in_msg2", test_payload_len_in_msg2},
        {"expected_payload_len_for_an_established_frame_excludes_header_and_tag", expected_payload_len_for_an_established_frame_excludes_header_and_tag},
        {"expected_payload_len_matches_what_build_msg1_and_build_msg2_actually_emit", expected_payload_len_matches_what_build_msg1_and_build_msg2_actually_emit},
        {"test_backoff_exponential", test_backoff_exponential},
        {"test_backoff_cap", test_backoff_cap},
        {"test_backoff_zero_base", test_backoff_zero_base},
        {"malformed_at_every_byte_and_pending_short_drop", malformed_at_every_byte_and_pending_short_drop},
        {"writer_bounds_and_atomic_outputs", writer_bounds_and_atomic_outputs},
        {"bounded_lifecycle_and_extra_transitions", bounded_lifecycle_and_extra_transitions},
        {"bounded_silent_storage_and_digest", bounded_silent_storage_and_digest},
        {"encrypted_aad_authentication_boundary", encrypted_aad_authentication_boundary},
    };
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run()) { ++failures; fprintf(stderr, "FAIL %s\n", tests[i].name); }
    }
    printf("FMP: %zu tests, %zu failures\n", sizeof(tests) / sizeof(tests[0]), failures);
    return failures ? 1 : 0;
}
