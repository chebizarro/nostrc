#include "transport/watcher.h"
#include "node/context.h"
#include "blocking_executor.h"
#include "ticker.h"
#include "select.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
struct fipsd_watcher {
    fipsd_transport *transport;
    fipsd_binder_ops ops;
    fipsd_watcher_source source;
    const fipsd_node_context *node;
    fipsd_runtime *runtime;
    GoChannel *events, *wake, *stop, *binding_updates;
    Ticker *ticker;
    bool started, bound, announced, reported, event_driven, initial_announce, retry_eligible;
    bool (*healthy)(void *);
    uint32_t streak;
    uint64_t generation, bound_at, retry_at, absent_since;
    _Atomic uint64_t exited;
    fipsd_presence_facts binding;
};
static uint64_t now_ms(fipsd_watcher *w)
{ return w->source.now_ms ? w->source.now_ms(w->source.user) : fipsd_transport_now_ms(); }
static bool stopped(fipsd_watcher *w)
{ return go_channel_is_closed(w->stop) || (w->runtime && go_context_is_canceled(fipsd_runtime_context(w->runtime))); }
static int publish(fipsd_watcher *w, bool present)
{
    if (!w->events) return 0;
    return fipsd_transport_emit(w->events, fipsd_runtime_context(w->runtime), w->stop,
        present ? FIPSD_TRANSPORT_BECAME_PRESENT : FIPSD_TRANSPORT_BECAME_ABSENT,
        w->transport->id, NULL, NULL, 0);
}
static void detach(fipsd_watcher *w, uint64_t now)
{
    w->ops.unbind(w->ops.user, w->generation);
    w->bound = false;
    fipsd_transport_links_clear(w->transport->shared);
    fipsd_presence_transition(&w->transport->shared->presence, FIPSD_ABSENT, now);
    atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_ABSENT);
    if (w->binding_updates) (void)go_channel_try_send(w->binding_updates, w);
}
int fipsd_watcher_step(fipsd_watcher *w, fipsd_presence_facts f, uint64_t now)
{
    if (stopped(w)) return -1;
    w->retry_eligible = f.valid && f.exists && f.up;
    fipsd_presence *p = &w->transport->shared->presence;
    bool dead = w->bound && (atomic_load(&w->exited) == w->generation ||
        (w->healthy && !w->healthy(w->ops.user)));
    bool gone = f.valid && (!f.exists || !f.up);
    bool replaced = f.valid && w->bound && (f.index != w->binding.index ||
        (f.has_mac && w->binding.has_mac && memcmp(f.mac, w->binding.mac, 6)));
    bool medium_changed = f.valid && f.medium != fipsd_presence_snapshot(p).interface.medium;
    if (f.valid) fipsd_presence_observe(p, f);
    if (w->bound) {
        if (gone || replaced || dead) {
            detach(w, now);
            w->streak = now - w->bound_at < 10000 ? w->streak + 1 : 0;
            w->retry_at = now + (w->streak ? fipsd_bind_backoff_ms(w->streak) : 0);
            w->absent_since = now; w->reported = false;
            if (w->streak == 3) FIPSD_WARN("transport", "%s: binding churn; holding health until stable", w->transport->name);
            if (w->announced) { w->announced = false; return publish(w, false); }
            return 0;
        }
        if (now - w->bound_at >= 10000) {
            w->streak = 0;
            if (!w->announced) { w->announced = true; return publish(w, true); }
        }
        if (medium_changed && w->events)
            return fipsd_transport_emit(w->events, fipsd_runtime_context(w->runtime), w->stop,
                FIPSD_TRANSPORT_MEDIUM_CHANGED, w->transport->id, NULL, NULL, 0);
        return 0;
    }
    fipsd_presence_view v = fipsd_presence_snapshot(p);
    if (!v.optional && !w->reported && now - w->absent_since >= 10000) {
        FIPSD_ERROR("transport", "%s: required interface remains unavailable", w->transport->name);
        w->reported = true;
    }
    if (!f.valid || gone) return 0;
    if (now < w->retry_at) return 0;
    w->retry_at = 0; /* consume; a failed bind installs a fresh retry */
    fipsd_presence_transition(p, FIPSD_BINDING, now);
    uint64_t gen = ++w->generation;
    fipsd_bind_result result = w->ops.bind(w->ops.user, w->runtime, w->stop, &f, gen);
    /* Setup may await blocking work. Stability and retry delay start when it
     * finishes, not when it began. max preserves explicit reducer timelines. */
    uint64_t completed = now_ms(w);
    if (completed > now) now = completed;
    if (result == FIPSD_BIND_OK) {
        w->bound = true; w->bound_at = now; w->binding = f;
        fipsd_presence_bound(p, f, now);
        atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_BOUND);
        if (w->binding_updates) (void)go_channel_try_send(w->binding_updates, w);
        if (stopped(w)) { detach(w, now); return -1; }
        w->announced = w->streak < 3;
        if (w->announced) return publish(w, true);
    } else {
        fipsd_presence_transition(p, FIPSD_ABSENT, now);
        if (result == FIPSD_BIND_RETRY) {
            uint32_t n = fipsd_presence_attempt(p);
            w->retry_at = now + fipsd_bind_backoff_ms(n);
            if (n == 1) FIPSD_ERROR("transport", "%s: interface present but bind refused; retrying", w->transport->name);
        }
    }
    return 0;
}
static void *read_facts(void *arg)
{
    fipsd_watcher *w = arg;
    fipsd_presence_facts *f = malloc(sizeof *f);
    if (!f) return NULL;
    if (w->source.read) *f = w->source.read(w->source.user);
    else if (!w->transport->interface_name)
        *f = (fipsd_presence_facts){.valid = true, .exists = true, .up = true, .medium = FIPS_PATH_Unknown};
    else if (!w->node) *f = fipsd_presence_read(w->transport->interface_name);
    else {
        GoRefPtr ref = fipsd_node_network(w->node);
        *f = fipsd_presence_from_snapshot(ref.ptr, w->transport->interface_name);
        go_refptr_release(&ref);
        if (f->valid && f->exists) {
            fipsd_presence_facts os = fipsd_presence_read(w->transport->interface_name);
            if (os.valid && os.index == f->index) {
                f->carrier = os.carrier; f->medium = os.medium; f->has_mac = os.has_mac; memcpy(f->mac, os.mac, 6);
            }
        }
    }
    return f;
}
static void *read_initial_facts(void *arg)
{
    fipsd_watcher *w = arg;
    /* Transports start before netmon. Its not-yet-populated snapshot cannot
     * hide a present device (and a missing raw/BPF privilege) at startup. */
    if (w->source.read || !w->transport->interface_name) return read_facts(arg);
    fipsd_presence_facts *f = malloc(sizeof *f);
    if (f) *f = fipsd_presence_read(w->transport->interface_name);
    return f;
}
uint64_t fipsd_watcher_next_deadline(fipsd_watcher *w)
{
    uint64_t deadline = UINT64_MAX;
    if (w->bound && !w->announced) deadline = w->bound_at + 10000;
    if (!w->bound) {
        if (w->retry_eligible && w->retry_at) deadline = w->retry_at; /* include overdue retries */
        if (!w->reported && !fipsd_presence_snapshot(&w->transport->shared->presence).optional &&
            w->absent_since + 10000 < deadline) deadline = w->absent_since + 10000;
    }
    return deadline;
}
static void *new_timer(void *unused) { (void)unused; return create_ticker(1000); }
static void *stop_timer(void *t) { stop_ticker(t); return NULL; }
static void actor(void *arg)
{
    fipsd_watcher *w = arg;
    while (!stopped(w)) {
        if (w->initial_announce) {
            w->initial_announce = false;
            if (publish(w, true)) break;
        }
        fipsd_presence_facts *f = go_blocking_submit(read_facts, w);
        int rc = fipsd_watcher_step(w, f ? *f : (fipsd_presence_facts){0}, now_ms(w));
        free(f);
        if (rc < 0) break;
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, w->stop, NULL, &value},
            {GO_SELECT_RECEIVE, go_context_done(fipsd_runtime_context(w->runtime)), NULL, &value},
            {GO_SELECT_RECEIVE, w->wake, NULL, &value},
            {GO_SELECT_RECEIVE, w->ticker ? w->ticker->c : w->source.ticks, NULL, &value}};
        int selected;
        if (w->event_driven && !w->source.ticks) {
            uint64_t now = now_ms(w), deadline = fipsd_watcher_next_deadline(w);
            if (deadline == UINT64_MAX) selected = go_select(cases, 3);
            else {
                GoSelectResult result = go_select_timeout(cases, 3, deadline > now ? deadline - now : 0);
                selected = result.selected_case;
                if (selected < 0) continue;
            }
        } else selected = go_select(cases, 4);
        if (selected < 2 || go_channel_is_closed(cases[selected].chan)) break;
    }
    if (w->bound) detach(w, now_ms(w));
    atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_STOPPED);
    if (w->ticker) { go_blocking_submit(stop_timer, w->ticker); w->ticker = NULL; }
}
fipsd_watcher *fipsd_watcher_create(fipsd_transport *t, const fipsd_binder_ops *ops, const fipsd_watcher_source *source)
{
    if (!t || !t->shared || !ops || !ops->bind || !ops->unbind) return NULL;
    fipsd_watcher *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->transport = t; w->ops = *ops;
    if (source) w->source = *source;
    w->wake = go_channel_create(1); w->stop = go_channel_create(1);
    atomic_init(&w->exited, 0);
    if (!w->wake || !w->stop) { fipsd_watcher_destroy(w); return NULL; }
    w->absent_since = now_ms(w);
    return w;
}
int fipsd_watcher_start(fipsd_watcher *w, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    if (w->started || stopped(w)) return -1;
    w->runtime = rt; w->node = node; w->events = events; w->absent_since = now_ms(w);
    nsync_mu_lock(&w->transport->shared->presence.mu);
    w->transport->shared->presence.view.since_ms = w->absent_since;
    nsync_mu_unlock(&w->transport->shared->presence.mu);
    if (!w->source.ticks && !w->event_driven) {
        w->ticker = go_blocking_submit(new_timer, NULL);
        if (!w->ticker) return -1;
    }
    atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_ABSENT);
    if (w->event_driven) {
        /* Required raw/BPF privileges are a startup error when the device
         * exists. Absence still starts the event-driven watcher normally. */
        fipsd_presence_facts *f = go_blocking_submit(read_initial_facts, w);
        /* The node starts transports before its dispatcher. Do not block
         * startup publishing to an inbox a raw-frame burst may have filled. */
        w->events = NULL;
        int rc = fipsd_watcher_step(w, f ? *f : (fipsd_presence_facts){0}, now_ms(w));
        w->events = events;
        w->initial_announce = w->bound && w->announced;
        free(f);
        if (rc < 0 || (!w->bound && fipsd_presence_snapshot(&w->transport->shared->presence).attempts)) {
            if (w->bound) detach(w, now_ms(w));
            atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_FAILED);
            return -1;
        }
    }
    if (fipsd_runtime_spawn(rt, actor, w) != 0) {
        if (w->bound) detach(w, now_ms(w));
        if (w->ticker) { go_blocking_submit(stop_timer, w->ticker); w->ticker = NULL; }
        atomic_store(&w->transport->shared->state, FIPSD_TRANSPORT_STOPPED); return -1;
    }
    atomic_store(&w->transport->shared->started, true);
    w->started = true; return 0;
}
void fipsd_watcher_network_changed(fipsd_watcher *w, const fipsd_netmon_event *e)
{
    if (!w || !e) return;
    if (!e->interface_name[0] || !w->transport->interface_name || !strcmp(e->interface_name, w->transport->interface_name))
        (void)go_channel_try_send(w->wake, w);
}
void fipsd_watcher_binding_exited(fipsd_watcher *w, uint64_t gen)
{
    /* Store monotonically so a late old task cannot erase a newer exit. Also
     * accepts an exit before bind() returns and publishes presence. */
    uint64_t old = atomic_load(&w->exited);
    while (old < gen && !atomic_compare_exchange_weak(&w->exited, &old, gen)) {}
    (void)go_channel_try_send(w->wake, w);
}
void fipsd_watcher_stop(fipsd_watcher *w) { if (w && w->stop) go_channel_close(w->stop); }
void fipsd_watcher_destroy(fipsd_watcher *w)
{
    if (!w) return;
    if (w->ticker) stop_ticker(w->ticker);
    if (w->wake) go_channel_unref(w->wake);
    if (w->stop) go_channel_unref(w->stop);
    free(w);
}

void fipsd_watcher_event_driven(fipsd_watcher *w, bool (*healthy)(void *))
{ if (w && !w->started) { w->event_driven = true; w->healthy = healthy; } }

void fipsd_watcher_binding_updates(fipsd_watcher *w, GoChannel *updates)
{ w->binding_updates = updates; }
