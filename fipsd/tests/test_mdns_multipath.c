/* mDNS multipath intake end to end (nostrc-tpgwq; upstream Feed::Mdns): two
 * fipsd nodes, each with two real UDP transports on 127.0.0.1 — transport 1
 * "lan" (medium Wired, the lowest id, so its port is the one mDNS
 * advertises) and transport 2 "wan" (medium Wireless) — and mDNS engines on
 * an in-memory multicast link (the fipsd_mdns_io seam) standing in for the
 * LAN, through each node's one network monitor.
 *
 *   1. The link is gated (datagrams held) while A dials B over the WAN
 *      transport, so the peers are linked before either hears an advert.
 *   2. The gate opens: each engine resolves the other's advert of an
 *      already-linked peer and hands it to the path intake instead of the
 *      dial path. A's Bundle gains (lan, B's advertised address) as a
 *      candidate, and it goes live through real probes over the LAN sockets.
 *      No mDNS dial and no second link happen.
 *   3. Preference: only path measurements and time are injected (equal RTTs,
 *      a clock skew past the switch dwell); the production Bundle reranks on
 *      a medium-change poll, dwells, and switches A to the Wired LAN path,
 *      and datagrams cross both ways (A to B over the LAN path).
 *
 * B is pinned to its WAN path (the production path_pin command) before the
 * adverts arrive, so only A switches: when B's own Bundle switches to its LAN
 * path on real samples while A switches, A's datagram to B can be lost (4 of
 * ~20 runs before the pin, each with B's own switch): that is the switch and
 * rebind path's, not the intake's, and is reported separately.
 *
 * Every wait is a bounded channel select; nothing sleeps. */
#include "node_test.h"
#include "node/handlers/internal.h"
#include "mdns/mdns.h"
#include "host_os.h"
#include "transport/watcher.h"
#include <pthread.h>

#define LINK_IF 7u
#define WAIT_MS 30000u
#define HELD_MAX 1024u

/* ---- the gated in-memory link ------------------------------------------ */
typedef struct endpoint { GoChannel *rx; bool joined[2]; uint8_t src[4]; bool open; } endpoint;
static pthread_mutex_t link_mu = PTHREAD_MUTEX_INITIALIZER;
static endpoint eps[2];
static bool gated = true;
static struct { endpoint *to; fipsd_mdns_datagram *d; } held[HELD_MAX];
static size_t held_count;
static void deliver(endpoint *to, const uint8_t src[4], const uint8_t *buf, size_t len)
{
    fipsd_mdns_datagram *d = malloc(sizeof *d + len);
    assert(d);
    memset(d, 0, sizeof *d);
    d->ifindex = LINK_IF; d->len = len; memcpy(d->bytes, buf, len);
    d->src.ip.width = 4; memcpy(d->src.ip.bytes, src, 4); d->src.port = FIPSD_MDNS_PORT;
    if (gated) {
        if (held_count < HELD_MAX) { held[held_count].to = to; held[held_count++].d = d; }
        else free(d); /* a lossy link: mDNS repeats itself */
        return;
    }
    if (!to->open || go_channel_try_send(to->rx, d) != 0) free(d);
}
static void open_gate(void)
{
    pthread_mutex_lock(&link_mu);
    gated = false;
    for (size_t i = 0; i < held_count; ++i)
        if (!held[i].to->open || go_channel_try_send(held[i].to->rx, held[i].d) != 0) free(held[i].d);
    held_count = 0;
    pthread_mutex_unlock(&link_mu);
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
    for (size_t i = 0; i < 2; ++i) {
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
/* The node's netmon: lan0, index 7, holding 127.0.0.1/8. */
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
/* The transports' presence facts: only the medium differs. */
static fipsd_presence_facts wired(void *u) { (void)u; return (fipsd_presence_facts){.valid = true, .exists = true, .up = true, .carrier = true, .index = LINK_IF, .medium = FIPS_PATH_Wired}; }
static fipsd_presence_facts wireless(void *u) { (void)u; return (fipsd_presence_facts){.valid = true, .exists = true, .up = true, .carrier = true, .index = LINK_IF, .medium = FIPS_PATH_Wireless}; }

/* ---- A's clock: host monotonic plus a skew the test advances ----------- */
static _Atomic uint64_t skew_ms;
static fips_instant skewed(void *ctx)
{
    (void)ctx;
    fips_clock host = fipsd_host_os_clock();
    fips_instant now = host.now(host.ctx), out;
    return now.ops->checked_add(now, fips_duration_from_millis(atomic_load(&skew_ms)), &out) == FIPS_OK ? out : now;
}

/* ---- nodes ------------------------------------------------------------- */
static test_node nodes[2];
static fipsd_transport *lan[2];
static fipsd_netmon_source lan_source = {.read = lan_read, .injected = true};
static const fipsd_watcher_source wired_source = {.read = wired}, wireless_source = {.read = wireless};
static struct {
    handlers *h;
    bool (*poll)(void *, fips_instant);
    bool (*command)(void *, void *);
    GoChannel *changed;
    uint64_t signature;
} hooks[2];
static size_t index_of(void *user) { size_t i = user == hooks[0].h ? 0 : 1; assert(user == hooks[i].h); return i; }
/* Signal a path-view transition of the node's one peer (not reads or ticks). */
static bool on_poll(void *user, fips_instant now)
{
    size_t i = index_of(user);
    bool ok = hooks[i].poll(user, now);
    handlers *h = user;
    uint64_t signature = 0;
    for (size_t k = 0; k < h->cfg.peer_capacity; ++k) {
        peer_rec *rec = &h->recs[k];
        if (!rec->used || !rec->path_live) continue;
        fips_path_bundle_view view; fips_path_bundle_view_get(rec->path, now, &view);
        signature = view.len + 1;
        for (size_t p = 0; p < view.len; ++p)
            signature = signature * 131 + view.paths[p].to.transport.value * 16 + (unsigned)view.paths[p].state * 2 + view.paths[p].active;
        break;
    }
    if (signature != hooks[i].signature) { hooks[i].signature = signature; (void)go_channel_try_send(hooks[i].changed, user); }
    return ok;
}
/* Dispatcher-side preference steps on A: inject equal measurements on every
 * path (as test_node_path_medium does), then step the Bundle with the poll a
 * medium change gives (hs_path_network_event, production step/run_outputs). */
typedef struct verdict { unsigned active; size_t paths; bool dwelling; uint64_t switched; } verdict;
static int measure_command, poll_command;
static GoChannel *verdicts;
static bool on_command(void *user, void *message)
{
    size_t i = index_of(user);
    if (message != &measure_command && message != &poll_command) return hooks[i].command(user, message);
    handlers *h = user;
    peer_rec *rec = hs_rec_find(h, &nodes[1 - i].identity.node_addr);
    assert(rec && rec->path_live);
    if (message == &measure_command)
        for (size_t p = 0; p < rec->path->set.len; ++p) {
            fips_peer_path *path = &rec->path->set.paths[p];
            path->state = FIPS_PATH_Live; path->acked = true;
            fips_path_liveness_new(&path->live);
            for (unsigned j = 0; j < 2; ++j)
                fips_path_liveness_sample(&path->live, hs_now(h), fips_duration_from_millis(20),
                    fips_duration_from_millis(30000), fips_duration_from_millis(60000));
            path->live.next_at = fips_deadline(hs_now(h), fips_duration_from_millis(1000000));
        }
    fipsd_netmon_event medium = {.kind = FIPSD_BINDING_MEDIUM_CHANGED, .transport = {1}};
    hs_path_network_event(h, &medium);
    verdict *v = calloc(1, sizeof *v); assert(v);
    const fips_peer_path *active = fips_path_set_active(&rec->path->set); assert(active);
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    v->active = active->transport_id.value; v->paths = rec->path->set.len;
    v->dwelling = rec->path->dwell_at.ops != NULL; v->switched = h->counters.path_switched;
    /* The peer table sends where the Bundle's active path is. */
    assert(info->transport.value == v->active && fips_transport_addr_equal(&info->addr, &active->addr));
    assert(go_channel_try_send(verdicts, v) == 0);
    return true;
}
static verdict call(test_node *n, int *command)
{
    assert(go_channel_send(n->h->commands, command) == 0);
    void *value = NULL;
    GoSelectCase c = {GO_SELECT_RECEIVE, verdicts, NULL, &value};
    assert(go_select_timeout(&c, 1, WAIT_MS).selected_case == 0 && value);
    verdict v = *(verdict *)value; free(value);
    return v;
}
static fipsd_transport *udp(uint32_t id, const char *name, const fipsd_watcher_source *facts)
{
    fipsd_udp_config c; fipsd_udp_config_default(&c);
    c.bind_addr = "127.0.0.1:0"; c.mtu = 1280; c.accept_connections = true; c.watcher_source = facts;
    fipsd_transport *t = fipsd_udp_create((fips_transport_id){id}, name, &c);
    assert(t);
    t->auto_connect = true; /* the path rules' dial fact */
    return t;
}
static void init_node(size_t i, const char *name, const char *yaml)
{
    test_node *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->name = name;
    n->config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(n->config);
    n->configs = go_swap_create(n->config, fipsd_config_free); assert(n->configs);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64; hcfg.notice_capacity = 256;
    hcfg.path_enabled = true;
    if (i == 0) hcfg.clock = (fips_clock){skewed, NULL};
    n->h = fipsd_node_handlers_create(&hcfg); assert(n->h);
    fipsd_node_options options = {.workers = 2, .peer_capacity = hcfg.peer_capacity, .netmon_source = &lan_source};
    fipsd_node_handlers_install(n->h, &options);
    hooks[i].h = options.user; hooks[i].poll = options.poll; hooks[i].command = options.command;
    hooks[i].changed = go_channel_create(1); assert(hooks[i].changed);
    options.poll = on_poll; options.command = on_command;
    n->node = fipsd_node_create(n->configs, &options); assert(n->node);
    lan[i] = udp(1, "lan", &wired_source);
    n->transport = udp(2, "wan", &wireless_source);
    n->addr = &n->udp_addr;
    assert(fipsd_transport_register(fipsd_node_transports(n->node), lan[i]) == 0);
    assert(fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0);
}

/* ---- observations ------------------------------------------------------ */
static GoChannel *observed[2];
static size_t dialled[2];
static const fipsd_mdns_event *await_started(size_t i, fipsd_mdns_event *out)
{
    for (;;) {
        void *value = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, observed[i], NULL, &value};
        assert(go_select_timeout(&c, 1, WAIT_MS).selected_case == 0 && value);
        fipsd_mdns_event *e = value;
        dialled[i] += e->kind == FIPSD_MDNS_EVENT_DIALLED;
        bool started = e->kind == FIPSD_MDNS_EVENT_STARTED;
        if (started) *out = *e;
        free(e);
        if (started) return out;
    }
}
static void drain_observed(size_t i)
{
    void *v;
    while (go_channel_try_receive(observed[i], &v) == 0) {
        fipsd_mdns_event *e = v;
        dialled[i] += e->kind == FIPSD_MDNS_EVENT_DIALLED;
        free(e);
    }
}
/* A live control command about peer; extra (stolen) adds parameters. */
static json_t *control(test_node *n, const char *name, const test_node *peer, json_t *extra)
{
    char npub[FIPS_NIP19_CAPACITY];
    assert(fips_peer_identity_npub(&peer->identity, npub, sizeof npub) == FIPS_IDENTITY_OK);
    json_t *params = extra ? extra : json_object();
    assert(params && json_object_set_new(params, "npub", json_string(npub)) == 0);
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + 5000000000u};
    json_t *out = p.command(p.user, name, params, &rc); json_decref(params);
    assert(out && !strcmp(json_string_value(json_object_get(out, "status")), "ok"));
    return out;
}
static json_t *path_show(test_node *n, const test_node *peer) { return control(n, "path_show", peer, NULL); }
/* The paths n's Bundle holds for peer: count, the active transport and, when
 * want_addr is set, a live path on transport 1 at want_addr. */
typedef struct paths_want { size_t count; unsigned active; const char *lan_addr; bool lan_live; } paths_want;
static bool paths(test_node *n, const test_node *peer, const paths_want *w)
{
    json_t *out = path_show(n, peer);
    json_t *rows = json_object_get(json_object_get(out, "data"), "paths");
    bool ok = json_array_size(rows) == w->count, active = false, lan_path = !w->lan_addr;
    for (size_t i = 0; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        unsigned id = (unsigned)json_integer_value(json_object_get(row, "transport_id"));
        if (json_is_true(json_object_get(row, "active"))) active = id == w->active;
        const char *addr = json_string_value(json_object_get(row, "addr"));
        const char *state = json_string_value(json_object_get(row, "state"));
        if (w->lan_addr && id == 1 && addr && !strcmp(addr, w->lan_addr) && (!w->lan_live || !strcmp(state, "live")))
            lan_path = true;
    }
    json_decref(out);
    return ok && active && lan_path;
}
static void await_paths(size_t i, const paths_want *w)
{
    uint64_t deadline = nt_ms() + WAIT_MS;
    while (!paths(&nodes[i], &nodes[1 - i], w)) {
        uint64_t now = nt_ms();
        assert(now < deadline);
        void *value = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, hooks[i].changed, NULL, &value};
        if (go_select_timeout(&c, 1, deadline - now).selected_case != 0) {
            json_t *out = path_show(&nodes[i], &nodes[1 - i]);
            char *text = json_dumps(out, JSON_COMPACT);
            fprintf(stderr, "%s: path view deadline expired: %s\n", nodes[i].name, text ? text : "?");
            free(text); json_decref(out);
            abort();
        }
    }
}
static void exchange(test_node *from, test_node *to)
{
    static const uint8_t payload[] = "over the LAN path";
    assert(fipsd_node_handlers_send(from->h, &to->identity.node_addr, 1000, 2000, payload, sizeof payload) == 0);
    fipsd_node_notice *e = nt_await(to, FIPSD_NOTICE_DATAGRAM, &from->identity.node_addr, WAIT_MS);
    if (!e) { fprintf(stderr, "no datagram %s -> %s\n", from->name, to->name); nt_print_trace("at the failed exchange"); }
    assert(e && e->len == sizeof payload && !memcmp(e->payload, payload, sizeof payload));
    free(e);
}

static const char *const yaml[2] = {
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false},"
    " path: {enabled: true}, rendezvous: {lan: {enabled: true, service_type: '_fipstest-multipath._udp.local.', scope: 'scope-mp'}}}",
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false},"
    " path: {enabled: true}, rendezvous: {lan: {enabled: true, service_type: '_fipstest-multipath._udp.local.', scope: 'scope-mp'}}}"};

int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    verdicts = go_channel_create(1); assert(verdicts);
    size_t monitors = fipsd_netmon_instances();
    fipsd_mdns_service *mdns[2];
    const char *names[2] = {"A", "B"};
    for (size_t i = 0; i < 2; ++i) {
        init_node(i, names[i], yaml[i]);
        mdns[i] = fipsd_mdns_service_create(nodes[i].configs, nodes[i].node, nodes[i].h); assert(mdns[i]);
        eps[i].src[0] = 10; eps[i].src[1] = 77; eps[i].src[2] = 0; eps[i].src[3] = (uint8_t)(1 + i);
        fipsd_mdns_io io = {.user = &eps[i], .open = link_open, .join = link_join, .leave = link_leave, .send = link_send, .close = link_close};
        fipsd_mdns_service_set_io(mdns[i], &io);
        observed[i] = go_channel_create(1024); assert(observed[i]);
        fipsd_mdns_service_observe(mdns[i], observed[i]);
    }
    for (size_t i = 0; i < 2; ++i) assert(fipsd_runtime_register(rt, fipsd_node_subsystem(nodes[i].node)) == 0);
    for (size_t i = 0; i < 2; ++i) assert(fipsd_runtime_register(rt, fipsd_mdns_service_subsystem(mdns[i])) == 0);
    nt_watch_publications();
    assert(fipsd_runtime_start(rt) == 0);
    assert(fipsd_netmon_instances() == monitors + 2); /* one per node, none of mDNS's own */
    uint16_t lan_port[2];
    for (size_t i = 0; i < 2; ++i) {
        nt_identity(&nodes[i]);
        fipsd_mdns_event started;
        assert(await_started(i, &started)->addr.port);
        lan_port[i] = started.addr.port; /* the lowest-id UDP transport's: lan */
        fips_transport_addr a; const char *text; size_t len; char buf[64]; unsigned port = 0;
        assert(fipsd_udp_address(lan[i], &a) && fips_transport_addr_as_str(&a, &text, &len) == FIPS_OK && len < sizeof buf);
        memcpy(buf, text, len); buf[len] = 0;
        assert(strrchr(buf, ':') && sscanf(strrchr(buf, ':') + 1, "%u", &port) == 1 && port == lan_port[i]);
        void *bound = NULL;
        GoSelectCase c = {GO_SELECT_RECEIVE, nt_socket_bound(nodes[i].transport), NULL, &bound};
        assert(go_select_timeout(&c, 1, WAIT_MS).selected_case == 0 && bound); /* wan bound too */
        assert(nt_socket_address(nodes[i].transport, &nodes[i].udp_addr));
    }
    /* 1. Linked over the WAN transport while the LAN is gated. */
    assert(fipsd_node_handlers_learn_identity(nodes[0].h, &nodes[1].identity) == 0);
    assert(fipsd_node_handlers_connect(nodes[0].h, &nodes[1].identity, (fips_transport_id){2}, nodes[1].addr) == 0);
    uint64_t deadline = nt_ms() + WAIT_MS;
    test_node *root = memcmp(nodes[0].identity.node_addr.bytes, nodes[1].identity.node_addr.bytes, 16) < 0 ? &nodes[0] : &nodes[1];
    for (size_t i = 0; i < 2; ++i) {
        fipsd_node_notice *up = nt_await(&nodes[i], FIPSD_NOTICE_LINK_UP, &nodes[1 - i].identity.node_addr, WAIT_MS);
        assert(up); free(up);
        /* Tree and filters settled, in the published snapshot mDNS reads. */
        nt_pair_state settled = {.root = root, .is_root = &nodes[i] == root};
        assert(nt_await_converged(&nodes[i], nt_pair_converged, &settled, deadline));
        nt_drain(&nodes[i]);
        nodes[i].link_stable = true;
    }
    paths_want wan_only = {.count = 1, .active = 2};
    assert(paths(&nodes[0], &nodes[1], &wan_only));
    json_decref(control(&nodes[1], "path_pin", &nodes[0], json_pack("{s:i}", "transport_id", 2)));
    /* 2. The adverts arrive: each Bundle gains the LAN path to the other's
     * advertised (lan) address, live through real probes. */
    open_gate();
    char want[2][64];
    for (size_t i = 0; i < 2; ++i) snprintf(want[i], sizeof want[i], "127.0.0.1:%u", lan_port[1 - i]);
    paths_want a_lan = {.count = 2, .active = 2, .lan_addr = want[0], .lan_live = true};
    paths_want b_lan = {.count = 2, .active = 2, .lan_addr = want[1], .lan_live = true};
    await_paths(0, &a_lan);
    await_paths(1, &b_lan);
    /* 3. The production Bundle prefers the Wired LAN path once it scores no
     * worse than the WAN path's margin: it dwells, then switches. */
    verdict v = call(&nodes[0], &measure_command);
    assert(v.paths == 2 && v.active == 2 && v.dwelling && v.switched == 0);
    atomic_fetch_add(&skew_ms, 2500); /* past the 2 s switch dwell */
    v = call(&nodes[0], &poll_command);
    assert(v.paths == 2 && v.active == 1 && v.switched == 1);
    paths_want a_switched = {.count = 2, .active = 1, .lan_addr = want[0]};
    assert(paths(&nodes[0], &nodes[1], &a_switched));
    paths_want b_pinned = {.count = 2, .active = 2, .lan_addr = want[1]};
    assert(paths(&nodes[1], &nodes[0], &b_pinned));
    exchange(&nodes[0], &nodes[1]);
    exchange(&nodes[1], &nodes[0]);
    for (size_t i = 0; i < 2; ++i) { nt_drain(&nodes[i]); drain_observed(i); nodes[i].link_stable = false; }
    /* The linked peer's adverts went to the intake, not the dial path. */
    assert(!dialled[0] && !dialled[1]);
    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    for (size_t i = 0; i < 2; ++i) { nt_drain(&nodes[i]); drain_observed(i); }
    assert(!dialled[0] && !dialled[1]);
    nt_print_trace("mDNS multipath intake, two nodes");
    nt_unwatch_publications();
    fipsd_runtime_destroy(rt);
    for (size_t i = 0; i < 2; ++i) {
        fipsd_mdns_service_destroy(mdns[i]);
        nt_destroy(&nodes[i]);
        fipsd_udp_destroy(lan[i]);
        go_channel_unref(observed[i]);
        go_channel_unref(hooks[i].changed);
    }
    assert(fipsd_netmon_instances() == monitors);
    pthread_mutex_lock(&link_mu);
    for (size_t i = 0; i < held_count; ++i) free(held[i].d);
    held_count = 0;
    pthread_mutex_unlock(&link_mu);
    go_channel_unref(verdicts);
    puts("mdns multipath: a linked peer's advert became a live LAN path candidate; the Bundle switched to the Wired LAN path: ok");
    return 0;
}
