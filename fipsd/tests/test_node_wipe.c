/* L4 (nostrc-f66sr.10): queued plaintext hygiene on the production node
 * handlers, two nodes on one loopback network. This binary carries its own copy
 * of handlers.c/session.c compiled with FIPSD_TEST_HOOKS, whose wipe seam
 * reports every slot wipe after it happened together with whether the bytes had
 * been non-zero. Ordering comes from channel barriers (a live control command,
 * a parked dispatcher gate) and notices, never from timers or heap scans. It
 * proves: pending slots are zero after consume, drop-oldest and session close;
 * held slots after consume, drop-oldest and expiry; both arrays at teardown;
 * the HCMD_SEND payload and the delivery notice on release; and that the pooled
 * packet slab a worker borrows is intact while its seal job is live and zero
 * once the node has recycled every job. */
#include "node_test.h"
#include "node/handlers/internal.h"
#include <stdatomic.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); nt_print_trace("wipe"); abort(); } } while (0)

static const char *yaml_a =
    "node: {identity: {nsec: '5555555555555555555555555555555555555555555555555555555555555555', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '6666666666666666666666666666666666666666666666666666666666666666', persistent: true}, control: {enabled: false}}";

/* The datagram under observation: long enough to be unmistakable in a slab. */
static uint8_t pattern[700];
static bool contains_pattern(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + sizeof pattern <= len; ++i) if (!memcmp(buf + i, pattern, sizeof pattern)) return true;
    return false;
}
/* Wipe seam observations. Hooks fire on either node's dispatcher (any
 * scheduler thread) and, for notices/teardown, on the test thread. */
static _Atomic unsigned dirty[FIPSD_WIPE_COUNT], clean[FIPSD_WIPE_COUNT], violations, borrowed_intact;
static void on_wipe(void *user, handlers *h, fipsd_wipe_event ev, const uint8_t *bytes, size_t size, bool was_dirty)
{
    (void)user;
    if (size && !sodium_is_zero(bytes, size)) atomic_fetch_add(&violations, 1);
    atomic_fetch_add(was_dirty ? &dirty[ev] : &clean[ev], 1);
    if (ev != FIPSD_WIPE_SCRATCH || !h || !h->dataplane) return;
    /* The scratch is released right after fipsd_dp_seal copied it into a pooled
     * packet whose seal job a worker now owns: that borrowed slab must still
     * carry the datagram. Dispatcher-side read of dispatcher-written bytes the
     * worker only reads; recycle also runs on this dispatcher, later. */
    fipsd_dataplane *d = h->dataplane;
    for (size_t i = 0; i < d->packet_count; ++i) {
        fipsd_dp_packet *p = &d->packets[i];
        if (atomic_load_explicit(&p->busy, memory_order_acquire) && p->session && contains_pattern(p->input, sizeof p->input))
            atomic_fetch_add(&borrowed_intact, 1);
    }
}
/* Once the node (and its worker set) is gone every borrowed slab was recycled. */
static void expect_pool_clean(const test_node *n)
{
    fipsd_dataplane *d = n->h->dataplane; CHECK(d && d->packets);
    for (size_t i = 0; i < d->packet_count; ++i) {
        fipsd_dp_packet *p = &d->packets[i];
        CHECK(!atomic_load(&p->busy));
        CHECK(sodium_is_zero(p->input, sizeof p->input) && sodium_is_zero(p->output, sizeof p->output));
    }
}

/* Dispatcher gate (as test_control_cancel.c): a test message in the handlers'
 * command mailbox parks that dispatcher until released, so everything queued
 * before it has been handled and nothing after it runs. Parking B keeps it
 * from answering A's session setup; parking A makes A's tables readable. */
typedef struct gate { GoChannel *entered, *release; } gate;
static gate gates[2];
static bool (*real_command)(void *, void *);
static bool gated_command(void *user, void *message)
{
    for (size_t i = 0; i < 2; ++i) if (message == &gates[i]) {
        CHECK(go_channel_send(gates[i].entered, &gates[i]) == 0);
        void *token = NULL; CHECK(go_channel_receive(gates[i].release, &token) == 0 && token == &gates[i]);
        return true;
    }
    return real_command(user, message);
}
static void gate_park(fipsd_node_handlers *h, gate *g)
{
    CHECK(go_channel_send(h->commands, g) == 0);
    void *token = NULL; CHECK(go_channel_receive(g->entered, &token) == 0 && token == g);
}
static void gate_release(gate *g) { CHECK(go_channel_send(g->release, g) == 0); }
/* nt_init with the gate wrapped around the production command callback. */
static void wt_init(test_node *n, fipsd_loopback_network *net, const char *name, const char *address,
                    const char *yaml, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n);
    n->name = name;
    n->config = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(n->config);
    n->configs = go_swap_create(n->config, fipsd_config_free);
    n->h = fipsd_node_handlers_create(hcfg);
    n->netmon = (fipsd_netmon_source){.read = nt_network, .injected = true};
    fipsd_node_options options = {.workers = 2, .peer_capacity = hcfg->peer_capacity, .netmon_source = &n->netmon};
    fipsd_node_handlers_install(n->h, &options);
    real_command = options.command; options.command = gated_command;
    n->node = fipsd_node_create(n->configs, &options);
    n->transport = fipsd_loopback_transport_create(net, (fips_transport_id){1}, "loop", address, 1280);
    CHECK(n->configs && n->h && n->node && n->transport);
    n->addr = fipsd_loopback_transport_address(n->transport);
    CHECK(fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0);
}
/* A live control command returns only after the dispatcher executed it and
 * everything queued ahead of it: a mailbox barrier. */
static void barrier(test_node *n)
{
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + 30000000000u};
    json_t *params = json_pack("{s:I}", "probe_id", (json_int_t)999); /* unknown probe: a reply without side effects */
    json_t *out = p.command(p.user, "probe_poll", params, &rc);
    json_decref(params);
    CHECK(out && json_object_get(out, "status"));
    json_decref(out);
}
/* Dispatcher-owned state, read only while that dispatcher is parked. */
static bool session_established(handlers *h, const fips_node_addr *peer)
{
    fips_session_entry *e = fips_session_table_by_peer(&h->sessions, *peer);
    session_ext *x = e ? hs_session_ext(h, e) : NULL;
    return x && x->established;
}
/* Park B so no session can complete, then leave A without an established
 * session to B (an idle purge may still be due) so the next sends queue. */
static void quiesce_session(test_node *a, test_node *b)
{
    gate_park(b->h, &gates[1]);
    gate_park(a->h, &gates[0]);
    bool established = session_established(a->h, &b->identity.node_addr);
    gate_release(&gates[0]);
    if (established) { fipsd_node_notice *e = nt_await(a, FIPSD_NOTICE_SESSION_DOWN, &b->identity.node_addr, NT_WAIT_MS); CHECK(e); free(e); }
}
/* B's tree position is what lets A open a session instead of holding behind a
 * lookup. Snapshot reads between bounded notice waits, as the handlers test. */
static void await_tree_position(test_node *a, const char *npub_b)
{
    for (unsigned i = 0; i < 600; ++i) {
        json_t *peers = nt_query(a, "show_peers"), *rows = json_object_get(json_object_get(peers, "data"), "peers");
        bool found = false;
        for (size_t r = 0; r < json_array_size(rows) && !found; ++r) {
            json_t *row = json_array_get(rows, r);
            found = !strcmp(json_string_value(json_object_get(row, "npub")), npub_b) && json_boolean_value(json_object_get(row, "has_tree_position"));
        }
        json_decref(peers);
        if (found) return;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_node_handlers_notices(a->h), NULL, &v}};
        if (go_select_timeout(cases, 1, 100).selected_case == 0 && v) { nt_record(a, v); free(v); }
    }
    CHECK(!"A never learned B's tree position");
}
static void send3(test_node *n, const fips_node_addr *dest)
{
    for (unsigned i = 0; i < 3; ++i) CHECK(fipsd_node_handlers_send(n->h, dest, 7, 9, pattern, sizeof pattern) == 0);
}

int main(void)
{
    (void)nt_init; (void)nt_destroy; /* replaced by wt_init and the split teardown above */
    for (size_t i = 0; i < sizeof pattern; ++i) pattern[i] = (uint8_t)(0x21 + i % 93);
    fipsd_test_wipe_hook = on_wipe;
    for (size_t i = 0; i < 2; ++i) { gates[i].entered = go_channel_create(1); gates[i].release = go_channel_create(1); CHECK(gates[i].entered && gates[i].release); }
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); CHECK(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); CHECK(net);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    /* Two slots per destination so a third datagram drops the oldest; a long
     * hold ladder so held datagrams outlive link-up and session setup; a short
     * idle purge so an established session steps aside between scenarios; a
     * link-dead window that tolerates B being parked. */
    hcfg.pending_per_destination = 2; hcfg.session_idle_timeout_ms = 1000; hcfg.link_dead_timeout_ms = 10000;
    hcfg.timing = (fipsd_peer_timing){500, 4000, 1000, 1000, 5, 2.0};
    hcfg.lookup_attempt_timeouts_secs[0] = 15; hcfg.lookup_attempt_count = 1;
    test_node a, b;
    wt_init(&a, net, "A", "loop:a", yaml_a, &hcfg);
    wt_init(&b, net, "B", "loop:b", yaml_b, &hcfg);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    nt_identity(&a); nt_identity(&b);
    char npub_b[FIPS_NIP19_CAPACITY]; CHECK(fips_peer_identity_npub(&b.identity, npub_b, sizeof npub_b) == FIPS_IDENTITY_OK);

    /* 1. Held: identities known, coordinates not, no peer to ask (bloom miss).
     * Three datagrams each for B and a stranger: the oldest of each is dropped. */
    fips_peer_identity stranger = b.identity; stranger.node_addr.bytes[0] ^= 0x55; stranger.node_addr.bytes[15] ^= 0x55;
    fips_peer_identity stranger2 = b.identity; stranger2.node_addr.bytes[1] ^= 0x33; stranger2.node_addr.bytes[14] ^= 0x33;
    CHECK(fipsd_node_handlers_learn_identity(a.h, &b.identity) == 0);
    CHECK(fipsd_node_handlers_learn_identity(a.h, &stranger) == 0);
    CHECK(fipsd_node_handlers_learn_identity(a.h, &stranger2) == 0);
    send3(&a, &b.identity.node_addr);
    send3(&a, &stranger.node_addr);
    /* 2. Link up; wait until A can route to B by tree position. */
    CHECK(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    await_tree_position(&a, npub_b);
    /* 3. Pending: with B parked, three sends open a session and queue (oldest
     * dropped); the barrier proves they were handled before B may answer. On
     * establishment A flushes the two pending and replays the two held
     * datagrams for B, each sealed from a scratch copy into a borrowed slab. */
    quiesce_session(&a, &b);
    send3(&a, &b.identity.node_addr);
    barrier(&a);
    gate_release(&gates[1]);
    for (unsigned n = 0; n < 4; ++n) {
        e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, &a.identity.node_addr, NT_WAIT_MS);
        CHECK(e && e->src_port == 7 && e->dst_port == 9 && e->len == sizeof pattern && !memcmp(e->payload, pattern, sizeof pattern));
        fipsd_node_notice_free(e); /* the consumer's release wipes the delivered plaintext */
    }
    /* 4. Expiry: the stranger's two survivors are released as one send failure. */
    e = nt_await(&a, FIPSD_NOTICE_SEND_FAILED, &stranger.node_addr, NT_WAIT_MS); CHECK(e && e->value == 2); free(e);
    /* 5. Close and teardown: with B parked again, three more sends queue two
     * pending behind a setup B cannot answer; one datagram for stranger2 is
     * held. Shutdown closes the session with its queue (SESSION_DOWN follows the
     * wipe) and destroy frees the held array still carrying plaintext. */
    quiesce_session(&a, &b);
    send3(&a, &b.identity.node_addr);
    CHECK(fipsd_node_handlers_send(a.h, &stranger2.node_addr, 7, 9, pattern, sizeof pattern) == 0);
    barrier(&a);
    fipsd_runtime_shutdown(rt);
    e = nt_await(&a, FIPSD_NOTICE_SESSION_DOWN, &b.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    gate_release(&gates[1]);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    fipsd_node_destroy(a.node); fipsd_node_destroy(b.node);
    expect_pool_clean(&a); expect_pool_clean(&b);
    CHECK(b.h->counters.native_delivered == 4);
    nt_drain(&a); nt_drain(&b);
    unsigned before_teardown = atomic_load(&dirty[FIPSD_WIPE_TEARDOWN]);
    fipsd_node_handlers_destroy(a.h); fipsd_node_handlers_destroy(b.h);
    fipsd_loopback_transport_destroy(a.transport); fipsd_loopback_transport_destroy(b.transport);
    go_swap_free(a.configs); go_swap_free(b.configs);
    fipsd_loopback_network_destroy(net);
    for (size_t i = 0; i < 2; ++i) { go_channel_free(gates[i].entered); go_channel_free(gates[i].release); }

    nt_print_trace("wipe scenarios");
    static const char *const names[FIPSD_WIPE_COUNT] = {"pending_consumed", "pending_dropped", "pending_closed",
        "held_consumed", "held_dropped", "held_expired", "scratch", "command", "notice", "teardown"};
    for (size_t i = 0; i < FIPSD_WIPE_COUNT; ++i)
        printf("  %-16s dirty=%u clean=%u\n", names[i], atomic_load(&dirty[i]), atomic_load(&clean[i]));
    printf("  borrowed slabs intact while live: %u; non-zero bytes after a wipe: %u\n", atomic_load(&borrowed_intact), atomic_load(&violations));
    CHECK(atomic_load(&violations) == 0);
    CHECK(atomic_load(&dirty[FIPSD_WIPE_PENDING_CONSUMED]) >= 2); /* step 3 flush */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_PENDING_DROPPED]) >= 2);  /* steps 3 and 5 */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_PENDING_CLOSED]) >= 1);   /* step 5 close_all */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_HELD_CONSUMED]) >= 2);    /* step 3 replay */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_HELD_DROPPED]) >= 2);     /* step 1, B and stranger */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_HELD_EXPIRED]) >= 1);     /* step 4 */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_SCRATCH]) >= 8);          /* body and session_plain per datagram */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_COMMAND]) >= 13);         /* every HCMD_SEND */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_NOTICE]) >= 4);           /* delivered datagrams */
    CHECK(atomic_load(&dirty[FIPSD_WIPE_TEARDOWN]) > before_teardown); /* stranger2's held array */
    CHECK(atomic_load(&borrowed_intact) >= 4);
    puts("node wipe: pending consume/drop/close, held consume/drop/expire, teardown, command, notice, borrowed slab: ok");
    return 0;
}
