#include "transport/watcher.h"
#include "transport/stream.h"
#include "select.h"
#include "test_tmpdir.h"
#include <libgo/fiber.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct fixture {
    fipsd_netmon *netmon;
    fipsd_bind_result result;
    uint64_t bind_delay;
    _Atomic uint64_t time, binds, unbinds, generation;
} fixture;
static uint64_t fake_now(void *p) { return atomic_load(&((fixture *)p)->time); }
static fipsd_bind_result bind_fake(void *arg, fipsd_runtime *rt, GoChannel *stop,
                                  const fipsd_presence_facts *facts, uint64_t gen)
{
    (void)rt; (void)stop; fixture *f = arg;
    assert(facts->up && facts->exists);
    atomic_fetch_add(&f->time, f->bind_delay);
    atomic_fetch_add(&f->binds, 1); atomic_store(&f->generation, gen); return f->result;
}
static void unbind_fake(void *arg, uint64_t gen)
{
    fixture *f = arg; assert(gen == atomic_load(&f->generation)); atomic_fetch_add(&f->unbinds, 1);
}
static fipsd_presence_facts up(uint32_t index)
{ return (fipsd_presence_facts){.valid = true, .exists = true, .up = true, .index = index, .medium = FIPS_PATH_Wired}; }
static void reducer(void)
{
    fixture f = {0}; fipsd_transport t = {.name = "test", .interface_name = "fake0"};
    fipsd_transport_shared s; fipsd_transport_shared_init(&t, &s, true);
    fipsd_binder_ops ops = {bind_fake, unbind_fake, &f};
    fipsd_watcher_source source = {.now_ms = fake_now, .user = &f};
    fipsd_watcher *w = fipsd_watcher_create(&t, &ops, &source); assert(w);
    fipsd_presence_facts absent = {.valid = true, .medium = FIPS_PATH_Unknown};
    assert(fipsd_watcher_step(w, absent, 0) == 0 && !atomic_load(&f.binds));
    f.result = FIPSD_BIND_ABSENT;
    assert(fipsd_watcher_step(w, up(1), 1) == 0 && !fipsd_presence_snapshot(&s.presence).attempts);
    f.result = FIPSD_BIND_RETRY;
    assert(fipsd_watcher_step(w, up(1), 2) == 0);
    assert(fipsd_presence_snapshot(&s.presence).attempts == 1);
    uint64_t before = atomic_load(&f.binds);
    assert(fipsd_watcher_step(w, up(1), 1001) == 0 && atomic_load(&f.binds) == before);
    f.result = FIPSD_BIND_OK;
    assert(fipsd_watcher_step(w, up(1), 1002) == 0);
    assert(fipsd_transport_shared_state(&t) == FIPSD_TRANSPORT_BOUND);
    fipsd_presence_facts carrier = up(1); carrier.carrier = true;
    assert(fipsd_watcher_step(w, carrier, 1003) == 0);
    carrier.carrier = false; assert(fipsd_watcher_step(w, carrier, 1004) == 0);
    assert(!atomic_load(&f.unbinds));
    /* Failed snapshot must not synthesize a disappearance. */
    assert(fipsd_watcher_step(w, (fipsd_presence_facts){0}, 1005) == 0 && !atomic_load(&f.unbinds));
    assert(fipsd_watcher_step(w, absent, 12000) == 0 && atomic_load(&f.unbinds) == 1);
    assert(fipsd_watcher_step(w, up(2), 12001) == 0);
    uint64_t generation = atomic_load(&f.generation);
    fipsd_watcher_binding_exited(w, generation - 1);
    assert(fipsd_watcher_step(w, up(2), 12002) == 0 && atomic_load(&f.unbinds) == 1);
    /* Three young bindings: 1s, 2s, 4s damping; stable binding clears it. */
    uint64_t now = 12003;
    for (unsigned i = 0; i < 3; ++i) {
        generation = atomic_load(&f.generation); fipsd_watcher_binding_exited(w, generation);
        assert(fipsd_watcher_step(w, up(2), now) == 0);
        before = atomic_load(&f.binds);
        uint64_t delay = fipsd_bind_backoff_ms(i + 1);
        assert(fipsd_watcher_step(w, up(2), now + delay - 1) == 0 && atomic_load(&f.binds) == before);
        now += delay; assert(fipsd_watcher_step(w, up(2), now) == 0 && atomic_load(&f.binds) == before + 1);
        now++;
    }
    assert(fipsd_watcher_step(w, up(2), now + 10000) == 0);
    assert(fipsd_watcher_step(w, up(3), now + 10001) == 0); /* replaced index without down edge */
    assert(fipsd_watcher_step(w, up(3), now + 10002) == 0);
    fipsd_presence_facts mac = up(3); mac.has_mac = true; mac.mac[0] = 1;
    /* New bind captures MAC, then a same-name/index different device detaches. */
    assert(fipsd_watcher_step(w, absent, now + 21000) == 0);
    assert(fipsd_watcher_step(w, mac, now + 21001) == 0);
    mac.mac[0] = 2; before = atomic_load(&f.unbinds);
    assert(fipsd_watcher_step(w, mac, now + 32000) == 0 && atomic_load(&f.unbinds) == before + 1);
    fipsd_watcher_destroy(w);
}
static void slow_bind(void)
{
    fixture f = {.time = 100, .result = FIPSD_BIND_RETRY, .bind_delay = 10000};
    fipsd_transport t = {.name = "slow", .interface_name = "fake0"};
    fipsd_transport_shared s; fipsd_transport_shared_init(&t, &s, true);
    fipsd_binder_ops ops = {bind_fake, unbind_fake, &f};
    fipsd_watcher_source source = {.now_ms = fake_now, .user = &f};
    fipsd_watcher *w = fipsd_watcher_create(&t, &ops, &source); assert(w);
    assert(fipsd_watcher_step(w, up(1), 100) == 0 && atomic_load(&f.time) == 10100);
    assert(fipsd_watcher_step(w, up(1), 11099) == 0 && atomic_load(&f.binds) == 1);
    atomic_store(&f.time, 11100); f.result = FIPSD_BIND_OK;
    assert(fipsd_watcher_step(w, up(1), 11100) == 0);
    assert(fipsd_presence_snapshot(&s.presence).since_ms == 21100);
    assert(fipsd_watcher_step(w, (fipsd_presence_facts){.valid = true}, 21101) == 0);
    /* Although setup took ten seconds this was a young binding, so it damps. */
    assert(fipsd_watcher_step(w, up(1), 22100) == 0 && atomic_load(&f.binds) == 2);
    fipsd_watcher_destroy(w);
}
static void *receive(GoChannel *c)
{
    void *v = NULL; GoSelectCase one = {GO_SELECT_RECEIVE, c, NULL, &v};
    GoSelectResult r = go_select_timeout(&one, 1, 5000); assert(r.ok && r.selected_case == 0); return v;
}
static fipsd_network_snapshot *sample(bool exists, uint32_t index)
{
    fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s);
    if (exists) { s->interface_count = 1; s->interfaces[0] = (fipsd_interface){.name = "fake0", .index = index, .up = true}; }
    return s;
}
static fipsd_network_snapshot *net_read(void *arg) { return receive(arg); }
static fipsd_presence_facts watcher_read(void *arg)
{
    fixture *f = arg; GoRefPtr ref = fipsd_netmon_snapshot(f->netmon);
    fipsd_presence_facts facts = fipsd_presence_from_snapshot(ref.ptr, "fake0");
    facts.medium = FIPS_PATH_Wireless; go_refptr_release(&ref); return facts;
}
static void forward(fipsd_netmon *m, fipsd_watcher *w)
{
    for (;;) {
        fipsd_netmon_event *e = receive(fipsd_netmon_events(m));
        bool end = e->kind == FIPSD_NETWORK_REFRESH;
        fipsd_watcher_network_changed(w, e); free(e); if (end) return;
    }
}
static void presence_event(GoChannel *events, fipsd_transport_event_kind kind)
{ fipsd_transport_event *e = receive(events); assert(e->kind == kind); free(e); }
typedef struct io_fixture { int fd; GoChannel *done; } io_fixture;
static void stream_reader(void *p)
{
    io_fixture *io = p; uint8_t bytes[128]; size_t n = 0;
    assert(fipsd_stream_read(io->fd, bytes, sizeof bytes, &n) == 1 && n == 114 && bytes[0] == 1);
    assert(fipsd_stream_read(io->fd, bytes, sizeof bytes, &n) == -1 && errno == EPROTO);
    assert(go_channel_send(io->done, io) == 0);
}
static void stream_writer(void *p)
{
    io_fixture *io = p; uint8_t bytes[114] = {1, 0, 110, 0};
    assert(fipsd_stream_write(io->fd, bytes, sizeof bytes) == 0);
    assert(gof_write(io->fd, bytes, 3) == 3); assert(shutdown(io->fd, SHUT_WR) == 0);
    assert(go_channel_send(io->done, io) == 0);
}
typedef struct emit_fixture { GoChannel *events, *stop, *done; GoContext *root; } emit_fixture;
static void blocked_emit(void *p)
{
    emit_fixture *f = p; uint8_t byte = 42;
    assert(fipsd_transport_emit(f->events, f->root, f->stop, FIPSD_TRANSPORT_PACKET,
                              (fips_transport_id){1}, NULL, &byte, 1) == -1);
    assert(go_channel_send(f->done, f) == 0);
}
static void actor(void)
{
    GoChannel *reads = go_channel_create(8), *wake = go_channel_create(1), *ticks = go_channel_create(1);
    GoChannel *binder_ticks = go_channel_create(1), *events = go_channel_create(8);
    assert(reads && wake && ticks && binder_ticks && events);
    fipsd_netmon_source ns = {.injected = true, .read = net_read, .user = reads, .wake = wake, .ticks = ticks};
    fixture f = {0}; f.netmon = fipsd_netmon_create(&ns); assert(f.netmon);
    fipsd_transport t = {.name = "test", .interface_name = "fake0"};
    fipsd_transport_shared s; fipsd_transport_shared_init(&t, &s, false);
    fipsd_binder_ops ops = {bind_fake, unbind_fake, &f};
    fipsd_watcher_source source = {.read = watcher_read, .user = &f, .ticks = binder_ticks, .now_ms = fake_now};
    fipsd_watcher *w = fipsd_watcher_create(&t, &ops, &source); assert(w);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && !fipsd_runtime_start(rt));
    assert(go_channel_send(reads, sample(false, 0)) == 0); assert(!fipsd_netmon_start(f.netmon, rt));
    forward(f.netmon, w);
    assert(fipsd_watcher_start(w, rt, NULL, events) == 0); /* absent is success */
    assert(!atomic_load(&f.binds));
    assert(go_channel_send(reads, sample(true, 42)) == 0); assert(go_channel_send(wake, &f) == 0);
    forward(f.netmon, w); presence_event(events, FIPSD_TRANSPORT_BECAME_PRESENT);
    assert(fipsd_presence_snapshot(&s.presence).interface.index == 42);
    atomic_store(&f.time, 11000);
    assert(go_channel_send(reads, sample(false, 0)) == 0); assert(go_channel_send(ticks, &f) == 0);
    forward(f.netmon, w); presence_event(events, FIPSD_TRANSPORT_BECAME_ABSENT);
    assert(atomic_load(&f.unbinds) == 1);
    assert(go_channel_send(reads, sample(true, 77)) == 0); assert(go_channel_send(wake, &f) == 0);
    forward(f.netmon, w); presence_event(events, FIPSD_TRANSPORT_BECAME_PRESENT);
    assert(fipsd_presence_snapshot(&s.presence).interface.index == 77 && atomic_load(&f.binds) == 2);
    int fd[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
#ifdef SO_NOSIGPIPE
    int one = 1; assert(setsockopt(fd[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) == 0);
#endif
    GoChannel *done = go_channel_create(2); assert(done);
    io_fixture reader = {fd[1], done}, writer = {fd[0], done};
    assert(!fipsd_runtime_spawn(rt, stream_reader, &reader)); assert(!fipsd_runtime_spawn(rt, stream_writer, &writer));
    receive(done); receive(done); close(fd[0]); close(fd[1]); go_channel_unref(done);
    /* A full destination must not prevent stop. The existing event remains
     * owned by its destination; the refused allocation is freed by emit. */
    emit_fixture blocked = {.events = go_channel_create(1), .stop = go_channel_create(1),
        .done = go_channel_create(1), .root = fipsd_runtime_context(rt)};
    assert(go_channel_send(blocked.events, &blocked) == 0);
    assert(fipsd_runtime_spawn(rt, blocked_emit, &blocked) == 0);
    go_channel_close(blocked.stop); receive(blocked.done);
    assert(receive(blocked.events) == &blocked);
    go_channel_unref(blocked.events); go_channel_unref(blocked.stop); go_channel_unref(blocked.done);
    fipsd_watcher_stop(w); fipsd_watcher_stop(w); fipsd_netmon_stop(f.netmon);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(atomic_load(&f.unbinds) == 2 && fipsd_transport_shared_state(&t) == FIPSD_TRANSPORT_STOPPED);
    fipsd_watcher_destroy(w); fipsd_netmon_destroy(f.netmon); fipsd_runtime_destroy(rt);
    go_channel_unref(events); go_channel_unref(reads); go_channel_unref(wake); go_channel_unref(ticks); go_channel_unref(binder_ticks);
}
int main(void)
{ (void)fipsd_test_tmpdir(NULL); reducer(); slow_bind(); actor(); puts("watcher and fiber stream tests passed"); return 0; }
