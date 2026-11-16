/* M7 regression (nostrc-f66sr.9): a live control command's waiting timeout is
 * an execution deadline. The production node, dispatcher and control provider
 * run on the real runtime; the test parks the dispatcher with a gate message in
 * the handlers' own command mailbox (a channel barrier, never a sleep). A queued
 * mutating `connect` whose waiter already gave up must be dropped unexecuted and
 * counted; a command that completes must mutate and reply exactly once; and when
 * completion and the deadline land in the same instant exactly one side wins
 * (executed <=> reply <=> leg, cancelled <=> counted). The claim's arrival is
 * swept across the deadline by queuing a varying amount of real dispatcher work
 * (IK initiations to pre-generated identities) ahead of the raced command. Every
 * Jansson allocation is counted: the dispatcher builds no reply for a cancelled
 * command and nothing is left live at exit, so an abandoned reply can neither
 * leak nor be freed twice. Shutdown abandon and destroy-time discard included. */
#include "node/context.h"
#include "node/handlers/internal.h"
#include "transport/loopback.h"
#include "config/config.h"
#include <fips/identity/mod.h>
#include <fips/identity/local.h>
#include <fips/identity/peer.h>
#include <stdatomic.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define FILLERS 12u

static const char *yaml_a =
    "node: {identity: {nsec: '5555555555555555555555555555555555555555555555555555555555555555', persistent: true}, control: {enabled: false}}";

/* Counted Jansson allocator: live objects return to zero at exit. */
static _Atomic size_t json_live, json_total;
static void *counted_alloc(size_t n)
{
    max_align_t *p = malloc(n + sizeof *p);
    if (!p) return NULL;
    atomic_fetch_add(&json_live, 1); atomic_fetch_add(&json_total, 1);
    return p + 1;
}
static void counted_free(void *p)
{
    if (!p) return;
    CHECK(atomic_fetch_sub(&json_live, 1) > 0);
    free((max_align_t *)p - 1);
}
/* No frame is ever delivered: a dial stays a pending leg, the observed mutation. */
static bool drop_all(void *user, const fips_transport_addr *from, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{ (void)user; (void)from; (void)to; (void)bytes; (void)len; return false; }
static fipsd_network_snapshot *empty_network(void *arg)
{ (void)arg; fipsd_network_snapshot *s = calloc(1, sizeof *s); CHECK(s); return s; }
static uint64_t monotonic_ns(void)
{ struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0); return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec; }

/* Dispatcher gate: a test message in the handlers' command mailbox. The wrapped
 * command callback parks the dispatcher on it until released, so every message
 * queued before the gate has been handled and every later one waits. For live
 * control commands the wrapper also counts, synchronously on the dispatcher,
 * the Jansson allocations the handler made: zero means no reply was built. */
typedef struct gate { GoChannel *entered, *release; } gate;
static bool (*real_command)(void *, void *);
static _Atomic(gate *) current_gate;
static _Atomic size_t control_seen, last_control_allocs;
static bool gated_command(void *user, void *message)
{
    gate *g = atomic_load(&current_gate);
    if (g && message == g) {
        CHECK(go_channel_send(g->entered, g) == 0);
        void *token = NULL; CHECK(go_channel_receive(g->release, &token) == 0 && token == g);
        return true;
    }
    const handler_command *c = message;
    if (c->kind != HCMD_CONTROL) return real_command(user, message);
    size_t before = atomic_load(&json_total);
    bool ok = real_command(user, message);
    atomic_store(&last_control_allocs, atomic_load(&json_total) - before);
    atomic_fetch_add(&control_seen, 1);
    return ok;
}
static void gate_init(gate *g) { g->entered = go_channel_create(1); g->release = go_channel_create(1); CHECK(g->entered && g->release); }
static void gate_free(gate *g) { go_channel_free(g->entered); go_channel_free(g->release); }
/* Returns with the dispatcher parked: all earlier mailbox messages are done. */
static void gate_park(fipsd_node_handlers *h, gate *g)
{
    atomic_store(&current_gate, g);
    CHECK(go_channel_send(h->commands, g) == 0);
    void *token = NULL; CHECK(go_channel_receive(g->entered, &token) == 0 && token == g);
}
static void gate_release(gate *g) { CHECK(go_channel_send(g->release, g) == 0); }

/* The production provider command path on a scheduler fiber, as the control
 * connection fiber runs it, with an absolute execution deadline. */
typedef struct waiter { fipsd_node_handlers *h; fipsd_runtime *rt; json_t *params; uint64_t deadline_ns; json_t *out; GoChannel *done; } waiter;
static void waiter_fiber(void *arg)
{
    waiter *w = arg;
    fipsd_control_provider p = fipsd_node_handlers_provider(w->h);
    fipsd_control_request_context rc = {.shutdown = fipsd_runtime_context(w->rt), .deadline_ns = w->deadline_ns};
    w->out = p.command(p.user, "connect", w->params, &rc);
    CHECK(go_channel_send(w->done, w) == 0);
}
static void waiter_start(waiter *w)
{ w->done = go_channel_create(1); CHECK(w->done); CHECK(fipsd_runtime_spawn(w->rt, waiter_fiber, w) == 0); }
static json_t *waiter_join(waiter *w)
{
    void *token = NULL; CHECK(go_channel_receive(w->done, &token) == 0 && token == w);
    go_channel_free(w->done); json_decref(w->params);
    return w->out;
}
/* B is dialled at A's own (started) endpoint; the filter drops the msg1. */
static json_t *connect_params(const char *npub)
{ return json_pack("{s:s,s:s,s:s}", "npub", npub, "address", "loop:a", "transport", "loop"); }
/* Dispatcher-owned state, read only while the dispatcher is parked at a gate
 * (the gate channels order these reads after its last write). */
static size_t legs_used(fipsd_node_handlers *h)
{ size_t k = 0; for (size_t i = 0; i < h->cfg.peer_capacity; ++i) k += h->legs[i].used ? 1 : 0; return k; }
static bool leg_for(fipsd_node_handlers *h, const fips_node_addr *peer)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->legs[i].used && !memcmp(h->legs[i].expected.node_addr.bytes, peer->bytes, 16)) return true;
    return false;
}
static size_t snapshot_connections(fipsd_node_handlers *h)
{
    fipsd_control_provider p = fipsd_node_handlers_provider(h);
    GoRefPtr ref = p.acquire(p.user); CHECK(ref.ptr);
    json_t *conns = fipsd_control_query(ref.ptr, "show_connections", NULL);
    go_refptr_release(&ref);
    size_t rows = json_array_size(json_object_get(json_object_get(conns, "data"), "connections"));
    json_decref(conns);
    return rows;
}
static void expect_connecting(json_t *out)
{
    CHECK(out && !strcmp(json_string_value(json_object_get(out, "status")), "ok"));
    CHECK(!strcmp(json_string_value(json_object_get(json_object_get(out, "data"), "result")), "connecting"));
    CHECK(out->refcount == 1); /* a single owner: the reply changed hands exactly once */
    json_decref(out);
}
static void peer_from_secret(fips_secp *secp, uint8_t fill, fips_peer_identity *out)
{
    uint8_t secret[32]; memset(secret, fill, sizeof secret);
    fips_identity *id = NULL;
    CHECK(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey pub = fips_identity_pubkey(id);
    CHECK(fips_peer_identity_from_pubkey(secp, &pub, out) == FIPS_IDENTITY_OK);
    fips_identity_destroy(id);
}

int main(void)
{
    json_set_alloc_funcs(counted_alloc, counted_free);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); CHECK(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); CHECK(net);
    fipsd_loopback_network_set_filter(net, drop_all, NULL);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = FILLERS + 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256;
    /* Long timers and a never-fed netmon backstop: only commands drive the
     * dispatcher; no tick, retransmit or expiry frees a leg or publishes. */
    hcfg.tick_ms = 60000; hcfg.tree_announce_min_interval_ms = 60000; hcfg.tree_reeval_ms = 60000; hcfg.bloom_debounce_ms = 60000;
    hcfg.timing = (fipsd_peer_timing){60000, 120000, 60000, 60000, 5, 2.0};
    fipsd_config *config = fipsd_config_parse(yaml_a, strlen(yaml_a), NULL); CHECK(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); CHECK(configs);
    fipsd_node_handlers *h = fipsd_node_handlers_create(&hcfg); CHECK(h);
    GoChannel *netmon_ticks = go_channel_create(1); CHECK(netmon_ticks);
    fipsd_netmon_source netmon = {.read = empty_network, .injected = true, .ticks = netmon_ticks};
    fipsd_node_options options = {.workers = 2, .peer_capacity = hcfg.peer_capacity, .netmon_source = &netmon};
    fipsd_node_handlers_install(h, &options);
    real_command = options.command; options.command = gated_command; /* the gate wraps the production callback */
    fipsd_node *node = fipsd_node_create(configs, &options); CHECK(node);
    fipsd_transport *transport = fipsd_loopback_transport_create(net, (fips_transport_id){1}, "loop", "loop:a", 1280); CHECK(transport);
    CHECK(fipsd_transport_register(fipsd_node_transports(node), transport) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(node)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    /* Peer identities: B is the raced target; the fillers are real dials that
     * cost one IK initiation each on the dispatcher. */
    fips_secp *secp = NULL; CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    fips_peer_identity b, filler[FILLERS];
    peer_from_secret(secp, 0x66, &b);
    for (unsigned i = 0; i < FILLERS; ++i) peer_from_secret(secp, (uint8_t)(0x70 + i), &filler[i]);
    fips_identity_destroy_secp(secp);
    char npub[FIPS_NIP19_CAPACITY]; CHECK(fips_peer_identity_npub(&b, npub, sizeof npub) == FIPS_IDENTITY_OK);
    const fips_transport_addr *self = fipsd_loopback_transport_address(transport);
    gate g; gate_init(&g);

    /* 1. The deadline expires while the dispatcher is parked: the command is
     * still queued when the waiter gives up. Resuming the dispatcher must drop
     * it unexecuted and counted: no leg, no snapshot row, no reply built. */
    gate_park(h, &g);
    CHECK(legs_used(h) == 0 && h->counters.commands_cancelled == 0 && atomic_load(&control_seen) == 0);
    waiter w1 = {.h = h, .rt = rt, .params = connect_params(npub), .deadline_ns = monotonic_ns() + UINT64_C(20000000)};
    waiter_start(&w1);
    CHECK(waiter_join(&w1) == NULL);          /* abandoned at its execution deadline */
    CHECK(legs_used(h) == 0);                 /* still parked: nothing ran */
    gate_release(&g);
    gate_park(h, &g);                         /* the cancelled command has been handled */
    CHECK(atomic_load(&control_seen) == 1 && atomic_load(&last_control_allocs) == 0);
    CHECK(h->counters.commands_cancelled == 1);
    CHECK(legs_used(h) == 0 && snapshot_connections(h) == 0);
    gate_release(&g);

    /* 2. Normal completion: mutates once, replies exactly once (single owner). */
    waiter w2 = {.h = h, .rt = rt, .params = connect_params(npub), .deadline_ns = monotonic_ns() + UINT64_C(5000000000)};
    waiter_start(&w2);
    expect_connecting(waiter_join(&w2));
    gate_park(h, &g);
    CHECK(atomic_load(&control_seen) == 2 && atomic_load(&last_control_allocs) > 0);
    CHECK(legs_used(h) == 1 && leg_for(h, &b.node_addr) && snapshot_connections(h) == 1 && h->counters.commands_cancelled == 1);
    CHECK(fipsd_node_handlers_disconnect(h, &b.node_addr) == 0); /* queued behind the gate */
    gate_release(&g);

    /* 3. Completion racing the deadline. Each round parks the dispatcher with
     * the raced connect queued behind the gate and 0..FILLERS real dials ahead
     * of it, then orders release and deadline three ways: release first (must
     * execute), deadline first (must cancel), and both in the same instant with
     * the dispatcher's arrival swept across the ~1 ms select floor by the filler
     * work (either outcome, but consistent). Legs are removed before the next
     * round, so the mutation is observed afresh every time. */
    size_t executed = 0, cancelled = 0, race_executed = 0, race_cancelled = 0;
    const unsigned rounds = 240;
    for (unsigned i = 0; i < rounds; ++i) {
        gate_park(h, &g);
        CHECK(legs_used(h) == 0);
        uint64_t cancelled_before = h->counters.commands_cancelled;
        size_t seen_before = atomic_load(&control_seen);
        unsigned mode = i % 3, fillers = mode == 2 ? (i / 3) % (FILLERS + 1) : 0;
        for (unsigned f = 0; f < fillers; ++f) CHECK(fipsd_node_handlers_connect(h, &filler[f], (fips_transport_id){1}, self) == 0);
        waiter w = {.h = h, .rt = rt, .params = connect_params(npub)};
        json_t *out;
        if (mode == 0) {
            w.deadline_ns = monotonic_ns() + UINT64_C(5000000000);
            waiter_start(&w); gate_release(&g); out = waiter_join(&w);
            CHECK(out);
        } else if (mode == 1) {
            w.deadline_ns = monotonic_ns() + UINT64_C(1000000);
            waiter_start(&w); out = waiter_join(&w); gate_release(&g);
            CHECK(!out);
        } else {
            w.deadline_ns = monotonic_ns() + (i % 2 ? UINT64_C(1000000) : 0);
            if (i % 4 == 2) { waiter_start(&w); gate_release(&g); }
            else { gate_release(&g); waiter_start(&w); }
            out = waiter_join(&w);
            if (out) ++race_executed; else ++race_cancelled;
        }
        gate_park(h, &g); /* the raced connect (executed or cancelled) has been handled */
        CHECK(atomic_load(&control_seen) == seen_before + 1);
        CHECK(legs_used(h) == fillers + (out ? 1 : 0));
        if (out) {
            ++executed;
            CHECK(h->counters.commands_cancelled == cancelled_before);
            CHECK(leg_for(h, &b.node_addr) && atomic_load(&last_control_allocs) > 0);
            expect_connecting(out);
            CHECK(fipsd_node_handlers_disconnect(h, &b.node_addr) == 0);
        } else {
            ++cancelled;
            CHECK(h->counters.commands_cancelled == cancelled_before + 1);
            CHECK(!leg_for(h, &b.node_addr) && atomic_load(&last_control_allocs) == 0);
        }
        for (unsigned f = 0; f < fillers; ++f) CHECK(fipsd_node_handlers_disconnect(h, &filler[f].node_addr) == 0);
        gate_release(&g);
    }
    CHECK(executed >= rounds / 3 && cancelled >= rounds / 3);
    CHECK(h->counters.commands_cancelled == 1 + cancelled);

    /* 4. Shutdown abandons a parked command; whatever is still queued when the
     * dispatcher stops is discarded at destroy without executing. */
    gate_park(h, &g);
    CHECK(legs_used(h) == 0);
    waiter w4 = {.h = h, .rt = rt, .params = connect_params(npub), .deadline_ns = monotonic_ns() + UINT64_C(5000000000)};
    waiter_start(&w4);
    fipsd_runtime_shutdown(rt);
    CHECK(waiter_join(&w4) == NULL);          /* shutdown, not the deadline, released it */
    gate_release(&g);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    CHECK(legs_used(h) == 0);                 /* never executed, before or after the stop */
    size_t notices = 0;
    for (void *v; go_channel_try_receive(fipsd_node_handlers_notices(h), &v) == 0; ++notices) {
        CHECK(((fipsd_node_notice *)v)->kind != FIPSD_NOTICE_LINK_UP); /* every frame was dropped */
        free(v);
    }
    gate_free(&g);
    fipsd_runtime_destroy(rt);
    fipsd_node_destroy(node);
    fipsd_node_handlers_destroy(h);
    fipsd_loopback_transport_destroy(transport);
    fipsd_loopback_network_destroy(net);
    go_swap_free(configs);
    go_channel_free(netmon_ticks);
    CHECK(atomic_load(&json_live) == 0);      /* every reply, params copy and snapshot released once */
    printf("control cancel: %u rounds, executed=%zu cancelled=%zu (race rounds: executed=%zu cancelled=%zu), "
           "%zu notices, json allocations=%zu live=0\n", rounds, executed, cancelled, race_executed, race_cancelled,
           notices, atomic_load(&json_total));
    puts("control cancel: expired command dropped unexecuted and counted, completed command mutates and replies once, "
         "race consistent, shutdown abandon, destroy discard, JSON balanced: ok");
    return 0;
}
