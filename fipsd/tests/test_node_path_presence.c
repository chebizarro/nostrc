/* Two real nodes/links, two loopback lanes; only candidate provisioning is a
 * dispatcher-side fixture. Presence traverses injected read -> netmon channel
 * -> node dispatcher -> production handler -> Bundle -> path_show/rebind. */
#include "node_test.h"
#include "node/handlers/internal.h"

static const char *yaml_a = "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b = "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";
static GoChannel *reads, *wake, *ticks, *refreshed, *changed, *seeded;
static void (*network_event)(void *, const fipsd_netmon_event *);
static bool (*command)(void *, void *), (*poll_node)(void *, fips_instant);
static fips_peer_identity remote;
static fips_transport_addr alternative;
static int seed_command;

static void *receive(GoChannel *c)
{
    void *value = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c, NULL, &value}};
    GoSelectResult result = go_select_timeout(cases, 1, NT_WAIT_MS);
    assert(result.selected_case == 0 && value);
    return value;
}
static fipsd_network_snapshot *read_network(void *arg)
{ (void)arg; return receive(reads); }
static fipsd_network_snapshot *sample(bool exists, bool up, bool carrier, uint32_t index)
{
    fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s);
    if (exists) {
        s->interface_count = 1;
        s->interfaces[0] = (fipsd_interface){.name = "test-X", .index = index, .up = up, .carrier = carrier};
    }
    return s;
}
static void on_network(void *user, const fipsd_netmon_event *e)
{
    static unsigned kinds;
    network_event(user, e);
    kinds |= 1u << e->kind;
    if (e->kind == FIPSD_NETWORK_REFRESH) {
        uintptr_t round = ((uintptr_t)e->generation << 8) | kinds;
        assert(go_channel_try_send(refreshed, (void *)round) == 0);
        kinds = 0;
    }
}
static bool on_poll(void *user, fips_instant now)
{
    bool ok = poll_node(user, now);
    /* Notify only on a path-view transition, not on control reads or ticks. */
    static uint64_t previous;
    handlers *h = user;
    peer_rec *rec = NULL;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used) { rec = &h->recs[i]; break; }
    uint64_t signature = 0;
    if (rec && rec->path_live) {
        fips_path_bundle_view view; fips_path_bundle_view_get(rec->path, now, &view);
        signature = view.len + 1;
        for (size_t i = 0; i < view.len; ++i)
            signature = signature * 131 + view.paths[i].to.transport.value * 16 +
                (unsigned)view.paths[i].state * 2 + view.paths[i].active;
    }
    if (signature != previous) {
        previous = signature;
        fips_path_bundle_view v;
        if (rec && rec->path_live) {
            fips_path_bundle_view_get(rec->path, now, &v);
            fprintf(stderr, "path view:");
            for (size_t i = 0; i < v.len; ++i)
                fprintf(stderr, " t%u(state=%d active=%d acked=%d)", v.paths[i].to.transport.value,
                    v.paths[i].state, v.paths[i].active, v.paths[i].acked);
            fprintf(stderr, " capable=%d\n", v.capable);
        }
        (void)go_channel_try_send(changed, &seed_command);
    }
    return ok;
}
/* Candidate fixtures use the same facts as these loopback transports. The
 * normal probe/ack exchange and all presence effects use production handlers. */
static size_t mtu(void *ctx, fips_transport_id t, const fips_transport_addr *a)
{ fipsd_transport *x = hs_transport(ctx, t); assert(x); return x->ops->mtu(x, a); }
static fips_path_medium medium(void *ctx, fips_transport_id t, const fips_transport_addr *a)
{ (void)ctx; (void)t; (void)a; return FIPS_PATH_Unknown; }
static bool no(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return false; }
static bool yes(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return true; }
static fips_path_class class_of(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return FIPS_PATH_Clear; }
static fips_path_kind kind_of(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return FIPS_PATH_Unfixed; }
static fips_path_role role(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return FIPS_PATH_Normal; }
static bool on_command(void *user, void *message)
{
    if (message != &seed_command) return command(user, message);
    handlers *h = user;
    peer_rec *rec = hs_rec_find(h, &remote.node_addr); assert(rec && rec->path_live);
    bool checked = false;
    for (size_t i = 0; i < h->index_map_capacity; ++i) {
        index_entry *entry = &h->index_map[i];
        if (!entry->used || memcmp(entry->peer.bytes, rec->addr.bytes, 16)) continue;
        assert(entry->transport == 1);
        assert(hs_index_lookup(h, 1, entry->index) == rec);
        assert(hs_index_lookup(h, 2, entry->index) == rec);
        rec->path->params.cfg.enabled = false;
        assert(hs_index_lookup(h, 2, entry->index) == NULL);
        assert(hs_index_lookup(h, 1, entry->index) == rec);
        rec->path->params.cfg.enabled = true;
        checked = true;
    }
    assert(checked);
    fips_path_facts facts = {h, mtu, medium, no, class_of, kind_of, role, yes, yes};
    fips_path_env env = {&facts, hs_mmp_link_cost(h, rec)};
    fips_path_input in = {.kind = FIPS_PATH_INPUT_Candidate,
        .to = {{2}, alternative}, .as.source = FIPS_PATH_Request};
    fips_path_output storage[FIPS_PATH_OUTPUTS_MAX];
    fips_path_outputs out = {storage, FIPS_PATH_OUTPUTS_MAX, 0};
    assert(fips_path_bundle_step(rec->path, hs_now(h), &in, &env, &out) == FIPS_OK);
    for (size_t i = 0; i < out.len; ++i) if (out.items[i].kind == FIPS_PATH_OUTPUT_Send) {
        uint8_t bytes[1280]; fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
        assert(fips_path_output_encode(&out.items[i], &w, NULL) == FIPS_OK);
        assert(hs_link_send_to(h, rec, out.items[i].to.transport, &out.items[i].to.addr, bytes, fips_writer_len(&w)));
    }
    assert(go_channel_try_send(seeded, &seed_command) == 0);
    return true;
}
static json_t *live(test_node *n, const char *name)
{
    char npub[FIPS_NIP19_CAPACITY];
    assert(fips_peer_identity_npub(&remote, npub, sizeof npub) == FIPS_IDENTITY_OK);
    json_t *params = json_pack("{s:s}", "npub", npub);
    fipsd_control_provider p = fipsd_node_handlers_provider(n->h);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fipsd_control_request_context rc = {.deadline_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec + 5000000000u};
    json_t *out = p.command(p.user, name, params, &rc); json_decref(params);
    assert(out && !strcmp(json_string_value(json_object_get(out, "status")), "ok"));
    return out;
}
static bool paths(test_node *a, size_t count, unsigned active, bool all_live)
{
    json_t *out = live(a, "path_show");
    json_t *rows = json_object_get(json_object_get(out, "data"), "paths");
    bool ok = json_array_size(rows) == count, found = false;
    for (size_t i = 0; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        unsigned id = (unsigned)json_integer_value(json_object_get(row, "transport_id"));
        if (json_is_true(json_object_get(row, "active"))) { found = id == active; }
        if (all_live && strcmp(json_string_value(json_object_get(row, "state")), "live")) ok = false;
    }
    json_decref(out); return ok && found;
}
static void await_paths(test_node *a, size_t count, unsigned active)
{
    while (!paths(a, count, active, true)) (void)receive(changed);
}
static void change(fipsd_network_snapshot *s, unsigned generation)
{
    assert(go_channel_send(reads, s) == 0 && go_channel_send(wake, &seed_command) == 0);
    uintptr_t round = (uintptr_t)receive(refreshed);
    assert((round >> 8) == generation);
    if (generation == 2 || generation == 3)
        assert((round & 255) == (1u << FIPSD_NETWORK_REFRESH));
}
int main(void)
{
    reads = go_channel_create(8); wake = go_channel_create(1); ticks = go_channel_create(1);
    refreshed = go_channel_create(16); changed = go_channel_create(1); seeded = go_channel_create(1);
    assert(reads && wake && ticks && refreshed && changed && seeded);
    assert(go_channel_send(reads, sample(true, true, true, 42)) == 0);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg);
    rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_loopback_network *x = fipsd_loopback_network_create(), *y = fipsd_loopback_network_create(); assert(x && y);
    fipsd_node_handlers_config cfg; fipsd_node_handlers_config_default(&cfg);
    cfg.peer_capacity = 4; cfg.tree_peer_capacity = 4; cfg.session_capacity = 4; cfg.cache_entries = 64;
    cfg.notice_capacity = 256; cfg.tick_ms = 100; cfg.path_enabled = true;
    test_node a, b;
    assert(nt_init(&a, x, "A", "loop:a", yaml_a, &cfg, 1));
    assert(nt_init(&b, x, "B", "loop:b", yaml_b, &cfg, 1));
    /* Recreate A before start to install the injected source and observation
     * callbacks. All callbacks retain the handlers' dispatcher ownership. */
    fipsd_node_destroy(a.node);
    a.netmon = (fipsd_netmon_source){.injected = true, .read = read_network, .wake = wake, .ticks = ticks};
    fipsd_node_options options = {.workers = 2, .peer_capacity = 4, .netmon_source = &a.netmon};
    fipsd_node_handlers_install(a.h, &options);
    network_event = options.network_event; command = options.command; poll_node = options.poll;
    options.network_event = on_network; options.command = on_command; options.poll = on_poll;
    a.node = fipsd_node_create(a.configs, &options); assert(a.node);
    a.transport->interface_name = "test-X"; /* optional fake loopback binding */
    assert(!b.transport->interface_name);
    assert(fipsd_transport_register(fipsd_node_transports(a.node), a.transport) == 0);
    fipsd_transport *a2 = fipsd_loopback_transport_create(y, (fips_transport_id){2}, "unbound", "loop:a2", 1280);
    fipsd_transport *b2 = fipsd_loopback_transport_create(y, (fips_transport_id){2}, "unbound", "loop:b2", 1280);
    assert(a2 && b2 && !a2->interface_name && !b2->interface_name);
    assert(fipsd_transport_register(fipsd_node_transports(a.node), a2) == 0);
    assert(fipsd_transport_register(fipsd_node_transports(b.node), b2) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0 && ((uintptr_t)receive(refreshed) >> 8) == 1);
    nt_identity(&a); nt_identity(&b); remote = b.identity; alternative = *fipsd_loopback_transport_address(b2);
    assert(fipsd_node_handlers_connect(a.h, &remote, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &remote.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    assert(go_channel_send(a.h->commands, &seed_command) == 0); (void)receive(seeded);
    await_paths(&a, 2, 1); puts("two acked paths: X active, unbound standby");
    change(sample(true, true, false, 42), 2); assert(paths(&a, 2, 1, true));
    change(sample(true, true, true, 42), 3); assert(paths(&a, 2, 1, true));
    puts("carrier down/up with IFF_UP retained: both paths unchanged");
    change(sample(false, false, false, 42), 4);
    assert(paths(&a, 1, 2, true)); puts("X removed: X path absent, Bundle switched to unbound path");
    change(sample(true, true, false, 99), 5);
    await_paths(&a, 2, 2); puts("X returned at new index with no carrier: candidate re-proved, unbound stays active");
    change(sample(true, false, true, 99), 6);
    assert(paths(&a, 1, 2, true)); puts("IFF_UP cleared with carrier retained: X absent, unbound unaffected");
    change(sample(true, true, true, 99), 7); await_paths(&a, 2, 2);
    /* Exercise the harness snapshot reader as well as the live path rows. */
    json_t *status = nt_query(&a, "show_status"); assert(status); json_decref(status);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b); nt_print_trace("path interface presence");
    fipsd_runtime_destroy(rt); nt_destroy(&a); nt_destroy(&b);
    fipsd_loopback_transport_destroy(a2); fipsd_loopback_transport_destroy(b2);
    fipsd_loopback_network_destroy(x); fipsd_loopback_network_destroy(y);
    go_channel_unref(reads); go_channel_unref(wake); go_channel_unref(ticks);
    go_channel_unref(refreshed); go_channel_unref(changed); go_channel_unref(seeded);
    return 0;
}
