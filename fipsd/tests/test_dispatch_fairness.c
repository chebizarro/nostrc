#include "node/context.h"
#include "config/config.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    fipsd_runtime *runtime;
    GoChannel *commands;
    _Atomic bool probe_ran;
    size_t commands_seen;
} fixture;

static fipsd_network_snapshot *network(void *arg)
{
    (void)arg;
    fipsd_network_snapshot *snapshot = calloc(1, sizeof *snapshot);
    assert(snapshot);
    return snapshot;
}

static void probe(void *arg)
{
    fixture *f = arg;
    atomic_store(&f->probe_ran, true);
    fipsd_runtime_shutdown(f->runtime);
}

static bool command(void *arg, void *value)
{
    fixture *f = arg;
    assert(value == f);
    if (!f->commands_seen++)
        assert(fipsd_runtime_spawn(f->runtime, probe, f) == 0);
    /* Model a continuously ready producer. The probe is runnable on the
     * same worker; emptying this mailbox must not be required to run it. */
    if (!atomic_load(&f->probe_ran))
        assert(go_channel_try_send(f->commands, f) == 0);
    return true;
}

int main(void)
{
    fixture f = {0};
    atomic_init(&f.probe_ran, false);
    f.commands = go_channel_create(1);
    assert(f.commands);
    const char *yaml = "node: {identity: {persistent: false}, control: {enabled: false}}";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL);
    assert(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free);
    assert(configs);
    fipsd_netmon_source source = {.read = network, .injected = true};
    fipsd_node_options options = {.workers = 1, .netmon_source = &source,
        .commands = f.commands, .command = command, .user = &f};
    fipsd_node *node = fipsd_node_create(configs, &options);
    assert(node);
    fipsd_runtime_config rcfg;
    fipsd_runtime_config_default(&rcfg);
    rcfg.workers = 1;
    rcfg.handle_signals = false;
    f.runtime = fipsd_runtime_create(&rcfg);
    assert(f.runtime);
    assert(fipsd_runtime_register(f.runtime, fipsd_node_subsystem(node)) == 0);
    assert(fipsd_runtime_start(f.runtime) == 0);
    assert(go_channel_send(f.commands, &f) == 0);
    assert(fipsd_runtime_run(f.runtime) == FIPSD_RUN_OK);
    assert(f.commands_seen && atomic_load(&f.probe_ran));
    fipsd_runtime_destroy(f.runtime);
    fipsd_node_destroy(node);
    go_channel_free(f.commands);
    go_swap_free(configs);
    puts("continuously ready dispatcher lets a runnable peer fiber run: ok");
    return 0;
}
