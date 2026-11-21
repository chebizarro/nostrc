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
static struct { unsigned transport; bool seed_old_clamp; } check_command;
static fipsd_dp_packet held;
static bool capture, interface_present = true;
static int capture_command, release_command;
static int probe_command;
static bool large_probe, await_proof;
static bool (*crypto_result)(void *, const fipsd_crypto_job *);

static bool on_crypto(void *user, const fipsd_crypto_job *job)
{
    const fipsd_dp_packet *p = job->owner;
    /* The injected interface exists only in the netmon fixture: loopback
     * keeps delivering even while test-X is absent. Model the missing medium
     * here, including decrypts queued before the presence event, rather than
     * letting a late probe legitimately re-admit a path on that fake link. */
    if (job->operation == FIPSD_CRYPTO_OPEN && p->transport.value == 1 && !interface_present)
        return true;
    if (capture && job->operation == FIPSD_CRYPTO_SEAL && !p->session && !p->path_message && !p->prompt) {
        capture = false;
        held = *p; held.job.owner = &held; go_refptr_retain(&held.snapshot);
        assert(held.transport.value == 1 && job->result == FIPS_NOISE_OK);
        assert(go_channel_try_send(seeded, &capture_command) == 0);
        return true; /* node recycles the original; the retained completion is ours */
    }
    if (large_probe && job->operation == FIPSD_CRYPTO_SEAL && p->path_message && p->body + job->output_size == 1000) {
        handlers *h = user;
        uint64_t before = fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){2}));
        bool ok = crypto_result(user, job);
        assert(fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){2})) == before + 1);
        large_probe = false;
        assert(go_channel_try_send(seeded, &probe_command) == 0);
        return ok;
    }
    return crypto_result(user, job);
}

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
    if ((e->kind == FIPSD_INTERFACE_UP || e->kind == FIPSD_INTERFACE_DOWN) && !strcmp(e->interface_name, "test-X"))
        interface_present = e->kind == FIPSD_INTERFACE_UP;
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
        const fips_peer_path *active = fips_path_set_active(&rec->path->set);
        if (await_proof && active && active->live.has_mtu && active->live.mtu >= 1000 - FIPS_FRAMING_FMP_FRAME_OVERHEAD) {
            await_proof = false;
            fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, remote.node_addr); assert(entry);
            session_ext *ext = hs_session_ext(h, entry); assert(ext && ext->mmp.path_mtu.current_mtu == 900);
            assert(go_channel_try_send(seeded, &probe_command) == 0);
        }
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
    handlers *h = user;
    if (message == &probe_command) {
        peer_rec *rec = hs_rec_find(h, &remote.node_addr); assert(rec);
        fips_peer_path *path = fips_path_set_path_mut(&rec->path->set, rec->path->set.active); assert(path);
        assert(path->transport_id.value == 2);
        assert(fips_path_liveness_prove(&path->live, 900 - FIPS_FRAMING_FMP_FRAME_OVERHEAD));
        assert(fipsd_dp_link_mtu(h, rec, path->transport_id, &path->addr) == 900);
        rec->path->params.cfg.enabled = false;
        assert(fipsd_dp_link_mtu(h, rec, path->transport_id, &path->addr) == 1100);
        rec->path->params.cfg.enabled = true;
        assert(fipsd_dp_peer_rebind(h, rec));
        uint8_t ordinary[1000 - FIPS_FRAMING_FMP_FRAME_OVERHEAD] = {FIPS_LINK_Heartbeat};
        assert(!hs_link_send(h, rec, ordinary, sizeof ordinary));
        size_t size = 1000 - FIPS_FRAMING_FMP_FRAME_OVERHEAD;
        uint32_t id = fips_path_liveness_seek_send(&path->live, hs_now(h), size, h->entropy);
        hs_timers_fold_path(h, rec);
        fips_path_probe probe = fips_path_probe_new(id, path->path_id, FIPS_PATH_ACTIVE, 0);
        probe.len = size;
        uint8_t bytes[1100]; fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
        assert(fips_path_probe_encode(&probe, &w, NULL) == FIPS_OK);
        large_probe = await_proof = true;
        assert(hs_link_send_to(h, rec, path->transport_id, &path->addr, bytes, fips_writer_len(&w)));
        return true;
    }
    if (message == &check_command || message == &capture_command || message == &release_command) {
        peer_rec *rec = hs_rec_find(h, &remote.node_addr); assert(rec && rec->path_live);
        if (message == &capture_command) {
            capture = true;
            const uint8_t heartbeat[] = {FIPS_LINK_Heartbeat};
            assert(hs_link_send(h, rec, heartbeat, sizeof heartbeat));
            return true;
        }
        if (message == &release_command) {
            assert(held.snapshot.ptr && held.transport.value == 1);
            assert(((fipsd_dp_snapshot *)held.snapshot.ptr)->peers[0].transport.value == 1);
            uint64_t old = fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){1}));
            uint64_t next = fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){2}));
            assert(fipsd_dp_result(h, &held.job));
            assert(fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){1})) == old);
            assert(fipsd_loopback_transport_sent(hs_transport(h, (fips_transport_id){2})) == next + 1);
            go_refptr_release(&held.snapshot);
            puts("pre-switch crypto completion: transport 1 +0, transport 2 +1");
        } else {
            const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
            assert(info->transport.value == check_command.transport);
            assert(fipsd_peer_table_endpoint(h->peers->table, info->transport, &info->addr) == rec->peer);
            assert(fipsd_dp_publish(h));
            const fipsd_dp_snapshot *s = h->dataplane->current.ptr;
            assert(s && s->peer_count == 1 && s->peers[0].transport.value == check_command.transport);
            assert(fips_transport_addr_equal(&s->peers[0].endpoint, &info->addr));
            if (check_command.transport == 2) {
                assert(fips_transport_addr_equal(&info->addr, &alternative));
                assert(s->peers[0].mtu <= 1100);
                fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, remote.node_addr);
                assert(entry);
                session_ext *ext = hs_session_ext(h, entry); assert(ext && ext->mmp_live);
                assert(ext->mmp.path_mtu.current_mtu <= 1100);
                assert(fipsd_dp_mtu_get(h->dataplane, &remote.node_addr) == 1100);
            }
            if (check_command.seed_old_clamp) fipsd_dp_mtu_set(h->dataplane, &rec->addr, 900);
        }
        assert(go_channel_try_send(seeded, message) == 0);
        return true;
    }
    if (message != &seed_command) return command(user, message);
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
    /* Direct fixture mutations must fold deadlines just like path.c's step. */
    hs_timers_fold_path(h, rec);
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
static void check_endpoint(test_node *a, unsigned transport, bool seed_old_clamp)
{
    check_command.transport = transport; check_command.seed_old_clamp = seed_old_clamp;
    assert(go_channel_send(a->h->commands, &check_command) == 0); (void)receive(seeded);
}
static void traffic(test_node *a, test_node *b, unsigned round)
{
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t payload[] = {(uint8_t)round, (uint8_t)i, 0x5a};
        assert(fipsd_node_handlers_send(a->h, &b->identity.node_addr, 1000, 2000, payload, sizeof payload) == 0);
        fipsd_node_notice *e = nt_await(b, FIPSD_NOTICE_DATAGRAM, &a->identity.node_addr, NT_WAIT_MS); assert(e);
        assert(e->len == sizeof payload && !memcmp(e->payload, payload, sizeof payload)); free(e);
    }
}
int main(void)
{
    reads = go_channel_create(8); wake = go_channel_create(1); ticks = go_channel_create(1);
    refreshed = go_channel_create(16); changed = go_channel_create(1); seeded = go_channel_create(4);
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
    crypto_result = options.crypto_result;
    options.network_event = on_network; options.command = on_command; options.poll = on_poll; options.crypto_result = on_crypto;
    a.node = fipsd_node_create(a.configs, &options); assert(a.node);
    a.transport->interface_name = "test-X"; /* optional fake loopback binding */
    assert(!b.transport->interface_name);
    assert(fipsd_transport_register(fipsd_node_transports(a.node), a.transport) == 0);
    fipsd_transport *a2 = fipsd_loopback_transport_create(y, (fips_transport_id){2}, "unbound", "loop:a2", 1100);
    fipsd_transport *b2 = fipsd_loopback_transport_create(y, (fips_transport_id){2}, "unbound", "loop:b2", 1100);
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
    traffic(&a, &b, 0); check_endpoint(&a, 1, true);
    change(sample(true, true, false, 42), 2); assert(paths(&a, 2, 1, true));
    change(sample(true, true, true, 42), 3); assert(paths(&a, 2, 1, true));
    puts("carrier down/up with IFF_UP retained: both paths unchanged");
    assert(go_channel_send(a.h->commands, &capture_command) == 0); (void)receive(seeded);
    change(sample(false, false, false, 42), 4);
    assert(paths(&a, 1, 2, true)); puts("X removed: X path absent, Bundle switched to unbound path");
    check_endpoint(&a, 2, false);
    assert(go_channel_send(a.h->commands, &release_command) == 0); (void)receive(seeded);
    uint64_t x_before = fipsd_loopback_transport_sent(a.transport), y_before = fipsd_loopback_transport_sent(a2);
    traffic(&a, &b, 1);
    uint64_t x_frames = fipsd_loopback_transport_sent(a.transport) - x_before;
    uint64_t y_frames = fipsd_loopback_transport_sent(a2) - y_before;
    printf("X absent, 8 delivered datagrams: transport 1 +%llu, transport 2 +%llu\n",
        (unsigned long long)x_frames, (unsigned long long)y_frames);
    assert(x_frames == 0 && y_frames >= 8);
    change(sample(true, true, false, 99), 5);
    await_paths(&a, 2, 2); puts("X returned at new index with no carrier: candidate re-proved, unbound stays active");
    check_endpoint(&a, 2, false); traffic(&a, &b, 2);
    change(sample(true, false, true, 99), 6);
    assert(paths(&a, 1, 2, true)); puts("IFF_UP cleared with carrier retained: X absent, unbound unaffected");
    x_before = fipsd_loopback_transport_sent(a.transport); y_before = fipsd_loopback_transport_sent(a2);
    traffic(&a, &b, 3);
    x_frames = fipsd_loopback_transport_sent(a.transport) - x_before;
    y_frames = fipsd_loopback_transport_sent(a2) - y_before;
    printf("X down again, 8 delivered datagrams: transport 1 +%llu, transport 2 +%llu\n",
        (unsigned long long)x_frames, (unsigned long long)y_frames);
    assert(x_frames == 0 && y_frames >= 8);
    change(sample(true, true, true, 99), 7); await_paths(&a, 2, 2);
    check_endpoint(&a, 2, false); traffic(&a, &b, 4);
    assert(go_channel_send(a.h->commands, &probe_command) == 0);
    (void)receive(seeded); (void)receive(seeded);
    puts("900-byte proven path: 1000-byte discovery frame sent and acknowledged on transport 2");
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
