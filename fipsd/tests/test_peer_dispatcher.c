#include "peer_test.h"
#include "peer/dispatcher.h"
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct dispatcher_test {
    fipsd_peer_dispatcher dispatcher;
    GoChannel *results, *started;
    _Atomic bool reader_done, failed;
    _Atomic unsigned reads;
    bool run_ok;
} dispatcher_test;
static bool effects(void *arg, fipsd_active_peer *p, const fipsd_peer_actions *a)
{
    (void)arg; (void)p; (void)a;
    return false; /* No transport actions should be due on the injected clock. */
}
static void result(void *arg, const fipsd_peer_command *c, fipsd_peer_admission r)
{
    dispatcher_test *t = arg;
    if (r != FIPSD_ADMIT_OK) atomic_store(&t->failed, true);
    (void)c;
    /* Command completion is a channel event; no sleeps to wait for writer. */
    (void)go_channel_send(t->results, (void *)1);
}
static void writer(void *arg)
{
    dispatcher_test *t = arg;
    (void)go_channel_send(t->started, (void *)1);
    t->run_ok = fipsd_peer_dispatcher_run(&t->dispatcher);
}
static void *reader(void *arg)
{
    dispatcher_test *t = arg;
    uint64_t generation = 0, packets = 0;
    do {
        GoRefPtr ref = fipsd_peer_table_snapshot_load(t->dispatcher.table);
        const fipsd_peer_table_snapshot *s = ref.ptr;
        if (!s || s->generation < generation || s->count > 1) atomic_store(&t->failed, true);
        if (s) {
            generation = s->generation;
            if (s->count) {
                uint64_t n = s->peers[0].data.link_stats.packets_recv;
                if (n < packets || s->peers[0].data.link_stats.bytes_recv != n * 10) atomic_store(&t->failed, true);
                packets = n;
            }
        }
        go_refptr_release(&ref);
        atomic_fetch_add(&t->reads, 1);
    } while (!atomic_load(&t->reader_done));
    return NULL;
}
int peer_dispatcher_tests(void)
{
    fipsd_runtime_config cfg;
    fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); CHECK(rt && fipsd_runtime_start(rt) == 0);
    fips_test_clock clock = {0};
    dispatcher_test t = {0};
    t.results = go_channel_create(1); t.started = go_channel_create(1);
    t.dispatcher = (fipsd_peer_dispatcher){
        .table = fipsd_peer_table_create(1, (fipsd_peer_caps){0}, NULL, 0),
        .inbox = go_channel_create(2), .context = fipsd_runtime_context(rt),
        .clock = fips_test_clock_adapter(&clock), .effects = effects, .result = result, .user = &t};
    CHECK(t.dispatcher.table && t.dispatcher.inbox && t.results && t.started);
    CHECK(fipsd_runtime_spawn(rt, writer, &t) == 0);
    void *token;
    CHECK(go_channel_receive(t.started, &token) == 0);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, reader, &t) == 0);
    fipsd_peer_command *c = calloc(1, sizeof(*c)); CHECK(c);
    c->kind = FIPSD_PEER_COMMAND_INSERT; c->insert = new_peer(1, 1, 1); CHECK(c->insert);
    CHECK(go_channel_send(t.dispatcher.inbox, c) == 0);
    CHECK(go_channel_receive(t.results, &token) == 0);
    for (unsigned i = 0; i < 500; ++i) {
        c = calloc(1, sizeof(*c)); CHECK(c);
        c->kind = FIPSD_PEER_COMMAND_EVENT; c->node = peer_id(1).node_addr;
        c->event.kind = FIPSD_PEER_HEARD; c->event.authenticated_bytes = 10; c->event.wall_ms = 1000;
        CHECK(go_channel_send(t.dispatcher.inbox, c) == 0);
        CHECK(go_channel_receive(t.results, &token) == 0);
    }
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    atomic_store(&t.reader_done, true); CHECK(pthread_join(thread, NULL) == 0);
    CHECK(t.run_ok && !atomic_load(&t.failed) && atomic_load(&t.reads));
    GoRefPtr ref = fipsd_peer_table_snapshot_load(t.dispatcher.table);
    CHECK(((fipsd_peer_table_snapshot *)ref.ptr)->peers[0].data.link_stats.packets_recv == 500);
    go_refptr_release(&ref);
    CHECK(go_channel_is_closed(t.dispatcher.inbox));
    CHECK(fipsd_peer_table_destroy(t.dispatcher.table));
    go_channel_unref(t.dispatcher.inbox); go_channel_unref(t.results); go_channel_unref(t.started);
    fipsd_runtime_destroy(rt);
    /* Closed-but-buffered inbox on an empty table (no deadline). go_select
     * reports the selected index, not receive success; do not leak its dequeue. */
    GoContext *parent = go_context_background();
    CancelContextResult child = go_context_with_cancel(parent);
    GoContext *context = child.context;
    fipsd_peer_dispatcher closed = {.table = fipsd_peer_table_create(1, (fipsd_peer_caps){0}, NULL, 0),
        .inbox = go_channel_create(1), .context = context,
        .clock = fips_test_clock_adapter(&clock), .effects = effects};
    CHECK(context && closed.table && closed.inbox);
    c = calloc(1, sizeof(*c)); CHECK(c);
    c->kind = FIPSD_PEER_COMMAND_INSERT; c->insert = new_peer(2, 1, 2); CHECK(c->insert);
    CHECK(go_channel_send(closed.inbox, c) == 0); go_channel_close(closed.inbox);
    CHECK(fipsd_peer_dispatcher_run(&closed));
    CHECK(fipsd_peer_table_count(closed.table) == 1);
    CHECK(fipsd_peer_table_destroy(closed.table));
    go_channel_unref(closed.inbox); go_context_unref(context); go_context_unref(parent);
    return 0;
}
