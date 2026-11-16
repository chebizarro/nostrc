#include "node/lifecycle.h"
#include "select.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

static uint64_t bit(size_t child) { return UINT64_C(1) << child; }
static void action(fipsd_node_actions *a, fipsd_node_action_kind kind, size_t child)
{ a->items[a->count++] = (fipsd_node_action){kind, child}; }
static void classify(fipsd_supervisor *s, fipsd_node_actions *a)
{
    fipsd_node_status *st = &s->status;
    if (!(st->up & s->transports) || (st->failed & s->required)) {
        st->state = FIPSD_NODE_FAILED; st->ready = false;
    } else {
        st->state = st->failed || st->absent ? FIPSD_NODE_DEGRADED : FIPSD_NODE_READY;
        st->ready = true;
    }
    action(a, FIPSD_NODE_PUBLISH, 0);
}
int fipsd_supervisor_init(fipsd_supervisor *s, size_t count, uint64_t transports, uint64_t required)
{
    if (count > FIPSD_NODE_MAX_CHILDREN) return -1;
    uint64_t configured = (UINT64_C(1) << count) - 1;
    if ((transports | required) & ~configured) return -1;
    *s = (fipsd_supervisor){.count = count, .configured = configured,
        .transports = transports, .required = required};
    return 0;
}
static void stopping(fipsd_supervisor *s, fipsd_node_actions *a)
{
    s->status.state = FIPSD_NODE_STOPPING; s->status.ready = false;
    s->pending = s->owned;
    action(a, FIPSD_NODE_PUBLISH, 0);
    for (size_t i = s->count; i > 0; --i)
        if (s->pending & bit(i - 1)) action(a, FIPSD_NODE_STOP_CHILD, i - 1);
    if (!s->pending) s->status.state = FIPSD_NODE_STOPPED;
}
void fipsd_supervisor_step(fipsd_supervisor *s, fipsd_node_event e, fipsd_node_actions *a)
{
    memset(a, 0, sizeof(*a));
    fipsd_node_status *st = &s->status;
    if (e.kind == FIPSD_NODE_START) {
        if (st->state != FIPSD_NODE_CREATED && st->state != FIPSD_NODE_STOPPED) return;
        *st = (fipsd_node_status){.state = FIPSD_NODE_STARTING};
        s->owned = 0; s->pending = s->configured;
        action(a, FIPSD_NODE_PUBLISH, 0);
        for (size_t i = 0; i < s->count; ++i) action(a, FIPSD_NODE_SPAWN, i);
        if (!s->pending) classify(s, a);
        return;
    }
    if (e.kind == FIPSD_NODE_STOP) {
        if (st->state != FIPSD_NODE_STOPPING && st->state != FIPSD_NODE_STOPPED &&
            st->state != FIPSD_NODE_CREATED) stopping(s, a);
        return;
    }
    if (e.kind == FIPSD_NODE_DRAIN && st->ready) {
        st->state = FIPSD_NODE_DRAINING; st->ready = false; s->deadline_ms = e.deadline_ms;
        action(a, FIPSD_NODE_GATE_PEERING, 0); action(a, FIPSD_NODE_ARM_DRAIN, 0);
        action(a, FIPSD_NODE_PUBLISH, 0); return;
    }
    if (e.kind == FIPSD_NODE_DRAIN_COMPLETE && st->state == FIPSD_NODE_DRAINING) {
        stopping(s, a); return;
    }
    if (e.child >= s->count) return;
    uint64_t b = bit(e.child);
    if (st->state == FIPSD_NODE_STARTING) {
        if (e.kind == FIPSD_NODE_ABSENT) { st->absent |= b; return; }
        if (e.kind == FIPSD_NODE_PRESENT) { st->absent &= ~b; return; }
        if (!(s->pending & b)) return;
        if (e.kind == FIPSD_NODE_UP || e.kind == FIPSD_NODE_UP_DEGRADED) {
            st->up |= b; s->owned |= b;
            if (e.kind == FIPSD_NODE_UP_DEGRADED) st->failed |= b;
        }
        else if (e.kind == FIPSD_NODE_START_FAILED) st->failed |= b;
        else return;
        s->pending &= ~b;
        if (!s->pending) classify(s, a);
    } else if (st->state == FIPSD_NODE_STOPPING && e.kind == FIPSD_NODE_CHILD_STOPPED) {
        s->pending &= ~b; s->owned &= ~b; st->up &= ~b; st->absent &= ~b;
        if (!s->pending) { st->state = FIPSD_NODE_STOPPED; action(a, FIPSD_NODE_PUBLISH, 0); }
    } else if (st->state == FIPSD_NODE_READY || st->state == FIPSD_NODE_DEGRADED) {
        if (!(st->up & b)) return;
        if (e.kind == FIPSD_NODE_EXITED) { st->up &= ~b; st->absent &= ~b; st->failed |= b; }
        else if (e.kind == FIPSD_NODE_ABSENT && !(st->absent & b)) st->absent |= b;
        else if (e.kind == FIPSD_NODE_PRESENT && (st->absent & b)) st->absent &= ~b;
        else return;
        classify(s, a);
    }
}
struct fipsd_lifecycle {
    fipsd_subsystem subsystem;
    fipsd_node_child children[FIPSD_NODE_MAX_CHILDREN];
    fipsd_supervisor supervisor;
    fipsd_runtime *runtime;
    GoChannel *inbox, *stop, *startup;
    GoSwap *status;
};
static bool publish(fipsd_lifecycle *l)
{
    fipsd_node_status *st = malloc(sizeof(*st));
    if (!st) return false;
    *st = l->supervisor.status;
    if (!go_swap_store(l->status, st)) { free(st); return false; }
    return true;
}
static void step(fipsd_lifecycle *l, fipsd_node_event e)
{
    fipsd_node_actions actions;
    fipsd_supervisor_step(&l->supervisor, e, &actions);
    for (size_t i = 0; i < actions.count; ++i) {
        fipsd_node_action a = actions.items[i];
        fipsd_node_child *c = &l->children[a.child];
        switch (a.kind) {
        case FIPSD_NODE_SPAWN: {
            int rc = (l->supervisor.status.failed & l->supervisor.required) ? -1 :
                c->start(c->user, l->runtime);
            FIPSD_DEBUG("node", "start %s: %s", c->name, rc ? "failed" : "up");
            if (!rc && c->absent && c->absent(c->user))
                step(l, (fipsd_node_event){.kind = FIPSD_NODE_ABSENT, .child = a.child});
            step(l, (fipsd_node_event){.kind = rc ? FIPSD_NODE_START_FAILED :
                c->degraded && c->degraded(c->user) ? FIPSD_NODE_UP_DEGRADED : FIPSD_NODE_UP,
                .child = a.child}); break;
        }
        case FIPSD_NODE_STOP_CHILD:
            c->stop(c->user);
            step(l, (fipsd_node_event){.kind = FIPSD_NODE_CHILD_STOPPED, .child = a.child}); break;
        case FIPSD_NODE_PUBLISH:
            if (!publish(l)) fipsd_runtime_shutdown(l->runtime);
            break;
        case FIPSD_NODE_GATE_PEERING:
        case FIPSD_NODE_ARM_DRAIN:
            /* Drain gating is reducer-only: the production actor stops with an
             * immediate STOP and the node handlers send Disconnect to every peer
             * while detaching on the dispatcher fiber (node/handlers/handshake.c). */
            break;
        }
    }
}
static void actor(void *arg)
{
    fipsd_lifecycle *l = arg;
    step(l, (fipsd_node_event){.kind = FIPSD_NODE_START});
    (void)go_channel_send(l->startup, l);
    while (!go_context_is_canceled(fipsd_runtime_context(l->runtime))) {
        void *value = NULL, *cancel = NULL;
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, l->stop, NULL, &cancel},
            {GO_SELECT_RECEIVE, go_context_done(fipsd_runtime_context(l->runtime)), NULL, &cancel},
            {GO_SELECT_RECEIVE, l->inbox, NULL, &value}};
        int selected = go_select(cases, 3);
        if (selected != 2 || !value) break;
        fipsd_node_event *e = value;
        /* No unimplemented drain effects exposed to production callers. */
        if (e->kind == FIPSD_NODE_EXITED || e->kind == FIPSD_NODE_ABSENT ||
            e->kind == FIPSD_NODE_PRESENT) step(l, *e);
        free(e);
    }
    go_channel_close(l->inbox);
    void *value;
    while (go_channel_try_receive(l->inbox, &value) == 0) free(value);
    step(l, (fipsd_node_event){.kind = FIPSD_NODE_STOP});
}
static int start(fipsd_subsystem *self, fipsd_runtime *rt)
{
    fipsd_lifecycle *l = self->user; l->runtime = rt;
    if (fipsd_runtime_spawn(rt, actor, l) != 0) return -1;
    void *token;
    if (go_channel_receive(l->startup, &token) != 0) return -1;
    return 0; /* Failed health remains control-visible, never reports ready. */
}
static void stop(fipsd_subsystem *self, fipsd_runtime *rt)
{ (void)rt; fipsd_lifecycle *l = self->user; go_channel_close(l->stop); }
fipsd_lifecycle *fipsd_lifecycle_create(const fipsd_node_child *children, size_t count)
{
    if (count > FIPSD_NODE_MAX_CHILDREN || (count && !children)) return NULL;
    uint64_t transports = 0, required = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!children[i].start || !children[i].stop || !children[i].name) return NULL;
        if (children[i].transport) transports |= bit(i);
        if (children[i].required) required |= bit(i);
    }
    fipsd_lifecycle *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    (void)fipsd_supervisor_init(&l->supervisor, count, transports, required);
    if (count) memcpy(l->children, children, count * sizeof(*children));
    l->subsystem = (fipsd_subsystem){"node", start, stop, l};
    l->inbox = go_channel_create(64); l->stop = go_channel_create(1); l->startup = go_channel_create(1);
    l->status = go_swap_create(NULL, free);
    if (!l->inbox || !l->stop || !l->startup || !l->status || !publish(l)) {
        fipsd_lifecycle_destroy(l); return NULL;
    }
    return l;
}
fipsd_subsystem *fipsd_lifecycle_subsystem(fipsd_lifecycle *l) { return &l->subsystem; }
GoSwap *fipsd_lifecycle_status(fipsd_lifecycle *l) { return l->status; }
GoChannel *fipsd_lifecycle_inbox(fipsd_lifecycle *l) { return l->inbox; }
void fipsd_lifecycle_destroy(fipsd_lifecycle *l)
{
    if (!l) return;
    if (l->inbox) go_channel_unref(l->inbox);
    if (l->stop) go_channel_unref(l->stop);
    if (l->startup) go_channel_unref(l->startup);
    go_swap_free(l->status); free(l);
}
