#include "node/context.h"
#include "config/config.h"
#include "peer/table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_transport {
    unsigned started, stopped;
    size_t accepted;
    const fipsd_node_context *context;
    GoChannel *events;
} fake_transport;
static int start_transport(fipsd_transport *t, fipsd_runtime *rt,
                            const fipsd_node_context *c, GoChannel *events)
{
    (void)rt; (void)events; fake_transport *f = t->user;
    assert(fipsd_node_identity(c));
    GoRefPtr cfg = fipsd_node_config(c); assert(cfg.ptr); go_refptr_release(&cfg);
    f->context = c; f->events = events;
    ++f->started; return 0; /* absent succeeds; binder may bind later */
}
static void stop_transport(fipsd_transport *t) { fake_transport *f = t->user; ++f->stopped; }
static fipsd_transport_state transport_state(const fipsd_transport *t)
{ (void)t; return FIPSD_TRANSPORT_ABSENT; }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *a)
{ (void)t; (void)a; return 1280; }
static int send_packet(fipsd_transport *t, const fips_transport_addr *a, const uint8_t *b, size_t n)
{ (void)t; (void)a; (void)b; (void)n; return -1; }
static const fipsd_transport_ops ops = {.start_async = start_transport, .stop = stop_transport,
    .state = transport_state, .mtu = mtu, .send = send_packet};
static fipsd_network_snapshot *network(void *arg)
{ (void)arg; fipsd_network_snapshot *s = calloc(1, sizeof(*s)); assert(s); return s; }
static bool fail_after_dispatch(void *arg, const fipsd_transport_event *event)
{
    (void)event; fake_transport *f = arg;
    uint8_t bytes[32] = {2}; fips_aead_key key;
    assert(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK);
    fips_cipher_state owner; fips_cipher_state_init(&owner, &key);
    for (size_t i = 0; i < 64; ++i) {
        fipsd_crypto_job *j = calloc(1, sizeof(*j)); assert(j);
        j->operation = FIPSD_CRYPTO_SEAL;
        assert(fips_cipher_state_take_counter(&owner, &j->counter) == FIPS_NOISE_OK);
        fips_cipher_state_init(&j->key, &key); j->output = malloc(16); j->capacity = 16;
        assert(j->output && fipsd_node_dispatch_crypto(f->context, j) == 0); ++f->accepted;
    }
    fips_cipher_state_destroy(&owner); fips_aead_key_destroy(&key);
    return false; /* shutdown must drain all 64 results even after this refusal */
}
int main(int argc, char **argv)
{
    bool failed_identity = argc > 1 && !strcmp(argv[1], "failure");
    bool fail_handler = argc > 1 && !strcmp(argv[1], "handler");
    const char *yaml = failed_identity ? "node: {identity: {persistent: true}}" : "node: {identity: {persistent: false}}";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    GoSwap *configs = go_swap_create(cfg, fipsd_config_free); assert(configs);
    fipsd_netmon_source source = {.read = network, .injected = true};
    fake_transport fake = {0};
    fipsd_node_options options = {.workers = 2, .netmon_source = &source,
        .transport_event = fail_handler ? fail_after_dispatch : NULL, .user = &fake};
    fipsd_node *node = fipsd_node_create(configs, &options); assert(node);
    fipsd_transport transport = {.ops = &ops, .id = {7}, .type = "test", .name = "test-absent", .user = &fake};
    fipsd_transport_registry *registry = fipsd_node_transports(node);
    assert(fipsd_transport_register(registry, &transport) == 0);
    assert(fipsd_transport_register(registry, &transport) == -1);
    fipsd_subsystem *subsystem = fipsd_node_subsystem(node); assert(subsystem);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    assert(fipsd_runtime_register(rt, subsystem) == 0 && fipsd_runtime_start(rt) == 0);
    GoRefPtr status = fipsd_node_status_load(fipsd_node_status_slot(node)); assert(status.ptr);
    fipsd_node_status *st = status.ptr;
    assert(st->state == (failed_identity ? FIPSD_NODE_FAILED : FIPSD_NODE_DEGRADED));
    assert(st->ready == !failed_identity);
    go_refptr_release(&status);
    assert(fipsd_transport_register(registry, &transport) == -1);
    if (!failed_identity) {
        const fipsd_node_context *context = fipsd_node_get_context(node);
        assert(fipsd_node_identity(context) && fipsd_node_peer_commands(context));
        GoRefPtr peers = fipsd_node_peers(context); assert(peers.ptr);
        assert(((fipsd_peer_table_snapshot *)peers.ptr)->count == 0); go_refptr_release(&peers);
    }
    if (fail_handler) {
        fipsd_transport_event *event = calloc(1, sizeof(*event)); assert(event);
        event->id = transport.id; event->kind = FIPSD_TRANSPORT_PACKET;
        assert(go_channel_send(fake.events, event) == 0);
    } else fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(!fail_handler || fake.accepted == 64);
    assert(fake.started == (failed_identity ? 0u : 1u) && fake.stopped == fake.started);
    status = fipsd_node_status_load(fipsd_node_status_slot(node));
    assert(((fipsd_node_status *)status.ptr)->state == FIPSD_NODE_STOPPED); go_refptr_release(&status);
    fipsd_runtime_destroy(rt); fipsd_node_destroy(node); go_swap_free(configs);
    puts("real node lifecycle, context, transport registration: ok"); return 0;
}
