#include "handshake_fixture.h"
#include <stdio.h>
static handshake_fixture *active;
static const char *yaml = "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111'}, control: {enabled: false}}";
void hf_wait(GoChannel *c)
{
    void *p = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c, NULL, &p}};
    GoSelectResult r = go_select_timeout(cases, 1, 10000);
    assert(r.selected_case == 0 && p);
}
void hf_state_read(handshake_fixture *f, hf_state *s)
{
    handlers *h = f->h;
    memset(s, 0, sizeof *s);
    s->peers = fipsd_peer_table_count(h->peers->table);
    s->indices = fips_index_allocator_count(&h->indices);
    for (size_t i = 0; i < h->index_map_capacity; ++i) s->bindings += h->index_map[i].used;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) s->legs += h->legs[i].used;
    s->sent = fipsd_loopback_transport_sent(f->server);
    s->promoted = h->counters.hs_promoted; s->next_link = h->next_link_id;
    s->restart_pending = h->restart_pending; s->restarts = h->counters.hs_restart;
    peer_rec *r = hs_rec_find(h, &f->remote.node_addr);
    if (r) {
        s->peer = r->peer; s->info = *fipsd_active_peer_info(r->peer); s->frames_in = r->frames_in;
        const fips_noise_session *keys = fips_rekey_keys(fipsd_active_peer_link_machine(r->peer), FIPS_REKEY_Current);
        assert(keys); s->keys = *keys;
    }
}
void hf_unchanged(const hf_state *a, const hf_state *b)
{ assert(!memcmp(a, b, sizeof *a)); }
static bool event(void *user, const fipsd_transport_event *e)
{
    handshake_fixture *f = active;
    if (e->kind != FIPSD_TRANSPORT_PACKET) return f->on_event(user, e);
    hf_state_read(f, &f->before);
    bool ok = f->on_event(user, e);
    hf_state_read(f, &f->after);
    assert(go_channel_send(f->received, f) == 0);
    return ok;
}
static bool crypto(void *user, const fipsd_crypto_job *j)
{
    handshake_fixture *f = active;
    bool ok = f->on_crypto(user, j);
    if (j->operation == FIPSD_CRYPTO_OPEN) assert(go_channel_send(f->completed, f) == 0);
    return ok;
}
static bool command(void *user, void *msg)
{
    handshake_fixture *f = active;
    if (msg != &f->request) return f->on_command(user, msg);
    f->request.fn(f, f->request.arg);
    assert(go_channel_send(f->commands_done, f) == 0);
    return true;
}
void hf_call(handshake_fixture *f, void (*fn)(handshake_fixture *, void *), void *arg)
{
    f->request.fn = fn; f->request.arg = arg;
    assert(go_channel_send(f->h->commands, &f->request) == 0); hf_wait(f->commands_done);
}
static void snapshot(handshake_fixture *f, void *arg) { hf_state_read(f, arg); }
void hf_snapshot(handshake_fixture *f, hf_state *s) { hf_call(f, snapshot, s); }
static fipsd_network_snapshot *network(void *arg)
{ (void)arg; return calloc(1, sizeof(fipsd_network_snapshot)); }
void hf_init(handshake_fixture *f)
{
    memset(f, 0, sizeof *f); active = f;
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    f->rt = fipsd_runtime_create(&rc); assert(f->rt);
    f->net = fipsd_loopback_network_create(); assert(f->net);
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    f->configs = go_swap_create(cfg, fipsd_config_free); assert(f->configs);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 4; hc.cache_entries = 32;
    hc.notice_capacity = 256; hc.timing.handshake_timeout_ms = 1000;
    f->h = fipsd_node_handlers_create(&hc); assert(f->h);
    static fipsd_netmon_source source;
    source = (fipsd_netmon_source){.read = network, .injected = true};
    fipsd_node_options options = {.workers = 2, .peer_capacity = 4, .netmon_source = &source};
    fipsd_node_handlers_install(f->h, &options);
    f->on_event = options.transport_event; f->on_crypto = options.crypto_result; f->on_command = options.command;
    options.transport_event = event; options.crypto_result = crypto; options.command = command;
    f->node = fipsd_node_create(f->configs, &options); assert(f->node);
    f->server = fipsd_loopback_transport_create(f->net, (fips_transport_id){1}, "loop", "loop:server", 1280);
    f->client = fipsd_loopback_transport_create(f->net, (fips_transport_id){1}, "client", "loop:client", 1280);
    f->alternate = fipsd_loopback_transport_create(f->net, (fips_transport_id){1}, "alternate", "loop:alternate", 1280);
    assert(f->server && f->client && f->alternate);
    assert(fipsd_transport_register(fipsd_node_transports(f->node), f->server) == 0);
    f->wire = go_channel_create(1024); f->received = go_channel_create(16);
    f->completed = go_channel_create(16); f->commands_done = go_channel_create(1);
    assert(f->wire && f->received && f->completed && f->commands_done);
    assert(fipsd_runtime_register(f->rt, fipsd_node_subsystem(f->node)) == 0);
    assert(fipsd_runtime_start(f->rt) == 0);
    assert(f->client->ops->start_async(f->client, f->rt, NULL, f->wire) == 0);
    assert(f->alternate->ops->start_async(f->alternate, f->rt, NULL, f->wire) == 0);
    const fipsd_node_context *ctx = fipsd_node_get_context(f->node);
    fips_xonly_pubkey pub = fips_identity_pubkey(fipsd_node_identity(ctx));
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &f->secp) == FIPS_IDENTITY_OK);
    assert(fips_peer_identity_from_pubkey(f->secp, &pub, &f->server_identity) == FIPS_IDENTITY_OK);
    uint8_t secret[32]; memset(secret, 0x22, sizeof secret);
    assert(fips_identity_from_secret_bytes(f->secp, fipsd_host_os_entropy(), secret, &f->id) == FIPS_IDENTITY_OK);
    pub = fips_identity_pubkey(f->id);
    assert(fips_peer_identity_from_pubkey(f->secp, &pub, &f->remote) == FIPS_IDENTITY_OK);
    f->local = (fips_rekey_node_keys){.secp = f->secp, .keypair = f->id, .entropy = fipsd_host_os_entropy()};
}
void hf_start(handshake_fixture *f, uint8_t epoch)
{
    fips_rekey_hs_clear(&f->hs);
    memset(f->local.epoch, epoch, sizeof f->local.epoch);
    fips_peer_key key = fips_peer_key_from_xonly(&f->server_identity.pubkey);
    ++f->index;
    assert(fips_fmp_rekey_profile.initiate(&f->local, &key, (fips_rekey_index){f->index, true}, &f->hs, &f->msg1));
}
void hf_send(handshake_fixture *f, fipsd_transport *from, const uint8_t *bytes, size_t len)
{
    assert(from->ops->send(from, fipsd_loopback_transport_address(f->server), bytes, len) == 0);
    hf_wait(f->received);
}
fipsd_transport_event *hf_wire(handshake_fixture *f, uint8_t type)
{
    for (;;) {
        void *value = NULL; GoSelectCase cases[] = {{GO_SELECT_RECEIVE, f->wire, NULL, &value}};
        GoSelectResult r = go_select_timeout(cases, 1, 10000); assert(r.selected_case == 0 && value);
        fipsd_transport_event *e = value;
        if (e->size && e->bytes[0] == type) return e;
        free(e);
    }
}
void hf_finish(handshake_fixture *f)
{
    fipsd_transport_event *e = hf_wire(f, 2);
    fips_rekey_read read = {0};
    assert(fips_fmp_rekey_profile.reply(&f->hs, &f->local, e->bytes, e->size, &read));
    fips_rekey_completed_clear(&f->live); f->live = read.done;
    sodium_memzero(&read, sizeof read); free(e);
}
void hf_heartbeat(handshake_fixture *f)
{
    uint8_t wire[128], plain[32], hb = FIPS_LINK_Heartbeat;
    fips_writer w; fips_writer_init(&w, plain, sizeof plain);
    assert(fips_fmp_prepend_inner_header(0, &hb, 1, &w, NULL) == FIPS_OK);
    size_t plen = fips_writer_len(&w), len = 0, aad = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
    uint64_t counter = fips_noise_session_current_send_counter(&f->live.session);
    fips_fmp_build_established_header((fips_session_index){f->live.theirs.value}, counter, 0, (uint16_t)plen, wire);
    assert(fips_noise_session_seal(&f->live.session, wire, aad, plain, plen, wire + aad, sizeof wire - aad, &len) == FIPS_NOISE_OK);
    hf_send(f, f->client, wire, aad + len); hf_wait(f->completed);
}
void hf_policy(handshake_fixture *f, bool deny, const char *path)
{
    FILE *out = fopen(path, "wb"); assert(out);
    const char *rule = deny ? "ALL\n" : "# default allow\n";
    assert(fputs(rule, out) >= 0 && fclose(out) == 0);
    char npub[FIPS_NIP19_CAPACITY], configured[1024];
    assert(fips_peer_identity_npub(&f->remote, npub, sizeof npub) == FIPS_IDENTITY_OK);
    snprintf(configured, sizeof configured, "%s\npeers: [{npub: '%s', connect_policy: auto_connect, addresses: [{transport: loopback, addr: 'loop:client'}]}]\n", yaml, npub);
    fipsd_config *cfg = fipsd_config_parse(configured, strlen(configured), NULL); assert(cfg);
    char missing[4096]; snprintf(missing, sizeof missing, "%s.missing", path);
    fipsd_acl_paths paths = {.allow = missing, .deny = path, .hosts = missing};
    assert(fipsd_config_enable_acl(cfg, &paths));
    assert(go_swap_store(f->configs, cfg));
}
void hf_destroy(handshake_fixture *f)
{
    fipsd_runtime_shutdown(f->rt); assert(fipsd_runtime_run(f->rt) == FIPSD_RUN_OK);
    f->client->ops->stop(f->client); f->alternate->ops->stop(f->alternate);
    fipsd_runtime_destroy(f->rt); fipsd_node_destroy(f->node); fipsd_node_handlers_destroy(f->h);
    fipsd_loopback_transport_destroy(f->server); fipsd_loopback_transport_destroy(f->client); fipsd_loopback_transport_destroy(f->alternate);
    fipsd_loopback_network_destroy(f->net); go_swap_free(f->configs);
    void *p; while (go_channel_try_receive(f->wire, &p) == 0) free(p);
    go_channel_unref(f->wire); go_channel_unref(f->received); go_channel_unref(f->completed); go_channel_unref(f->commands_done);
    fips_rekey_hs_clear(&f->hs); fips_rekey_completed_clear(&f->live);
    fips_identity_destroy(f->id); fips_identity_destroy_secp(f->secp);
    sodium_memzero(f, sizeof *f); active = NULL;
}
