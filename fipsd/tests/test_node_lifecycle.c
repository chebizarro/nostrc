#include "node/lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void send_event(fipsd_supervisor *s, fipsd_node_event_kind kind, size_t child)
{
    fipsd_node_actions actions;
    fipsd_supervisor_step(s, (fipsd_node_event){.kind = kind, .child = child}, &actions);
}
static void table(void)
{
    /* Restart/escalation table: transport failures are fatal only at zero up;
     * workers optional; missing interfaces are reversible, never a restart. */
    for (unsigned failures = 0; failures < 8; ++failures) {
        fipsd_supervisor s;
        assert(fipsd_supervisor_init(&s, 3, 3, 0) == 0);
        send_event(&s, FIPSD_NODE_START, 0);
        for (size_t i = 0; i < 3; ++i)
            send_event(&s, failures & (1u << i) ? FIPSD_NODE_START_FAILED : FIPSD_NODE_UP, i);
        fipsd_node_state expected = (failures & 3) == 3 ? FIPSD_NODE_FAILED :
            failures ? FIPSD_NODE_DEGRADED : FIPSD_NODE_READY;
        assert(s.status.state == expected);
        uint64_t up = s.status.up;
        send_event(&s, FIPSD_NODE_UP, 2); /* no implicit restart */
        assert(s.status.up == up);
        send_event(&s, FIPSD_NODE_STOP, 0);
        for (size_t i = 0; i < 3; ++i) send_event(&s, FIPSD_NODE_CHILD_STOPPED, i);
        assert(s.status.state == FIPSD_NODE_STOPPED);
        send_event(&s, FIPSD_NODE_START, 0);
        assert(s.status.failed == 0 && s.pending == 7);
    }
    fipsd_supervisor s;
    assert(fipsd_supervisor_init(&s, 3, 1, 2) == 0);
    send_event(&s, FIPSD_NODE_START, 0);
    send_event(&s, FIPSD_NODE_ABSENT, 0);
    for (size_t i = 0; i < 3; ++i) send_event(&s, FIPSD_NODE_UP, i);
    assert(s.status.state == FIPSD_NODE_DEGRADED && s.status.ready && s.status.up == 7);
    send_event(&s, FIPSD_NODE_PRESENT, 0); assert(s.status.state == FIPSD_NODE_READY);
    send_event(&s, FIPSD_NODE_EXITED, 2); assert(s.status.state == FIPSD_NODE_DEGRADED);
    send_event(&s, FIPSD_NODE_PRESENT, 2); assert(s.status.state == FIPSD_NODE_DEGRADED);
    send_event(&s, FIPSD_NODE_EXITED, 0); assert(s.status.state == FIPSD_NODE_FAILED);
    assert(s.owned == 7); /* exited handle still requires cleanup */
    send_event(&s, FIPSD_NODE_STOP, 0);
    for (size_t i = 0; i < 3; ++i) send_event(&s, FIPSD_NODE_CHILD_STOPPED, i);
    assert(s.status.state == FIPSD_NODE_STOPPED);

    assert(fipsd_supervisor_init(&s, 2, 1, 2) == 0);
    send_event(&s, FIPSD_NODE_START, 0); send_event(&s, FIPSD_NODE_UP, 0);
    send_event(&s, FIPSD_NODE_START_FAILED, 1); assert(s.status.state == FIPSD_NODE_FAILED);
    assert(fipsd_supervisor_init(&s, 0, 0, 0) == 0);
    send_event(&s, FIPSD_NODE_START, 0); assert(s.status.state == FIPSD_NODE_FAILED);

    assert(fipsd_supervisor_init(&s, 2, 1, 0) == 0);
    send_event(&s, FIPSD_NODE_START, 0); send_event(&s, FIPSD_NODE_UP, 0); send_event(&s, FIPSD_NODE_UP, 1);
    fipsd_node_actions a;
    fipsd_supervisor_step(&s, (fipsd_node_event){.kind = FIPSD_NODE_DRAIN, .deadline_ms = 123}, &a);
    assert(s.status.state == FIPSD_NODE_DRAINING && !s.status.ready && s.deadline_ms == 123);
    assert(a.count == 3 && a.items[0].kind == FIPSD_NODE_GATE_PEERING && a.items[1].kind == FIPSD_NODE_ARM_DRAIN);
    fipsd_supervisor_step(&s, (fipsd_node_event){.kind = FIPSD_NODE_DRAIN_COMPLETE}, &a);
    assert(a.count == 3 && a.items[1].child == 1 && a.items[2].child == 0);
}
typedef struct fake {
    GoChannel *trace;
    size_t index;
    bool fail;
} fake;
static int child_start(void *arg, fipsd_runtime *rt)
{
    (void)rt; fake *f = arg;
    assert(go_channel_send(f->trace, (void *)(f->index + 1)) == 0);
    return f->fail ? -1 : 0;
}
static void child_stop(void *arg)
{
    fake *f = arg;
    assert(go_channel_send(f->trace, (void *)(f->index + 101)) == 0);
}
static void ordering(bool fail)
{
    GoChannel *trace = go_channel_create(16);
    fake f[6]; fipsd_node_child children[6];
    const char *names[] = {"config", "identity", "transport", "netmon", "dispatcher", "workers"};
    for (size_t i = 0; i < 6; ++i) {
        f[i] = (fake){trace, i, fail && i == 3};
        children[i] = (fipsd_node_child){.name = names[i], .required = i == 0 || i == 1,
            .transport = i == 2, .start = child_start, .stop = child_stop, .user = &f[i]};
    }
    fipsd_lifecycle *l = fipsd_lifecycle_create(children, 6); assert(l);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt);
    assert(fipsd_runtime_register(rt, fipsd_lifecycle_subsystem(l)) == 0);
    assert(fipsd_runtime_start(rt) == 0);
    for (size_t i = 0; i < 6; ++i) { void *e; assert(go_channel_receive(trace, &e) == 0 && (size_t)e == i + 1); }
    GoRefPtr status = fipsd_node_status_load(fipsd_lifecycle_status(l));
    assert(status.ptr && ((fipsd_node_status *)status.ptr)->state == (fail ? FIPSD_NODE_DEGRADED : FIPSD_NODE_READY));
    go_refptr_release(&status);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    for (size_t i = 6; i > 0; --i) {
        if (f[i - 1].fail) continue;
        void *e; assert(go_channel_receive(trace, &e) == 0 && (size_t)e == i + 100);
    }
    status = fipsd_node_status_load(fipsd_lifecycle_status(l));
    assert(((fipsd_node_status *)status.ptr)->state == FIPSD_NODE_STOPPED);
    go_refptr_release(&status);
    fipsd_runtime_destroy(rt); fipsd_lifecycle_destroy(l); go_channel_unref(trace);
}
int main(int argc, char **argv)
{
    (void)argv; table(); ordering(argc > 1);
    puts("node lifecycle/supervisor: ok"); return 0;
}
