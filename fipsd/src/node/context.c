#include "instr/recorder.h"
#include "instr/writer.h"
#include "node/perf_profile.h"
#include "node/context.h"
#include "config/config.h"
#include "peer/dispatcher.h"
#include "host_os.h"
#include "log.h"
#include "blocking_executor.h"
#include "select.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

struct fipsd_node_context {
    GoSwap *configs;
    GoRefPtr initial;
    fips_identity *identity;
    fips_secp *secp;
    uint8_t epoch[8];
    fips_instant started_at;
    fipsd_peer_dispatcher peers;
    fipsd_netmon *netmon;
    fipsd_worker_set *workers;
};
typedef struct transport_child {
    struct fipsd_node *node;
    fipsd_transport *transport;
} transport_child;
struct fipsd_node {
    fipsd_node_context context;
    fipsd_node_options options;
    fipsd_transport_registry registry;
    transport_child transports[FIPSD_TRANSPORT_MAX];
    fipsd_lifecycle *lifecycle;
    fipsd_runtime *runtime;
    CancelContextResult dispatcher_cancel;
    GoChannel *transport_events, *dispatch_stop, *dispatch_done, *workers_done;
    size_t transport_offset, worker_child, netmon_child;
    bool attached;
};
const fips_identity *fipsd_node_identity(const fipsd_node_context *c) { return c->identity; }
const fips_secp *fipsd_node_secp(const fipsd_node_context *c) { return c->secp; }
const uint8_t *fipsd_node_startup_epoch(const fipsd_node_context *c) { return c->epoch; }
GoRefPtr fipsd_node_config(const fipsd_node_context *c) { return go_swap_load(c->configs); }
GoRefPtr fipsd_node_network(const fipsd_node_context *c) { return fipsd_netmon_snapshot(c->netmon); }
GoRefPtr fipsd_node_peers(const fipsd_node_context *c) { return fipsd_peer_table_snapshot_load(c->peers.table); }
GoChannel *fipsd_node_peer_commands(const fipsd_node_context *c) { return c->peers.inbox; }
int fipsd_node_dispatch_crypto(const fipsd_node_context *c, fipsd_crypto_job *j)
{ return fipsd_worker_set_dispatch(c->workers, j); }
static void noop(void *arg) { (void)arg; }
static int config_start(void *arg, fipsd_runtime *rt)
{
    fipsd_node *n = arg; n->runtime = rt;
    FIPSD_PERF_START();
    n->context.initial = go_swap_load(n->context.configs);
    return n->context.initial.ptr ? 0 : -1;
}
static void *identity_load(void *arg)
{
    fipsd_node *n = arg;
    fipsd_node_context *c = &n->context;
    fips_entropy entropy = fipsd_host_os_entropy();
    if (fips_identity_init_secp(entropy, &c->secp) != FIPS_IDENTITY_OK) return NULL;
    const fipsd_config *cfg = c->initial.ptr;
    const char *secret = fipsd_config_string(fipsd_config_get(cfg, "node.identity.nsec"));
    fips_identity_status status;
    if (secret) status = fips_identity_from_secret_str(c->secp, entropy, secret, strlen(secret), &c->identity);
    else status = fips_identity_generate(c->secp, entropy, &c->identity);
    if (status != FIPS_IDENTITY_OK) return NULL;
    entropy.fill(entropy.ctx, c->epoch, sizeof(c->epoch));
    fips_clock clock = fipsd_host_os_clock(); c->started_at = clock.now(clock.ctx);
    return c;
}
static int identity_start(void *arg, fipsd_runtime *rt)
{
    (void)rt; fipsd_node *n = arg;
    const fipsd_config *cfg = n->context.initial.ptr;
    if (!cfg) return -1;
    /* Never silently turn a requested stable identity into a new ephemeral
     * identity. Disk persistence is a separate host key-store integration. */
    if (fipsd_config_bool(fipsd_config_get(cfg, "node.identity.persistent")) &&
        !fipsd_config_string(fipsd_config_get(cfg, "node.identity.nsec"))) {
        FIPSD_ERROR("node", "persistent identity needs an explicit nsec until host key persistence is implemented");
        return -1;
    }
    return go_blocking_submit(identity_load, n) ? 0 : -1;
}
static int registry_start(void *arg, fipsd_runtime *rt)
{ (void)rt; fipsd_node *n = arg; n->registry.sealed = true; return 0; }
static int transport_start(void *arg, fipsd_runtime *rt)
{
    transport_child *c = arg;
    if (!c->node->context.identity) return -1;
    return c->transport->ops->start_async(c->transport, rt, &c->node->context,
                                         c->node->transport_events);
}
static bool transport_absent(void *arg)
{ transport_child *c = arg; return c->transport->ops->state(c->transport) == FIPSD_TRANSPORT_ABSENT; }
static void transport_stop(void *arg)
{ transport_child *c = arg; c->transport->ops->stop(c->transport); }
static int netmon_start(void *arg, fipsd_runtime *rt)
{ fipsd_node *n = arg; return fipsd_netmon_start(n->context.netmon, rt); }
static void netmon_stop(void *arg)
{ fipsd_node *n = arg; fipsd_netmon_stop(n->context.netmon); }
static bool effects(void *arg, fipsd_active_peer *peer, const fipsd_peer_actions *actions)
{
    (void)arg; (void)peer; (void)actions;
    FIPSD_ERROR("node", "peer protocol effects require the node handlers (node/handlers/handlers.h)");
    return false;
}
static void notify(fipsd_node *n, fipsd_node_event_kind kind, size_t child)
{
    fipsd_node_event *event = malloc(sizeof(*event));
    if (!event) { fipsd_runtime_shutdown(n->runtime); return; }
    *event = (fipsd_node_event){.kind = kind, .child = child};
    /* Never block the dispatcher on its own downstream supervision queue. */
    if (go_channel_try_send(fipsd_lifecycle_inbox(n->lifecycle), event) != 0) {
        free(event);
        if (!go_context_is_canceled(fipsd_runtime_context(n->runtime)))
            fipsd_runtime_shutdown(n->runtime);
    }
}
/* Events serviced per wake before timers and the publish step run once. */
#define FIPSD_DISPATCH_BATCH 64u
typedef struct dispatch_state {
    GoChannel *network, *results, *inbox, *commands;
    bool failed;
} dispatch_state;
static void dispatch_one(fipsd_node *n, dispatch_state *s, int selected, void *value);
static bool admits_commands(fipsd_node *n)
{ return !n->options.command_ready || n->options.command_ready(n->options.user); }
static void dispatch(void *arg)
{
    fipsd_node *n = arg;
    fipsd_node_context *c = &n->context;
    dispatch_state s = {.network = fipsd_netmon_events(c->netmon), .results = fipsd_worker_set_results(c->workers),
        .inbox = c->peers.inbox, .commands = n->options.command ? n->options.commands : NULL};
    void *user = n->options.user;
    bool attached = n->attached;
    for (;;) {
        void *value = NULL, *cancel = NULL;
        /* A gated mailbox stays unread: its producers park on it, and the
         * completions that free the gate (case 3) wake this select. */
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, n->dispatch_stop, NULL, &cancel},
            {GO_SELECT_RECEIVE, n->transport_events, NULL, &value},
            {GO_SELECT_RECEIVE, s.network, NULL, &value},
            {GO_SELECT_RECEIVE, s.results, NULL, &value},
            {GO_SELECT_RECEIVE, s.inbox, NULL, &value},
            {GO_SELECT_RECEIVE, admits_commands(n) ? s.commands : NULL, NULL, &value}};
        fips_instant now = c->peers.clock.now(c->peers.clock.ctx);
        /* One select multiplexes every source with the nearest stored deadline:
         * peer machines/links and the handlers' timer wheel. A failed node
         * only drains owned work until lifecycle closes the inputs. */
        uint64_t timeout = s.failed ? UINT64_MAX : fipsd_peer_dispatcher_wait_ms(&c->peers, now);
        if (!s.failed && n->options.wait_ms) {
            uint64_t w = n->options.wait_ms(user, now);
            if (w < timeout) timeout = w;
        }
        int selected;
        if (timeout == UINT64_MAX) selected = go_select(cases, 6);
        else selected = go_select_timeout(cases, 6, timeout).selected_case;
        if (selected == 0) break;
        if (selected > 0 && !value) {
            /* Disabled closed channels must not create a busy select loop. */
            if (selected == 2) s.network = NULL;
            if (selected == 3) { s.results = NULL; go_channel_close(n->workers_done); }
            if (selected == 4) s.inbox = NULL;
            if (selected == 5) s.commands = NULL;
            continue;
        }
        dispatch_one(n, &s, selected, value);
        /* Service the burst already queued, bounded, before timers and the
         * publish step: completions first so slabs return ahead of new
         * admissions, then packets, peer commands, and originated sends while
         * the pool has headroom. Closed sources fall through to the select. */
        for (unsigned left = FIPSD_DISPATCH_BATCH - 1; left && !s.failed; --left) {
            void *v = NULL; int source;
            if (s.results && go_channel_try_receive(s.results, &v) == 0 && v) source = 3;
            else if (go_channel_try_receive(n->transport_events, &v) == 0 && v) source = 1;
            else if (s.inbox && go_channel_try_receive(s.inbox, &v) == 0 && v) source = 4;
            else if (s.commands && admits_commands(n) && go_channel_try_receive(s.commands, &v) == 0 && v) source = 5;
            else break;
            dispatch_one(n, &s, source, v);
        }
        FIPSD_INSTR_GATE(timer_on);
        FIPSD_INSTR_BEGIN(timer_on, timer_start);
        if (!s.failed && !fipsd_peer_dispatcher_service(&c->peers)) s.failed = true;
        FIPSD_INSTR_END(timer_on, timer_start, DRIVE_PEER_TIMERS);
        if (!s.failed && n->options.poll && !n->options.poll(user, c->peers.clock.now(c->peers.clock.ctx))) s.failed = true;
        if (s.failed) {
            /* Keep draining owned work until lifecycle closes the inputs and
             * observes result EOF. A failed handler must not strand workers
             * behind a full completion queue during shutdown. */
            fipsd_runtime_shutdown(n->runtime);
        }
    }
    /* Close before drain so a racing producer either succeeds into the drain
     * or observes closed and keeps ownership. */
    go_channel_close(c->peers.inbox);
    void *left = NULL;
    while (go_channel_try_receive(c->peers.inbox, &left) == 0) fipsd_peer_command_discard(left);
    if (attached && n->options.detach) n->options.detach(user);
    go_channel_close(n->dispatch_done);
}
static void dispatch_one(fipsd_node *n, dispatch_state *s, int selected, void *value)
{
    fipsd_node_context *c = &n->context;
    void *user = n->options.user;
    bool failed = s->failed;
    if (selected == 1) {
        fipsd_transport_event *e = value;
        FIPSD_PERF_WAIT(e, TRANSPORT_QUEUE_WAIT);
        fipsd_transport *t = fipsd_transport_find(&n->registry, e->id);
        if (!t) failed = true;
        else if (e->kind == FIPSD_TRANSPORT_BECAME_ABSENT ||
                 e->kind == FIPSD_TRANSPORT_BECAME_PRESENT || e->kind == FIPSD_TRANSPORT_EXITED) {
            for (size_t i = 0; i < n->registry.count; ++i) if (n->registry.items[i] == t)
                notify(n, e->kind == FIPSD_TRANSPORT_EXITED ? FIPSD_NODE_EXITED :
                    e->kind == FIPSD_TRANSPORT_BECAME_ABSENT ? FIPSD_NODE_ABSENT : FIPSD_NODE_PRESENT,
                    n->transport_offset + i);
        } else if (e->kind != FIPSD_TRANSPORT_MEDIUM_CHANGED && !failed)
            failed = !n->options.transport_event || !n->options.transport_event(user, e);
        if (!failed && n->options.network_event &&
            (e->kind == FIPSD_TRANSPORT_BECAME_PRESENT || e->kind == FIPSD_TRANSPORT_BECAME_ABSENT ||
             e->kind == FIPSD_TRANSPORT_MEDIUM_CHANGED)) {
            fipsd_netmon_event facts = {.transport = e->id,
                .kind = e->kind == FIPSD_TRANSPORT_BECAME_PRESENT ? FIPSD_BINDING_UP :
                    e->kind == FIPSD_TRANSPORT_BECAME_ABSENT ? FIPSD_BINDING_DOWN : FIPSD_BINDING_MEDIUM_CHANGED};
            n->options.network_event(user, &facts);
        }
        free(e);
    } else if (selected == 2) {
        fipsd_netmon_event *e = value;
        if (e->kind == FIPSD_NETWORK_READ_FAILED) {
            FIPSD_WARN("netmon", "interface snapshot read failed");
            notify(n, FIPSD_NODE_ABSENT, n->netmon_child);
        } else if (e->kind == FIPSD_NETWORK_REFRESH) notify(n, FIPSD_NODE_PRESENT, n->netmon_child);
        for (size_t i = 0; i < n->registry.count; ++i) {
            fipsd_transport *t = n->registry.items[i];
            if (t->ops->network_changed) t->ops->network_changed(t, e);
        }
        if (!failed && n->options.network_event) n->options.network_event(user, e);
        free(e);
    } else if (selected == 3) {
        fipsd_crypto_job *j = value;
        if (!failed) failed = !n->options.crypto_result || !n->options.crypto_result(user, j);
        fipsd_crypto_job_destroy(j);
    } else if (selected == 4) {
        if (failed) fipsd_peer_command_discard(value);
        else if (!fipsd_peer_dispatcher_command(&c->peers, value)) failed = true;
    } else if (selected == 5) {
        if (!n->options.command(user, value)) failed = true;
    }
    s->failed = failed;
}
static int dispatcher_start(void *arg, fipsd_runtime *rt)
{
    fipsd_node *n = arg;
    fipsd_node_context *c = &n->context;
    const fipsd_config *cfg = c->initial.ptr;
    fipsd_peer_caps caps = {0};
    if (cfg) {
        caps.peers = (size_t)fipsd_config_uint(fipsd_config_get(cfg, "node.limits.max_peers"));
        caps.links = (size_t)fipsd_config_uint(fipsd_config_get(cfg, "node.limits.max_links"));
        caps.connections = (size_t)fipsd_config_uint(fipsd_config_get(cfg, "node.limits.max_connections"));
    }
    c->peers.table = fipsd_peer_table_create(n->options.peer_capacity, caps, NULL, 0);
    n->dispatcher_cancel = go_context_with_cancel(fipsd_runtime_context(rt));
    c->peers.context = n->dispatcher_cancel.context;
    if (!c->peers.table || !c->peers.context) return -1;
    c->peers.clock = n->options.clock.now ? n->options.clock : fipsd_host_os_clock();
    c->peers.effects = n->options.peer_effects ? n->options.peer_effects : effects;
    c->peers.result = n->options.peer_result; c->peers.user = n->options.user;
    if (!fipsd_peer_table_publish(c->peers.table)) return -1;
    /* Attach before the dispatcher fiber exists: still single-threaded here, so
     * the handlers' first snapshot is published when start returns. */
    if (n->options.attach) {
        fipsd_node_attach a = {.context = c, .peers = &c->peers, .registry = &n->registry,
            .runtime = rt, .cancel = c->peers.context};
        if (!n->options.attach(n->options.user, &a)) { FIPSD_ERROR("node", "handler attach failed"); return -1; }
        n->attached = true;
    }
    return fipsd_runtime_spawn(rt, dispatch, n);
}
static void dispatcher_stop(void *arg)
{
    fipsd_node *n = arg;
    go_channel_close(n->dispatch_stop);
    n->dispatcher_cancel.cancel(n->dispatcher_cancel.context);
    void *value;
    (void)go_channel_receive(n->dispatch_done, &value);
}
static int workers_start(void *arg, fipsd_runtime *rt)
{
    fipsd_node *n = arg;
    size_t live = fipsd_worker_set_start(n->context.workers, rt);
    (void)live;
    return 0; /* short pool stays owned; initial health predicate degrades it */
}
static bool workers_degraded(void *arg)
{ fipsd_node *n = arg; return fipsd_worker_set_live(n->context.workers) < n->options.workers; }
static void workers_stop(void *arg)
{
    fipsd_node *n = arg; fipsd_worker_set_stop(n->context.workers);
    /* Completion EOF is observed by the still-live dispatcher, not stolen by
     * the shutdown actor. This barrier is event-driven and bounded by grace. */
    void *value;
    (void)go_channel_receive(n->workers_done, &value);
}
fipsd_node *fipsd_node_create(GoSwap *configs, const fipsd_node_options *options)
{
    if (!configs) return NULL;
    fipsd_node *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    if (options) n->options = *options;
    if (!n->options.workers) n->options.workers = 2;
    if (!n->options.queue_capacity) n->options.queue_capacity = 64;
    if (!n->options.peer_capacity) n->options.peer_capacity = 256;
    if (!n->options.crypto_queue_capacity) n->options.crypto_queue_capacity = n->options.queue_capacity;
    n->context.configs = configs;
    n->context.netmon = fipsd_netmon_create(n->options.netmon_source);
    n->context.peers.inbox = go_channel_create(n->options.queue_capacity);
    n->context.workers = fipsd_worker_set_create(n->options.workers, n->options.crypto_queue_capacity);
    n->transport_events = go_channel_create(n->options.queue_capacity);
    n->dispatch_done = go_channel_create(1);
    n->dispatch_stop = go_channel_create(1); n->workers_done = go_channel_create(1);
    if (!n->context.netmon || !n->context.peers.inbox || !n->context.workers || !n->transport_events ||
        !n->dispatch_done || !n->dispatch_stop || !n->workers_done) { fipsd_node_destroy(n); return NULL; }
    return n;
}
fipsd_transport_registry *fipsd_node_transports(fipsd_node *n) { return &n->registry; }
fipsd_subsystem *fipsd_node_subsystem(fipsd_node *n)
{
    if (n->lifecycle) return fipsd_lifecycle_subsystem(n->lifecycle);
    fipsd_node_child children[FIPSD_NODE_MAX_CHILDREN];
    size_t count = 0;
    children[count++] = (fipsd_node_child){.name = "config", .required = true, .start = config_start, .stop = noop, .user = n};
    children[count++] = (fipsd_node_child){.name = "identity", .required = true, .start = identity_start, .stop = noop, .user = n};
    children[count++] = (fipsd_node_child){.name = "registry", .required = true, .start = registry_start, .stop = noop, .user = n};
    n->transport_offset = count;
    for (size_t i = 0; i < n->registry.count; ++i) {
        n->transports[i] = (transport_child){n, n->registry.items[i]};
        children[count++] = (fipsd_node_child){.name = n->registry.items[i]->name, .transport = true,
            .start = transport_start, .stop = transport_stop, .user = &n->transports[i], .absent = transport_absent};
    }
    n->netmon_child = count;
    children[count++] = (fipsd_node_child){.name = "netmon", .required = true, .start = netmon_start, .stop = netmon_stop, .user = n};
    children[count++] = (fipsd_node_child){.name = "dispatcher", .required = true, .start = dispatcher_start, .stop = dispatcher_stop, .user = n};
    n->worker_child = count;
    children[count++] = (fipsd_node_child){.name = "workers", .start = workers_start, .stop = workers_stop, .user = n, .degraded = workers_degraded};
    n->registry.sealed = true;
    n->lifecycle = fipsd_lifecycle_create(children, count);
    return n->lifecycle ? fipsd_lifecycle_subsystem(n->lifecycle) : NULL;
}
GoSwap *fipsd_node_status_slot(fipsd_node *n) { return n->lifecycle ? fipsd_lifecycle_status(n->lifecycle) : NULL; }
const fipsd_node_context *fipsd_node_get_context(const fipsd_node *n) { return &n->context; }
void fipsd_node_destroy(fipsd_node *n)
{
    if (!n) return;
    FIPSD_INSTR_SHUTDOWN();
    FIPSD_PERF_SHUTDOWN();
    fipsd_lifecycle_destroy(n->lifecycle);
    fipsd_worker_set_destroy(n->context.workers);
    fipsd_netmon_destroy(n->context.netmon);
    if (n->context.peers.table) (void)fipsd_peer_table_destroy(n->context.peers.table);
    if (n->dispatcher_cancel.context) go_context_unref(n->dispatcher_cancel.context);
    if (n->context.peers.inbox) go_channel_unref(n->context.peers.inbox);
    if (n->transport_events) {
        void *e; while (go_channel_try_receive(n->transport_events, &e) == 0) free(e);
        go_channel_unref(n->transport_events);
    }
    if (n->dispatch_done) go_channel_unref(n->dispatch_done);
    if (n->dispatch_stop) go_channel_unref(n->dispatch_stop);
    if (n->workers_done) go_channel_unref(n->workers_done);
    fips_identity_destroy(n->context.identity); fips_identity_destroy_secp(n->context.secp);
    go_refptr_release(&n->context.initial); sodium_memzero(n->context.epoch, sizeof(n->context.epoch)); free(n);
}
