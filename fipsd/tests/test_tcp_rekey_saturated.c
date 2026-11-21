/* Two nodes over real TCP must rekey within the normal protocol deadline
 * while authenticated application traffic saturates both data lanes. Unknown-
 * index junk is not a substitute: it can starve every new-key proof frame
 * without providing either authenticated liveness or evidence of cutover. */
#include "node_test.h"
#include "node/handlers/internal.h"
#include "transport/tcp/internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";

typedef struct pump {
    fipsd_node_handlers *h;
    fips_node_addr to;
    _Atomic bool stop, post_rekey;
    uint64_t offered, refused;
    uint8_t payload[1000];
} pump;
static void *run_pump(void *arg)
{
    pump *p = arg;
    while (!atomic_load(&p->stop)) {
        ++p->offered;
        p->payload[0] = atomic_load(&p->post_rekey) ? 1 : 0;
        /* Producer load shaping only, not a handshake wait or retry. */
        if (fipsd_node_handlers_send(p->h, &p->to, 2000, 2000, p->payload, sizeof p->payload)) {
            ++p->refused;
            struct timespec backoff = {0, 1000000};
            (void)nanosleep(&backoff, NULL);
        }
    }
    return NULL;
}
static void pump_init(pump *p, test_node *n, const fips_node_addr *to)
{
    memset(p, 0, sizeof *p);
    p->h = n->h; p->to = *to; atomic_init(&p->stop, false); atomic_init(&p->post_rekey, false);
}
static uint64_t secs_ms(fips_duration d) { return d.secs * 1000 + d.nanos / 1000000; }

static void disconnect_after_load(test_node *n, const fips_node_addr *peer)
{
    /* The public disconnect is deliberately nonblocking. After saturation,
     * park on its mailbox instead of racing it for a free slot or retrying. */
    handler_command *command = calloc(1, sizeof *command); assert(command);
    command->kind = HCMD_DISCONNECT; command->node = *peer;
    GoSelectCase send = {GO_SELECT_SEND, n->h->commands, command, NULL};
    GoSelectResult result = go_select_timeout(&send, 1, NT_WAIT_MS);
    assert(result.selected_case == 0 && result.ok);
}

int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    fipsd_config *probe = fipsd_config_parse(yaml_a, strlen(yaml_a), NULL); assert(probe);
    fipsd_config_core_params(probe, &hcfg.core); hcfg.has_core = true; fipsd_config_free(probe);
    /* As test_node_two_nodes.c: the link rotates within seconds, its floor
     * above the machine's 5 s `young` window. Handshake and resends keep their
     * deployed values: they are the deadline asserted below. */
    fips_rekey_config *rk = &hcfg.core.link_rekey;
    rk->after = (fips_duration){6, 0}; rk->jitter = 1;
    rk->link_age = rk->session_age = (fips_duration){0, 0};
    rk->floor_link = rk->floor_session = (fips_duration){6, 0};
    test_node a, b;
    assert(nt_init(&a, NULL, "A", NULL, yaml_a, &hcfg, 1));
    assert(nt_init(&b, NULL, "B", NULL, yaml_b, &hcfg, 1));
    /* Keep application deliveries off the lifecycle notice channels. A full
     * native sink drops payloads without hiding a rekey notice. */
    GoChannel *sinks[2] = {go_channel_create(1), go_channel_create(1)};
    assert(sinks[0] && sinks[1]);
    fipsd_node_handlers_set_native_sink(a.h, sinks[0]);
    fipsd_node_handlers_set_native_sink(b.h, sinks[1]);
    /* A small bounded data queue makes real producer bursts saturate it;
     * the kernel backlog ahead of control frames is bounded separately. */
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        tcp_instance *u = n->transport->user;
        u->config.send_buf_size = u->config.recv_buf_size = 65536;
        u->config.send_queue_depth = 4;
    }
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0);
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        void *value = NULL;
        GoSelectCase bound = {GO_SELECT_RECEIVE, nt_socket_bound(n->transport), NULL, &value};
        GoSelectResult result = go_select_timeout(&bound, 1, NT_WAIT_MS);
        assert(result.selected_case == 0 && value);
        assert(nt_socket_address(n->transport, &n->udp_addr));
    }
    nt_identity(&a); nt_identity(&b);
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    a.link_stable = b.link_stable = true; /* no link churn while saturated */

    /* FSP needs announced routing coordinates, not just an up TCP link. */
    test_node *root = memcmp(a.identity.node_addr.bytes, b.identity.node_addr.bytes, 16) < 0 ? &a : &b;
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        nt_pair_state want = {.root = root, .is_root = n == root};
        assert(nt_await_converged(n, nt_pair_converged, &want, nt_ms() + NT_WAIT_MS));
    }
    /* Drive real session setup and then continuous data in both directions. */
    pump to_b, to_a;
    pump_init(&to_b, &a, &b.identity.node_addr);
    pump_init(&to_a, &b, &a.identity.node_addr);
    /* Establish one application session before loading both directions. This
     * test targets link rekey, not crossed initial FSP handshakes. */
    assert(fipsd_node_handlers_send(a.h, &b.identity.node_addr, 2000, 2000, to_b.payload, sizeof to_b.payload) == 0);
    e = nt_await(&a, FIPSD_NOTICE_SESSION_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_SESSION_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    /* Preparation can itself cross an epoch. Fence the published view and
     * discard preparation notices before measuring a fresh flip under load. */
    bool initial_k[2];
    for (size_t i = 0; i < 2; ++i) {
        test_node *n = i ? &b : &a;
        assert(nt_barrier(n, nt_ms() + NT_WAIT_MS));
        json_t *peers = nt_query(n, "show_peers");
        json_t *rows = json_object_get(json_object_get(peers, "data"), "peers");
        assert(json_array_size(rows) == 1);
        json_t *k = json_object_get(json_array_get(rows, 0), "current_k_bit");
        assert(json_is_boolean(k)); initial_k[i] = json_is_true(k);
        json_decref(peers);
        nt_drain(n);
    }
    uint64_t load_started = nt_ms();
    tcp_instance *ua = a.transport->user, *ub = b.transport->user;
    uint64_t drops_before[2] = {atomic_load(&ua->shared.diagnostics.data_dropped),
        atomic_load(&ub->shared.diagnostics.data_dropped)};
    pthread_t pumps[2];
    assert(pthread_create(&pumps[0], NULL, run_pump, &to_b) == 0);
    assert(pthread_create(&pumps[1], NULL, run_pump, &to_a) == 0);

    /* The next rekey is due within `after` +/- jitter; its handshake retains
     * the deployed deadline. Measure from load admission, not fixture setup. */
    uint64_t due = secs_ms(rk->after) + (uint64_t)rk->jitter * 1000, deadline = load_started + due + secs_ms(rk->handshake);
    uint64_t rekeyed[2];
    for (size_t i = 0; i < 2; ++i) {
        test_node *n = i ? &b : &a, *other = i ? &a : &b;
        uint64_t now = nt_ms();
        e = nt_await(n, FIPSD_NOTICE_LINK_REKEYED, &other->identity.node_addr, deadline > now ? deadline - now : 0);
        if (!e || e->k == initial_k[i])
            fprintf(stderr, "%s: initial K=%d, rekey notice=%s K=%d after %llu ms of load\n",
                n->name, initial_k[i], e ? "present" : "missing", e ? e->k : 0,
                (unsigned long long)(nt_ms() - load_started));
        assert(e && e->k != initial_k[i]); free(e);
        rekeyed[i] = nt_ms();
    }
    uint64_t sat_a = atomic_load(&ua->shared.diagnostics.data_dropped), sat_b = atomic_load(&ub->shared.diagnostics.data_dropped);
    /* Mark new application payloads only after both cutovers. Consume old
     * deliveries rather than mistaking a retained pre-rekey packet for proof. */
    atomic_store(&to_a.post_rekey, true); atomic_store(&to_b.post_rekey, true);
    bool delivered[2] = {false, false};
    uint64_t delivery_deadline = nt_ms() + NT_WAIT_MS;
    while (!delivered[0] || !delivered[1]) {
        void *value = NULL;
        GoSelectCase cases[2] = {{GO_SELECT_RECEIVE, sinks[0], NULL, &value},
            {GO_SELECT_RECEIVE, sinks[1], NULL, &value}};
        uint64_t now = nt_ms();
        GoSelectResult result = go_select_timeout(cases, 2, delivery_deadline > now ? delivery_deadline - now : 0);
        assert(result.selected_case >= 0 && value);
        fipsd_node_notice *received = value;
        assert(received->kind == FIPSD_NOTICE_DATAGRAM && received->len == sizeof to_a.payload);
        if (received->payload[0] == 1) delivered[result.selected_case] = true;
        fipsd_node_notice_free(received);
    }
    for (size_t i = 0; i < 2; ++i) atomic_store(&(i ? &to_a : &to_b)->stop, true);
    for (size_t i = 0; i < 2; ++i) assert(pthread_join(pumps[i], NULL) == 0);
    for (size_t i = 0; i < 2; ++i) {
        tcp_instance *u = i ? ub : ua; pump *p = i ? &to_a : &to_b;
        printf("%s: pump offered=%llu refused=%llu; data_dropped=%llu control_dropped=%llu control_sent=%llu\n",
            i ? "B->A" : "A->B", (unsigned long long)p->offered, (unsigned long long)p->refused,
            (unsigned long long)atomic_load(&u->shared.diagnostics.data_dropped),
            (unsigned long long)atomic_load(&u->shared.diagnostics.control_dropped),
            (unsigned long long)atomic_load(&u->shared.diagnostics.control_sent));
    }
    printf("link rekey under saturation: A %llu ms, B %llu ms after load admission (due %llu ms, deadline %llu ms)\n",
        (unsigned long long)(rekeyed[0] - load_started), (unsigned long long)(rekeyed[1] - load_started),
        (unsigned long long)due, (unsigned long long)(deadline - load_started));
    /* Both TCP queues actually saturated, not just the command mailboxes. */
    assert(sat_a > drops_before[0] && sat_b > drops_before[1]);
    /* Keep the upstream lane classification: link handshakes use control;
     * prompts, application traffic, heartbeats and MMP remain data. */
    for (size_t i = 0; i < 2; ++i) {
        const fipsd_transport_diagnostics *d = &(i ? ub : ua)->shared.diagnostics;
        const pump *p = i ? &to_a : &to_b;
        uint64_t sent = atomic_load(&d->control_sent), refused = atomic_load(&d->control_dropped);
        assert(p->offered > p->refused && sent > 0 && refused * 4 <= sent);
    }
    a.link_stable = b.link_stable = false;

    disconnect_after_load(&a, &b.identity.node_addr);
    e = nt_await(&a, FIPSD_NOTICE_LINK_DOWN, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_DOWN, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    nt_print_trace("TCP link rekey with both data lanes saturated");
    fipsd_runtime_destroy(rt);
    fipsd_node_handlers_set_native_sink(a.h, NULL);
    fipsd_node_handlers_set_native_sink(b.h, NULL);
    for (size_t i = 0; i < 2; ++i) {
        void *value = NULL;
        while (go_channel_try_receive(sinks[i], &value) == 0) fipsd_node_notice_free(value);
        go_channel_free(sinks[i]);
    }
    nt_destroy(&a); nt_destroy(&b);
    puts("tcp saturated rekey: data lanes refusing, link rekey K flip on both ends within the deadline, no link churn: ok");
    return 0;
}
