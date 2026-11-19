/* Two in-process nodes over the simulated BLE radio (nostrc-gfi8h.6.1): no
 * configured peers, only discovery. Each node's transport scans and
 * advertises, learns the other's PSM from its advert, dials the L2CAP channel,
 * exchanges keys and announces the peer; the node auto-connects on the key
 * hint and runs IK over the link, then the spanning tree, bloom filters, an
 * XK session and datagrams both ways. Then a node-directed disconnect and
 * redial, a link loss and redial, and an adapter power cycle the transport
 * recovers from by itself through rediscovery. Both nodes auto-connect, so the
 * first IK handshakes cross over the one shared device address (nostrc-8rtmi).
 * Every wait is a notice, a transport event or a snapshot; nothing sleeps. */
#include "node_test.h"
#include "transport/transport.h"
#include "transport/ble/ble.h"
#include "node/handlers/internal.h"
#include "log.h"
#include <stdatomic.h>

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";
static fipsd_ble_air *air;
static fipsd_ble_radio *radio_of[2];
/* Armed per radio: lose that many of the FMP msg1 frames it writes next
 * (SDU mode; an FD stream bypasses the radio's filter). */
static atomic_uint lose_msg1[2];
static bool radio_filter(void *user, const uint8_t *bytes, size_t len)
{
    atomic_uint *armed = user;
    unsigned n = atomic_load(armed);
    while (n && len && bytes[0] == FIPS_FMP_PHASE_MSG1)
        if (atomic_compare_exchange_weak(armed, &n, n - 1)) return false;
    return true;
}
static json_t *data_of(json_t *response, const char *key)
{
    assert(response);
    json_t *data = json_object_get(response, "data");
    assert(data);
    return key ? json_object_get(data, key) : data;
}
static bool ble_node_init(test_node *n, const char *name, const char *yaml, const fipsd_node_handlers_config *hcfg, uint8_t dev,
                          bool auto_connect)
{
    memset(n, 0, sizeof *n);
    n->name = name;
    n->config = fipsd_config_parse(yaml, strlen(yaml), NULL);
    if (!n->config) return false;
    n->configs = go_swap_create(n->config, fipsd_config_free);
    n->h = fipsd_node_handlers_create(hcfg);
    n->netmon = (fipsd_netmon_source){.read = nt_network, .injected = true};
    fipsd_node_options options = {.workers = 2, .peer_capacity = hcfg->peer_capacity, .netmon_source = &n->netmon};
    fipsd_node_handlers_install(n->h, &options);
    n->node = fipsd_node_create(n->configs, &options);
    fipsd_ble_radio_config rc = {.device = {0x02, 0, 0, 0, 0, dev}, .send_mtu = 1280, .recv_mtu = 1280};
#ifdef FIPSD_TEST_BLE_FD
    rc.mode = FIPSD_BLE_RADIO_FD;
#endif
    fipsd_ble_radio *radio = fipsd_ble_radio_create(air, &rc);
    radio_of[dev & 1] = radio;
    if (radio) fipsd_ble_radio_set_filter(radio, radio_filter, &lose_msg1[dev & 1]);
    fipsd_ble_config c; fipsd_ble_config_default(&c);
    c.adapter = FIPSD_BLE_RADIO_ADAPTER; c.auto_connect = auto_connect; c.connect_timeout_ms = 3000; c.probe_cooldown_ms = 250;
    n->transport = radio ? fipsd_ble_create((fips_transport_id){1}, "ble", &c, fipsd_ble_radio_backend(radio)) : NULL;
    fipsd_ble_peer self = {FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0, dev}};
    if (!n->configs || !n->h || !n->node || !n->transport || !fipsd_ble_peer_to_addr(&self, &n->udp_addr)) return false;
    n->addr = &n->udp_addr;
    return fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0;
}
static void ble_node_destroy(test_node *n)
{
    fipsd_node_destroy(n->node);
    fipsd_node_handlers_destroy(n->h);
    fipsd_ble_destroy(n->transport);
    go_swap_free(n->configs);
}
/* Notices race each other (a bloom filter may be accepted before the tree
 * change is noticed): wait for a set, in any order. */
typedef struct expect { fipsd_notice_kind kind; const fips_node_addr *peer; bool seen; fipsd_node_notice *notice; } expect;
static void await_set(test_node *n, expect *want, size_t count)
{
    GoChannel *c = fipsd_node_handlers_notices(n->h);
    for (size_t left = count; left;) {
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c, NULL, &value}};
        GoSelectResult r = go_select_timeout(cases, 1, NT_WAIT_MS);
        assert(r.selected_case == 0 && value);
        fipsd_node_notice *e = value; nt_record(n, e);
        bool kept = false;
        for (size_t i = 0; i < count && !kept; ++i)
            if (!want[i].seen && want[i].kind == e->kind && (!want[i].peer || !memcmp(want[i].peer->bytes, e->peer.bytes, 16))) {
                want[i].seen = kept = true; want[i].notice = e; --left;
            }
        if (!kept) free(e);
    }
}
static void links_up(test_node *a, test_node *b)
{
    fipsd_node_notice *e = nt_await(a, FIPSD_NOTICE_LINK_UP, &b->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(b, FIPSD_NOTICE_LINK_UP, &a->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
}
static void links_down(test_node *a, test_node *b)
{
    fipsd_node_notice *e = nt_await(a, FIPSD_NOTICE_LINK_DOWN, &b->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(b, FIPSD_NOTICE_LINK_DOWN, &a->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
}
/* Pending handshake legs in a fresh view (show_connections). */
static size_t legs_of(test_node *n)
{
    assert(nt_barrier(n, nt_ms() + NT_WAIT_MS));
    GoRefPtr ref = nt_snapshot(n); assert(ref.ptr);
    size_t legs = ((const fipsd_control_snapshot *)ref.ptr)->connections_count;
    go_refptr_release(&ref);
    return legs;
}
/* A crossing settles after both LINK_UPs: the tie-break may still move the
 * link onto the winner's handshake, and a frame sent on the side it leaves is
 * dropped. Settled is the converged pair with no handshake leg left on either
 * node, so the datagrams below cross the link both ends keep. The move's own
 * LINK_DOWN/LINK_UP are then consumed, so the next step's waits see only the
 * notices that step causes. */
static bool settled_view(const fipsd_control_snapshot *s, const void *arg)
{ return s->connections_count == 0 && nt_pair_converged(s, arg); }
static void settled(test_node *a, test_node *b, const test_node *root)
{
    for (test_node *n = a; n; n = n == a ? b : NULL) {
        nt_pair_state want = {.root = root, .is_root = n == root};
        assert(nt_await_converged(n, settled_view, &want, nt_ms() + NT_WAIT_MS));
    }
    nt_drain(a); nt_drain(b);
}
/* Link lost and no handshake leg left: nothing redials until the test does,
 * so this view is stable and the notices queued until it are spent. */
static bool unlinked_view(const fipsd_control_snapshot *s, const void *arg)
{ (void)arg; return json_array_size(s->peers) == 0 && s->connections_count == 0; }
static void datagram(test_node *from, test_node *to, uint16_t sport, uint16_t dport, const char *text)
{
    assert(fipsd_node_handlers_send(from->h, &to->identity.node_addr, sport, dport, (const uint8_t *)text, strlen(text) + 1) == 0);
    fipsd_node_notice *e = nt_await(to, FIPSD_NOTICE_DATAGRAM, &from->identity.node_addr, NT_WAIT_MS); assert(e);
    assert(e->len == strlen(text) + 1 && !memcmp(e->payload, text, e->len) && e->src_port == sport && e->dst_port == dport); free(e);
}
int main(void)
{
    (void)nt_init; (void)nt_destroy; /* the shared harness's loopback constructors are not used here */
    fipsd_log *log = getenv("FIPSD_TEST_DEBUG") ? fipsd_log_create(FIPSD_LOG_DEBUG, NULL) : NULL;
    if (log) fipsd_log_set_default(log);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    air = fipsd_ble_air_create(); assert(air);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    hcfg.link_dead_timeout_ms = 30000;
    test_node a, b;
    /* Both transports scan, advertise, probe and settle their simultaneous
     * dials, and both nodes auto-connect on the key hint, so two IK handshakes
     * cross over the one channel between the two device addresses. */
    assert(ble_node_init(&a, "A", yaml_a, &hcfg, 0x0A, true) && ble_node_init(&b, "B", yaml_b, &hcfg, 0x0B, true));
    fipsd_ble_radio *ra = radio_of[0], *rb = radio_of[1];
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    nt_watch_publications();
    assert(fipsd_runtime_start(rt) == 0);
    nt_identity(&a); nt_identity(&b);
    /* 1. Discovery -> PSM -> L2CAP -> key exchange -> auto-connect -> IK. */
    links_up(&a, &b);
    assert(fipsd_ble_connected_count(a.transport) == 1 && fipsd_ble_connected_count(b.transport) == 1);
    /* 2. Tree and bloom over the link; authenticated frames verify its claim. */
    bool a_smaller = memcmp(a.identity.node_addr.bytes, b.identity.node_addr.bytes, 16) < 0;
    test_node *root = a_smaller ? &a : &b, *leaf = a_smaller ? &b : &a;
    expect at_leaf[] = {{FIPSD_NOTICE_TREE_CHANGED, NULL, false, NULL}, {FIPSD_NOTICE_FILTER_ACCEPTED, &root->identity.node_addr, false, NULL}};
    expect at_root[] = {{FIPSD_NOTICE_FILTER_ACCEPTED, &leaf->identity.node_addr, false, NULL}};
    await_set(leaf, at_leaf, 2); await_set(root, at_root, 1);
    fipsd_node_notice *e = at_leaf[0].notice;
    assert(!e->is_root && e->depth == 1 && !memcmp(e->root.bytes, root->identity.node_addr.bytes, 16));
    free(at_leaf[0].notice); free(at_leaf[1].notice); free(at_root[0].notice);
    assert(fipsd_ble_is_verified(a.transport, b.addr) == 1 && fipsd_ble_is_verified(b.transport, a.addr) == 1);
    settled(&a, &b, root);
    /* 3. XK session and datagrams both ways. */
    assert(fipsd_node_handlers_send(a.h, &b.identity.node_addr, 1000, 2000, (const uint8_t *)"hello from a", 13) == 0);
    expect up[] = {{FIPSD_NOTICE_SESSION_UP, &b.identity.node_addr, false, NULL}};
    expect got[] = {{FIPSD_NOTICE_SESSION_UP, &a.identity.node_addr, false, NULL}, {FIPSD_NOTICE_DATAGRAM, &a.identity.node_addr, false, NULL}};
    await_set(&a, up, 1); await_set(&b, got, 2);
    e = got[1].notice; assert(e->len == 13 && !memcmp(e->payload, "hello from a", 13));
    free(up[0].notice); free(got[0].notice); free(got[1].notice);
    datagram(&b, &a, 2000, 1000, "hello from b");
    /* 4. The control projection, as upstream show_transports / show_peers. */
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        nt_pair_state want = {.root = root, .is_root = n == root};
        assert(nt_await_converged(n, nt_pair_converged, &want, nt_ms() + NT_WAIT_MS));
        json_t *transports = nt_query(n, "show_transports");
        json_t *tr = json_array_get(data_of(transports, "transports"), 0), *stats = json_object_get(tr, "stats");
        assert(!strcmp(json_string_value(json_object_get(tr, "type")), "ble") && !strcmp(json_string_value(json_object_get(tr, "state")), "up"));
        assert(json_integer_value(json_object_get(tr, "mtu")) == 2048 && !json_object_get(tr, "local_addr") && !json_object_get(tr, "interface"));
        assert(json_object_size(stats) == 19 && json_integer_value(json_object_get(stats, "packets_sent")) > 0);
        assert(json_integer_value(json_object_get(stats, "packets_recv")) > 0 && json_integer_value(json_object_get(stats, "advertisements_sent")) == 1);
        assert(json_integer_value(json_object_get(stats, "connections_established")) + json_integer_value(json_object_get(stats, "connections_accepted")) >= 1);
        assert(!json_integer_value(json_object_get(stats, "pubkey_exchange_failures")) && !json_object_get(stats, "pool_inbound"));
        json_decref(transports);
        json_t *peers = nt_query(n, "show_peers");
        json_t *row = json_array_get(data_of(peers, "peers"), 0);
        assert(json_array_size(data_of(peers, "peers")) == 1 && !strcmp(json_string_value(json_object_get(row, "transport_type")), "ble"));
        json_decref(peers);
        assert(n->transport->ops->mtu(n->transport, n == &a ? b.addr : a.addr) == 1280); /* the L2CAP channel's */
    }
    /* 5. A node-directed disconnect, then the node's redial over the learned PSM. */
    assert(fipsd_node_handlers_disconnect(a.h, &b.identity.node_addr) == 0);
    links_down(&a, &b);
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    links_up(&a, &b);
    settled(&a, &b, root);
    datagram(&a, &b, 1000, 2000, "after the redial");
    /* 6. The link dies under both nodes, twice: the redial after the first loss
     * crosses the dial A's transport makes on rediscovery, and the second loss
     * lands straight after that crossing's LINK_UPs, while a leg may still be
     * pending beside the link (awaiting its msg2, or with its msg1 lost, as
     * when it rode a stream the transport replaced as a duplicate). In SDU
     * mode B's redial msg1 is lost on the air, so B surely holds a stale leg
     * at the second loss. Each loss must end every leg at once, so a redial
     * from the node that held one runs well inside the handshake timeout
     * instead of being swallowed by the stale leg. */
    uint64_t budget = hcfg.timing.handshake_timeout_ms / 4;
    for (int loss = 0; loss < 2; ++loss) {
        size_t held_a = legs_of(&a), held_b = legs_of(&b);
#ifndef FIPSD_TEST_BLE_FD
        if (loss == 0) atomic_store(&lose_msg1[1], 1);
        else assert(atomic_load(&lose_msg1[1]) == 0 && held_b == 1);
#endif
        test_node *redialer = held_a && !held_b ? &a : &b, *answerer = redialer == &a ? &b : &a;
        uint64_t lost = nt_ms();
        assert(fipsd_ble_radio_drop_links(rb) >= 1);
        assert(nt_await_converged(&a, unlinked_view, NULL, lost + budget) && nt_await_converged(&b, unlinked_view, NULL, lost + budget));
        nt_drain(&a); nt_drain(&b);
        assert(fipsd_node_handlers_connect(redialer->h, &answerer->identity, (fips_transport_id){1}, answerer->addr) == 0);
        links_up(&a, &b);
        uint64_t recovered = nt_ms() - lost;
        printf("ble link loss %d: legs held A=%zu B=%zu, %s redialled, recovered in %llu ms (timeout %llu ms)\n", loss + 1,
               held_a, held_b, redialer->name, (unsigned long long)recovered, (unsigned long long)hcfg.timing.handshake_timeout_ms);
        assert(recovered < budget);
    }
    settled(&a, &b, root);
    datagram(&b, &a, 2000, 1000, "after the link loss");
    /* 7. Adapter power cycle: absent, every link lost; present again, the
     * transport relistens, readvertises and rescans, and discovery alone
     * brings the link back. */
    fipsd_ble_radio_set_adapter(ra, true, false);
    links_down(&a, &b);
    fipsd_ble_radio_set_adapter(ra, true, true);
    links_up(&a, &b);
    settled(&a, &b, root);
    datagram(&a, &b, 1000, 2000, "after the power cycle");
    assert(fipsd_ble_radio_stats_read(ra).listens == 2 && fipsd_ble_radio_stats_read(rb).listens == 1);
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    nt_print_trace("two-node BLE proof over the simulated radio");
    nt_unwatch_publications();
    fipsd_runtime_destroy(rt);
    ble_node_destroy(&a); ble_node_destroy(&b);
    fipsd_ble_air_destroy(air);
    if (log) { fipsd_log_set_default(NULL); fipsd_log_destroy(log); }
    puts("ble two nodes: discovery, PSM, L2CAP, key exchange, auto-connect IK, tree, bloom, XK, datagrams both ways, "
         "disconnect+redial, link loss+redial, adapter off/on rediscovery: ok");
    return 0;
}
