/* Two-node loopback proof (nostrc-9keoz.6 gate): two in-process nodes with
 * seeded identities over the loopback transport, driven to IK link
 * establishment both ways, spanning-tree convergence, bloom exchange, an XK
 * session, an end-to-end datagram, one rekey cycle (K flip on both) and a
 * disconnect. Every assertion is a notice or a published control snapshot. */
#include "node_test.h"
#include "transport/transport.h"
#include "node/handlers/internal.h"

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";

static void fast_rekey(fipsd_node_handlers_config *c, const fipsd_config *cfg)
{
    fipsd_config_core_params(cfg, &c->core);
    c->has_core = true;
    /* Deployed floors are 31 s; the proof rotates within seconds. The link
     * floor stays above the machine's 5 s `young` window, inside which a fresh
     * msg1 is a re-dial rather than a rekey. */
    c->core.link_rekey.after = (fips_duration){6, 0}; c->core.link_rekey.jitter = 1;
    c->core.link_rekey.link_age = c->core.link_rekey.session_age = (fips_duration){0, 0};
    c->core.link_rekey.floor_link = c->core.link_rekey.floor_session = (fips_duration){6, 0};
    c->core.session_rekey.after = (fips_duration){3, 0}; c->core.session_rekey.jitter = 1;
    c->core.session_rekey.link_age = c->core.session_rekey.session_age = (fips_duration){0, 0};
    c->core.session_rekey.floor_link = c->core.session_rekey.floor_session = (fips_duration){0, 0};
}
static json_t *data_of(json_t *response, const char *key)
{
    assert(response);
    json_t *data = json_object_get(response, "data");
    if (!data) { char *text = json_dumps(response, 0); fprintf(stderr, "response: %s\n", text); free(text); }
    assert(data);
    return key ? json_object_get(data, key) : data;
}
int main(void)
{
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    (void)fipsd_test_tmpdir(NULL);
#endif
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    hcfg.link_dead_timeout_ms = 30000;
    fipsd_config *probe = fipsd_config_parse(yaml_a, strlen(yaml_a), NULL); assert(probe);
    fast_rekey(&hcfg, probe); fipsd_config_free(probe);
    test_node a, b;
    GoRefPtr retained_tables[2] = {{0}, {0}};
    uint8_t retained_keys[2][32];
    assert(nt_init(&a, net, "A", "loop:a", yaml_a, &hcfg, 1));
    assert(nt_init(&b, net, "B", "loop:b", yaml_b, &hcfg, 1));
    /* Metadata-only loopback interface; no host interface is created. */
#if !defined(FIPSD_TEST_UDP) && !defined(FIPSD_TEST_TCP)
    a.transport->interface_name = "loop-test";
    b.transport->interface_name = "loop-test";
#endif
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0);
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        void *value = NULL;
        GoSelectCase bound = {GO_SELECT_RECEIVE, nt_socket_bound(n->transport), NULL, &value};
        GoSelectResult result = go_select_timeout(&bound, 1, NT_WAIT_MS);
        assert(result.selected_case == 0 && value);
        assert(nt_socket_address(n->transport, &n->udp_addr));
        assert(nt_socket_count(n->transport) == 0);
    }
#endif
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        GoRefPtr status = fipsd_node_status_load(fipsd_node_status_slot(n->node));
        assert(status.ptr && ((fipsd_node_status *)status.ptr)->ready);
        go_refptr_release(&status);
    }
    nt_identity(&a); nt_identity(&b);
    /* 1. FMP IK link, both directions established. */
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    assert(nt_socket_count(a.transport) == 1);
    assert(nt_socket_count(b.transport) == 1);
#endif
    /* 2. Spanning tree: the larger address adopts the smaller as root. */
    bool a_smaller = memcmp(a.identity.node_addr.bytes, b.identity.node_addr.bytes, 16) < 0;
    test_node *root = a_smaller ? &a : &b, *leaf = a_smaller ? &b : &a;
    e = nt_await(leaf, FIPSD_NOTICE_TREE_CHANGED, NULL, NT_WAIT_MS); assert(e);
    assert(!e->is_root && e->depth == 1 && !memcmp(e->root.bytes, root->identity.node_addr.bytes, 16)); free(e);
    /* 3. Bloom filters exchanged. */
    e = nt_await(&a, FIPSD_NOTICE_FILTER_ACCEPTED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_FILTER_ACCEPTED, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        json_t *tree = nt_query(n, "show_tree");
        char root_hex[33]; (void)fips_node_addr_format(&root->identity.node_addr, root_hex, sizeof root_hex);
        assert(!strcmp(json_string_value(data_of(tree, "root")), root_hex));
        assert(json_boolean_value(data_of(tree, "is_root")) == (n == root));
        assert(json_integer_value(data_of(tree, "depth")) == (n == root ? 0 : 1));
        assert(json_array_size(data_of(tree, "my_coords")) == (size_t)(n == root ? 1 : 2));
        assert(json_integer_value(data_of(tree, "peer_tree_count")) == 1);
        json_decref(tree);
        json_t *peers = nt_query(n, "show_peers");
        assert(json_array_size(data_of(peers, "peers")) == 1);
        json_t *row = json_array_get(data_of(peers, "peers"), 0);
        assert(json_boolean_value(json_object_get(row, "has_tree_position")));
        assert(json_boolean_value(json_object_get(row, n == root ? "is_child" : "is_parent")));
        json_decref(peers);
        json_t *bloom = nt_query(n, "show_bloom");
        json_t *filters = data_of(bloom, "peer_filters");
        assert(json_array_size(filters) == 1 && json_boolean_value(json_object_get(json_array_get(filters, 0), "has_filter")));
        json_decref(bloom);
    }
    /* 4. FSP XK session and an end-to-end datagram, then the reverse direction. */
    static const uint8_t hello[] = "hello from a", reply[] = "hello from b";
    assert(fipsd_node_handlers_send(a.h, &b.identity.node_addr, 1000, 2000, hello, sizeof hello) == 0);
    e = nt_await(&a, FIPSD_NOTICE_SESSION_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_SESSION_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, &a.identity.node_addr, NT_WAIT_MS); assert(e);
    assert(e->len == sizeof hello && !memcmp(e->payload, hello, sizeof hello) && e->src_port == 1000 && e->dst_port == 2000); free(e);
    assert(fipsd_node_handlers_send(b.h, &a.identity.node_addr, 2000, 1000, reply, sizeof reply) == 0);
    e = nt_await(&a, FIPSD_NOTICE_DATAGRAM, &b.identity.node_addr, NT_WAIT_MS); assert(e);
    assert(e->len == sizeof reply && !memcmp(e->payload, reply, sizeof reply)); free(e);
    /* A reader can retain initial peer/session tables (including tag-zero
     * link keys) through the
     * later cutover/removal; no borrowed mutable pointers or recycled keys. */
    for (size_t i = 0; i < 2; ++i) {
        test_node *n = i ? &b : &a;
        retained_tables[i] = go_swap_load(n->h->dataplane->tables);
        const fipsd_dp_snapshot *table = retained_tables[i].ptr;
        assert(table && table->peer_count == 1 && table->epoch_count >= 2);
        bool initial_session = false, zero_link = false;
        for (size_t j = 0; j < table->epoch_count; ++j) {
            initial_session |= table->epochs[j].session;
            zero_link |= !table->epochs[j].session && table->epochs[j].tag == 0;
        }
        /* Link and FSP constructors share the node's tag source: the first
         * FSP epoch is not necessarily zero. Both kinds must be published. */
        assert(initial_session && zero_link);
        memcpy(retained_keys[i], table->epochs[0].recv.key.bytes, 32);
    }
    /* 5. One rekey cycle: the K bit flips on both ends (session and link). */
    e = nt_await(&a, FIPSD_NOTICE_SESSION_REKEYED, &b.identity.node_addr, NT_WAIT_MS); assert(e && e->k); free(e);
    e = nt_await(&b, FIPSD_NOTICE_SESSION_REKEYED, &a.identity.node_addr, NT_WAIT_MS); assert(e && e->k); free(e);
    /* Right after the cutover the previous keys are still draining: upstream's
     * show_sessions keeps `state: established` and says so in `is_draining`
     * (nostrc-f66sr.1 regression: C used to report a `draining` state). */
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        json_t *sessions = nt_query(n, "show_sessions");
        json_t *row = json_array_get(data_of(sessions, "sessions"), 0);
        assert(row && !strcmp(json_string_value(json_object_get(row, "state")), "established"));
        assert(json_is_true(json_object_get(row, "is_draining")) && json_is_true(json_object_get(row, "current_k_bit")));
        json_decref(sessions);
    }
    e = nt_await(&a, FIPSD_NOTICE_LINK_REKEYED, &b.identity.node_addr, NT_WAIT_MS); assert(e && e->k); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_REKEYED, &a.identity.node_addr, NT_WAIT_MS); assert(e && e->k); free(e);
    /* Traffic still flows under the new epochs. */
    assert(fipsd_node_handlers_send(a.h, &b.identity.node_addr, 1000, 2000, reply, sizeof reply) == 0);
    e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        json_t *sessions = nt_query(n, "show_sessions");
        json_t *rows = data_of(sessions, "sessions");
        assert(json_array_size(rows) == 1);
        json_t *row = json_array_get(rows, 0);
        assert(!strcmp(json_string_value(json_object_get(row, "identity")), "proven"));
        assert(json_boolean_value(json_object_get(row, "current_k_bit")));
        json_decref(sessions);
        json_t *peers = nt_query(n, "show_peers");
        json_t *row2 = json_array_get(data_of(peers, "peers"), 0);
        assert(json_boolean_value(json_object_get(row2, "current_k_bit")));
        assert(json_object_get(row2, "our_session_index"));
        json_decref(peers);
        json_t *mmp = nt_query(n, "show_mmp");
        assert(json_array_size(data_of(mmp, "peers")) == 1 && json_array_size(data_of(mmp, "sessions")) == 1);
        json_decref(mmp);
        json_t *routing = nt_query(n, "show_routing");
        assert(json_integer_value(json_object_get(json_object_get(data_of(routing, NULL), "forwarding"), "delivered_packets")) >= 1);
        json_decref(routing);
        json_t *links = nt_query(n, "show_links"); assert(json_array_size(data_of(links, "links")) == 1); json_decref(links);
        json_t *status = nt_query(n, "show_status");
        assert(json_integer_value(data_of(status, "peer_count")) == 1 && json_integer_value(data_of(status, "session_count")) == 1);
        json_decref(status);
        json_t *transports = nt_query(n, "show_transports");
        json_t *tr = json_array_get(data_of(transports, "transports"), 0);
        json_t *stats = json_object_get(tr, "stats");
        assert(!strcmp(json_string_value(json_object_get(tr, "state")), "up"));
#if !defined(FIPSD_TEST_UDP) && !defined(FIPSD_TEST_TCP)
        json_t *iface = json_object_get(tr, "interface");
        assert(!strcmp(json_string_value(json_object_get(iface, "presence")), "present"));
        assert(!json_object_get(iface, "medium") && !json_object_get(iface, "index"));
        assert(json_is_false(json_object_get(iface, "carrier")));
        assert(json_integer_value(json_object_get(iface, "binds")) == 1);
        assert(json_object_size(stats) == 0); /* upstream LiveStats::Empty */
        fips_link_stats live = fipsd_transport_stats_snapshot(&n->transport->shared->stats);
        assert(live.packets_sent > 0 && live.packets_recv > 0);
#else
        assert(!json_object_get(tr, "interface") && json_is_string(json_object_get(tr, "local_addr")));
        assert(json_integer_value(json_object_get(stats, "packets_sent")) > 0);
        assert(json_integer_value(json_object_get(stats, "packets_recv")) > 0);
        assert(json_integer_value(json_object_get(stats, "bytes_sent")) > 0);
        assert(json_integer_value(json_object_get(stats, "bytes_recv")) > 0);
        assert(!json_object_get(stats, "frames_sent") && !json_object_get(stats, "pktinfo_recv"));
        assert(json_is_integer(json_object_get(stats, "send_errors")) && json_is_integer(json_object_get(stats, "recv_errors")));
#ifdef FIPSD_TEST_UDP
        assert(json_object_size(stats) == 8 && !json_object_get(stats, "pool_inbound"));
#else
        assert(json_object_size(stats) == 15 && !json_object_get(stats, "kernel_drops"));
        assert(json_integer_value(json_object_get(stats, "pool_inbound")) +
            json_integer_value(json_object_get(stats, "pool_outbound")) == 1);
#endif
#endif
        json_decref(transports);
        json_t *cache = nt_query(n, "show_cache"); assert(data_of(cache, "entries")); json_decref(cache);
    }
    /* 6. Disconnect: both ends drop the link. */
    assert(fipsd_node_handlers_disconnect(a.h, &b.identity.node_addr) == 0);
    e = nt_await(&a, FIPSD_NOTICE_LINK_DOWN, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_DOWN, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    assert(nt_socket_count(a.transport) == 0);
    assert(nt_socket_count(b.transport) == 0);
#endif
    {
        json_t *peers = nt_query(&b, "show_peers");
        assert(json_array_size(data_of(peers, "peers")) == 0);
        json_decref(peers);
    }
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    for (size_t i = 0; i < 2; ++i) {
        const fipsd_dp_snapshot *old = retained_tables[i].ptr;
        assert(old->peer_count == 1 && !memcmp(old->epochs[0].recv.key.bytes, retained_keys[i], 32));
        assert(old != (i ? b.h : a.h)->dataplane->current.ptr);
        go_refptr_release(&retained_tables[i]);
    }
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        const fipsd_dp_stats *stats = &n->h->dataplane->stats;
        for (size_t i = 0; i < 2; ++i) {
            uint64_t sealed = atomic_load(&stats->sealed[i]), opened = atomic_load(&stats->opened[i]);
            assert(sealed && opened);
            printf("%s worker[%zu]: sealed=%llu opened=%llu\n", n->name, i,
                (unsigned long long)sealed, (unsigned long long)opened);
        }
        uint64_t marks = atomic_load(&stats->replay_marks), off = atomic_load(&stats->off_dispatcher_replay_marks);
        assert(marks > 0 && off == 0);
        printf("%s dispatcher replay_marks=%llu; off-dispatcher replay writes=%llu\n",
            n->name, (unsigned long long)marks, (unsigned long long)off);
    }
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    nt_print_trace("two-node real UDP proof");
#else
    nt_print_trace("two-node loopback proof");
#endif
    fipsd_runtime_destroy(rt);
    nt_destroy(&a); nt_destroy(&b);
    fipsd_loopback_network_destroy(net);
    puts("two nodes: link both ways, tree converged, bloom exchanged, XK session, datagrams, rekey K flip, disconnect: ok");
    return 0;
}
