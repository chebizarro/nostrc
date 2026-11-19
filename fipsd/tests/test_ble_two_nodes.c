/* Two in-process nodes over the simulated BLE radio (nostrc-gfi8h.6.1): no
 * configured peers, only discovery. Each node's transport scans and
 * advertises, learns the other's PSM from its advert, dials the L2CAP channel,
 * exchanges keys and announces the peer; the node auto-connects on the key
 * hint and runs IK over the link, then the spanning tree, bloom filters, an
 * XK session and datagrams both ways. Then a node-directed disconnect and
 * redial, a link loss and redial, and an adapter power cycle the transport
 * recovers from by itself through rediscovery. Both nodes auto-connect, so the
 * first IK handshakes cross over the one shared device address (nostrc-8rtmi).
 * Then the verified-link challenger (nostrc-o14sw, nostrc-z3kcb): a peer that
 * reconnects at the device address of a link the node still holds as verified
 * is held, unannounced, until Noise authenticates it as the same node, and only
 * then replaces the link, with the session intact; one that authenticates as
 * another node is dropped and the verified link carries on.
 * Every wait is a notice, a transport event or a snapshot; nothing sleeps. */
#include "node_test.h"
#include "transport/transport.h"
#include "transport/ble/ble.h"
#include "transport/ble/internal.h"
#include "node/handlers/internal.h"
#include "log.h"
#include <stdatomic.h>

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";
static const char *yaml_c =
    "node: {identity: {nsec: '3333333333333333333333333333333333333333333333333333333333333333', persistent: true}, control: {enabled: false}}";
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
typedef struct ble_spec {
    uint8_t dev;
    bool auto_connect, scan, advertise, accept;
    uint32_t connect_timeout_ms; /* also the tie window a verified link's same-address newcomer races in */
} ble_spec;
static bool ble_node_open(test_node *n, const char *name, const char *yaml, const fipsd_node_handlers_config *hcfg,
                          const ble_spec *spec, fipsd_ble_radio **radio_out)
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
    fipsd_ble_radio_config rc = {.device = {0x02, 0, 0, 0, 0, spec->dev}, .send_mtu = 1280, .recv_mtu = 1280};
#ifdef FIPSD_TEST_BLE_FD
    rc.mode = FIPSD_BLE_RADIO_FD;
#endif
    fipsd_ble_radio *radio = fipsd_ble_radio_create(air, &rc);
    *radio_out = radio;
    if (radio) fipsd_ble_radio_set_filter(radio, radio_filter, &lose_msg1[spec->dev & 1]);
    fipsd_ble_config c; fipsd_ble_config_default(&c);
    c.adapter = FIPSD_BLE_RADIO_ADAPTER; c.auto_connect = spec->auto_connect; c.scan = spec->scan; c.advertise = spec->advertise;
    c.accept_connections = spec->accept; c.connect_timeout_ms = spec->connect_timeout_ms; c.probe_cooldown_ms = 250;
    n->transport = radio ? fipsd_ble_create((fips_transport_id){1}, "ble", &c, fipsd_ble_radio_backend(radio)) : NULL;
    fipsd_ble_peer self = {FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0, spec->dev}};
    if (!n->configs || !n->h || !n->node || !n->transport || !fipsd_ble_peer_to_addr(&self, &n->udp_addr)) return false;
    n->addr = &n->udp_addr;
    return fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0;
}
static bool ble_node_init(test_node *n, const char *name, const char *yaml, const fipsd_node_handlers_config *hcfg, uint8_t dev,
                          bool auto_connect)
{
    ble_spec spec = {.dev = dev, .auto_connect = auto_connect, .scan = true, .advertise = true, .accept = true, .connect_timeout_ms = 3000};
    return ble_node_open(n, name, yaml, hcfg, &spec, &radio_of[dev & 1]);
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
static fipsd_log *test_log;
static void handlers_config(fipsd_node_handlers_config *hcfg)
{
    fipsd_node_handlers_config_default(hcfg);
    hcfg->peer_capacity = 4; hcfg->tree_peer_capacity = 4; hcfg->session_capacity = 4; hcfg->cache_entries = 64;
    hcfg->notice_capacity = 256; hcfg->tick_ms = 200; hcfg->tree_announce_min_interval_ms = 100; hcfg->bloom_debounce_ms = 100;
    hcfg->link_dead_timeout_ms = 30000;
}
static fipsd_runtime *runtime_new(void)
{
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    air = fipsd_ble_air_create(); assert(air);
    return rt;
}
static void discovery_proof(void)
{
    fipsd_runtime *rt = runtime_new();
    fipsd_node_handlers_config hcfg; handlers_config(&hcfg);
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
    puts("ble two nodes: discovery, PSM, L2CAP, key exchange, auto-connect IK, tree, bloom, XK, datagrams both ways, "
         "disconnect+redial, link loss+redial, adapter off/on rediscovery: ok");
}

/* ---- the verified-link challenger (nostrc-o14sw, nostrc-z3kcb) ---------------------------------
 * The pool and radio tests cover the transport's rule with the node simulated by hand; here two real
 * nodes run it. X, the smaller node, never dials: Y dials X, so the settled link is the one Y
 * dialled, the tie-break loser (each side keeps the link the smaller node dialled). Then X reconnects
 * at the same device address while Y still holds the old link as verified: Y's transport must keep it
 * authoritative and hold the newcomer, unannounced under an address of its own, until Noise over it
 * authenticates X, and only then replace the link. The window the newcomer races in is the transport's
 * connect timeout, set far past what a loaded sanitizer run takes. */
#define CHALLENGE_WINDOW_MS 120000u
typedef struct tally {
    const fips_node_addr *only; /* count only this peer's notices; NULL: every peer's */
    unsigned link_up, link_down, session_up, session_down, handshake_failed, datagrams;
} tally;
static void tally_note(tally *t, const fipsd_node_notice *e)
{
    if (t->only && memcmp(t->only->bytes, e->peer.bytes, 16)) return;
    t->link_up += e->kind == FIPSD_NOTICE_LINK_UP; t->link_down += e->kind == FIPSD_NOTICE_LINK_DOWN;
    t->session_up += e->kind == FIPSD_NOTICE_SESSION_UP; t->session_down += e->kind == FIPSD_NOTICE_SESSION_DOWN;
    t->handshake_failed += e->kind == FIPSD_NOTICE_HANDSHAKE_FAILED; t->datagrams += e->kind == FIPSD_NOTICE_DATAGRAM;
}
/* nt_await that keeps count of every notice it passes. */
static fipsd_node_notice *await_tallied(test_node *n, tally *t, fipsd_notice_kind kind, const fips_node_addr *peer)
{
    GoChannel *c = fipsd_node_handlers_notices(n->h);
    for (;;) {
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c, NULL, &value}};
        GoSelectResult r = go_select_timeout(cases, 1, NT_WAIT_MS);
        if (r.selected_case != 0 || !value) { fprintf(stderr, "%s: no notice %s\n", n->name, nt_kind_name(kind)); return NULL; }
        fipsd_node_notice *e = value;
        nt_record(n, e); tally_note(t, e);
        if (e->kind == kind && (!peer || !memcmp(peer->bytes, e->peer.bytes, 16))) return e;
        free(e);
    }
}
/* Everything the node has published so far, counted (after a barrier, so nothing it has done is still in flight). */
static void tally_drain(test_node *n, tally *t)
{
    assert(nt_barrier(n, nt_ms() + NT_WAIT_MS));
    void *value;
    while (go_channel_try_receive(fipsd_node_handlers_notices(n->h), &value) == 0) { nt_record(n, value); tally_note(t, value); free(value); }
}
static void datagram_tallied(test_node *from, test_node *to, tally *at_to, const char *text)
{
    assert(fipsd_node_handlers_send(from->h, &to->identity.node_addr, 1000, 2000, (const uint8_t *)text, strlen(text) + 1) == 0);
    fipsd_node_notice *e = await_tallied(to, at_to, FIPSD_NOTICE_DATAGRAM, &from->identity.node_addr); assert(e);
    assert(e->len == strlen(text) + 1 && !memcmp(e->payload, text, e->len)); free(e);
}
/* The address the node has its one peer at. */
static char *peer_addr_of(test_node *n)
{
    json_t *peers = nt_query(n, "show_peers");
    json_t *list = data_of(peers, "peers");
    assert(json_array_size(list) == 1);
    char *addr = strdup(json_string_value(json_object_get(json_array_get(list, 0), "transport_addr"))); assert(addr);
    json_decref(peers);
    return addr;
}
static uint64_t ble_stat(test_node *n, const char *key)
{
    assert(nt_barrier(n, nt_ms() + NT_WAIT_MS));
    json_t *transports = nt_query(n, "show_transports");
    json_t *stats = json_object_get(json_array_get(data_of(transports, "transports"), 0), "stats");
    uint64_t v = (uint64_t)json_integer_value(json_object_get(stats, key));
    json_decref(transports);
    return v;
}
/* The transport's own view of a device's challenger, under the pool lock. */
static bool challenger_of(test_node *holder, const fips_transport_addr *device, fips_transport_addr *alias)
{
    ble_instance *u = holder->transport->user; fipsd_ble_peer p;
    assert(fipsd_ble_peer_from_addr(device, &p));
    nsync_mu_lock(&u->mu);
    const ble_link *c = ble_pool_challenger_at(&u->pool, &p);
    if (c && alias) *alias = c->addr;
    nsync_mu_unlock(&u->mu);
    return c != NULL;
}
/* Woken by the transport's own pool wake, which a challenger's admission and a link's end both send. */
static void await_pool(test_node *n, bool (*ready)(test_node *, const void *), const void *arg)
{
    ble_instance *u = n->transport->user;
    const uint64_t deadline = nt_ms() + NT_WAIT_MS;
    while (!ready(n, arg)) {
        uint64_t now = nt_ms(); assert(now < deadline);
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, u->changed, NULL, &v}};
        assert(go_select_timeout(cases, 1, deadline - now).selected_case == 0);
    }
}
static bool has_challenger(test_node *n, const void *device) { return challenger_of(n, device, NULL); }
/* Connections the transport still runs, a closing one until it has told the node of its end. */
static size_t live_conns(test_node *n)
{
    ble_instance *u = n->transport->user; size_t k = 0;
    nsync_mu_lock(&u->mu);
    for (const ble_conn *c = u->conns; c; c = c->next) ++k;
    nsync_mu_unlock(&u->mu);
    return k;
}
static bool conns_are(test_node *n, const void *count) { return live_conns(n) == *(const size_t *)count; }
static uint64_t link_id_at(test_node *n, const fips_transport_addr *a)
{
    ble_instance *u = n->transport->user;
    nsync_mu_lock(&u->mu);
    const ble_link *l = ble_pool_get(&u->pool, a); uint64_t id = l ? l->id : 0;
    nsync_mu_unlock(&u->mu);
    return id;
}

typedef struct duo {
    fipsd_runtime *rt;
    test_node x, y; /* x: the smaller node, never dials; y: dials x */
    test_node z;    /* optional claimant: a third identity on x's device address */
    bool has_z;
    fipsd_ble_radio *rx, *ry, *rz;
    fipsd_node_handlers_config hcfg;
} duo;
_Static_assert(sizeof(duo) < 64 * 1024, "a stack fixture"); /* Windows has a 1 MB stack, shared by everything below */
/* Y dials X, the nodes converge, an XK session carries datagrams both ways: one verified link, the loser. */
static void duo_up(duo *d, bool claimant)
{
    nt_events.count = 0;
    memset(d, 0, sizeof *d);
    d->rt = runtime_new();
    handlers_config(&d->hcfg);
    if (claimant) {
        /* Made first: the air resolves a device address to the newest radio that holds it, and x and y must win it. */
        ble_spec sz = {.dev = 0x0A, .auto_connect = false, .scan = false, .advertise = false, .accept = false, .connect_timeout_ms = CHALLENGE_WINDOW_MS};
        assert(ble_node_open(&d->z, "Z", yaml_c, &d->hcfg, &sz, &d->rz));
        assert(fipsd_runtime_register(d->rt, fipsd_node_subsystem(d->z.node)) == 0);
        d->has_z = true;
    }
    ble_spec sx = {.dev = 0x0A, .auto_connect = false, .scan = false, .advertise = true, .accept = true, .connect_timeout_ms = CHALLENGE_WINDOW_MS};
    ble_spec sy = {.dev = 0x0B, .auto_connect = true, .scan = true, .advertise = true, .accept = true, .connect_timeout_ms = CHALLENGE_WINDOW_MS};
    assert(ble_node_open(&d->x, "X", yaml_b, &d->hcfg, &sx, &d->rx) && ble_node_open(&d->y, "Y", yaml_a, &d->hcfg, &sy, &d->ry));
    assert(fipsd_runtime_register(d->rt, fipsd_node_subsystem(d->x.node)) == 0);
    assert(fipsd_runtime_register(d->rt, fipsd_node_subsystem(d->y.node)) == 0);
    nt_watch_publications();
    assert(fipsd_runtime_start(d->rt) == 0);
    nt_identity(&d->x); nt_identity(&d->y);
    if (d->has_z) nt_identity(&d->z);
    assert(memcmp(d->x.identity.node_addr.bytes, d->y.identity.node_addr.bytes, 16) < 0); /* X is the smaller node */
    links_up(&d->x, &d->y);
    assert(fipsd_ble_connected_count(d->x.transport) == 1 && fipsd_ble_connected_count(d->y.transport) == 1);
    settled(&d->x, &d->y, &d->x);
    assert(fipsd_ble_is_verified(d->x.transport, d->y.addr) == 1 && fipsd_ble_is_verified(d->y.transport, d->x.addr) == 1);
    ble_instance *uy = d->y.transport->user;
    nsync_mu_lock(&uy->mu); const ble_link *l1 = ble_pool_get(&uy->pool, d->x.addr); bool y_dialled = l1 && l1->outbound; nsync_mu_unlock(&uy->mu);
    assert(y_dialled); /* the settled link is the one the larger node dialled */
    assert(fipsd_node_handlers_send(d->x.h, &d->y.identity.node_addr, 1000, 2000, (const uint8_t *)"hello from x", 13) == 0);
    expect up[] = {{FIPSD_NOTICE_SESSION_UP, &d->y.identity.node_addr, false, NULL}};
    expect got[] = {{FIPSD_NOTICE_SESSION_UP, &d->x.identity.node_addr, false, NULL}, {FIPSD_NOTICE_DATAGRAM, &d->x.identity.node_addr, false, NULL}};
    await_set(&d->x, up, 1); await_set(&d->y, got, 2);
    free(up[0].notice); free(got[0].notice); free(got[1].notice);
    datagram(&d->y, &d->x, 2000, 1000, "hello from y");
    nt_drain(&d->x); nt_drain(&d->y);
}
static void duo_down(duo *d)
{
    fipsd_runtime_shutdown(d->rt);
    assert(fipsd_runtime_run(d->rt) == FIPSD_RUN_OK);
    nt_drain(&d->x); nt_drain(&d->y);
    if (d->has_z) nt_drain(&d->z);
    nt_unwatch_publications();
    fipsd_runtime_destroy(d->rt);
    ble_node_destroy(&d->x); ble_node_destroy(&d->y);
    if (d->has_z) ble_node_destroy(&d->z);
    fipsd_ble_air_destroy(air);
}
static bool addr_text_is(const char *text, const fips_transport_addr *a)
{ return strlen(text) == a->len && !memcmp(text, a->bytes, a->len); }
/* The newcomer's alias is held beside the verified link: unproven, not a peer address, unknown to the node. */
static void assert_held(duo *d, const fips_transport_addr *alias, const char *peer_addr, uint64_t replaced, uint64_t declined)
{
    test_node *x = &d->x, *y = &d->y;
    fipsd_ble_peer nobody;
    assert(!fipsd_ble_peer_from_addr(alias, &nobody) && !addr_text_is(peer_addr, alias));
    assert(fipsd_ble_connected_count(y->transport) == 2);
    assert(fipsd_ble_is_verified(y->transport, x->addr) == 1 && fipsd_ble_is_verified(y->transport, alias) == 0);
    char *bound = peer_addr_of(y);
    assert(!strcmp(bound, peer_addr) && legs_of(y) == 0);
    free(bound);
    assert(ble_stat(y, "duplicate_link_replacements") == replaced && ble_stat(y, "duplicate_node_declines") == declined);
}
/* X reconnects at its own device address while Y still holds the old link as verified. */
static void challenger_promoted(void)
{
    duo d; duo_up(&d, false);
    test_node *x = &d.x, *y = &d.y;
    tally tx = {.only = &y->identity.node_addr}, ty = {.only = &x->identity.node_addr};
    char *old_addr = peer_addr_of(y);
    assert(addr_text_is(old_addr, x->addr));
    const uint64_t replaced = ble_stat(y, "duplicate_link_replacements"), declined = ble_stat(y, "duplicate_node_declines");
    const uint64_t old_link = link_id_at(y, x->addr);
    /* X's end of the link is lost and Y's is not, as when X restarts inside Y's supervision timeout. */
    assert(fipsd_ble_radio_orphan_links(d.rx) == 1);
    fipsd_node_notice *e = await_tallied(x, &tx, FIPSD_NOTICE_LINK_DOWN, &y->identity.node_addr); assert(e); free(e);
    assert(fipsd_ble_connected_count(y->transport) == 1 && fipsd_ble_is_verified(y->transport, x->addr) == 1 && link_id_at(y, x->addr) == old_link);
    /* X dials Y again at the same device address inside the tie window, and the newcomer wins the tie-break. */
    assert(x->transport->ops->connect_async(x->transport, y->addr) == 0);
    await_pool(y, has_challenger, x->addr);
    fips_transport_addr alias;
    assert(challenger_of(y, x->addr, &alias));
    assert_held(&d, &alias, old_addr, replaced, declined);
    /* The node's Noise over the newcomer: X handshakes as the same identity. */
    assert(fipsd_node_handlers_connect(x->h, &y->identity, (fips_transport_id){1}, y->addr) == 0);
    e = await_tallied(x, &tx, FIPSD_NOTICE_LINK_UP, &y->identity.node_addr); assert(e); free(e);
    e = await_tallied(y, &ty, FIPSD_NOTICE_LINK_UP, &x->identity.node_addr); assert(e); free(e);
    /* Promoted: the old link is dropped (and the node told, nothing left there to end), the newcomer is the verified link. */
    size_t one = 1;
    await_pool(y, conns_are, &one);
    assert(fipsd_ble_connected_count(y->transport) == 1 && !challenger_of(y, x->addr, NULL));
    assert(fipsd_ble_is_verified(y->transport, &alias) == 1 && fipsd_ble_is_verified(y->transport, x->addr) == -1 && link_id_at(y, x->addr) == 0);
    assert(ble_stat(y, "duplicate_link_replacements") == replaced + 1 && ble_stat(y, "duplicate_node_declines") == declined);
    char *moved = peer_addr_of(y), *at_x = peer_addr_of(x);
    assert(addr_text_is(moved, &alias) && addr_text_is(at_x, y->addr));
    free(moved); free(at_x); free(old_addr);
    /* The XK session rides the new link: data both ways, and nothing but the one link-level re-handshake. */
    datagram_tallied(y, x, &tx, "after the promotion, y to x");
    datagram_tallied(x, y, &ty, "after the promotion, x to y");
    tally_drain(x, &tx); tally_drain(y, &ty);
    assert(tx.link_down == 1 && tx.link_up == 1 && ty.link_down == 1 && ty.link_up == 1); /* X lost its link and has the new one; Y swapped its peer's */
    assert(!tx.session_up && !tx.session_down && !ty.session_up && !ty.session_down && !tx.handshake_failed && !ty.handshake_failed);
    assert(tx.datagrams == 1 && ty.datagrams == 1);
    assert(fipsd_ble_is_verified(x->transport, y->addr) == 1);
    nt_print_trace("challenger promoted after the node's Noise as the same identity");
    duo_down(&d);
    puts("ble two nodes challenger: verified link kept, same-address newcomer held unannounced, promoted only by the node's Noise "
         "as the same identity, old link dropped, session survived: ok");
}
/* The air's wake on every channel opened or freed: until `radio` has had one channel, and none is left. */
static void await_channel_cycle(fipsd_ble_radio *radio)
{
    const uint64_t deadline = nt_ms() + NT_WAIT_MS;
    while (fipsd_ble_radio_stats_read(radio).channels != 1 || fipsd_ble_radio_channels(radio) != 0) {
        uint64_t now = nt_ms(); assert(now < deadline);
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_ble_air_changed(air), NULL, &v}};
        assert(go_select_timeout(cases, 1, deadline - now).selected_case == 0);
    }
}
/* Someone else at X's device address: first under its own name, then claiming to be X and authenticating as itself. */
static void challenger_refused(void)
{
    duo d; duo_up(&d, true);
    test_node *x = &d.x, *y = &d.y, *z = &d.z;
    tally tx = {.only = &y->identity.node_addr}, ty = {.only = &x->identity.node_addr}, tz = {0};
    char *old_addr = peer_addr_of(y);
    assert(addr_text_is(old_addr, x->addr));
    const uint64_t replaced = ble_stat(y, "duplicate_link_replacements"), declined = ble_stat(y, "duplicate_node_declines");
    const uint64_t old_link = link_id_at(y, x->addr);
    /* Under its own name the address is occupied by another node: the channel is refused at once, no challenger, no stat. */
    void *bound = NULL;
    GoSelectCase up[] = {{GO_SELECT_RECEIVE, fipsd_ble_bound(z->transport), NULL, &bound}};
    assert(go_select_timeout(up, 1, NT_WAIT_MS).selected_case == 0);
    assert(z->transport->ops->connect_async(z->transport, y->addr) == 0);
    await_channel_cycle(d.rz);
    assert(fipsd_ble_connected_count(y->transport) == 1 && !challenger_of(y, x->addr, NULL) && link_id_at(y, x->addr) == old_link);
    assert(ble_stat(y, "duplicate_link_replacements") == replaced && ble_stat(y, "duplicate_node_declines") == declined);
    /* The unauthenticated part of a claim: Z's pre-handshake key exchange now names X. Its Noise identity stays its own.
     * Z's transport has bound by now, so its start-up no longer writes the key. */
    ((ble_instance *)z->transport->user)->local_key = ((ble_instance *)x->transport->user)->local_key;
    assert(z->transport->ops->connect_async(z->transport, y->addr) == 0);
    await_pool(y, has_challenger, x->addr);
    fips_transport_addr alias;
    assert(challenger_of(y, x->addr, &alias));
    assert_held(&d, &alias, old_addr, replaced, declined);
    /* The verified link goes on carrying traffic while the claim waits. */
    datagram_tallied(y, x, &tx, "while the claim waits, y to x");
    datagram_tallied(x, y, &ty, "while the claim waits, x to y");
    /* Z's Noise over the newcomer authenticates it as Z, not X: dropped, and the node is told of its end. */
    assert(fipsd_node_handlers_connect(z->h, &y->identity, (fips_transport_id){1}, y->addr) == 0);
    fipsd_node_notice *e = await_tallied(y, &ty, FIPSD_NOTICE_LINK_DOWN, &z->identity.node_addr); assert(e); free(e);
    size_t one = 1, none = 0;
    await_pool(y, conns_are, &one);
    await_pool(z, conns_are, &none);
    assert(fipsd_ble_connected_count(y->transport) == 1 && !challenger_of(y, x->addr, NULL));
    assert(fipsd_ble_is_verified(y->transport, x->addr) == 1 && link_id_at(y, x->addr) == old_link);
    assert(ble_stat(y, "duplicate_node_declines") == declined + 1 && ble_stat(y, "duplicate_link_replacements") == replaced);
    char *still = peer_addr_of(y);
    assert(!strcmp(still, old_addr));
    free(still); free(old_addr);
    datagram_tallied(y, x, &tx, "after the refusal, y to x");
    datagram_tallied(x, y, &ty, "after the refusal, x to y");
    tally_drain(x, &tx); tally_drain(y, &ty); tally_drain(z, &tz);
    /* X and Y never saw a link or session change: the verified link was never in question. */
    assert(!tx.link_up && !tx.link_down && !ty.link_up && !ty.link_down && !tx.session_up && !tx.session_down && !ty.session_up && !ty.session_down);
    assert(!tx.handshake_failed && !ty.handshake_failed && tx.datagrams == 2 && ty.datagrams == 2);
    nt_print_trace("challenger refused: claimed X, authenticated as Z");
    duo_down(&d);
    puts("ble two nodes challenger: same-address claimant of another identity dropped (named, or claiming X and authenticating as itself), "
         "verified link and session untouched: ok");
}

int main(void)
{
    (void)nt_init; (void)nt_destroy; /* the shared harness's loopback constructors are not used here */
    test_log = getenv("FIPSD_TEST_DEBUG") ? fipsd_log_create(FIPSD_LOG_DEBUG, NULL) : NULL;
    if (test_log) fipsd_log_set_default(test_log);
    discovery_proof();
    challenger_promoted();
    challenger_refused();
    if (test_log) { fipsd_log_set_default(NULL); fipsd_log_destroy(test_log); }
    return 0;
}
