#include "peer_test.h"
#include <fips/proto/fmp/silent.h>
static void init(fipsd_peer_machine *m, fips_link_direction direction)
{
    fips_peer_identity id = peer_id(1);
    fipsd_peer_machine_init(m, (fips_link_id){7}, direction, &id,
        (fips_fmp_leg_stamp){1000, fips_test_instant(0)}, fipsd_peer_timing_default());
}
static bool has(const fipsd_peer_actions *a, fipsd_peer_action_kind kind)
{
    for (size_t i = 0; i < a->count; ++i) if (a->items[i].kind == kind) return true;
    return false;
}
static void step(fipsd_peer_machine *m, uint64_t now, fipsd_peer_event_kind event, fipsd_peer_actions *a)
{ fipsd_peer_event e = {.kind = event}; fipsd_peer_machine_step(m, fips_test_instant(now), &e, a); }
int peer_machine_tests(void)
{
    fipsd_peer_machine m;
    fipsd_peer_actions a;
    init(&m, FIPS_LINK_OUTBOUND);
    fipsd_peer_event dial = {.kind = FIPSD_PEER_DIAL, .connection_oriented = true};
    fipsd_peer_machine_step(&m, fips_test_instant(0), &dial, &a);
    CHECK(m.state == FIPSD_PEER_CONNECTING && has(&a, FIPSD_PEER_OPEN_TRANSPORT));
    step(&m, 1, FIPSD_PEER_CONNECT_FAILED, &a);
    CHECK(m.state == FIPSD_PEER_CLOSED && has(&a, FIPSD_PEER_LOST_HANDSHAKE));
    CHECK(m.closed_until.opaque[0] == 5001);
    step(&m, 5001, FIPSD_PEER_DIAL, &a);
    CHECK(m.state == FIPSD_PEER_CLOSED && !a.count); /* terminal, new connection per dial */
    init(&m, FIPS_LINK_OUTBOUND);
    step(&m, 0, FIPSD_PEER_DIAL, &a);
    uint8_t wire[] = {0xaa, 0xbb};
    CHECK(fips_fmp_connection_set_handshake_msg1(&m.conn, wire, sizeof wire) == FIPS_OK);
    const uint64_t times[] = {1000, 3000, 7000, 15000};
    const uint64_t next[] = {3000, 7000, 15000, 31000};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        step(&m, times[i] - 1, FIPSD_PEER_POLL, &a); CHECK(!a.count);
        step(&m, times[i], FIPSD_PEER_POLL, &a);
        CHECK(a.count == 1 && a.items[0].kind == FIPSD_PEER_SEND_HANDSHAKE);
        CHECK(a.items[0].bytes.len == 2 && !memcmp(a.items[0].bytes.data, wire, 2));
        CHECK(m.resend_at.opaque[0] == next[i]);
        step(&m, times[i], FIPSD_PEER_POLL, &a); CHECK(!a.count); /* one outstanding send */
        fipsd_peer_event sent = {.kind = FIPSD_PEER_RESEND_RESULT, .success = true};
        fipsd_peer_machine_step(&m, fips_test_instant(times[i]), &sent, &a);
        CHECK(m.conn.resend_count == i + 1);
    }
    step(&m, 30000, FIPSD_PEER_POLL, &a);
    CHECK(m.state == FIPSD_PEER_CLOSED && has(&a, FIPSD_PEER_TEARDOWN));
    CHECK(!has(&a, FIPSD_PEER_SEND_HANDSHAKE));
    CHECK(!fipsd_peer_machine_next_wake(&m).ops);
    /* Timeout wins if delayed delivery coalesces timeout and resend. */
    init(&m, FIPS_LINK_OUTBOUND); step(&m, 0, FIPSD_PEER_DIAL, &a);
    CHECK(fips_fmp_connection_set_handshake_msg1(&m.conn, wire, 2) == FIPS_OK);
    step(&m, 30000, FIPSD_PEER_POLL, &a);
    CHECK(has(&a, FIPSD_PEER_LOST_HANDSHAKE) && !has(&a, FIPSD_PEER_SEND_HANDSHAKE));
    /* Failed sends don't advance the resend count, custom backoff goes to core. */
    init(&m, FIPS_LINK_OUTBOUND); m.timing.resend_backoff = 3.0;
    step(&m, 0, FIPSD_PEER_DIAL, &a);
    CHECK(fips_fmp_connection_set_handshake_msg1(&m.conn, wire, 2) == FIPS_OK);
    step(&m, 1000, FIPSD_PEER_POLL, &a); CHECK(m.resend_at.opaque[0] == 4000);
    step(&m, 1000, FIPSD_PEER_RESEND_RESULT, &a);
    CHECK(!m.conn.resend_count && m.resend_at.opaque[0] == 2000);
    step(&m, 1001, FIPSD_PEER_SEND_FAILED, &a);
    CHECK(m.send_failed && m.state == FIPSD_PEER_HANDSHAKING);
    /* Startup epoch detection requires BOTH known epochs; absence is not restart. */
    for (unsigned old = 0; old < 3; ++old) for (unsigned fresh = 0; fresh < 3; ++fresh) {
        init(&m, FIPS_LINK_OUTBOUND); step(&m, 0, FIPSD_PEER_DIAL, &a);
        fipsd_peer_event msg2 = {.kind = FIPSD_PEER_MSG2_AUTHENTICATED};
        msg2.outbound.has_existing_peer = true;
        msg2.outbound.existing_peer_epoch.present = old != 0;
        msg2.outbound.existing_peer_epoch.bytes[0] = (uint8_t)old;
        msg2.wire.remote_epoch.present = fresh != 0;
        msg2.wire.remote_epoch.bytes[0] = (uint8_t)fresh;
        fipsd_peer_machine_step(&m, fips_test_instant(10), &msg2, &a);
        bool restart = old && fresh && old != fresh;
        CHECK(has(&a, FIPSD_PEER_INVALIDATE) == restart);
        CHECK(has(&a, FIPSD_PEER_PROMOTE) == restart);
        CHECK(has(&a, FIPSD_PEER_CROSS_KEEP) == !restart);
    }
    /* Both crossing directions, own-dial override, both address orderings. */
    for (unsigned win = 0; win < 2; ++win) for (unsigned own = 0; own < 2; ++own) {
        init(&m, FIPS_LINK_OUTBOUND); step(&m, 0, FIPSD_PEER_DIAL, &a);
        fipsd_peer_event e = {.kind = FIPSD_PEER_MSG2_AUTHENTICATED,
            .outbound = {.has_existing_peer = true, .our_outbound_wins = win != 0, .own_dial = own != 0}};
        fipsd_peer_machine_step(&m, fips_test_instant(10), &e, &a);
        CHECK(has(&a, (win || own) ? FIPSD_PEER_CROSS_SWAP : FIPSD_PEER_CROSS_KEEP));
    }
    fips_peer_identity x = peer_id(1), y = peer_id(2);
    CHECK(fips_fmp_cross_connection_winner(&x.node_addr, &y.node_addr, true));
    CHECK(!fips_fmp_cross_connection_winner(&y.node_addr, &x.node_addr, true));
    CHECK(fips_fmp_cross_connection_winner(&y.node_addr, &x.node_addr, false));
    init(&m, FIPS_LINK_INBOUND);
    fipsd_peer_event inbound = {.kind = FIPSD_PEER_INBOUND_AUTHENTICATED};
    inbound.wire.peer_identity = x;
    inbound.wire.msg2_payload = (fips_fmp_bytes){wire, 2};
    fipsd_peer_machine_step(&m, fips_test_instant(0), &inbound, &a);
    CHECK(m.state == FIPSD_PEER_AUTHORIZING && !a.count);
    step(&m, 1, FIPSD_PEER_AUTHORIZED, &a);
    CHECK(has(&a, FIPSD_PEER_SEND_HANDSHAKE) && has(&a, FIPSD_PEER_PROMOTE));
    step(&m, 2, FIPSD_PEER_PROMOTED, &a); CHECK(m.state == FIPSD_PEER_ESTABLISHED);
    step(&m, 3, FIPSD_PEER_HEARD, &a); CHECK(m.state == FIPSD_PEER_ACTIVE);
    step(&m, 10003, FIPSD_PEER_POLL, &a); CHECK(has(&a, FIPSD_PEER_SEND_HEARTBEAT));
    step(&m, 10004, FIPSD_PEER_DISCONNECT, &a);
    CHECK(has(&a, FIPSD_PEER_SEND_DISCONNECT) && !has(&a, FIPSD_PEER_LOST_LINK));
    init(&m, FIPS_LINK_INBOUND);
    inbound.inbound.at_max_peers = true;
    fipsd_peer_machine_step(&m, fips_test_instant(0), &inbound, &a);
    CHECK(m.state == FIPSD_PEER_FAILED && !has(&a, FIPSD_PEER_PROMOTE));
    /* Established same epoch is delegated, restart invalidates before promotion. */
    init(&m, FIPS_LINK_INBOUND); m.state = FIPSD_PEER_ESTABLISHED;
    inbound.inbound = (fips_fmp_establish_snapshot){.has_existing_peer = true,
        .existing_peer_epoch = {.present = true, .bytes = {1}}};
    inbound.wire.remote_epoch = (fips_fmp_epoch){.present = true, .bytes = {1}};
    fipsd_peer_machine_step(&m, fips_test_instant(0), &inbound, &a);
    CHECK(a.count == 1 && has(&a, FIPSD_PEER_REKEY_INPUT));
    inbound.wire.remote_epoch.bytes[0] = 2;
    fipsd_peer_machine_step(&m, fips_test_instant(1), &inbound, &a);
    CHECK(a.items[0].kind == FIPSD_PEER_INVALIDATE && has(&a, FIPSD_PEER_LOST_LINK));
    /* Reconciler retry count is durable; cap, expiry, connected and drain gates. */
    fipsd_peer_retry r = {0};
    fipsd_peer_retry_policy policy = {5000, 300000, 30000, 5};
    const uint64_t delays[] = {10000, 20000, 40000, 80000, 160000, 300000, 300000};
    for (size_t i = 0; i < sizeof delays / sizeof delays[0]; ++i) {
        CHECK(fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), false, true, true, false, false));
        CHECK(r.after.opaque[0] == delays[i]);
        CHECK(!fipsd_peer_retry_take(&r, policy, fips_test_instant(delays[i] - 1), false, true, false));
        CHECK(!fipsd_peer_retry_take(&r, policy, fips_test_instant(delays[i]), false, false, false));
        CHECK(fipsd_peer_retry_take(&r, policy, fips_test_instant(delays[i]), false, true, false));
        CHECK(r.after.opaque[0] == delays[i] + 30000);
    }
    fipsd_peer_retry_init(&r);
    CHECK(fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), true, true, true, false, false));
    CHECK(!r.count && r.after.opaque[0] == 5000);
    CHECK(fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), true, true, true, false, false));
    CHECK(r.count == 1 && r.after.opaque[0] == 10000);
    CHECK(!fipsd_peer_retry_take(&r, policy, fips_test_instant(10000), true, true, false) && !r.scheduled);
    CHECK(!fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), true, true, false, false, false));
    policy.max_retries = 0;
    CHECK(!fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), false, true, true, false, false));
    CHECK(fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), true, true, true, false, false));
    CHECK(!fipsd_peer_retry_take(&r, policy, fips_test_instant(10000), false, true, true) && !r.scheduled);
    policy.max_retries = 1;
    r = (fipsd_peer_retry){.scheduled = true, .count = 1};
    CHECK(!fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), false, false, false, false, false) && !r.scheduled);
    r = (fipsd_peer_retry){.scheduled = true, .expires = fips_test_instant(10)};
    CHECK(!fipsd_peer_retry_take(&r, policy, fips_test_instant(10), false, true, false));
    r = (fipsd_peer_retry){.scheduled = true, .reconnect = true, .count = UINT32_MAX};
    CHECK(fipsd_peer_retry_failed(&r, policy, fips_test_instant(0), false, true, true, false, false));
    CHECK(r.count == UINT32_MAX && r.after.opaque[0] == 300000);
    /* Silent-session cooldown: third distinct setup 30s, doubles to 600s;
     * duplicate captures do not buy another strike. Reuse the existing core. */
    fips_fmp_silent_record record[1];
    fips_fmp_silent_sessions silent;
    fips_fmp_silent_sessions_init(&silent, record, 1, fips_test_instant(0));
    fips_fmp_epoch epoch = {.present = true, .bytes = {1}};
    fips_fmp_refusal_start refusal;
    bool started = false;
    for (unsigned n = 1; n <= 9; ++n) {
        fips_fmp_ephemeral_digest digest = {{0}}; digest.bytes[0] = (uint8_t)n;
        CHECK(fips_fmp_silent_sessions_ended(&silent, x.node_addr, epoch, &digest, fips_test_instant(n), &started, &refusal) == FIPS_OK);
        CHECK(started == (n >= 3));
        if (started) {
            uint64_t delay = fips_fmp_backoff_ms(n - 3, 30000, 600000);
            CHECK(refusal.backoff.remaining_ms == delay);
        }
    }
    uint32_t count = 0;
    CHECK(fips_fmp_silent_sessions_count(&silent, &x.node_addr, &count) && count == 9);
    CHECK(fips_fmp_silent_sessions_note_refused(&silent, &x.node_addr, &count) && count == 1);
    CHECK(fips_fmp_silent_sessions_heard(&silent, &x.node_addr) == 1);
    CHECK(!fips_fmp_silent_sessions_count(&silent, &x.node_addr, &count));
    return 0;
}
