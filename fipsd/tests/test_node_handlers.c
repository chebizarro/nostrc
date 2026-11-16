/* Per-handler scenarios (nostrc-9keoz.6), adapted from the upstream handler
 * tests (mmp.rs silence/heartbeat pacing, session.rs held-packet fate,
 * timeout.rs stranded release) plus C-host contracts: handshake msg2 loss and
 * retransmission, pending-connection projection and timeout, dead-link
 * detection on authenticated silence, held datagram release without a route,
 * single-node control seeds, diagnostic probe through the live provider. */
#include "node_test.h"
#include "node/handlers/internal.h"
#include <stdatomic.h>

static const char *yaml_a =
    "node: {identity: {nsec: '3333333333333333333333333333333333333333333333333333333333333333', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '4444444444444444444444444444444444444444444444444444444444444444', persistent: true}, control: {enabled: false}}";
typedef struct filter_state { _Atomic int drop_msg2_once, drop_all_from_b, drop_msg1, dropped; const fips_transport_addr *b; } filter_state;
static filter_state filter;
static bool keep(void *user, const fips_transport_addr *from, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{
    filter_state *f = user; (void)to;
    if (len && bytes[0] == 2 && atomic_exchange(&f->drop_msg2_once, 0)) { atomic_fetch_add(&f->dropped, 1); return false; }
    if (len && bytes[0] == 1 && atomic_load(&f->drop_msg1)) { atomic_fetch_add(&f->dropped, 1); return false; }
    if (atomic_load(&f->drop_all_from_b) && f->b && fips_transport_addr_equal(from, f->b)) { atomic_fetch_add(&f->dropped, 1); return false; }
    return true;
}
static json_t *data_of(json_t *response, const char *key)
{
    assert(response);
    json_t *data = json_object_get(response, "data");
    assert(data);
    return key ? json_object_get(data, key) : data;
}
static json_t *poll_probe(test_node *n, json_int_t id);
static json_t *live(test_node *n, const char *name, json_t *params)
{
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + 5000000000u};
    json_t *out = p.command(p.user, name, params, &rc);
    json_decref(params);
    return out;
}
int main(void)
{
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_loopback_network_set_filter(net, keep, &filter);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    /* Short deadlines so silence, retransmission and held-datagram release are
     * observable in seconds; the ladder/timeouts keep their shapes. */
    hcfg.link_dead_timeout_ms = 3000;
    hcfg.timing = (fipsd_peer_timing){500, 4000, 1000, 1000, 5, 2.0};
    hcfg.lookup_attempt_timeouts_secs[0] = 1; hcfg.lookup_attempt_count = 1;
    test_node a, b;
    assert(nt_init(&a, net, "A", "loop:a", yaml_a, &hcfg, 1));
    assert(nt_init(&b, net, "B", "loop:b", yaml_b, &hcfg, 1));
    filter.b = b.addr;
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0);
    nt_identity(&a); nt_identity(&b);
    /* Single-node control seeds before any peer: identity reportable, self root. */
    {
        json_t *status = nt_query(&a, "show_status");
        assert(!strcmp(json_string_value(json_object_get(status, "status")), "ok"));
        assert(json_integer_value(data_of(status, "peer_count")) == 0);
        json_decref(status);
        json_t *tree = nt_query(&a, "show_tree");
        assert(json_boolean_value(data_of(tree, "is_root")) && json_boolean_value(data_of(tree, "declaration_signed")));
        assert(json_integer_value(data_of(tree, "declaration_sequence")) >= 1 && json_array_size(data_of(tree, "my_coords")) == 1);
        json_decref(tree);
        json_t *peers = nt_query(&a, "show_peers"); assert(json_array_size(data_of(peers, "peers")) == 0); json_decref(peers);
        json_t *metrics = nt_query(&a, "show_metrics"); assert(json_object_get(data_of(metrics, NULL), "handshake")); json_decref(metrics);
    }
    /* Scenario 1: the first msg2 is lost; the pending leg retransmits msg1 at the
     * core's 500 ms rung and the link still comes up (upstream timeout.rs
     * drive_handshake_retransmits: only successful sends advance the schedule). */
    atomic_store(&filter.drop_msg2_once, 1);
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    assert(atomic_load(&filter.dropped) == 1);
    {
        json_t *metrics = nt_query(&a, "show_metrics");
        json_t *hs = json_object_get(data_of(metrics, NULL), "handshake");
        assert(json_integer_value(json_object_get(hs, "bad_state")) == 0);
        json_decref(metrics);
    }
    /* Scenario 2: dead link. B's frames are dropped; A hears nothing for
     * link_dead_timeout and reaps the peer (mmp.rs: silence reaches the timeout
     * exactly on the last-receive basis; a failed heartbeat does not satisfy it). */
    atomic_store(&filter.drop_all_from_b, 1);
    e = nt_await(&a, FIPSD_NOTICE_LINK_DOWN, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    atomic_store(&filter.drop_all_from_b, 0);
    /* B learns of the loss through A's Disconnect or its own silence timer. */
    e = nt_await(&b, FIPSD_NOTICE_LINK_DOWN, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    {
        json_t *metrics = nt_query(&a, "show_metrics");
        json_t *hs = json_object_get(data_of(metrics, NULL), "handshake");
        assert(json_object_size(hs) == 8);
        json_decref(metrics);
        json_t *peers = nt_query(&a, "show_peers"); assert(json_array_size(data_of(peers, "peers")) == 0); json_decref(peers);
    }
    /* Scenario 3: unreachable dial. msg1 is dropped forever: the connection is
     * projected while pending, resends run their ladder, then the handshake
     * times out and the leg is torn down (timeout.rs drive_handshake_timeouts). */
    atomic_store(&filter.drop_msg1, 1);
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    bool projected = false;
    for (unsigned i = 0; i < 50 && !projected; ++i) {
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_node_handlers_notices(a.h), NULL, &v}};
        GoSelectResult r = go_select_timeout(cases, 1, 100);
        if (r.selected_case == 0 && v) { nt_record(&a, v); free(v); }
        json_t *conns = nt_query(&a, "show_connections");
        json_t *rows = data_of(conns, "connections");
        if (json_array_size(rows) == 1) {
            json_t *row = json_array_get(rows, 0);
            projected = !strcmp(json_string_value(json_object_get(row, "handshake_state")), "handshaking") &&
                !strcmp(json_string_value(json_object_get(row, "direction")), "outbound");
        }
        json_decref(conns);
    }
    assert(projected);
    e = nt_await(&a, FIPSD_NOTICE_HANDSHAKE_FAILED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    assert(atomic_load(&filter.dropped) >= 3); /* initial msg1 plus retransmissions */
    atomic_store(&filter.drop_msg1, 0);
    {
        json_t *conns = nt_query(&a, "show_connections"); assert(json_array_size(data_of(conns, "connections")) == 0); json_decref(conns);
    }
    /* Scenario 4: a datagram for an unknown destination with a known identity
     * is held behind a lookup; with no peer that may reach it the lookup is a
     * bloom miss and the hold is released as a send failure once the ladder
     * plus handshake window elapse (session.rs held fate, timeout.rs stranded). */
    fips_peer_identity stranger = b.identity; stranger.node_addr.bytes[0] ^= 0x55; stranger.node_addr.bytes[15] ^= 0x55;
    assert(fipsd_node_handlers_learn_identity(a.h, &stranger) == 0);
    static const uint8_t payload[] = "held";
    assert(fipsd_node_handlers_send(a.h, &stranger.node_addr, 1, 2, payload, sizeof payload) == 0);
    e = nt_await(&a, FIPSD_NOTICE_SEND_FAILED, &stranger.node_addr, NT_WAIT_MS); assert(e && e->value == 1); free(e);
    /* Scenario 5: reconnect after the loss and run a diagnostic probe through
     * the live provider command path (probe.rs api_probe_start/poll). */
    assert(fipsd_node_handlers_connect(b.h, &a.identity, (fips_transport_id){1}, a.addr) == 0);
    e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&a, FIPSD_NOTICE_FILTER_ACCEPTED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    char npub[FIPS_NIP19_CAPACITY]; assert(fips_peer_identity_npub(&b.identity, npub, sizeof npub) == FIPS_IDENTITY_OK);
    json_t *started = live(&a, "probe_start", json_pack("{s:s}", "npub", npub));
    assert(started && !strcmp(json_string_value(json_object_get(started, "status")), "ok"));
    json_int_t id = json_integer_value(json_object_get(json_object_get(started, "data"), "probe_id"));
    json_decref(started);
    assert(id >= 1);
    e = nt_await(&a, FIPSD_NOTICE_SESSION_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    /* The peer's data adopts the probe's session (probe.rs
     * probe_session_disowned): the host refuses the core's teardown and the
     * report states the refusal, not a teardown that did not happen
     * (nostrc-f66sr.1 regression: the session used to be reported torn down). */
    e = nt_await(&b, FIPSD_NOTICE_SESSION_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    /* The traffic baseline is taken by the observe that passes the session
     * stage, so the peer's data must land after that stage reports ok. */
    for (unsigned i = 0;; ++i) {
        json_t *r = live(&a, "probe_poll", json_pack("{s:I}", "probe_id", id)); assert(r && i < 400);
        json_t *d = json_object_get(r, "data");
        assert(!strcmp(json_string_value(json_object_get(d, "state")), "running"));
        const char *verdict = json_string_value(json_object_get(json_object_get(json_object_get(d, "report"), "session"), "verdict"));
        bool passed = verdict && !strcmp(verdict, "ok");
        json_decref(r);
        if (passed) break;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_node_handlers_notices(a.h), NULL, &v}};
        GoSelectResult sel = go_select_timeout(cases, 1, 100);
        if (sel.selected_case == 0 && v) { nt_record(&a, v); free(v); }
    }
    static const uint8_t adopt[] = "adopt";
    assert(fipsd_node_handlers_send(b.h, &a.identity.node_addr, 7, 8, adopt, sizeof adopt) == 0);
    e = nt_await(&a, FIPSD_NOTICE_DATAGRAM, &b.identity.node_addr, NT_WAIT_MS); assert(e && e->len == sizeof adopt); free(e);
    json_t *polled = poll_probe(&a, id);
    json_t *report = json_object_get(json_object_get(polled, "data"), "report");
    assert(report && !strcmp(json_string_value(json_object_get(report, "overall")), "ok"));
    json_t *cleanup = json_object_get(report, "cleanup");
    assert(!json_boolean_value(json_object_get(json_object_get(report, "session"), "preexisting")));
    assert(!json_boolean_value(json_object_get(cleanup, "session_created_and_torn_down")));
    assert(json_boolean_value(json_object_get(cleanup, "session_left_intact")));
    assert(!strcmp(json_string_value(json_object_get(cleanup, "left_intact_reason")), "adopted_by_traffic"));
    json_decref(polled);
    {
        json_t *sessions = nt_query(&a, "show_sessions");
        assert(json_array_size(data_of(sessions, "sessions")) == 1);
        assert(json_boolean_value(json_object_get(json_array_get(data_of(sessions, "sessions"), 0), "is_initiator")));
        json_decref(sessions);
    }
    /* A second probe finds that session and leaves it: preexisting. */
    started = live(&a, "probe_start", json_pack("{s:s}", "npub", npub));
    assert(started && !strcmp(json_string_value(json_object_get(started, "status")), "ok"));
    id = json_integer_value(json_object_get(json_object_get(started, "data"), "probe_id"));
    json_decref(started);
    polled = poll_probe(&a, id);
    report = json_object_get(json_object_get(polled, "data"), "report");
    cleanup = json_object_get(report, "cleanup");
    assert(!strcmp(json_string_value(json_object_get(report, "overall")), "ok"));
    assert(json_boolean_value(json_object_get(json_object_get(report, "session"), "preexisting")));
    assert(!json_boolean_value(json_object_get(cleanup, "session_created_and_torn_down")));
    assert(json_boolean_value(json_object_get(cleanup, "session_left_intact")));
    assert(!strcmp(json_string_value(json_object_get(cleanup, "left_intact_reason")), "preexisting"));
    json_decref(polled);
    {
        json_t *sessions = nt_query(&a, "show_sessions");
        assert(json_array_size(data_of(sessions, "sessions")) == 1);
        json_decref(sessions);
    }
    json_t *unknown = live(&a, "probe_poll", json_pack("{s:I}", "probe_id", (json_int_t)999));
    assert(unknown && !strcmp(json_string_value(json_object_get(unknown, "status")), "error")); json_decref(unknown);
    json_t *shown = live(&a, "path_show", json_pack("{s:s}", "npub", npub));
    assert(shown && !strcmp(json_string_value(json_object_get(shown, "status")), "ok")); json_decref(shown);
    json_t *disconnected = live(&a, "disconnect", json_pack("{s:s}", "npub", npub));
    assert(disconnected && !strcmp(json_string_value(json_object_get(disconnected, "status")), "ok")); json_decref(disconnected);
    e = nt_await(&b, FIPSD_NOTICE_LINK_DOWN, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(a.h->counters.hs_msg2 == 1 && a.h->counters.hs_promoted == 2);
    nt_drain(&a); nt_drain(&b);
    nt_print_trace("handler scenarios");
    fipsd_runtime_destroy(rt);
    nt_destroy(&a); nt_destroy(&b);
    fipsd_loopback_network_destroy(net);
    puts("node handlers: seeds, msg2 loss/retransmit, dead link, pending timeout, held release, probe/path/disconnect commands: ok");
    return 0;
}
/* Drive a probe to `done` through the live provider, recording notices meanwhile. */
static json_t *poll_probe(test_node *n, json_int_t id)
{
    json_t *polled = NULL;
    for (unsigned i = 0; i < 400 && !polled; ++i) {
        json_t *r = live(n, "probe_poll", json_pack("{s:I}", "probe_id", id));
        assert(r && !strcmp(json_string_value(json_object_get(r, "status")), "ok"));
        if (!strcmp(json_string_value(json_object_get(json_object_get(r, "data"), "state")), "done")) polled = r;
        else {
            json_decref(r);
            void *v = NULL;
            GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_node_handlers_notices(n->h), NULL, &v}};
            GoSelectResult sel = go_select_timeout(cases, 1, 100);
            if (sel.selected_case == 0 && v) { nt_record(n, v); free(v); }
        }
    }
    assert(polled);
    return polled;
}
