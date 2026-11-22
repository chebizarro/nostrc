/* Real IK peer, dispatcher, transport fact notifications, path handler and
 * endpoint publication. Only path measurements/time are injected: selection,
 * dwell and switching run in the production Bundle (no sleeps or OS media). */
#include "node/handlers/internal.h"
#include "transport/loopback.h"
#include "transport/transport.h"
#include "fakes/host.h"
#include <assert.h>
#include <stdio.h>

typedef struct fixture {
    handlers *h;
    fipsd_node *node;
    fipsd_transport *lanes[2];
    GoSwap *configs;
    GoChannel *replies, *events;
    fipsd_node_options original;
    fipsd_transport_ops ops;
    const fipsd_transport_ops *loop_ops;
    fips_peer_identity remote;
    fips_transport_addr address;
    _Atomic uint64_t ms;
} fixture;
typedef struct result { unsigned active; size_t paths; uint64_t switches; bool dwelling; } result;
typedef struct request { bool dial, reset, reprobe, tick, roam; unsigned rtt[2]; const char *type; uint64_t ms; } request;
static fixture *started;
static fips_instant now(void *p) { return fips_test_instant(atomic_load(&((fixture *)p)->ms)); }
static fixture *owner(handlers *h) { return h->clock.ctx; }
static peer_rec *peer(handlers *h)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used) return &h->recs[i];
    return NULL;
}
static void reply(handlers *h)
{
    result *r = calloc(1, sizeof *r); assert(r);
    peer_rec *p = peer(h);
    if (p) {
        const fipsd_peer_info *info = fipsd_active_peer_info(p->peer);
        const fips_peer_path *active = fips_path_set_active(&p->path->set); assert(active);
        r->active = active->transport_id.value; r->paths = p->path->set.len;
        r->switches = h->counters.path_switched; r->dwelling = p->path->dwell_at.ops != NULL;
        assert(info->transport.value == r->active);
        assert(fipsd_peer_table_endpoint(h->peers->table, info->transport, &info->addr) == p->peer);
        assert(fipsd_dp_publish(h));
        const fipsd_dp_snapshot *snapshot = h->dataplane->current.ptr;
        assert(snapshot && snapshot->peer_count == 1 && snapshot->peers[0].transport.value == r->active);
    }
    assert(go_channel_try_send(owner(h)->replies, r) == 0);
}
static void measurements(handlers *h, const unsigned rtt[2])
{
    peer_rec *p = peer(h); assert(p && p->path_live);
    for (size_t i = 0; i < p->path->set.len; ++i) {
        fips_peer_path *path = &p->path->set.paths[i];
        path->state = FIPS_PATH_Live; path->acked = true;
        fips_path_liveness_new(&path->live);
        for (unsigned j = 0; j < 2; ++j)
            fips_path_liveness_sample(&path->live, hs_now(h),
                fips_duration_from_millis(rtt[path->transport_id.value - 1]),
                fips_duration_from_millis(30000), fips_duration_from_millis(60000));
        path->live.next_at = fips_deadline(hs_now(h), fips_duration_from_millis(1000000));
    }
}
static bool command(void *user, void *message)
{
    handlers *h = user; fixture *f = owner(h); request *r = message;
    atomic_store(&f->ms, r->ms);
    if (r->dial) assert(hs_dial(h, &f->remote, (fips_transport_id){1}, &f->address, false));
    else {
        peer_rec *p = peer(h); assert(p && p->path_live);
        if (r->roam) {
            assert(p->path->set.len == 1 && !p->path->capable);
            /* Remember the current endpoint as a discovery candidate. It is
             * not an alternative until a subsequently authenticated roam. */
            (void)fips_path_candidates_report(&p->path->table, f->lanes[0]->id,
                p->path->set.paths[0].addr, FIPS_PATH_Discovery, &p->path->set);
            /* Establish an exact, non-stale cache before the hot mutation. */
            (void)hs_timers_due(h, HS_TIMER_PATHS, hs_now(h));
            assert(!h->timers.stale && !hs_timers_due(h, HS_TIMER_PATHS, hs_now(h)));
            fips_transport_addr moved;
            assert(fips_transport_addr_from_string("loop:roamed", 11, &moved) == FIPS_OK);
            hs_path_account(h, p, f->lanes[0], &moved, 80, FIPS_LINK_Heartbeat);
            assert(p->path->eligible);
            assert(fips_transport_addr_equal(&p->path->set.paths[0].addr, &moved));
            assert(hs_timers_due(h, HS_TIMER_PATHS, hs_now(h)));
            uint64_t sent = p->path->record.sent;
            /* The ordinary dispatcher poll must fire at this instant; the
             * fixture's maintenance tick is one million milliseconds away. */
            assert(f->original.poll(h, hs_now(h)));
            assert(p->path->record.sent == sent + 1);
            puts("single-path roam folds and fires its capability probe before a tick");
            reply(h); return true;
        }
        if (r->reset) {
            json_t *pin = hs_path_pin(h, &p->addr, (fips_transport_id){1}, NULL, false, true); json_decref(pin);
            hs_path_peer_removed(h, p); hs_path_peer_added(h, p);
            f->lanes[1]->type = "loopback"; /* seed within the original link scope */
        }
        if (r->reset || r->reprobe) {
            /* A valid incoming probe admits the second path using host facts. */
            fips_path_probe probe = fips_path_probe_new(1, 102, 0, 0); probe.len = 1200;
            uint8_t bytes[1280]; fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
            assert(fips_path_probe_encode(&probe, &w, NULL) == FIPS_OK);
            hs_path_message(h, p, f->lanes[1], &f->address, FIPS_LINK_PathProbe, bytes + 1, fips_writer_len(&w) - 1);
            assert(p->path->set.len == 2);
            assert(p->path->params.cfg.switch_margin == 1.3);
            assert(p->path->params.cfg.switch_dwell.secs == 2 && !p->path->params.cfg.switch_dwell.nanos);
            assert(p->path->params.cfg.min_samples == 2);
        }
        if (r->type) f->lanes[1]->type = r->type; /* dispatcher-owned fixture metadata */
        measurements(h, r->rtt);
        if (r->tick) hs_path_poll(h, hs_now(h));
    }
    reply(h); return true;
}
static void network_event(void *user, const fipsd_netmon_event *e)
{
    handlers *h = user;
    owner(h)->original.network_event(user, e);
    if (e->kind == FIPSD_BINDING_UP || e->kind == FIPSD_BINDING_DOWN || e->kind == FIPSD_BINDING_MEDIUM_CHANGED)
        reply(h);
}
static int start_transport(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *ctx, GoChannel *events)
{
    started->events = events;
    return started->loop_ops->start_async(t, rt, ctx, events);
}
static result receive(fixture *f)
{
    void *out = NULL; GoSelectCase one = {GO_SELECT_RECEIVE, f->replies, NULL, &out};
    GoSelectResult selected = go_select_timeout(&one, 1, 10000); assert(selected.selected_case == 0 && out);
    result r = *(result *)out; free(out); return r;
}
static result call(fixture *f, uint64_t ms, request r)
{
    r.ms = ms;
    assert(go_channel_send(f->h->commands, &r) == 0); return receive(f);
}
static result medium(fixture *f, fipsd_runtime *rt, unsigned lane, fips_path_medium m,
                     fipsd_transport_event_kind kind)
{
    fipsd_transport *t = f->lanes[lane - 1];
    fipsd_presence_facts facts = {.valid = true, .exists = true, .up = true, .medium = m};
    if (kind == FIPSD_TRANSPORT_BECAME_ABSENT)
        fipsd_presence_transition(&t->shared->presence, FIPSD_ABSENT, 0);
    else fipsd_presence_bound(&t->shared->presence, facts, 0);
    assert(fipsd_transport_emit(f->events, fipsd_runtime_context(rt), NULL, kind, t->id, NULL, NULL, 0) == 0);
    return receive(f);
}
static fipsd_network_snapshot *network(void *p)
{ (void)p; fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s); return s; }
static void init(fixture *f, fipsd_loopback_network *net, const char *name, char digit)
{
    memset(f, 0, sizeof *f); atomic_init(&f->ms, 0);
    char yaml[256], key[65]; memset(key, digit, 64); key[64] = 0;
    snprintf(yaml, sizeof yaml, "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false}}", key);
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    f->configs = go_swap_create(cfg, fipsd_config_free); assert(f->configs);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 2; hc.cache_entries = 16;
    hc.clock = (fips_clock){now, f}; hc.tick_ms = 1000000; hc.notice_capacity = 256;
    hc.path_enabled = true;
    f->h = fipsd_node_handlers_create(&hc); assert(f->h);
    f->replies = go_channel_create(8); assert(f->replies);
    fipsd_netmon_source nm = {.read = network, .injected = true};
    fipsd_node_options o = {.workers = 2, .peer_capacity = 2, .netmon_source = &nm};
    fipsd_node_handlers_install(f->h, &o); f->original = o;
    o.command = command; o.network_event = network_event;
    f->node = fipsd_node_create(f->configs, &o); assert(f->node);
    f->lanes[0] = fipsd_loopback_transport_create(net, (fips_transport_id){1}, name, name, 1280);
    assert(f->lanes[0] && !fipsd_transport_register(fipsd_node_transports(f->node), f->lanes[0]));
}
static void link_up(fixture *f)
{
    GoChannel *notices = fipsd_node_handlers_notices(f->h);
    for (;;) {
        void *out = NULL; GoSelectCase one = {GO_SELECT_RECEIVE, notices, NULL, &out};
        GoSelectResult s = go_select_timeout(&one, 1, 10000); assert(s.selected_case == 0 && out);
        bool up = ((fipsd_node_notice *)out)->kind == FIPSD_NOTICE_LINK_UP; free(out); if (up) return;
    }
}
static bool drop(void *p, const fips_transport_addr *from, const fips_transport_addr *to, const uint8_t *b, size_t n)
{ (void)p; (void)from; (void)to; (void)b; (void)n; return false; }
static void destroy(fixture *f)
{
    fipsd_node_destroy(f->node); fipsd_node_handlers_destroy(f->h);
    for (unsigned i = 0; i < 2; ++i) if (f->lanes[i]) fipsd_loopback_transport_destroy(f->lanes[i]);
    go_swap_free(f->configs); go_channel_unref(f->replies);
}
int main(void)
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fixture a, b; init(&a, net, "loop:medium-a", '1'); init(&b, net, "loop:medium-b", '2');
    a.lanes[1] = fipsd_loopback_transport_create(net, (fips_transport_id){2}, "lane2", "loop:medium-a2", 1280);
    assert(a.lanes[1] && !fipsd_transport_register(fipsd_node_transports(a.node), a.lanes[1]));
    a.loop_ops = a.lanes[0]->ops; a.ops = *a.loop_ops; a.ops.start_async = start_transport;
    a.lanes[0]->ops = &a.ops; started = &a;
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)));
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)));
    assert(!fipsd_runtime_start(rt));
    const fipsd_node_context *ctx = fipsd_node_get_context(b.node);
    fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(ctx));
    assert(fips_peer_identity_from_pubkey(fipsd_node_secp(ctx), &key, &a.remote) == FIPS_IDENTITY_OK);
    a.address = *fipsd_loopback_transport_address(b.lanes[0]);
    (void)call(&a, 0, (request){.dial = true}); link_up(&a); link_up(&b);
    fipsd_loopback_network_set_filter(net, drop, NULL);
    (void)call(&a, 1, (request){.roam = true});
    result r = call(&a, 100, (request){.reset = true, .rtt = {10, 10}}); assert(r.active == 1);
    (void)medium(&a, rt, 1, FIPS_PATH_Wireless, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    assert(r.active == 1 && r.dwelling);
    r = call(&a, 2099, (request){.tick = true, .rtt = {10, 10}}); assert(r.active == 1);
    r = call(&a, 2100, (request){.tick = true, .rtt = {10, 10}}); assert(r.active == 2);
    puts("equal cost: detected wired wins, only after the 2s dwell");
    /* At exactly 1.3 times best the cable wins; just beyond it, Wi-Fi wins. */
    (void)call(&a, 2200, (request){.reset = true, .rtt = {100, 160}});
    r = medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED); assert(r.dwelling);
    r = call(&a, 4200, (request){.tick = true, .rtt = {100, 160}}); assert(r.active == 2);
    (void)call(&a, 4300, (request){.rtt = {100, 161}});
    r = medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED); assert(r.dwelling);
    r = call(&a, 6300, (request){.tick = true, .rtt = {100, 161}}); assert(r.active == 1);
    puts("margin: score 2.6 is inside 2.0 * 1.3; score 2.61 is outside");
    (void)call(&a, 6400, (request){.reset = true, .rtt = {10, 20}});
    (void)medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = call(&a, 8400, (request){.tick = true, .rtt = {10, 20}}); assert(r.active == 2);
    r = medium(&a, rt, 2, FIPS_PATH_Wireless, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    assert(r.active == 2 && !r.dwelling); /* same medium: ordinary score hysteresis */
    (void)medium(&a, rt, 1, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = call(&a, 10400, (request){.tick = true, .rtt = {10, 20}}); assert(r.active == 1);
    (void)medium(&a, rt, 1, FIPS_PATH_Wireless, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED); assert(r.dwelling);
    r = medium(&a, rt, 2, FIPS_PATH_Unknown, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    assert(r.active == 1 && !r.dwelling); /* losing the preference cancels its dwell */
    r = call(&a, 12400, (request){.tick = true, .rtt = {10, 20}}); assert(r.active == 1);
    (void)medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = call(&a, 14400, (request){.tick = true, .rtt = {10, 20}}); assert(r.active == 2);
    puts("medium changes rerank immediately; unknown cancels preference dwell, not core hysteresis");
    r = medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_BECAME_ABSENT);
    assert(r.active == 1 && r.paths == 1 && fipsd_transport_medium(a.lanes[1]) == FIPS_PATH_Unknown);
    (void)medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_BECAME_PRESENT);
    (void)call(&a, 16500, (request){.reprobe = true, .rtt = {10, 10}});
    (void)medium(&a, rt, 2, FIPS_PATH_Wired, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    r = call(&a, 18500, (request){.tick = true, .rtt = {10, 10}}); assert(r.active == 2);
    puts("binding loss immediately fails over; returned measured cable wins again");
    (void)call(&a, 18600, (request){.reset = true, .rtt = {10, 10}, .type = "ethernet"});
    r = medium(&a, rt, 2, FIPS_PATH_Unknown, FIPSD_TRANSPORT_MEDIUM_CHANGED); assert(r.dwelling);
    r = call(&a, 20600, (request){.tick = true, .rtt = {10, 10}}); assert(r.active == 2);
    puts("unknown Ethernet retains the C wired kind default");
    (void)call(&a, 20700, (request){.reset = true, .rtt = {10, 10}, .type = "ble"});
    r = medium(&a, rt, 2, FIPS_PATH_Unknown, FIPSD_TRANSPORT_MEDIUM_CHANGED);
    assert(r.active == 1 && !r.dwelling);
    r = call(&a, 22700, (request){.tick = true, .rtt = {10, 10}}); assert(r.active == 1);
    puts("unknown BLE retains Ble, behind wireless; unknown loopback has no medium preference");
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt); destroy(&a); destroy(&b); fipsd_loopback_network_destroy(net);
    return 0;
}
