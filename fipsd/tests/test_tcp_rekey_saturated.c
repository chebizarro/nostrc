/* nostrc-085xw acceptance: two nodes over real TCP, with each connection's
 * data lane held saturated, still complete a link rekey (K flip on both ends)
 * within the protocol's normal deadline and keep the link up. The saturating
 * writers are host threads offering well-formed FMP frames, addressed to no
 * session index, to the transport's data lane as fast as it takes them; the
 * receiving node drops them silently (unknown index), so the only thing they
 * contend for is the sending connection's queue and socket. Each refused
 * offer is the saturation being measured, counted in data_dropped. Unlike
 * real traffic they never authenticate, so the receiver's liveness and its
 * adoption of the new epoch rest on the sender's own data-lane frames (the
 * rekey prompt, heartbeats, MMP), which stay data, as upstream
 * (nostrc-3vl7v): a refused pump backs off 1 ms, so the lane stays full and
 * refusing while those frames still meet a free slot, as the node's own
 * frames do when they are the traffic. */
#include "node_test.h"
#include "transport/tcp/internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";

typedef struct pump {
    fipsd_transport *t;
    fips_transport_addr to;
    _Atomic bool stop;
    uint64_t offered, refused;
    uint8_t frame[1280];
} pump;
static void *run_pump(void *arg)
{
    pump *p = arg;
    while (!atomic_load(&p->stop)) {
        ++p->offered;
        /* Load shaping, not synchronization: after a refusal the next offer
         * waits 1 ms, which refills the lane long before it drains. */
        if (fipsd_transport_send_lane(p->t, FIPSD_TRANSPORT_LANE_DATA, &p->to, p->frame, sizeof p->frame)) {
            ++p->refused;
            struct timespec backoff = {0, 1000000};
            (void)nanosleep(&backoff, NULL);
        }
    }
    return NULL;
}
static void pump_init(pump *p, fipsd_transport *t, const fips_transport_addr *to)
{
    memset(p, 0, sizeof *p);
    p->t = t; p->to = *to; atomic_init(&p->stop, false);
    size_t payload = sizeof p->frame - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
    p->frame[0] = 0; /* established phase */
    p->frame[2] = (uint8_t)payload; p->frame[3] = (uint8_t)(payload >> 8);
    memset(p->frame + 4, 0xff, 4); /* receiver index: no session */
}
/* The sole pooled connection's peer address (B knows A by its ephemeral port). */
static fips_transport_addr only_peer(fipsd_transport *t)
{
    tcp_instance *u = t->user;
    nsync_mu_lock(&u->mu);
    assert(u->connections && !u->connections->next);
    fips_transport_addr to = u->connections->peer;
    nsync_mu_unlock(&u->mu);
    return to;
}
static uint64_t secs_ms(fips_duration d) { return d.secs * 1000 + d.nanos / 1000000; }

int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    /* Above the rekey deadline below (37 s): the pumps' frames never
     * authenticate, so with heartbeats on the data lane (nostrc-3vl7v, as
     * upstream) a receiver starved enough to stop draining could otherwise
     * see no live frame for 30 s, a property of this synthetic load (real
     * saturating traffic is itself live), not of the lanes. */
    hcfg.link_dead_timeout_ms = 60000;
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
    /* 64 KiB socket buffers, as upstream's lane tests: the lane bypasses the
     * data queue, not bytes already in the kernel, so the kernel's share of
     * the backlog ahead of a control frame stays small. Before start. */
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        tcp_instance *u = n->transport->user;
        u->config.send_buf_size = u->config.recv_buf_size = 65536;
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
    uint64_t up = nt_ms();
    a.link_stable = b.link_stable = true; /* no link churn while saturated */

    /* Saturate both directions' data lanes from the moment the link is up. */
    pump to_b, to_a;
    pump_init(&to_b, a.transport, b.addr);
    fips_transport_addr a_at_b = only_peer(b.transport);
    pump_init(&to_a, b.transport, &a_at_b);
    pthread_t pumps[2];
    assert(pthread_create(&pumps[0], NULL, run_pump, &to_b) == 0);
    assert(pthread_create(&pumps[1], NULL, run_pump, &to_a) == 0);

    /* Normal deadline: the rekey is due `after` +/- jitter past link-up, and a
     * rekey handshake not finished within `handshake` is abandoned. */
    uint64_t due = secs_ms(rk->after) + (uint64_t)rk->jitter * 1000, deadline = up + due + secs_ms(rk->handshake);
    uint64_t rekeyed[2];
    for (size_t i = 0; i < 2; ++i) {
        test_node *n = i ? &b : &a, *other = i ? &a : &b;
        uint64_t now = nt_ms();
        e = nt_await(n, FIPSD_NOTICE_LINK_REKEYED, &other->identity.node_addr, deadline > now ? deadline - now : 0);
        assert(e && e->k); free(e);
        rekeyed[i] = nt_ms();
    }
    tcp_instance *ua = a.transport->user, *ub = b.transport->user;
    uint64_t sat_a = atomic_load(&ua->shared.diagnostics.data_dropped), sat_b = atomic_load(&ub->shared.diagnostics.data_dropped);
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
    printf("link rekey under saturation: A %llu ms, B %llu ms after link-up (due %llu ms, deadline %llu ms)\n",
        (unsigned long long)(rekeyed[0] - up), (unsigned long long)(rekeyed[1] - up),
        (unsigned long long)due, (unsigned long long)(deadline - up));
    /* The data lanes were refusing the whole time the rekey went through. */
    assert(sat_a > 0 && sat_b > 0);
    /* The link handshakes (IK and rekey msg1/msg2) took the control lane; MMP,
     * tree and heartbeats stay data, as upstream (nostrc-3vl7v). The data lane
     * was at capacity again and again (each refusal is a full queue), the
     * control lane refused at most a fifth of its frames. Without the lane, a
     * rekey message is offered to the same full queue and only gets through
     * when it wins a momentarily free slot.
     * The control lane is bounded (upstream PATH_QUEUE_DEPTH), so it still
     * refuses while the socket itself is not draining: a slow receiver (as
     * under TSan) can then cost a rekey message, recovered by the protocol's
     * resend inside the deadline above. */
    for (size_t i = 0; i < 2; ++i) {
        const fipsd_transport_diagnostics *d = &(i ? ub : ua)->shared.diagnostics;
        const pump *p = i ? &to_a : &to_b;
        uint64_t sent = atomic_load(&d->control_sent), refused = atomic_load(&d->control_dropped);
        assert(p->refused > 1000 && sent > 0 && refused * 4 <= sent);
    }
    a.link_stable = b.link_stable = false;

    assert(fipsd_node_handlers_disconnect(a.h, &b.identity.node_addr) == 0);
    e = nt_await(&a, FIPSD_NOTICE_LINK_DOWN, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_DOWN, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    nt_print_trace("TCP link rekey with both data lanes saturated");
    fipsd_runtime_destroy(rt);
    nt_destroy(&a); nt_destroy(&b);
    puts("tcp saturated rekey: data lanes refusing, link rekey K flip on both ends within the deadline, no link churn: ok");
    return 0;
}
