/* Small upstream spanning-tree scenarios through the production UDP runtime.
 * No borrowed handler state: observations use immutable queries and notices. */
#define FIPSD_TEST_UDP 1
#include "node_test.h"
#include "node/handlers/internal.h"

/* Upstream STP receiver-report fallback is 30s. This lossless local gate
 * requires convergence within that window; delivery uses chaos' 60s bound. */
#define TREE_BOUND_MS 30000u
static test_node nodes[NT_MAX_NODES];
static bool edges[NT_MAX_NODES][NT_MAX_NODES];
static char addresses[NT_MAX_NODES][33];
static size_t count;
typedef struct bloom_edge_view {
    bool present, tree, child, received, sent;
    uint64_t received_sequence, sent_sequence;
    fips_bloom_filter inbound, outbound;
} bloom_edge_view;
typedef struct bloom_view { bloom_edge_view peers[NT_MAX_NODES]; } bloom_view;
static bloom_view bloom_views[NT_MAX_NODES];
static struct { bloom_view *view; } read_bloom;
static GoChannel *bloom_done;
/* Test actions execute on the production dispatcher, never by borrowing its
 * mutable cache from the test thread. The completion channel orders results. */
static bool (*real_command)(void *, void *);
static int prepare_return_route, flood_return_route;
static GoChannel *return_route_done;
static bool multi_command(void *user, void *message)
{
    handlers *h = user;
    if (message == &read_bloom) {
        memset(read_bloom.view, 0, sizeof *read_bloom.view);
        for (size_t i = 0; i < count; ++i) {
            const fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &nodes[i].identity.node_addr);
            if (!p) continue;
            bloom_edge_view *v = &read_bloom.view->peers[i];
            v->present = true; v->tree = p->tree; v->child = p->child;
            v->received = p->has_inbound; v->received_sequence = p->inbound_sequence;
            v->sent = p->has_sequence && p->sent_len; v->sent_sequence = p->sequence;
            if (v->received) v->inbound = p->inbound;
            if (v->sent) v->outbound = p->sent[0];
        }
        assert(!go_channel_send(bloom_done, message));
        return true;
    }
    if (message != &prepare_return_route && message != &flood_return_route) return real_command(user, message);
    const fips_node_addr *initiator = &nodes[0].identity.node_addr;
    peer_rec *carrier = hs_rec_find(h, &nodes[1].identity.node_addr); assert(carrier);
    if (message == &prepare_return_route) {
        assert(!fips_session_table_by_peer(&h->sessions, *initiator));
        (void)fips_coord_cache_remove(&h->cache, initiator->bytes, NULL);
        /* Force the real setup's return hint to be unfunded, independently of
         * clock speed, scheduler order or the time spent converging the tree. */
        fipsd_peer_tokens_init(&carrier->coord_hints, 0, 0, 0, hs_now(h));
        h->dataplane->needs_publish = true;
    } else {
        fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, *initiator); assert(entry);
        session_ext *ext = hs_session_ext(h, entry);
        assert(ext && ext->established && !ext->is_initiator);
        assert(fips_coord_cache_get_entry(&h->cache, initiator->bytes));
        peer_rec unfunded = {0}; fipsd_peer_tokens_init(&unfunded.coord_hints, 0, 0, 0, hs_now(h));
        size_t hints = fips_coord_cache_probation_bound(&h->cache) + 1;
        for (size_t i = 0; i < hints; ++i) {
            fips_node_addr path[] = {{{0xab, (uint8_t)i}}, *fips_tree_state_root(h->tree)};
            fipsd_dp_hint(h, &unfunded, &path[0], (fips_fsp_coords){path[0].bytes, 2});
        }
        /* Before the fix, the initiator was still probationary after successful
         * msg3, and these unrelated hints deterministically evicted it. */
        assert(fips_coord_cache_get_entry(&h->cache, initiator->bytes));
        assert(h->dataplane->needs_publish && fipsd_dp_publish(h));
        fips_session_datagram dg; fips_session_datagram_init(&dg, h->me, *initiator, NULL, 0);
        fips_writer wire; fips_writer_init(&wire, h->route_buf, sizeof h->route_buf);
        fips_route_outcome out;
        assert(fipsd_dp_forward(h->dataplane->current.ptr, &h->router, &dg, NULL, false, hs_now(h),
            h->dataplane->coord_scratch, h->dataplane->route_peers, &wire, &out) == FIPS_OK);
        assert(out.kind == FIPS_ROUTE_Forward && !memcmp(out.next_hop.bytes, carrier->addr.bytes, 16));
    }
    assert(!go_channel_send(return_route_done, message));
    return true;
}
static void multi_options(fipsd_node_options *options)
{ real_command = options->command; options->command = multi_command; }
static void observe_bloom(void)
{
    for (size_t i = 0; i < count; ++i) {
        read_bloom.view = &bloom_views[i];
        assert(!go_channel_send(nodes[i].h->commands, &read_bloom));
        void *done = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, bloom_done, NULL, &done};
        GoSelectResult r = go_select_timeout(&c, 1, NT_WAIT_MS);
        assert(r.selected_case == 0 && done == &read_bloom);
    }
}
static void return_route_action(int *action)
{
    assert(!go_channel_send(nodes[2].h->commands, action));
    void *done = NULL;
    GoSelectCase c = {GO_SELECT_RECEIVE, return_route_done, NULL, &done};
    GoSelectResult r = go_select_timeout(&c, 1, NT_WAIT_MS);
    assert(r.selected_case == 0 && done == action);
}
static json_t *data(json_t *r) { assert(r); json_t *d = json_object_get(r, "data"); assert(d); return d; }
static const char *str(json_t *r, const char *k) { return json_string_value(json_object_get(r, k)); }
static size_t index_of(const char *addr)
{
    for (size_t i = 0; addr && i < count; ++i) if (!strcmp(addr, addresses[i])) return i;
    return count;
}
/* Each component has the smallest-address root. Parent edges must exist,
 * depths decrease exactly by one, and the self-to-root coordinates must be
 * the parent's coordinates with self prepended (not just plausible depths). */
static bool converged(void)
{
    json_t *trees[NT_MAX_NODES];
    bool ok = true;
    for (size_t i = 0; i < count; ++i) trees[i] = nt_query(&nodes[i], "show_tree");
    for (size_t i = 0; i < count; ++i) {
        bool seen[NT_MAX_NODES] = {false}; seen[i] = true;
        for (size_t pass = 0; pass < count; ++pass)
            for (size_t a = 0; a < count; ++a) for (size_t b = 0; b < count; ++b)
                if (seen[a] && edges[a][b]) seen[b] = true;
        size_t root = i, degree = 0;
        for (size_t j = 0; j < count; ++j) {
            if (seen[j] && strcmp(addresses[j], addresses[root]) < 0) root = j;
            if (edges[i][j]) ++degree;
        }
        json_t *t = data(trees[i]);
        json_t *peer_rows = json_object_get(t, "peers");
        for (size_t j = 0; j < json_array_size(peer_rows); ++j) {
            json_t *row = json_array_get(peer_rows, j);
            size_t peer = index_of(str(row, "node_addr"));
            if (peer >= count || !json_equal(json_object_get(row, "coords"), json_object_get(data(trees[peer]), "my_coords"))) ok = false;
        }
        const char *r = str(t, "root");
        json_t *coords = json_object_get(t, "my_coords");
        size_t depth = (size_t)json_integer_value(json_object_get(t, "depth"));
        if (!r || strcmp(r, addresses[root]) || json_is_true(json_object_get(t, "is_root")) != (i == root) ||
            json_array_size(coords) != depth + 1 || depth >= count ||
            index_of(json_string_value(json_array_get(coords, 0))) != i ||
            index_of(json_string_value(json_array_get(coords, depth))) != root ||
            (size_t)json_integer_value(json_object_get(t, "peer_tree_count")) != degree) ok = false;
        if (i == root) { if (depth != 0) ok = false; }
        else {
            size_t parent = index_of(str(t, "parent"));
            if (parent >= count || !edges[i][parent]) ok = false;
            else {
                json_t *pt = data(trees[parent]), *pc = json_object_get(pt, "my_coords");
                if (depth != (size_t)json_integer_value(json_object_get(pt, "depth")) + 1) ok = false;
                for (size_t k = 1; k <= depth && k < count; ++k)
                    if (!json_equal(json_array_get(coords, k), json_array_get(pc, k - 1))) ok = false;
            }
        }
        json_t *bloom = nt_query(&nodes[i], "show_bloom");
        json_t *filters = json_object_get(data(bloom), "peer_filters");
        if (json_array_size(filters) != degree) ok = false;
        for (size_t j = 0; j < degree; ++j) {
            json_t *f = json_array_get(filters, j);
            if (!json_is_true(json_object_get(f, "has_filter"))) ok = false;
            const char *role = str(f, "tree_role");
            size_t peer = index_of(str(f, "peer")), expected = 0;
            if (peer >= count || !role || !strcmp(role, "none")) continue;
            for (size_t k = 0; k < count; ++k) if (seen[k]) {
                json_t *kc = json_object_get(data(trees[k]), "my_coords");
                bool below = false;
                for (size_t x = 0; x < json_array_size(kc); ++x)
                    if (index_of(json_string_value(json_array_get(kc, x))) == (!strcmp(role, "child") ? peer : i)) below = true;
                if (below == !strcmp(role, "child")) ++expected;
            }
            /* Counts gate topology readiness, not membership: a stale
             * filter can have enough entries but still miss a target. */
            if (json_number_value(json_object_get(f, "estimated_count")) + 0.25 < (double)expected) ok = false;
        }
        json_decref(bloom);
    }
    for (size_t i = 0; i < count; ++i) json_decref(trees[i]);
    return ok;
}
static void await_tree(const char *phase, uint64_t start)
{
    while (!converged()) {
        size_t owner; fipsd_node_notice *e = nt_next(nodes, count, start + TREE_BOUND_MS, &owner);
        if (!e) { nt_print_trace(phase); fprintf(stderr, "%s: convergence deadline exceeded\n", phase); abort(); }
        fipsd_node_notice_free(e);
    }
    printf("%s: tree + adjacent filters %llu ms (bound %u ms)\n", phase, (unsigned long long)(nt_ms() - start), TREE_BOUND_MS);
    fflush(stdout);
}
/* Read immutable copies on each dispatcher. Published control/dataplane views
 * can describe different dispatch batches; even canonical filters can be from
 * before a newer announcement was sent. Match both ends' latest sequences as
 * well as the directional aggregate, so no older in-flight update can undo
 * readiness. The reply channels order the copies; notices drive the wait. */
static bool bloom_reachability(void)
{
    bool ok = true;
    observe_bloom();
    json_t *trees[NT_MAX_NODES];
    for (size_t i = 0; i < count; ++i) trees[i] = nt_query(&nodes[i], "show_tree");
    for (size_t i = 0; i < count; ++i) {
        bool covered[NT_MAX_NODES] = {false}; covered[i] = true;
        for (size_t p = 0; p < count; ++p) {
            if (!edges[i][p]) continue;
            bool child = index_of(str(data(trees[p]), "parent")) == i;
            bool parent = index_of(str(data(trees[i]), "parent")) == p;
            if (!child && !parent) continue;
            const bloom_edge_view *peer = &bloom_views[i].peers[p], *sender = &bloom_views[p].peers[i];
            if (!peer->present || !peer->tree || peer->child != child || !peer->received || !sender->sent ||
                peer->received_sequence != sender->sent_sequence || !fips_bloom_equal(&peer->inbound, &sender->outbound)) {
                ok = false;
            }
            fips_bloom_filter expected; fips_bloom_init(&expected);
            for (size_t j = 0; j < count; ++j) if (j != i) {
                json_t *coords = json_object_get(data(trees[j]), "my_coords");
                bool below = false;
                for (size_t x = 0; x < json_array_size(coords); ++x)
                    if (index_of(json_string_value(json_array_get(coords, x))) == (child ? p : i)) below = true;
                if (below == child) {
                    fips_bloom_insert(&expected, &nodes[j].identity.node_addr);
                    covered[j] = true;
                }
            }
            if (!peer->received || !fips_bloom_equal(&peer->inbound, &expected)) ok = false;
        }
        for (size_t j = 0; j < count; ++j) assert(covered[j]);
    }
    for (size_t i = 0; i < count; ++i) json_decref(trees[i]);
    return ok;
}
static void await_bloom(uint64_t start)
{
    /* A topology change and its exact directional aggregates are asynchronous.
     * Keep the ORIGINAL convergence deadline; do not retry application sends. */
    while (!bloom_reachability()) {
        size_t owner; fipsd_node_notice *e = nt_next(nodes, count, start + TREE_BOUND_MS, &owner);
        if (!e) { nt_print_trace("exact directional bloom deadline"); abort(); }
        fipsd_node_notice_free(e);
    }
    printf("exact directional bloom: %llu ms (bound %u ms)\n", (unsigned long long)(nt_ms() - start), TREE_BOUND_MS);
}
/* A cold indirect control probe is also a regression for feeding the
 * initiating lookup's outcome back into the probe machine. Data below adopts
 * the session it opens; no test-only cache seeding or sleeps. */
static bool session_ready(size_t owner, size_t peer)
{
    json_t *r = nt_query(&nodes[owner], "show_sessions"), *rows = json_object_get(data(r), "sessions");
    bool ready = false;
    for (size_t i = 0; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        const char *addr = str(row, "remote_addr"), *state = str(row, "state"), *identity = str(row, "identity");
        if (addr && state && identity && !strcmp(addr, addresses[peer]) &&
            !strcmp(state, "established") && !strcmp(identity, "proven")) ready = true;
    }
    json_decref(r); return ready;
}
static void prepare_session(size_t from, size_t to, bool require_lookup)
{
    if (!require_lookup && session_ready(from, to) && session_ready(to, from)) return;
    char npub[FIPS_NIP19_CAPACITY];
    assert(fips_peer_identity_npub(&nodes[to].identity, npub, sizeof npub) == FIPS_IDENTITY_OK);
    fipsd_control_provider provider = fipsd_node_handlers_provider(nodes[from].h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec + NT_WAIT_MS * UINT64_C(1000000)};
    json_t *params = json_pack("{s:s}", "npub", npub);
    json_t *started = provider.command(provider.user, "probe_start", params, &rc); json_decref(params);
    json_int_t id = json_integer_value(json_object_get(data(started), "probe_id")); assert(id); json_decref(started);
    uint64_t deadline = nt_ms() + NT_WAIT_MS;
    for (;;) {
        params = json_pack("{s:I}", "probe_id", id);
        json_t *r = provider.command(provider.user, "probe_poll", params, &rc); json_decref(params);
        json_t *report = json_object_get(data(r), "report"), *bloom = json_object_get(report, "bloom"), *discovery = json_object_get(report, "discovery");
        if (!strcmp(str(bloom, "verdict"), "failed")) {
            json_dumpf(report, stderr, JSON_INDENT(2));
            fprintf(stderr, "\nprobe %zu -> %zu; exact bloom ready: %d\n", from, to, bloom_reachability());
            nt_print_trace("probe bloom failed");
            abort();
        }
        json_t *session = json_object_get(report, "session");
        bool observed = json_is_true(json_object_get(session, "established")) &&
            (!strcmp(str(session, "verdict"), "ok") || !strcmp(str(session, "verdict"), "skipped"));
        if (observed && require_lookup) {
            assert(!strcmp(str(discovery, "source"), "lookup"));
            assert(!strcmp(str(discovery, "verdict"), "ok"));
            assert(json_integer_value(json_object_get(bloom, "fanout")) > 0);
        }
        bool ready = observed && session_ready(from, to) && session_ready(to, from);
        json_decref(r); if (ready) break;
        size_t owner; fipsd_node_notice *e = nt_next(nodes, count, deadline, &owner); assert(e); fipsd_node_notice_free(e);
    }
}
static void prove_lookup(void)
{
    for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j)
        if (!edges[i][j]) { prepare_session(i, j, true); return; }
}
static void deliver(size_t from, size_t to)
{
    /* Separate route/delivery from the first-write-during-XK queue contract.
     * Wait for proof on both endpoints and the diagnostic adoption baseline,
     * then send exactly once. This is completion-driven, not a timing delay. */
    prepare_session(from, to, false);
    uint8_t payload[] = {0x6d, (uint8_t)from, (uint8_t)to, 0x00, 0xff};
    assert(fipsd_node_handlers_send(nodes[from].h, &nodes[to].identity.node_addr, 1000, 2000, payload, sizeof payload) == 0);
    uint64_t deadline = nt_ms() + NT_WAIT_MS;
    for (;;) {
        size_t owner; fipsd_node_notice *e = nt_next(nodes, count, deadline, &owner);
        if (!e) {
            nt_print_trace("delivery timeout");
            fprintf(stderr, "missing datagram %zu -> %zu\n", from, to);
            for (size_t i = 0; i < count; ++i) {
                const char *queries[] = {"show_sessions", "show_cache", "show_metrics"};
                for (size_t j = 0; j < 3; ++j) {
                    json_t *q = nt_query(&nodes[i], queries[j]); char *text = json_dumps(q, JSON_COMPACT);
                    fprintf(stderr, "%s %s: %s\n", nodes[i].name, queries[j], text); free(text); json_decref(q);
                }
            }
            abort();
        }
        if (e->kind == FIPSD_NOTICE_LOOKUP_FAILED || e->kind == FIPSD_NOTICE_SEND_FAILED) {
            nt_print_trace("failed resolution");
            for (size_t i = 0; i < count; ++i) {
                json_t *q = nt_query(&nodes[i], "show_metrics"); char *text = json_dumps(q, JSON_COMPACT);
                fprintf(stderr, "%s metrics: %s\n", nodes[i].name, text); free(text); json_decref(q);
            }
            abort();
        }
        bool arrived = e->kind == FIPSD_NOTICE_DATAGRAM && owner == to;
        if (arrived) {
            assert(!memcmp(e->peer.bytes, nodes[from].identity.node_addr.bytes, 16));
            assert(e->src_port == 1000 && e->dst_port == 2000 && e->len == sizeof payload && !memcmp(e->payload, payload, sizeof payload));
        }
        fipsd_node_notice_free(e);
        if (arrived) return;
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    bool return_route = !strcmp(argv[1], "ReturnRoute");
    const char *shape = return_route ? "Line" : argv[1];
    if (return_route) { return_route_done = go_channel_create(1); assert(return_route_done); }
    assert(!strcmp(shape, "Line") || !strcmp(shape, "Star") || !strcmp(shape, "Ring") || !strcmp(shape, "FullMesh"));
    count = !strcmp(shape, "Line") ? 3 : !strcmp(shape, "Star") ? 4 : 5;
    nt_topology(edges, count, shape);
    bloom_done = go_channel_create(1); assert(bloom_done);
    (void)fipsd_test_tmpdir(NULL);
    /* Header helpers used by other tests; this test selects all owners. */
    (void)nt_await;
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false; rc.grace_ms = 20000;
    fipsd_runtime *runtime = fipsd_runtime_create(&rc); assert(runtime);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 8; hc.cache_entries = 64; hc.notice_capacity = 1024;
    /* Retain upstream's 500ms tree/bloom pacing and 1s tick. */
    char names[NT_MAX_NODES][3 * sizeof(size_t) + 2];
    for (size_t i = 0; i < count; ++i) {
        char yaml[256], key[65]; memset(key, '1' + (int)i, 64); key[64] = 0;
        snprintf(yaml, sizeof yaml, "node: {identity: {nsec: '%s', persistent: false}, control: {enabled: false}}", key);
        snprintf(names[i], sizeof names[i], "n%zu", i);
        assert(nt_init_with_options(&nodes[i], NULL, names[i], NULL, yaml, &hc, 1, multi_options));
        assert(fipsd_runtime_register(runtime, fipsd_node_subsystem(nodes[i].node)) == 0);
    }
    assert(fipsd_runtime_start(runtime) == 0);
    for (size_t i = 0; i < count; ++i) {
        void *v = NULL; GoSelectCase bound = {GO_SELECT_RECEIVE, nt_socket_bound(nodes[i].transport), NULL, &v};
        GoSelectResult r = go_select_timeout(&bound, 1, NT_WAIT_MS); assert(r.selected_case == 0 && v);
        assert(nt_socket_address(nodes[i].transport, &nodes[i].udp_addr));
        nt_identity(&nodes[i]);
        assert(fips_node_addr_format(&nodes[i].identity.node_addr, addresses[i], sizeof addresses[i]) == FIPS_IDENTITY_OK);
    }
    for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < count; ++j) if (i != j)
        assert(fipsd_node_handlers_learn_identity(nodes[i].h, &nodes[j].identity) == 0);
    uint64_t start = nt_ms();
    for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j) if (edges[i][j])
        assert(fipsd_node_handlers_connect(nodes[i].h, &nodes[j].identity, (fips_transport_id){1}, nodes[j].addr) == 0);
    await_tree(shape, start);
    /* Cold lookups require the directional filters, not just their counts.
     * Keep the original convergence deadline and wait on filter notices. */
    await_bloom(start);
    if (return_route) {
        return_route_action(&prepare_return_route);
        prepare_session(0, 2, true);
        return_route_action(&flood_return_route);
        deliver(2, 0);
        puts("responder return route survives unfunded probation churn: ok");
    } else if (strcmp(shape, "FullMesh")) {
        prove_lookup();
        for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < count; ++j) if (i != j) deliver(i, j);
        await_bloom(start);
        printf("%s: all %zu ordered address resolutions + datagrams %llu ms (delivery bound 60000 ms per pair)\n", shape, count * (count - 1), (unsigned long long)(nt_ms() - start));
    }
    /* Remove a live parent edge, including bridge cuts in line/star. Connected
     * graphs recover around the cut; partitions elect one root per component. */
    size_t a = 0, b = count;
    for (; a < count; ++a) {
        json_t *tree = nt_query(&nodes[a], "show_tree");
        if (!json_is_true(json_object_get(data(tree), "is_root"))) b = index_of(str(data(tree), "parent"));
        json_decref(tree); if (b < count) break;
    }
    assert(a < count && b < count && edges[a][b]);
    edges[a][b] = edges[b][a] = false;
    start = nt_ms();
    assert(fipsd_node_handlers_disconnect(nodes[a].h, &nodes[b].identity.node_addr) == 0);
    await_tree("removed parent edge", start);
    if (!strcmp(shape, "FullMesh")) {
        /* No sessions before the cut: verify cold lookup/routing, not stale
         * session-cache recovery (a separate routing-error contract). */
        await_bloom(start);
        prove_lookup();
        for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < count; ++j) if (i != j) deliver(i, j);
        await_bloom(start);
        printf("FullMesh minus parent edge: all 20 resolutions/datagrams, including >=2 hops: ok\n");
    }
    start = nt_ms();
    edges[a][b] = edges[b][a] = true;
    assert(fipsd_node_handlers_connect(nodes[a].h, &nodes[b].identity, (fips_transport_id){1}, nodes[b].addr) == 0);
    await_tree("restored parent edge", start);
    fipsd_runtime_shutdown(runtime); assert(fipsd_runtime_run(runtime) == FIPSD_RUN_OK);
    for (size_t i = 0; i < count; ++i) nt_drain(&nodes[i]);
    fipsd_runtime_destroy(runtime);
    for (size_t i = 0; i < count; ++i) nt_destroy(&nodes[i]);
    if (return_route) go_channel_free(return_route_done);
    go_channel_free(bloom_done);
    puts("multi-node UDP: ok");
    return 0;
}
