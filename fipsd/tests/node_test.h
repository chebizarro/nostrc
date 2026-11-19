/* Shared harness for the node handler tests: N in-process nodes on one
 * loopback network, each with its own runtime subsystem, handlers, provider
 * and notice channel. Assertions are event-driven (notices and snapshots);
 * the only waits are bounded channel selects, never sleeps. */
#ifndef FIPSD_NODE_TEST_H
#define FIPSD_NODE_TEST_H
#include "node/context.h"
#include "node/handlers/handlers.h"
#ifdef FIPSD_TEST_HOOKS
#include "node/handlers/internal.h"
#endif
#include "node/transport_loopback.h"
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
#ifdef FIPSD_TEST_TOR
/* FIPSD_TEST_TCP plus FIPSD_TEST_TOR: each node is a directory-mode Tor
 * transport (onion listener on loopback, reading its onion name from a
 * hostname file) dialing through one loopback SOCKS5 fake standing in for
 * Tor, which routes each onion CONNECT to that node's listener. A node's
 * address is its onion and HiddenServicePort virtual port. */
#include "transport/tor/tor.h"
#include "transport/tcp/tcp.h"
#include "socks5_fixture.h"
static socks5_fixture nt_proxy;
static unsigned nt_proxy_users; /* nodes sharing the proxy; the last destroy stops it */
static const char *nt_tor_dir;
static const char *nt_onion(const char *name)
{ return name[0] == 'A' ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2.onion" : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2.onion"; }
static bool nt_tor_address(fipsd_transport *t, fips_transport_addr *out)
{
    fips_transport_addr listen; char text[96];
    if (!fipsd_tor_listener_address(t, &listen) || !fipsd_tor_onion_address(t)) return false;
    socks5_fixture_route(&nt_proxy, fipsd_tor_onion_address(t), &listen);
    snprintf(text, sizeof text, "%s:443", fipsd_tor_onion_address(t));
    return fips_transport_addr_from_string(text, strlen(text), out) == FIPS_OK;
}
#define nt_socket_bound fipsd_tor_bound
#define nt_socket_address nt_tor_address
#define nt_socket_count fipsd_tor_connected_count
#elif defined(FIPSD_TEST_TCP)
#include "transport/tcp/tcp.h"
#define nt_socket_bound fipsd_tcp_bound
#define nt_socket_address fipsd_tcp_address
#define nt_socket_count fipsd_tcp_connected_count
#else
#include "transport/udp/udp.h"
#define nt_socket_bound fipsd_udp_bound
#define nt_socket_address fipsd_udp_address
#define nt_socket_count fipsd_udp_connected_count
#endif
#include "test_tmpdir.h"
#endif
#include "config/config.h"
#include "control/protocol.h"
#include "peer/table.h"
#include "select.h"
#include <fips/identity/peer.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/encoding.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef FIPSD_TEST_HOOKS
static inline void nt_watch_publications(void)
{ fipsd_test_published = go_channel_create(1); assert(fipsd_test_published); }
static inline void nt_unwatch_publications(void)
{ go_channel_unref(fipsd_test_published); fipsd_test_published = NULL; }
#endif

#define NT_WAIT_MS 60000u
static uint64_t nt_ms(void)
{
    static uint64_t origin;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (!origin) origin = now;
    return now - origin;
}
typedef struct test_node {
    const char *name;
    fipsd_config *config;
    GoSwap *configs;
    fipsd_node_handlers *h;
    fipsd_node *node;
    fipsd_transport *transport;
    fipsd_netmon_source netmon;
    fips_peer_identity identity;
    const fips_transport_addr *addr;
    fips_transport_addr udp_addr;
    bool link_stable; /* fail on churn during a test-defined stable phase */
} test_node;
typedef struct nt_trace { char lines[256][128]; size_t count; } nt_trace;
static nt_trace nt_events;
static const char *nt_kind_name(fipsd_notice_kind k)
{
    static const char *const names[] = {"LINK_UP", "LINK_DOWN", "HANDSHAKE_FAILED", "TREE_CHANGED", "FILTER_ACCEPTED",
        "FILTER_SENT", "SESSION_UP", "SESSION_DOWN", "DATAGRAM", "LINK_REKEYED", "SESSION_REKEYED", "SEND_FAILED",
        "LOOKUP_RESOLVED", "LOOKUP_FAILED", "PATH_EVENT"};
    return (size_t)k < sizeof names / sizeof names[0] ? names[k] : "?";
}
static void nt_record(const test_node *n, const fipsd_node_notice *e)
{
    if (n->link_stable) assert(e->kind != FIPSD_NOTICE_LINK_UP && e->kind != FIPSD_NOTICE_LINK_DOWN);
    if (nt_events.count == 256) return;
    char peer[12]; (void)fips_node_addr_short_hex(&e->peer, peer, sizeof peer);
    snprintf(nt_events.lines[nt_events.count++], 128, "%7llu ms  %s: %-16s peer=%s k=%d root=%d depth=%llu value=%llu len=%zu",
        (unsigned long long)nt_ms(), n->name, nt_kind_name(e->kind), peer, e->k, e->is_root, (unsigned long long)e->depth, (unsigned long long)e->value, e->len);
}
static fipsd_network_snapshot *nt_network(void *arg)
{ (void)arg; fipsd_network_snapshot *s = calloc(1, sizeof(*s)); assert(s); return s; }
/* Receive notices until one of `kind` (and optionally `peer`) arrives. Every
 * notice is recorded in the trace. NULL on the bounded timeout. */
static fipsd_node_notice *nt_await(test_node *n, fipsd_notice_kind kind, const fips_node_addr *peer, uint64_t timeout_ms)
{
    GoChannel *c = fipsd_node_handlers_notices(n->h);
    for (;;) {
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c, NULL, &value}};
        GoSelectResult r = go_select_timeout(cases, 1, timeout_ms);
        if (r.selected_case != 0 || !value) return NULL;
        fipsd_node_notice *e = value;
        nt_record(n, e);
        if (e->kind == kind && (!peer || !memcmp(peer->bytes, e->peer.bytes, 16))) return e;
        free(e);
    }
}
static void nt_drain(test_node *n)
{
    void *value;
    while (go_channel_try_receive(fipsd_node_handlers_notices(n->h), &value) == 0) { nt_record(n, value); free(value); }
}
static bool nt_init_with_options(test_node *n, fipsd_loopback_network *net, const char *name, const char *address,
                    const char *yaml, const fipsd_node_handlers_config *hcfg, uint32_t transport_id,
                    void (*configure)(fipsd_node_options *))
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
    if (configure) configure(&options);
    n->node = fipsd_node_create(n->configs, &options);
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    (void)net; (void)address;
#ifdef FIPSD_TEST_TCP
    fipsd_tcp_config udp; fipsd_tcp_config_default(&udp);
#else
    fipsd_udp_config udp; fipsd_udp_config_default(&udp);
#endif
#if defined(FIPSD_TEST_UDP_V6) || defined(FIPSD_TEST_TCP_V6)
    udp.bind_addr = "[::1]:0";
#else
    udp.bind_addr = "127.0.0.1:0";
#endif
    udp.mtu = 1280;
#ifdef FIPSD_TEST_TOR
    if (!nt_proxy_users++) socks5_fixture_start(&nt_proxy);
    if (!nt_tor_dir) nt_tor_dir = fipsd_test_tmpdir(NULL);
    char hostname[512]; snprintf(hostname, sizeof hostname, "%s/%s.hostname", nt_tor_dir, name);
    FILE *file = fopen(hostname, "w"); if (!file) return false;
    fprintf(file, "%s\n", nt_onion(name)); fclose(file);
    fipsd_tor_config tor; fipsd_tor_config_default(&tor);
    tor.mode = "directory"; tor.socks5_addr = nt_proxy.addr; tor.mtu = udp.mtu;
    tor.hostname_file = hostname; tor.bind_addr = udp.bind_addr;
    n->transport = fipsd_tor_create((fips_transport_id){transport_id}, "tor", &tor);
#elif defined(FIPSD_TEST_TCP)
    n->transport = fipsd_tcp_create((fips_transport_id){transport_id}, "tcp", &udp);
#else
    n->transport = fipsd_udp_create((fips_transport_id){transport_id}, "udp", &udp);
#endif
#else
    n->transport = fipsd_loopback_transport_create(net, (fips_transport_id){transport_id}, "loop", address, 1280);
#endif
    if (!n->configs || !n->h || !n->node || !n->transport) return false;
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
    n->addr = &n->udp_addr;
#else
    n->addr = fipsd_loopback_transport_address(n->transport);
#endif
    return fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0;
}
static inline bool nt_init(test_node *n, fipsd_loopback_network *net, const char *name, const char *address,
                    const char *yaml, const fipsd_node_handlers_config *hcfg, uint32_t transport_id)
{
    return nt_init_with_options(n, net, name, address, yaml, hcfg, transport_id, NULL);
}
/* After the runtime started: public identity for the other node's dial. */
static void nt_identity(test_node *n)
{
    const fipsd_node_context *c = fipsd_node_get_context(n->node);
    fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(c));
    assert(fips_peer_identity_from_pubkey(fipsd_node_secp(c), &key, &n->identity) == FIPS_IDENTITY_OK);
}
static GoRefPtr nt_snapshot(test_node *n)
{
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    return p.acquire(p.user);
}
static json_t *nt_query(test_node *n, const char *name)
{
    GoRefPtr ref = nt_snapshot(n);
    assert(ref.ptr);
    json_t *out = fipsd_control_query(ref.ptr, name, NULL);
    go_refptr_release(&ref);
    return out;
}
/* A notice is not a convergence barrier: the control view is coalesced, and
 * e.g. FILTER_ACCEPTED can precede the peer's final parent declaration.
 * path_show is read-only; HCMD_CONTROL publishes even its "peer not found"
 * response (we ask about ourselves) BEFORE resolving the command ticket.
 * Keep that barrier and every notice wait inside the caller's absolute deadline.
 * Evaluate all fields against one retained, fresh, immutable view per attempt. */
typedef bool (*nt_converged_predicate)(const fipsd_control_snapshot *, const void *);
static inline bool nt_barrier(test_node *n, uint64_t deadline)
{
    uint64_t now = nt_ms();
    if (now >= deadline) return false;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {
        .deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + (deadline - now) * 1000000u};
    char npub[128];
    assert(fips_peer_identity_npub(&n->identity, npub, sizeof npub) == FIPS_IDENTITY_OK);
    json_t *params = json_pack("{s:s}", "npub", npub);
    assert(params);
    fipsd_control_provider provider = fipsd_node_handlers_provider(n->h);
    json_t *reply = provider.command(provider.user, "path_show", params, &rc);
    json_decref(params);
    bool completed = reply != NULL;
    json_decref(reply);
    return completed && nt_ms() < deadline;
}
static inline bool nt_await_converged(test_node *n, nt_converged_predicate predicate,
                                      const void *arg, uint64_t deadline)
{
    while (nt_barrier(n, deadline)) {
        /* The barrier itself emits a pulse. Drain BEFORE loading the view:
         * a publication racing the subsequent read is either in that view or
         * leaves a pulse for the select. Never spin on our own barrier. */
#ifdef FIPSD_TEST_HOOKS
        assert(fipsd_test_published);
        void *pulse;
        (void)go_channel_try_receive(fipsd_test_published, &pulse);
#endif
        GoRefPtr ref = nt_snapshot(n);
        bool ready = ref.ptr && predicate(ref.ptr, arg);
        go_refptr_release(&ref);
        if (ready) return true;
        /* No timer polling and no dependency on another LINK_UP. A queued
         * notice also counts: always barrier again after receiving it, so a
         * state change racing the previous observation cannot be missed. */
        uint64_t now = nt_ms();
        if (now >= deadline) break;
        void *value = NULL;
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, fipsd_node_handlers_notices(n->h), NULL, &value},
#ifdef FIPSD_TEST_HOOKS
            {GO_SELECT_RECEIVE, fipsd_test_published, NULL, &value},
#endif
        };
        GoSelectResult result = go_select_timeout(cases, sizeof cases / sizeof cases[0], deadline - now);
        if (result.selected_case < 0 || !value) break;
        if (result.selected_case == 0) { nt_record(n, value); free(value); }
    }
    fprintf(stderr, "%s: convergence deadline expired\n", n->name);
    const char *queries[] = {"show_peers", "show_tree", "show_bloom", "show_connections"};
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; ++i) {
        json_t *out = nt_query(n, queries[i]);
        char *text = json_dumps(out, JSON_COMPACT);
        fprintf(stderr, "%s %s: %s\n", n->name, queries[i], text ? text : "<no snapshot>");
        free(text); json_decref(out);
    }
    for (size_t i = 0; i < nt_events.count; ++i) fprintf(stderr, "%s\n", nt_events.lines[i]);
    return false;
}
typedef struct nt_pair_state { const test_node *root; bool is_root; } nt_pair_state;
static inline bool nt_pair_converged(const fipsd_control_snapshot *s, const void *arg)
{
    const nt_pair_state *want = arg;
    char root[33];
    (void)fips_node_addr_format(&want->root->identity.node_addr, root, sizeof root);
    const json_t *peer = json_array_get(s->peers, 0);
    const json_t *filter = json_array_get(s->bloom.peer_filters, 0);
    return s->tree.root && !strcmp(s->tree.root, root) && s->tree.is_root == want->is_root &&
        s->tree.depth == (uint64_t)(want->is_root ? 0 : 1) &&
        json_array_size(s->tree.my_coords) == (size_t)(want->is_root ? 1 : 2) && s->tree.peer_tree_count == 1 &&
        json_array_size(s->peers) == 1 && json_is_true(json_object_get(peer, "has_tree_position")) &&
        json_is_true(json_object_get(peer, want->is_root ? "is_child" : "is_parent")) &&
        json_array_size(s->bloom.peer_filters) == 1 && json_is_true(json_object_get(filter, "has_filter"));
}
static inline bool nt_peer_count(const fipsd_control_snapshot *s, const void *arg)
{ return json_array_size(s->peers) == *(const size_t *)arg; }
static void nt_destroy(test_node *n)
{
    fipsd_node_destroy(n->node);
    fipsd_node_handlers_destroy(n->h);
#if defined(FIPSD_TEST_UDP) || defined(FIPSD_TEST_TCP)
#ifdef FIPSD_TEST_TOR
    fipsd_tor_destroy(n->transport);
    if (nt_proxy_users && !--nt_proxy_users) socks5_fixture_stop(&nt_proxy);
#elif defined(FIPSD_TEST_TCP)
    fipsd_tcp_destroy(n->transport);
#else
    fipsd_udp_destroy(n->transport);
#endif
#else
    fipsd_loopback_transport_destroy(n->transport);
#endif
    go_swap_free(n->configs);
}
static void nt_print_trace(const char *title)
{
    printf("== %s (%zu events)\n", title, nt_events.count);
    for (size_t i = 0; i < nt_events.count; ++i) printf("  %s\n", nt_events.lines[i]);
    fflush(stdout);
}
/* Fixed small graphs shared by the multi-node tests. */
#define NT_MAX_NODES 5
static inline void nt_topology(bool edges[NT_MAX_NODES][NT_MAX_NODES], size_t count, const char *shape)
{
    assert(count >= 3 && count <= NT_MAX_NODES);
    memset(edges, 0, sizeof(bool) * NT_MAX_NODES * NT_MAX_NODES);
    for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j) {
        bool edge = !strcmp(shape, "FullMesh") || (!strcmp(shape, "Star") ? i == 0 : j == i + 1);
        if (!strcmp(shape, "Ring") && i == 0 && j == count - 1) edge = true;
        edges[i][j] = edges[j][i] = edge;
    }
}
/* Select across all owners, using an absolute deadline (unrelated notices
 * cannot extend it). Notices need not carry a freshly published snapshot. */
static inline fipsd_node_notice *nt_next(test_node *nodes, size_t count, uint64_t deadline, size_t *owner)
{
    assert(count <= NT_MAX_NODES);
    GoSelectCase cases[NT_MAX_NODES];
    void *value = NULL;
    for (size_t i = 0; i < count; ++i)
        cases[i] = (GoSelectCase){GO_SELECT_RECEIVE, fipsd_node_handlers_notices(nodes[i].h), NULL, &value};
    uint64_t now = nt_ms();
    if (now >= deadline) return NULL;
    GoSelectResult result = go_select_timeout(cases, count, deadline - now);
    if (result.selected_case < 0 || (size_t)result.selected_case >= count || !value) return NULL;
    *owner = (size_t)result.selected_case;
    nt_record(&nodes[*owner], value);
    return value;
}
#endif
