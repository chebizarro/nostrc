/* mDNS LAN rendezvous end to end in one process (nostrc-9nxwk.5): fipsd
 * nodes with NO configured peers, real UDP transports on 127.0.0.1, whose
 * mDNS engines share an in-memory multicast link (the fipsd_mdns_io seam)
 * standing in for one LAN: every datagram sent on interface 7 reaches every
 * member, the sender's own copy included, as IP_MULTICAST_LOOP delivers it.
 * The seam replaces only the multicast sockets; two engines in one process on
 * real multicast depend on the host's loopback behaviour, which upstream
 * also found unreliable (its matched-scope test is #[ignore]d). Real sockets
 * are exercised by two fipsd processes (README: mDNS).
 *
 *   A, B  service type T, scope S: discover each other and establish an IK
 *         link both ways (upstream matched_scope_peers_observe_each_other).
 *   C     type T, scope X: each side skips the other as cross-scope
 *         (cross_scope_advert_is_filtered) and no link forms.
 *   D     type U: never sees, and is never seen by, A/B/C
 *         (isolated_service_types_do_not_cross_feed).
 *
 * Absence is proven, not waited for: after every engine saw its own advert
 * (so its announcements were delivered to the link), a sentinel advert is
 * injected into D and into A; the receive queue is FIFO, so once an engine
 * reports the sentinel it has processed every datagram delivered before it.
 * Every wait is a bounded channel select; nothing sleeps. */
#include "node_test.h"
#include "node/handlers/internal.h"
#include "mdns/mdns.h"
#include "mdns/records.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <pthread.h>

#define LINK_IF 7u
#define NODES 4
#define WAIT_MS 30000u

/* ---- the in-memory link ------------------------------------------------ */
typedef struct endpoint { GoChannel *rx; bool joined[2]; uint8_t src[4]; bool open; } endpoint;
static pthread_mutex_t link_mu = PTHREAD_MUTEX_INITIALIZER;
static endpoint eps[NODES + 1];
static void deliver(endpoint *to, const uint8_t src[4], const uint8_t *buf, size_t len)
{
    fipsd_mdns_datagram *d = malloc(sizeof *d + len);
    assert(d);
    memset(d, 0, sizeof *d);
    d->ifindex = LINK_IF; d->len = len; memcpy(d->bytes, buf, len);
    d->src.ip.width = 4; memcpy(d->src.ip.bytes, src, 4); d->src.port = FIPSD_MDNS_PORT;
    if (go_channel_try_send(to->rx, d) != 0) free(d);
}
static int link_open(void *user, fipsd_runtime *rt, GoChannel *rx)
{
    (void)rt; endpoint *e = user;
    pthread_mutex_lock(&link_mu); e->rx = rx; e->open = true; pthread_mutex_unlock(&link_mu);
    return 0;
}
static bool link_join(void *user, uint32_t ifindex, int family, const uint8_t v4[4])
{
    (void)v4; endpoint *e = user;
    if (ifindex != LINK_IF) return false;
    pthread_mutex_lock(&link_mu); e->joined[family == AF_INET6] = true; pthread_mutex_unlock(&link_mu);
    return true;
}
static void link_leave(void *user, uint32_t ifindex, int family, const uint8_t v4[4])
{
    (void)v4; (void)ifindex; endpoint *e = user;
    pthread_mutex_lock(&link_mu); e->joined[family == AF_INET6] = false; pthread_mutex_unlock(&link_mu);
}
static bool link_send(void *user, uint32_t ifindex, int family, const uint8_t v4[4], const fipsd_mdns_sockaddr *dst,
                      const uint8_t *buf, size_t len)
{
    (void)v4; endpoint *from = user;
    if (ifindex != LINK_IF) return false;
    pthread_mutex_lock(&link_mu);
    for (size_t i = 0; i <= NODES; ++i) {
        endpoint *to = &eps[i];
        if (!to->open) continue;
        if (dst ? (dst->ip.width == 4 && !memcmp(dst->ip.bytes, to->src, 4)) : to->joined[family == AF_INET6])
            deliver(to, from->src, buf, len);
    }
    pthread_mutex_unlock(&link_mu);
    return true;
}
static void link_close(void *user)
{
    endpoint *e = user;
    pthread_mutex_lock(&link_mu); e->open = false; e->joined[0] = e->joined[1] = false; pthread_mutex_unlock(&link_mu);
}
/* The link's one interface: lan0, index 7, holding 127.0.0.1/8, so the
 * nodes' loopback UDP addresses are on the link the adverts arrive on. */
static fipsd_network_snapshot *lan_read(void *user)
{
    (void)user;
    fipsd_network_snapshot *s = calloc(1, sizeof *s);
    assert(s);
    s->interface_count = 1;
    snprintf(s->interfaces[0].name, sizeof s->interfaces[0].name, "lan0");
    s->interfaces[0].index = LINK_IF; s->interfaces[0].up = true; s->interfaces[0].carrier = true;
    s->address_count = 1;
    s->addresses[0] = (fipsd_interface_address){.index = LINK_IF, .ip = {.bytes = {127, 0, 0, 1}, .width = 4}, .prefix = 8};
    return s;
}

/* Hold each node's first UDP send (IK msg1) until BOTH mDNS auto-dials
 * have reached the transport. This forces crossing dials without sleeps,
 * configured peers, manual connects, or changing production admission. */
static struct {
    fipsd_transport *transport;
    const fipsd_transport_ops *real;
    fipsd_transport_ops ops;
    _Atomic bool sent;
} crossing[2];
static GoChannel *dial_ready, *dial_release;
static int crossing_send(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{
    size_t i = t == crossing[0].transport ? 0 : 1;
    assert(t == crossing[i].transport);
    if (!atomic_exchange(&crossing[i].sent, true)) {
        assert(go_channel_try_send(dial_ready, t) == 0);
        void *value = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, dial_release, NULL, &value};
        assert(go_select_timeout(&c, 1, WAIT_MS).selected_case == 0 && value);
    }
    return crossing[i].real->send(t, to, bytes, len);
}
static bool single_winner(const fipsd_control_snapshot *s, const void *arg)
{
    bool outbound = *(const bool *)arg;
    const char *direction = json_string_value(json_object_get(json_array_get(s->links, 0), "direction"));
    return json_array_size(s->peers) == 1 && json_array_size(s->links) == 1 && s->connection_count == 0 &&
        direction && !strcmp(direction, outbound ? "outbound" : "inbound");
}
static void assert_winner(test_node *n, bool outbound, uint64_t deadline)
{ assert(nt_await_converged(n, single_winner, &outbound, deadline)); }
static void exchange(test_node *from, test_node *to)
{
    static const uint8_t payload[] = "stable mDNS winner";
    assert(fipsd_node_handlers_send(from->h, &to->identity.node_addr, 1000, 2000, payload, sizeof payload) == 0);
    fipsd_node_notice *e = nt_await(to, FIPSD_NOTICE_DATAGRAM, &from->identity.node_addr, WAIT_MS);
    assert(e && e->len == sizeof payload && !memcmp(e->payload, payload, sizeof payload));
    free(e);
}

/* ---- events ------------------------------------------------------------ */
typedef struct log_t { fipsd_mdns_event items[512]; size_t n; } log_t;
static log_t logs[NODES];
static GoChannel *observed[NODES];
static const char *names[NODES] = {"A", "B", "C", "D"};
static void record(size_t i, fipsd_mdns_event *e)
{
    if (logs[i].n < 512) logs[i].items[logs[i].n++] = *e;
    printf("%s: kind=%d npub=%.20s scope=%s reason=%s port=%u\n", names[i], e->kind, e->npub, e->has_scope ? e->scope : "-",
           e->reason, e->addr.port);
    free(e);
}
typedef bool (*pred)(const fipsd_mdns_event *, const void *);
static const fipsd_mdns_event *find(size_t i, pred p, const void *arg)
{
    for (size_t k = 0; k < logs[i].n; ++k) if (p(&logs[i].items[k], arg)) return &logs[i].items[k];
    return NULL;
}
/* Receive node i's events until one satisfies p; NULL on the deadline. */
static const fipsd_mdns_event *await(size_t i, pred p, const void *arg, uint64_t deadline)
{
    for (;;) {
        const fipsd_mdns_event *hit = find(i, p, arg);
        if (hit) return hit;
        uint64_t now = nt_ms();
        if (now >= deadline) return NULL;
        void *value = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, observed[i], NULL, &value};
        if (go_select_timeout(&c, 1, deadline - now).selected_case != 0 || !value) return NULL;
        record(i, value);
    }
}
static void drain(size_t i) { void *v; while (go_channel_try_receive(observed[i], &v) == 0) record(i, v); }
static bool is_started(const fipsd_mdns_event *e, const void *a) { (void)a; return e->kind == FIPSD_MDNS_EVENT_STARTED; }
static bool is_own(const fipsd_mdns_event *e, const void *a) { (void)a; return e->kind == FIPSD_MDNS_EVENT_SKIPPED && !strcmp(e->reason, "own"); }
static bool discovered(const fipsd_mdns_event *e, const void *npub) { return e->kind == FIPSD_MDNS_EVENT_DISCOVERED && !strcmp(e->npub, npub); }
static bool cross_scope(const fipsd_mdns_event *e, const void *npub) { return e->kind == FIPSD_MDNS_EVENT_SKIPPED && !strcmp(e->reason, "cross-scope") && !strcmp(e->npub, npub); }
static bool mentions(const fipsd_mdns_event *e, const void *npub) { return !strcmp(e->npub, npub) && e->kind != FIPSD_MDNS_EVENT_STARTED; }

/* A FIPS advert from npub `npub` for `service`, scope `scope`, injected into
 * one endpoint as if multicast by 10.77.0.99. */
static void inject_advert(size_t to, const char *service, const char *npub, const char *scope)
{
    static fipsd_mdns_rr rr;
    uint8_t buf[FIPSD_MDNS_MAX_PACKET];
    char label[64], host[96];
    fipsd_mdns_advert_label(npub, label, sizeof label);
    snprintf(host, sizeof host, "%s.local.", label);
    fipsd_mdns_name svc, inst, h;
    assert(fipsd_mdns_name_from_text(service, &svc) && fipsd_mdns_name_prepend(label, &svc, &inst) && fipsd_mdns_name_from_text(host, &h));
    fipsd_mdns_writer w; fipsd_mdns_writer_init(&w, buf, sizeof buf, 0, FIPSD_MDNS_FLAG_QR | FIPSD_MDNS_FLAG_AA);
    fipsd_mdns_rr_ptr(&rr, &svc, &inst, 4500); assert(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    fipsd_mdns_rr_srv(&rr, &inst, 9, &h, 120, true); assert(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    fipsd_mdns_txt_entry txt[] = {{"npub", npub}, {"scope", scope}, {"v", "1"}};
    assert(fipsd_mdns_rr_txt(&rr, &inst, txt, 3, 4500, true)); assert(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ANSWER, &rr, 0, false));
    fipsd_mdns_rr_a(&rr, &h, (const uint8_t[4]){127, 0, 0, 1}, 120, true); assert(fipsd_mdns_put_rr(&w, FIPSD_MDNS_ADDITIONAL, &rr, 0, false));
    size_t n = fipsd_mdns_writer_finish(&w); assert(n);
    pthread_mutex_lock(&link_mu);
    assert(eps[to].open);
    deliver(&eps[to], (const uint8_t[4]){10, 77, 0, 99}, buf, n);
    pthread_mutex_unlock(&link_mu);
}

static const char *const yaml[NODES] = {
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false},"
    " rendezvous: {lan: {enabled: true, service_type: '_fipstest-matched._udp.local.', scope: 'scope-shared'}}}",
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false},"
    " rendezvous: {lan: {enabled: true, service_type: '_fipstest-matched._udp.local.', scope: 'scope-shared'}}}",
    "node: {identity: {nsec: '3333333333333333333333333333333333333333333333333333333333333333', persistent: true}, control: {enabled: false},"
    " rendezvous: {lan: {enabled: true, service_type: '_fipstest-matched._udp.local.', scope: 'scope-other'}}}",
    "node: {identity: {nsec: '4444444444444444444444444444444444444444444444444444444444444444', persistent: true}, control: {enabled: false},"
    " rendezvous: {lan: {enabled: true, service_type: '_fipstest-isolated._udp.local.', scope: 'scope-shared'}}}"};

int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 8; hcfg.tree_peer_capacity = 8; hcfg.session_capacity = 8; hcfg.cache_entries = 64; hcfg.notice_capacity = 256;
    static test_node nodes[NODES];
    dial_ready = go_channel_create(2); dial_release = go_channel_create(2);
    assert(dial_ready && dial_release);
    fipsd_mdns_service *mdns[NODES];
    fipsd_netmon_source lan = {.read = lan_read, .injected = true};
    for (size_t i = 0; i < NODES; ++i) {
        assert(nt_init(&nodes[i], NULL, names[i], NULL, yaml[i], &hcfg, 1));
        if (i < 2) {
            nodes[i].transport->auto_connect = true;
            crossing[i].transport = nodes[i].transport;
            crossing[i].real = nodes[i].transport->ops;
            crossing[i].ops = *crossing[i].real;
            crossing[i].ops.send = crossing_send;
            nodes[i].transport->ops = &crossing[i].ops;
        }
        mdns[i] = fipsd_mdns_service_create(nodes[i].configs, nodes[i].node, nodes[i].h);
        assert(mdns[i]);
        eps[i].src[0] = 10; eps[i].src[1] = 77; eps[i].src[2] = 0; eps[i].src[3] = (uint8_t)(1 + i);
        fipsd_mdns_io io = {.user = &eps[i], .open = link_open, .join = link_join, .leave = link_leave, .send = link_send, .close = link_close};
        fipsd_mdns_service_set_io(mdns[i], &io);
        fipsd_mdns_service_set_netmon(mdns[i], &lan);
        observed[i] = go_channel_create(1024); assert(observed[i]);
        fipsd_mdns_service_observe(mdns[i], observed[i]);
    }
    for (size_t i = 0; i < NODES; ++i) assert(fipsd_runtime_register(rt, fipsd_node_subsystem(nodes[i].node)) == 0);
    for (size_t i = 0; i < NODES; ++i) assert(fipsd_runtime_register(rt, fipsd_mdns_service_subsystem(mdns[i])) == 0);
    nt_watch_publications();
    assert(fipsd_runtime_start(rt) == 0);
    for (size_t i = 0; i < 2; ++i) {
        void *value = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, dial_ready, NULL, &value};
        assert(go_select_timeout(&c, 1, WAIT_MS).selected_case == 0 && value);
    }
    for (size_t i = 0; i < 2; ++i) assert(go_channel_try_send(dial_release, &crossing[i]) == 0);
    char npub[NODES][128];
    for (size_t i = 0; i < NODES; ++i) {
        nt_identity(&nodes[i]);
        assert(fips_peer_identity_npub(&nodes[i].identity, npub[i], sizeof npub[i]) == FIPS_IDENTITY_OK);
    }
    uint64_t deadline = nt_ms() + WAIT_MS;
    uint16_t port[NODES];
    for (size_t i = 0; i < NODES; ++i) {
        const fipsd_mdns_event *e = await(i, is_started, NULL, deadline);
        assert(e && !strcmp(e->npub, npub[i]) && e->addr.port);
        port[i] = e->addr.port;
        assert(nt_socket_address(nodes[i].transport, &nodes[i].udp_addr));
    }
    /* A and B: the matched-scope pair discovers each other at the other's
     * advertised port, on the link's interface, with the shared scope. */
    for (size_t i = 0; i < 2; ++i) {
        size_t j = 1 - i;
        const fipsd_mdns_event *e = await(i, discovered, npub[j], deadline);
        assert(e && e->has_scope && !strcmp(e->scope, "scope-shared"));
        assert(e->addr.port == port[j] && e->addr.ip.width == 4 && e->addr.ip.bytes[0] == 127);
        assert(e->iface_count == 1 && e->ifaces[0] == LINK_IF);
    }
    /* ... and an IK link forms both ways, with no configured peer. */
    for (size_t i = 0; i < 2; ++i) {
        fipsd_node_notice *up = nt_await(&nodes[i], FIPSD_NOTICE_LINK_UP, &nodes[1 - i].identity.node_addr, WAIT_MS);
        assert(up);
        free(up);
    }
    bool a_outbound = memcmp(nodes[0].identity.node_addr.bytes, nodes[1].identity.node_addr.bytes, 16) < 0;
    test_node *root = a_outbound ? &nodes[0] : &nodes[1];
    for (size_t i = 0; i < 2; ++i) {
        assert_winner(&nodes[i], i == 0 ? a_outbound : !a_outbound, deadline);
        nt_pair_state want = {.root = root, .is_root = &nodes[i] == root};
        assert(nt_await_converged(&nodes[i], nt_pair_converged, &want, deadline));
        /* Flush pre-convergence notices, then reject ANY link churn while
         * exercising this winner, not just another lucky count==1 sample. */
        nt_drain(&nodes[i]);
        nodes[i].link_stable = true;
    }
    exchange(&nodes[0], &nodes[1]);
    exchange(&nodes[1], &nodes[0]);
    /* Every engine has seen its own advert echoed back and skipped it. */
    for (size_t i = 0; i < NODES; ++i) assert(await(i, is_own, NULL, deadline));
    /* C is filtered by A and B, and filters them, as cross-scope. */
    assert(await(0, cross_scope, npub[2], deadline) && await(1, cross_scope, npub[2], deadline));
    assert(await(2, cross_scope, npub[0], deadline) && await(2, cross_scope, npub[1], deadline));
    /* Sentinels: D processes everything delivered before its sentinel; so
     * does A. Neither ever heard of the other. */
    char sentinel[128];
    {
        fips_secp *secp = NULL; fips_identity *id = NULL; uint8_t secret[32]; memset(secret, 0x5e, sizeof secret);
        assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
        assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
        assert(fips_identity_npub(id, sentinel, sizeof sentinel) == FIPS_IDENTITY_OK);
        fips_identity_destroy(id); fips_identity_destroy_secp(secp);
    }
    inject_advert(3, "_fipstest-isolated._udp.local.", sentinel, "sentinel");
    inject_advert(0, "_fipstest-matched._udp.local.", sentinel, "sentinel");
    assert(await(3, cross_scope, sentinel, deadline));
    assert(await(0, cross_scope, sentinel, deadline));
    for (size_t i = 0; i < 3; ++i) assert(!find(3, mentions, npub[i]));
    assert(!find(0, mentions, npub[3]));
    /* No link to C or D. */
    for (size_t i = 0; i < 2; ++i) {
        assert_winner(&nodes[i], i == 0 ? a_outbound : !a_outbound, deadline);
        json_t *peers = nt_query(&nodes[i], "show_peers");
        json_t *list = json_object_get(json_object_get(peers, "data"), "peers");
        assert(json_array_size(list) == 1);
        json_decref(peers);
    }
    for (size_t i = 2; i < NODES; ++i) {
        const size_t none = 0;
        assert(nt_await_converged(&nodes[i], nt_peer_count, &none, deadline));
        json_t *peers = nt_query(&nodes[i], "show_peers");
        assert(json_array_size(json_object_get(json_object_get(peers, "data"), "peers")) == 0);
        json_decref(peers);
    }
    for (size_t i = 0; i < 2; ++i) { nt_drain(&nodes[i]); nodes[i].link_stable = false; }
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    for (size_t i = 0; i < NODES; ++i) { nt_drain(&nodes[i]); drain(i); }
    nt_print_trace("mDNS discovery, four nodes on one in-memory LAN");
    assert(nodes[0].h->counters.hs_cross_keep + nodes[0].h->counters.hs_cross_swap +
           nodes[1].h->counters.hs_cross_keep + nodes[1].h->counters.hs_cross_swap > 0);
    nt_unwatch_publications();
    fipsd_runtime_destroy(rt);
    go_channel_unref(dial_ready); go_channel_unref(dial_release);
    for (size_t i = 0; i < NODES; ++i) {
        fipsd_mdns_service_destroy(mdns[i]);
        nt_destroy(&nodes[i]);
        go_channel_unref(observed[i]);
    }
    puts("mdns two nodes: discovered without configuration, IK link both ways; cross-scope and isolated types filtered: ok");
    return 0;
}
