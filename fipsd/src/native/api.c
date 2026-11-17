#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/api.h"
#include "native/fdpass.h"
#include "native/link.h"
#include "native/protocol.h"
#include "native/seqpacket.h"
#include "log.h"
#include "node/metrics.h"
#include "node/status.h"
#include "node/transport.h"
#include "privilege.h"
#include <fips/identity/encoding.h>
#include <fips/identity/local.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/peer.h>
#include <fips/proto/framing.h>
#include <go.h>
#include <libgo/fiber.h>
#include <nsync.h>
#include <sodium.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MODULE "native"

typedef struct flow_rec {
    bool used, released;
    fipsd_native_api *api;
    uint64_t id;
    fipsd_native_flow_key key;
    fips_xonly_pubkey pubkey;
    int sock;            /* the daemon's half */
    _Atomic int pin;     /* the daemon's copy of the client's half while it may still be in flight, else -1 */
    GoChannel *inbound;  /* fipsd_native_datagram*, capacity per_flow */
    _Atomic uint64_t datagrams, bytes;
    _Atomic bool closed;
    unsigned refs;       /* reader, feeder and any transient user (inject) */
} flow_rec;
typedef struct listener_rec {
    bool used;
    fipsd_native_api *api;
    uint16_t port;
    int sock;
    GoChannel *arrivals; /* fipsd_native_arrival*, capacity backlog */
    unsigned refs;
} listener_rec;
typedef struct connection {
    fipsd_native_api *api;
    int fd, kept;
    struct connection *next, *prev;
} connection;
struct fipsd_native_api {
    fipsd_subsystem subsystem;
    GoSwap *configs;
    fipsd_node *node;
    fipsd_node_handlers *handlers;
    fipsd_runtime *runtime;
    fipsd_metrics *metrics;
    fipsd_native_limits limits;
    bool debug;
    uint16_t max_payload;
    char path[PATH_MAX];
    bool bound;
    uint64_t socket_device, socket_inode;
    nsync_mu listener_mu; /* listener fd and accept_waiter; never held across I/O */
    gof_fiber_handle accept_waiter;
    int listener, cancel_socket;
    _Atomic unsigned connections;
    char npub[FIPS_NIP19_CAPACITY];
    const fips_secp *secp;
    nsync_mu mu; /* registry, tables and the connection list; never held across gof I/O */
    fipsd_native_registry *registry;
    flow_rec *flows;
    size_t flow_cap;
    listener_rec *listeners;
    size_t listener_cap;
    connection *conns;
    GoChannel *sink, *events;
    void (*before_flow_reap)(void *, uint64_t);
    void *flow_reap_user;
    GoChannel *halt; /* closed by stop: wakes flow readers parked on the dispatcher mailbox */
    _Atomic bool stopping;
    fipsd_control_provider base;
};

static uint64_t mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t wall_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void event(fipsd_native_api *a, fipsd_native_event_kind kind, uint64_t flow, uint16_t port)
{
    if (!a->events) return;
    fipsd_native_event *e = malloc(sizeof *e);
    if (!e) return;
    *e = (fipsd_native_event){.kind = kind, .flow = flow, .port = port};
    if (go_channel_try_send(a->events, e) != 0) free(e);
}
static void count(fipsd_native_api *a, fipsd_metric metric, uint64_t n) { fipsd_metrics_add(a->metrics, metric, n); }

int fipsd_native_resolve_path(const char *override, const fipsd_control_path_env *env, char *out, size_t cap)
{
    int n;
    if (override) n = snprintf(out, cap, "%s", override);
    else if (env->run_exists) n = snprintf(out, cap, "/run/fips/api.sock");
    else if ((env->macos || env->freebsd) && (env->var_run_exists || (env->macos && env->privileged)))
        n = snprintf(out, cap, "/var/run/fips/api.sock");
    else if (env->xdg_runtime_dir && *env->xdg_runtime_dir) n = snprintf(out, cap, "%s/fips/api.sock", env->xdg_runtime_dir);
    else n = snprintf(out, cap, "/tmp/fips-api.sock");
    if (n <= 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* ---- flows ---------------------------------------------------------------- */
static void flow_unref_locked(fipsd_native_api *a, flow_rec *f)
{
    (void)a;
    if (--f->refs) return;
    (void)gof_io_reset_fd(f->sock);
    close(f->sock);
    int pin = atomic_exchange(&f->pin, -1);
    if (pin >= 0) close(pin);
    void *v;
    while (go_channel_try_receive(f->inbound, &v) == 0) fipsd_native_datagram_free(v);
    go_channel_free(f->inbound);
    memset(f, 0, sizeof *f);
    f->sock = -1; atomic_init(&f->pin, -1);
}
static void unpin(fipsd_native_api *a, flow_rec *f)
{
    int pin = atomic_exchange(&f->pin, -1);
    if (pin < 0) return;
    close(pin);
    event(a, FIPSD_NATIVE_EVENT_FLOW_UNPINNED, f->id, 0);
}
/* Give one flow's registry entry back when its client is done with it: the
 * only site that releases one, whether it was connected or accepted. */
static void flow_end(fipsd_native_api *a, flow_rec *f)
{
    nsync_mu_lock(&a->mu);
    atomic_store(&f->closed, true);
    if (!f->released) {
        f->released = true;
        if (fipsd_native_registry_release(a->registry, &f->key)) count(a, FIPSD_METRIC_native_flows_closed, 1);
    }
    go_channel_close(f->inbound);
    uint64_t id = f->id;
    flow_unref_locked(a, f);
    nsync_mu_unlock(&a->mu);
    event(a, FIPSD_NATIVE_EVENT_FLOW_RELEASED, id, 0);
}
/* Drain one flow's daemon half, forwarding what the client writes. */
static void flow_reader_fiber(void *arg)
{
    flow_rec *f = arg;
    fipsd_native_api *a = f->api;
    uint8_t *buf = malloc(FIPSD_NATIVE_MAX_DATAGRAM);
    if (!buf) { flow_end(a, f); return; }
    for (;;) {
        size_t len = 0;
        fipsd_native_received r = fipsd_native_recv_fiber(f->sock, buf, FIPSD_NATIVE_MAX_DATAGRAM, &len);
        if (r == FIPSD_NATIVE_RECEIVED_EOF) { FIPSD_DEBUG(MODULE, "flow %llu closed by its client", (unsigned long long)f->id); break; }
        if (r != FIPSD_NATIVE_RECEIVED_DATAGRAM) {
            if (errno != ECANCELED) FIPSD_DEBUG(MODULE, "flow %llu read failed: %s", (unsigned long long)f->id, strerror(errno));
            break;
        }
        /* The first datagram proves the client holds the descriptor. */
        unpin(a, f);
        atomic_fetch_add(&f->datagrams, 1);
        atomic_fetch_add(&f->bytes, len);
        event(a, FIPSD_NATIVE_EVENT_FLOW_READ, f->id, 0);
        if (len > a->max_payload) { count(a, FIPSD_METRIC_native_drop_oversize, 1); continue; }
        /* native.sent_* are counted where the dispatcher takes the datagram
         * (session.c send_data), the host seam every native datagram crosses.
         * A full dispatcher mailbox is back-pressure: this reader parks on it
         * until the dispatcher drains (or stop closes halt), so the pressure
         * reaches the client's socket as EAGAIN instead of a drop here. */
        if (fipsd_node_handlers_send_wait(a->handlers, &f->key.peer, f->key.local, f->key.remote, buf, len, a->halt) != 0) {
            if (atomic_load(&a->stopping)) break;
            FIPSD_DEBUG(MODULE, "flow %llu: outbound datagram refused by the dispatcher", (unsigned long long)f->id);
        }
    }
    sodium_memzero(buf, FIPSD_NATIVE_MAX_DATAGRAM);
    free(buf);
    flow_end(a, f);
}
/* Write datagrams the node delivered onto the client's descriptor. A full
 * client buffer drops the datagram and keeps the flow (ENOBUFS: Darwin and
 * FreeBSD SOCK_DGRAM have no sender-side queue to wait on; Linux SEQPACKET
 * parks this writer instead). Upstream drops it uncounted; here it is counted
 * as native.drop_client_buffer_full. */
static void flow_feeder_fiber(void *arg)
{
    flow_rec *f = arg;
    fipsd_native_api *a = f->api;
    for (;;) {
        void *v = NULL;
        if (go_channel_receive(f->inbound, &v) != 0 || !v) break;
        fipsd_native_datagram *dg = v;
        int rc = fipsd_native_send_fiber(f->sock, dg->bytes, dg->len, -1);
        int error = errno;
        fipsd_native_datagram_free(dg);
        if (rc == 0) continue;
        if (error == ENOBUFS) {
            count(a, FIPSD_METRIC_native_drop_client_buffer_full, 1);
            FIPSD_DEBUG(MODULE, "flow %llu: write dropped a datagram", (unsigned long long)f->id);
            continue;
        }
        if (error != ECANCELED) FIPSD_DEBUG(MODULE, "flow %llu: write failed: %s", (unsigned long long)f->id, strerror(error));
        break;
    }
    if (a->before_flow_reap) a->before_flow_reap(a->flow_reap_user, f->id);
    nsync_mu_lock(&a->mu);
    flow_unref_locked(a, f);
    nsync_mu_unlock(&a->mu);
}
/* Build one flow's socket pair and inbound queue. fds[0] is the daemon's
 * half (non-blocking, registered for cancellation), fds[1] the client's. */
static bool wire_flow(fipsd_native_api *a, int fds[2], GoChannel **inbound)
{
    if (fipsd_native_pair(fds) != 0) return false;
    size_t per_flow = a->limits.per_flow ? a->limits.per_flow : 1;
    size_t budget = per_flow * FIPSD_NATIVE_ARRIVAL_ALLOWANCE;
    if (fipsd_native_set_sndbuf(fds[0], budget) != 0) FIPSD_WARN(MODULE, "could not size a flow's send buffer: %s", strerror(errno));
    if (fipsd_native_set_rcvbuf(fds[1], budget) != 0) FIPSD_WARN(MODULE, "could not size a flow's receive buffer: %s", strerror(errno));
    *inbound = go_channel_create(per_flow);
    if (fipsd_native_set_nonblocking(fds[0], true) != 0 || gof_io_reset_fd(fds[0]) != 0 || !*inbound) {
        int e = errno ? errno : ENOMEM;
        if (*inbound) go_channel_free(*inbound);
        close(fds[0]); close(fds[1]); errno = e; return false;
    }
    return true;
}
/* Record a flow and start its two fibers; on failure everything handed in is
 * released, the registry entry included. */
static bool flow_start(fipsd_native_api *a, uint64_t id, const fipsd_native_flow_key *key, const fips_xonly_pubkey *pubkey,
                       int sock, int pin, GoChannel *inbound)
{
    nsync_mu_lock(&a->mu);
    flow_rec *f = NULL;
    for (size_t i = 0; i < a->flow_cap && !f; ++i) if (!a->flows[i].used) f = &a->flows[i];
    if (!f || atomic_load(&a->stopping)) {
        (void)fipsd_native_registry_release(a->registry, key);
        nsync_mu_unlock(&a->mu);
        (void)gof_io_reset_fd(sock); close(sock); if (pin >= 0) close(pin);
        void *v; while (go_channel_try_receive(inbound, &v) == 0) fipsd_native_datagram_free(v);
        go_channel_free(inbound);
        return false;
    }
    memset(f, 0, sizeof *f);
    f->used = true; f->api = a; f->id = id; f->key = *key; f->pubkey = *pubkey; f->sock = sock; f->inbound = inbound; f->refs = 2;
    atomic_init(&f->pin, pin); atomic_init(&f->datagrams, 0); atomic_init(&f->bytes, 0); atomic_init(&f->closed, false);
    nsync_mu_unlock(&a->mu);
    if (fipsd_runtime_spawn(a->runtime, flow_feeder_fiber, f) != 0) {
        nsync_mu_lock(&a->mu);
        (void)fipsd_native_registry_release(a->registry, key);
        f->refs = 1; flow_unref_locked(a, f);
        nsync_mu_unlock(&a->mu);
        return false;
    }
    if (fipsd_runtime_spawn(a->runtime, flow_reader_fiber, f) != 0) {
        f->released = true;
        nsync_mu_lock(&a->mu);
        (void)fipsd_native_registry_release(a->registry, key);
        go_channel_close(f->inbound); /* the feeder exits and drops its reference */
        flow_unref_locked(a, f);
        nsync_mu_unlock(&a->mu);
        return false;
    }
    return true;
}

/* ---- listeners ------------------------------------------------------------ */
static void listener_unref_locked(fipsd_native_api *a, listener_rec *l)
{
    (void)a;
    if (--l->refs) return;
    (void)gof_io_reset_fd(l->sock);
    close(l->sock);
    void *v;
    while (go_channel_try_receive(l->arrivals, &v) == 0) free(v);
    go_channel_free(l->arrivals);
    memset(l, 0, sizeof *l);
    l->sock = -1;
}
static void listener_end(fipsd_native_api *a, listener_rec *l)
{
    nsync_mu_lock(&a->mu);
    /* Every pinned flow on this port came from this listener, and no later
     * listener can hold the port until this release has been served. */
    for (size_t i = 0; i < a->flow_cap; ++i)
        if (a->flows[i].used && a->flows[i].key.local == l->port) unpin(a, &a->flows[i]);
    fipsd_native_registry_release_listener(a->registry, l->port);
    go_channel_close(l->arrivals);
    uint16_t port = l->port;
    listener_unref_locked(a, l);
    nsync_mu_unlock(&a->mu);
    FIPSD_DEBUG(MODULE, "listener on port %u closed by its client", (unsigned)port);
    event(a, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, port);
}
/* End of file is observable only by reading: a listener that never read its
 * own half could not notice its client had gone. A client has no message to
 * send here, so anything but end of file is discarded. */
static void listener_reader_fiber(void *arg)
{
    listener_rec *l = arg;
    fipsd_native_api *a = l->api;
    uint8_t *buf = malloc(FIPSD_NATIVE_MAX_DATAGRAM);
    if (buf) {
        for (;;) {
            size_t len = 0;
            fipsd_native_received r = fipsd_native_recv_fiber(l->sock, buf, FIPSD_NATIVE_MAX_DATAGRAM, &len);
            if (r != FIPSD_NATIVE_RECEIVED_DATAGRAM) break;
        }
        free(buf);
    }
    listener_end(a, l);
}
/* Wire one arriving flow and hand its descriptor to the listener's client:
 * every held datagram onto the flow's half first, the arrival carrying the
 * descriptor last, both as non-parking sends onto pairs no process reads yet.
 * The daemon keeps its copy of the client's half (the pin) until the client
 * writes on the flow or closes the listener. */
static void hand_over(fipsd_native_api *a, listener_rec *l, const fipsd_native_arrival *arrival)
{
    int fds[2]; GoChannel *inbound;
    if (!wire_flow(a, fds, &inbound)) { FIPSD_WARN(MODULE, "could not open a socket pair for an arriving flow: %s", strerror(errno)); return; }
    fipsd_native_flow_key key; fips_xonly_pubkey pubkey;
    fipsd_native_datagram **held = NULL; size_t held_count = 0;
    nsync_mu_lock(&a->mu);
    fipsd_native_registry_error err = atomic_load(&a->stopping) ? FIPSD_NATIVE_REG_NO_PENDING
        : fipsd_native_registry_accept(a->registry, arrival->flow, inbound, mono_ms(), &key, &pubkey, &held, &held_count);
    if (err == FIPSD_NATIVE_REG_OK) count(a, FIPSD_METRIC_native_flows_accepted, 1);
    nsync_mu_unlock(&a->mu);
    if (err != FIPSD_NATIVE_REG_OK) {
        FIPSD_DEBUG(MODULE, "an arriving flow was gone before it could be wired");
        (void)gof_io_reset_fd(fds[0]); close(fds[0]); close(fds[1]); go_channel_free(inbound);
        return;
    }
    bool failed = false; int error = 0;
    for (size_t i = 0; i < held_count && !failed; ++i)
        if (fipsd_native_send_once(fds[0], held[i]->bytes, held[i]->len) < 0) { failed = true; error = errno; }
    if (!failed) {
        char peer[FIPS_NIP19_CAPACITY] = "";
        (void)fips_encode_npub(&pubkey, peer, sizeof peer);
        json_t *m = json_pack("{s:I,s:s,s:s,s:i,s:i,s:i,s:I}", "flow_id", (json_int_t)arrival->flow, "peer", peer, "node", a->npub,
            "local_port", (int)key.local, "remote_port", (int)key.remote, "max_payload", (int)a->max_payload, "held", (json_int_t)held_count);
        char *message = m ? fipsd_native_encode_object(m) : NULL;
        json_decref(m);
        if (!message) { failed = true; error = ENOMEM; }
        else if (fipsd_native_sendmsg_fd(l->sock, message, strlen(message), fds[1], 0) < 0) { failed = true; error = errno; }
        free(message);
    }
    for (size_t i = 0; i < held_count; ++i) fipsd_native_datagram_free(held[i]);
    free(held);
    if (failed) {
        /* Three parts, all required: close the pair, give the registry entry
         * back and count the drop. */
        fipsd_native_drop_reason reason = fipsd_native_drop_reason_why(error);
        (void)gof_io_reset_fd(fds[0]); close(fds[0]); close(fds[1]);
        void *v; while (go_channel_try_receive(inbound, &v) == 0) fipsd_native_datagram_free(v);
        go_channel_free(inbound);
        nsync_mu_lock(&a->mu);
        (void)fipsd_native_registry_release(a->registry, &key);
        nsync_mu_unlock(&a->mu);
        count(a, fipsd_native_drop_reason_metric(reason), 1);
        return;
    }
    (void)flow_start(a, arrival->flow, &key, &pubkey, fds[0], fds[1], inbound);
}
static void listener_arrivals_fiber(void *arg)
{
    listener_rec *l = arg;
    fipsd_native_api *a = l->api;
    for (;;) {
        void *v = NULL;
        if (go_channel_receive(l->arrivals, &v) != 0 || !v) break;
        hand_over(a, l, v);
        free(v);
    }
    nsync_mu_lock(&a->mu);
    listener_unref_locked(a, l);
    nsync_mu_unlock(&a->mu);
}

/* ---- commands ------------------------------------------------------------- */
static json_t *shutting_down(void) { return fipsd_native_deny("ECONNREFUSED", "node is shutting down"); }
static json_t *refused(const fipsd_native_refusal *why)
{
    char message[256];
    fipsd_native_refusal_message(why, message, sizeof message);
    return fipsd_native_deny(fipsd_native_registry_errno(why->error), message);
}
static json_t *do_connect(fipsd_native_api *a, const fipsd_native_command *cmd, int *pass)
{
    char message[1024];
    fips_xonly_pubkey key;
    fips_identity_status st = fips_decode_npub(a->secp, cmd->peer, strlen(cmd->peer), &key);
    if (st != FIPS_IDENTITY_OK) {
        snprintf(message, sizeof message, "invalid peer: %s", fips_identity_error_name(st));
        return fipsd_native_deny("EINVAL", message);
    }
    /* The pair exists before the port is claimed, so a failure to build it
     * cannot leave a port held by a flow that does not exist. */
    int fds[2]; GoChannel *inbound;
    if (!wire_flow(a, fds, &inbound)) {
        snprintf(message, sizeof message, "could not open a flow: %s", strerror(errno));
        return fipsd_native_deny("EMFILE", message);
    }
    uint16_t held = 0; uint64_t id = 0; fipsd_native_refusal why;
    nsync_mu_lock(&a->mu);
    bool stopping = atomic_load(&a->stopping);
    fipsd_native_registry_error err = stopping ? FIPSD_NATIVE_REG_NO_PENDING
        : fipsd_native_registry_connect(a->registry, &key, cmd->remote, cmd->has_local, cmd->local, inbound, mono_ms(), &held, &id, &why);
    if (err == FIPSD_NATIVE_REG_OK) count(a, FIPSD_METRIC_native_flows_opened, 1);
    nsync_mu_unlock(&a->mu);
    if (err != FIPSD_NATIVE_REG_OK) {
        (void)gof_io_reset_fd(fds[0]); close(fds[0]); close(fds[1]); go_channel_free(inbound);
        return stopping ? shutting_down() : refused(&why);
    }
    fipsd_native_flow_key k = {.peer = fips_node_addr_from_pubkey(&key), .remote = cmd->remote, .local = held};
    if (!flow_start(a, id, &k, &key, fds[0], -1, inbound)) {
        close(fds[1]);
        return atomic_load(&a->stopping) ? shutting_down() : fipsd_native_deny("EMFILE", "could not open a flow: no flow slot is free");
    }
    /* The dispatcher needs the key behind the address to open a session. */
    fips_peer_identity identity;
    if (fips_peer_identity_from_pubkey(a->secp, &key, &identity) == FIPS_IDENTITY_OK &&
        fipsd_node_handlers_learn_identity(a->handlers, &identity) != 0)
        FIPSD_DEBUG(MODULE, "flow %llu: could not seed the peer's identity", (unsigned long long)id);
    char peer[FIPS_NIP19_CAPACITY] = "";
    (void)fips_encode_npub(&key, peer, sizeof peer);
    json_t *data = json_pack("{s:I,s:i,s:i,s:s,s:s,s:i}", "flow_id", (json_int_t)id, "local_port", (int)held,
        "remote_port", (int)cmd->remote, "peer", peer, "node", a->npub, "max_payload", (int)a->max_payload);
    *pass = fds[1];
    return fipsd_native_ok(data);
}
static json_t *do_listen(fipsd_native_api *a, const fipsd_native_command *cmd, int *pass)
{
    char message[1024];
    size_t backlog = a->limits.backlog, queue = backlog ? backlog : 1;
    int fds[2];
    if (fipsd_native_pair(fds) != 0) {
        snprintf(message, sizeof message, "could not open a listener: %s", strerror(errno));
        return fipsd_native_deny("EMFILE", message);
    }
    /* Both halves, because the two kernels charge different ones. */
    size_t budget = queue * FIPSD_NATIVE_ARRIVAL_ALLOWANCE;
    if (fipsd_native_set_sndbuf(fds[0], budget) != 0) FIPSD_WARN(MODULE, "could not size a listener's send buffer: %s", strerror(errno));
    if (fipsd_native_set_rcvbuf(fds[1], budget) != 0) FIPSD_WARN(MODULE, "could not size a listener's receive buffer: %s", strerror(errno));
    GoChannel *arrivals = go_channel_create(queue);
    if (fipsd_native_set_nonblocking(fds[0], true) != 0 || gof_io_reset_fd(fds[0]) != 0 || !arrivals) {
        snprintf(message, sizeof message, "could not open a listener: %s", strerror(errno ? errno : ENOMEM));
        if (arrivals) go_channel_free(arrivals);
        close(fds[0]); close(fds[1]);
        return fipsd_native_deny("EMFILE", message);
    }
    nsync_mu_lock(&a->mu);
    bool stopping = atomic_load(&a->stopping);
    listener_rec *l = NULL;
    for (size_t i = 0; i < a->listener_cap && !l; ++i) if (!a->listeners[i].used) l = &a->listeners[i];
    uint16_t held = 0; fipsd_native_refusal why = {0};
    fipsd_native_registry_error err = stopping || !l ? FIPSD_NATIVE_REG_NO_PENDING
        : fipsd_native_registry_listen(a->registry, cmd->has_local, cmd->local, arrivals, &held, &why);
    if (err == FIPSD_NATIVE_REG_OK) {
        memset(l, 0, sizeof *l);
        l->used = true; l->api = a; l->port = held; l->sock = fds[0]; l->arrivals = arrivals; l->refs = 2;
    }
    nsync_mu_unlock(&a->mu);
    if (err != FIPSD_NATIVE_REG_OK) {
        (void)gof_io_reset_fd(fds[0]); close(fds[0]); close(fds[1]); go_channel_free(arrivals);
        if (stopping) return shutting_down();
        if (!l) return fipsd_native_deny("EMFILE", "could not open a listener: the listener table is full");
        return refused(&why);
    }
    bool spawned = fipsd_runtime_spawn(a->runtime, listener_arrivals_fiber, l) == 0;
    if (spawned && fipsd_runtime_spawn(a->runtime, listener_reader_fiber, l) != 0) {
        /* The arrivals fiber runs; close its channel and let it drop its reference. */
        nsync_mu_lock(&a->mu);
        fipsd_native_registry_release_listener(a->registry, held);
        go_channel_close(l->arrivals);
        listener_unref_locked(a, l);
        nsync_mu_unlock(&a->mu);
        spawned = false;
    } else if (!spawned) {
        nsync_mu_lock(&a->mu);
        fipsd_native_registry_release_listener(a->registry, held);
        l->refs = 1; listener_unref_locked(a, l);
        nsync_mu_unlock(&a->mu);
    }
    if (!spawned) { close(fds[1]); return fipsd_native_deny("EMFILE", "could not open a listener: no fiber"); }
    json_t *data = json_pack("{s:i,s:s,s:I}", "local_port", (int)held, "node", a->npub, "backlog", (json_int_t)backlog);
    *pass = fds[1];
    return fipsd_native_ok(data);
}
static flow_rec *flow_by_id_locked(fipsd_native_api *a, uint64_t id)
{
    /* Logical removal precedes FLOW_RELEASED; feeder/inject references may
     * keep the storage alive, but must not keep the flow addressable. */
    for (size_t i = 0; i < a->flow_cap; ++i)
        if (a->flows[i].used && !a->flows[i].released && a->flows[i].id == id) return &a->flows[i];
    return NULL;
}
static json_t *no_such_flow(uint64_t id)
{
    char message[64];
    snprintf(message, sizeof message, "no such flow: %llu", (unsigned long long)id);
    return fipsd_native_deny("ENOENT", message);
}
static json_t *do_stats(fipsd_native_api *a, const fipsd_native_command *cmd)
{
    nsync_mu_lock(&a->mu);
    flow_rec *f = flow_by_id_locked(a, cmd->flow);
    json_t *data = f ? json_pack("{s:I,s:i,s:I,s:I,s:b}", "flow_id", (json_int_t)f->id, "local_port", (int)f->key.local,
        "rx_datagrams", (json_int_t)atomic_load(&f->datagrams), "rx_bytes", (json_int_t)atomic_load(&f->bytes),
        "closed", (int)atomic_load(&f->closed)) : NULL;
    nsync_mu_unlock(&a->mu);
    return f ? fipsd_native_ok(data) : no_such_flow(cmd->flow);
}
static json_t *do_inject(fipsd_native_api *a, const fipsd_native_command *cmd)
{
    nsync_mu_lock(&a->mu);
    flow_rec *f = flow_by_id_locked(a, cmd->flow);
    if (f) ++f->refs;
    int sock = f ? f->sock : -1;
    nsync_mu_unlock(&a->mu);
    if (!f) return no_such_flow(cmd->flow);
    json_t *response = NULL;
    for (uint32_t i = 0; i < cmd->repeat; ++i) {
        if (fipsd_native_send_fiber(sock, cmd->data, cmd->data_len, FIPSD_NATIVE_REPLY_TIMEOUT_MS) != 0) {
            char message[256];
            snprintf(message, sizeof message, "wrote %u of %u datagrams: %s", (unsigned)i, (unsigned)cmd->repeat, strerror(errno));
            response = fipsd_native_deny("EIO", message);
            break;
        }
    }
    nsync_mu_lock(&a->mu);
    flow_unref_locked(a, f);
    nsync_mu_unlock(&a->mu);
    if (response) return response;
    return fipsd_native_ok(json_pack("{s:I,s:I,s:I}", "flow_id", (json_int_t)cmd->flow, "datagrams", (json_int_t)cmd->repeat,
        "bytes", (json_int_t)(cmd->data_len * cmd->repeat)));
}
static json_t *do_arrive(fipsd_native_api *a, const fipsd_native_command *cmd)
{
    char message[256];
    fips_xonly_pubkey key;
    fips_identity_status st = fips_decode_npub(a->secp, cmd->peer, strlen(cmd->peer), &key);
    if (st != FIPS_IDENTITY_OK) {
        snprintf(message, sizeof message, "invalid peer: %s", fips_identity_error_name(st));
        return fipsd_native_deny("EINVAL", message);
    }
    fipsd_native_datagram *dg = fipsd_native_datagram_new(cmd->data, cmd->data_len);
    if (!dg) return fipsd_native_deny("ECONNREFUSED", "allocation failed");
    fips_node_addr peer = fips_node_addr_from_pubkey(&key);
    nsync_mu_lock(&a->mu);
    if (atomic_load(&a->stopping)) { nsync_mu_unlock(&a->mu); fipsd_native_datagram_free(dg); return shutting_down(); }
    fipsd_native_outcome outcome = fipsd_native_link_deliver(a->registry, &peer, &key, cmd->src, cmd->dst, dg, mono_ms());
    nsync_mu_unlock(&a->mu);
    fipsd_native_link_count(a->metrics, &outcome, cmd->data_len);
    char text[96];
    fipsd_native_outcome_text(&outcome, text, sizeof text);
    bool named = outcome.kind == FIPSD_NATIVE_OUTCOME_ANNOUNCED || outcome.kind == FIPSD_NATIVE_OUTCOME_HELD;
    return fipsd_native_ok(json_pack("{s:s,s:o}", "outcome", text, "flow_id", named ? json_integer((json_int_t)outcome.flow) : json_null()));
}
static json_t *answer(connection *c, const uint8_t *line, size_t len, int *pass)
{
    fipsd_native_api *a = c->api;
    char message[1024];
    if (!fipsd_native_bounded_depth((const char *)line, len)) return fipsd_native_deny("EINVAL", "invalid request: nesting limit exceeded");
    json_error_t error;
    json_t *request = json_loadb((const char *)line, len, JSON_REJECT_DUPLICATES, &error);
    if (!request) {
        snprintf(message, sizeof message, "invalid request: %s at line %d column %d", error.text, error.line, error.column);
        return fipsd_native_deny("EINVAL", message);
    }
    if (!json_is_object(request) || !json_is_string(json_object_get(request, "command"))) {
        json_decref(request);
        return fipsd_native_deny("EINVAL", "invalid request: missing field `command`");
    }
    fipsd_native_command cmd;
    fipsd_native_command_error e = fipsd_native_parse(request, &cmd, message, sizeof message);
    json_decref(request);
    if (e != FIPSD_NATIVE_CMD_OK) return fipsd_native_deny(fipsd_native_command_errno(e), message);
    /* Refused by name rather than reported as unknown: a client driving the
     * harness needs to tell "this node will not" from "this build cannot". */
    const char *debug_name = fipsd_native_debug_name(cmd.kind);
    if (debug_name && !a->debug) {
        snprintf(message, sizeof message, "'%s' is a debug command and is disabled; set node.native_api.debug_commands to enable it", debug_name);
        fipsd_native_command_clear(&cmd);
        return fipsd_native_deny("ECONNREFUSED", message);
    }
    json_t *response = NULL;
    switch (cmd.kind) {
    case FIPSD_NATIVE_CMD_CONNECT: response = do_connect(a, &cmd, pass); break;
    case FIPSD_NATIVE_CMD_LISTEN: response = do_listen(a, &cmd, pass); break;
    case FIPSD_NATIVE_CMD_STATS: response = do_stats(a, &cmd); break;
    case FIPSD_NATIVE_CMD_INJECT: response = do_inject(a, &cmd); break;
    case FIPSD_NATIVE_CMD_ARRIVE: response = do_arrive(a, &cmd); break;
    }
    fipsd_native_command_clear(&cmd);
    return response;
}

/* ---- connections ---------------------------------------------------------- */
static void connection_close(connection *c)
{
    fipsd_native_api *a = c->api;
    if (c->kept >= 0) close(c->kept);
    nsync_mu_lock(&a->mu);
    if (c->prev) c->prev->next = c->next; else a->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    nsync_mu_unlock(&a->mu);
    (void)gof_io_reset_fd(c->fd);
    close(c->fd);
    atomic_fetch_sub(&a->connections, 1);
    free(c);
    event(a, FIPSD_NATIVE_EVENT_CONNECTION_CLOSED, 0, 0);
}
/* Answer the commands on one connection in order until it closes or
 * misbehaves. The daemon keeps its copy of the descriptor in the last reply
 * until the client's next command or the connection's end. */
static void connection_fiber(void *arg)
{
    connection *c = arg;
    uint8_t *buf = malloc(FIPSD_NATIVE_MAX_COMMAND + 1);
    size_t len = 0;
    bool eof = false;
    while (buf) {
        size_t line_len = 0, consumed = 0; bool too_large = false;
        bool have = fipsd_native_line_take(buf, len, &line_len, &consumed, &too_large);
        if (too_large) { FIPSD_DEBUG(MODULE, "connection ended: native API command too large"); break; }
        if (!have) {
            if (eof) {
                /* A final command without its newline is still answered. */
                if (!len) break;
                line_len = consumed = len;
            } else {
                fipsd_native_chunk chunk;
                if (fipsd_native_recvmsg_fd(c->fd, buf + len, FIPSD_NATIVE_MAX_COMMAND + 1 - len, &chunk, -1) != 0) break;
                if (chunk.fd >= 0) close(chunk.fd); /* a client passing descriptors is not this protocol */
                if (chunk.len == 0) { eof = true; continue; }
                len += chunk.len;
                continue;
            }
        }
        if (c->kept >= 0) { close(c->kept); c->kept = -1; }
        int pass = -1;
        json_t *response = answer(c, buf, line_len, &pass);
        memmove(buf, buf + consumed, len - consumed); len -= consumed;
        char *encoded = response ? fipsd_native_encode_reply(response) : NULL;
        json_decref(response);
        if (!encoded) { if (pass >= 0) close(pass); break; }
        size_t size = strlen(encoded);
        /* One whole line per sendmsg; a short write is an error, never retried. */
        ssize_t n = fipsd_native_sendmsg_fd(c->fd, encoded, size, pass, FIPSD_NATIVE_REPLY_TIMEOUT_MS);
        free(encoded);
        if (n != (ssize_t)size) { if (pass >= 0) close(pass); break; }
        c->kept = pass;
        if (eof && !len) break;
    }
    free(buf);
    connection_close(c);
}
static int socket_new(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { int e = errno; close(fd); errno = e; return -1; }
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) { int e = errno; close(fd); errno = e; return -1; }
#endif
    return fd;
}
static void accept_fiber(void *arg)
{
    fipsd_native_api *a = arg;
    GoContext *ctx = fipsd_runtime_context(a->runtime);
    nsync_mu_lock(&a->listener_mu);
    a->accept_waiter = gof_hook_current();
    nsync_mu_unlock(&a->listener_mu);
    while (!go_context_is_canceled(ctx)) {
        int fd = gof_accept(a->listener, NULL, NULL, -1);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!go_context_is_canceled(ctx)) FIPSD_WARN(MODULE, "accept failed: %s", strerror(errno));
            break;
        }
        if (go_context_is_canceled(ctx)) { close(fd); break; }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { close(fd); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) { close(fd); continue; }
#endif
        if (atomic_load(&a->connections) >= FIPSD_NATIVE_MAX_CONNECTIONS || gof_io_reset_fd(fd) != 0) { close(fd); continue; }
        connection *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->api = a; c->fd = fd; c->kept = -1;
        nsync_mu_lock(&a->mu);
        c->next = a->conns; if (a->conns) a->conns->prev = c; a->conns = c;
        nsync_mu_unlock(&a->mu);
        atomic_fetch_add(&a->connections, 1);
        if (fipsd_runtime_spawn(a->runtime, connection_fiber, c) != 0) connection_close(c);
    }
    nsync_mu_lock(&a->listener_mu);
    a->accept_waiter = NULL;
    close(a->listener); a->listener = -1;
    close(a->cancel_socket); a->cancel_socket = -1;
    nsync_mu_unlock(&a->listener_mu);
}

/* ---- the node's datagram sink --------------------------------------------- */
static void deliver_notice(fipsd_native_api *a, fipsd_node_notice *n)
{
    FIPSD_PERF_WAIT(n, ENDPOINT_EVENT_WAIT);
    fipsd_native_outcome outcome = {.kind = FIPSD_NATIVE_OUTCOME_DROPPED, .reason = FIPSD_NATIVE_REASON_NO_PORT};
    if (!n->has_pubkey) FIPSD_DEBUG(MODULE, "inbound datagram from a peer with no known key, dropped");
    else {
        fipsd_native_datagram *dg = fipsd_native_datagram_new(n->payload, n->len);
        if (!dg) outcome.reason = FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL;
        else {
            nsync_mu_lock(&a->mu);
            outcome = fipsd_native_link_deliver(a->registry, &n->peer, &n->pubkey, n->src_port, n->dst_port, dg, mono_ms());
            nsync_mu_unlock(&a->mu);
        }
    }
    fipsd_native_link_count(a->metrics, &outcome, n->len);
    event(a, FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED, 0, n->dst_port);
    fipsd_node_notice_free(n);
}
static void expire(fipsd_native_api *a, uint64_t now)
{
    uint64_t ids[64];
    nsync_mu_lock(&a->mu);
    size_t n = fipsd_native_registry_expire(a->registry, now, ids, 64);
    nsync_mu_unlock(&a->mu);
    if (!n) return;
    count(a, FIPSD_METRIC_native_flows_expired, n);
    FIPSD_WARN(MODULE, "%zu pending flow(s) expired before a listener took them", n);
}
static void rx_fiber(void *arg)
{
    fipsd_native_api *a = arg;
    GoContext *ctx = fipsd_runtime_context(a->runtime);
    uint64_t last = mono_ms();
    while (!go_context_is_canceled(ctx)) {
        void *value = NULL, *token = NULL;
        GoSelectCase cases[] = {
            {.op = GO_SELECT_RECEIVE, .chan = a->sink, .recv_buf = &value},
            {.op = GO_SELECT_RECEIVE, .chan = go_context_done(ctx), .recv_buf = &token}
        };
        GoSelectResult r = go_select_timeout(cases, 2, 1000);
        if (r.selected_case == 1) break;
        if (r.selected_case == 0 && value) {
            deliver_notice(a, value);
            /* Drain what the dispatcher already queued before re-registering
             * the select: one wake delivers a burst, bounded by the sink. */
            for (unsigned left = FIPSD_NATIVE_SINK_CAPACITY; left; --left) {
                value = NULL;
                if (go_channel_try_receive(a->sink, &value) != 0 || !value) break;
                deliver_notice(a, value);
            }
        }
        uint64_t now = mono_ms();
        if (now - last >= 1000) { last = now; expire(a, now); }
    }
}

/* ---- lifecycle ------------------------------------------------------------ */
static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int unix_addr(const char *path, struct sockaddr_un *addr)
{
    if (!*path || strlen(path) >= sizeof addr->sun_path) { errno = ENAMETOOLONG; return -1; }
    memset(addr, 0, sizeof *addr); addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, strlen(path) + 1);
    return 0;
}
/* The control socket's parent policy: a parent the daemon created or a
 * canonical runtime directory is secured; shared or custom parents are left alone. */
static int private_parent(const char *path, gid_t gid, char *err, size_t cap)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    bool created = !is_dir(parent);
    if (created) {
        for (char *p = parent + (parent[0] == '/'); ; ++p) {
            if (*p != '/' && *p) continue;
            char saved = *p; *p = 0;
            if (mkdir(parent, FIPSD_SOCKET_DIR_MODE) != 0 && errno != EEXIST) return -1;
            struct stat st;
            if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
            *p = saved;
            if (!saved) break;
        }
    }
    char xdg[PATH_MAX] = "";
    const char *env = getenv("XDG_RUNTIME_DIR");
    if (env && *env) snprintf(xdg, sizeof xdg, "%s/fips", env);
    bool canonical = !strcmp(parent, "/run/fips") || !strcmp(parent, "/var/run/fips") || (*xdg && !strcmp(parent, xdg));
    if (created || canonical) return fipsd_privilege_secure_private_dir(parent, gid, err, cap);
    return 0;
}
static int bind_listener(fipsd_native_api *a)
{
    char err[512] = "";
    gid_t gid = getegid();
    int group = fipsd_privilege_resolve_group(FIPSD_SOCKET_GROUP, &gid, err, sizeof err);
    if (group < 0) { FIPSD_ERROR(MODULE, "%s", err); return -1; }
    if (private_parent(a->path, gid, err, sizeof err) != 0) {
        FIPSD_ERROR(MODULE, "cannot secure socket parent: %s %s", err, strerror(errno)); return -1;
    }
    struct sockaddr_un address;
    if (unix_addr(a->path, &address) != 0) return -1;
    struct stat st;
    if (lstat(a->path, &st) == 0) {
        /* Reclaim a dead daemon's socket, never a live one's and never a
         * file or symlink. */
        if (!S_ISSOCK(st.st_mode) || (st.st_uid != geteuid() && geteuid() != 0)) { errno = EEXIST; return -1; }
        int probe = socket_new();
        if (probe < 0) return -1;
        int rc = connect(probe, (struct sockaddr *)&address, sizeof address);
        int e = errno; close(probe);
        if (rc == 0 || e != ECONNREFUSED) { errno = EADDRINUSE; return -1; }
        struct stat again;
        if (lstat(a->path, &again) != 0 || again.st_dev != st.st_dev || again.st_ino != st.st_ino) { errno = EEXIST; return -1; }
        if (unlink(a->path) != 0) return -1;
    } else if (errno != ENOENT) return -1;
    int fd = socket_new();
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0) { close(fd); return -1; }
    if (lstat(a->path, &st) != 0) { close(fd); return -1; }
    a->bound = true; a->socket_device = (uint64_t)st.st_dev; a->socket_inode = (uint64_t)st.st_ino;
    if (fipsd_privilege_secure_socket(a->path, gid, err, sizeof err) != 0 || listen(fd, 128) != 0) {
        FIPSD_ERROR(MODULE, "cannot secure/listen: %s", err); close(fd); return -1;
    }
    return fd;
}
/* The smallest configured transport MTU, which is what bounds a datagram on
 * every path this node has. */
static uint32_t transport_mtu(const fipsd_config *c)
{
    uint32_t mtu = UINT32_MAX;
    const fipsd_config_value *transports = fipsd_config_get(c, "transports");
    for (size_t i = 0; i < fipsd_config_count(transports); ++i) {
        const fipsd_config_value *type = fipsd_config_member(transports, fipsd_config_key(transports, i));
        const fipsd_config_value *value = fipsd_config_member(type, "mtu");
        if (value) { uint64_t n = fipsd_config_uint(value); if (n && n < mtu) mtu = (uint32_t)n; }
        else for (size_t j = 0; j < fipsd_config_count(type); ++j) {
            const fipsd_config_value *instance = fipsd_config_member(type, fipsd_config_key(type, j));
            uint64_t n = fipsd_config_uint(fipsd_config_member(instance, "mtu"));
            if (n && n < mtu) mtu = (uint32_t)n;
        }
    }
    return mtu == UINT32_MAX ? 1357 : mtu;
}
static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_native_api *a = subsystem->user;
    a->runtime = rt;
    GoRefPtr config = go_swap_load(a->configs);
    const char *override = fipsd_config_string(fipsd_config_get(config.ptr, "node.native_api.socket_path"));
    fipsd_control_path_env env = {.run_exists = is_dir("/run/fips"), .var_run_exists = is_dir("/var/run/fips"),
        .privileged = geteuid() == 0, .xdg_runtime_dir = getenv("XDG_RUNTIME_DIR")};
#if defined(__APPLE__)
    env.macos = true;
#elif defined(__FreeBSD__)
    env.freebsd = true;
#endif
    int rc = fipsd_native_resolve_path(override, &env, a->path, sizeof a->path);
    go_refptr_release(&config);
    if (rc != 0) { FIPSD_ERROR(MODULE, "cannot resolve the API socket path: %s", strerror(errno)); return -1; }
    const fipsd_node_context *ctx = fipsd_node_get_context(a->node);
    const fips_identity *identity = ctx ? fipsd_node_identity(ctx) : NULL;
    if (!identity) { FIPSD_ERROR(MODULE, "the node has no identity; native API not started"); return -1; }
    if (fips_identity_npub(identity, a->npub, sizeof a->npub) != FIPS_IDENTITY_OK) return -1;
    a->secp = fipsd_node_secp(ctx);
    a->metrics = (fipsd_metrics *)fipsd_node_handlers_metrics(a->handlers);
    atomic_store(&a->stopping, false);
    /* Fresh per start: the previous run's fibers have joined. */
    if (a->halt) go_channel_free(a->halt);
    a->halt = go_channel_create(1);
    if (!a->halt) return -1;
    fipsd_node_handlers_set_native_sink(a->handlers, a->sink);
    a->listener = bind_listener(a);
    if (a->listener < 0) { FIPSD_ERROR(MODULE, "cannot bind %s: %s", a->path, strerror(errno)); fipsd_node_handlers_set_native_sink(a->handlers, NULL); return -1; }
    a->cancel_socket = socket_new();
    if (a->cancel_socket < 0 || fipsd_runtime_spawn(rt, rx_fiber, a) != 0 || fipsd_runtime_spawn(rt, accept_fiber, a) != 0) {
        /* A spawned rx fiber ends with the runtime's context; the accept
         * fiber owns the listener once it runs, so only an unspawned one is
         * closed here. */
        fipsd_node_handlers_set_native_sink(a->handlers, NULL);
        close(a->listener); a->listener = -1;
        if (a->cancel_socket >= 0) close(a->cancel_socket);
        a->cancel_socket = -1;
        return -1;
    }
    FIPSD_INFO(MODULE, "native API socket listening at %s", a->path);
    return 0;
}
static void stop(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    (void)rt;
    fipsd_native_api *a = subsystem->user;
    atomic_store(&a->stopping, true);
    if (a->halt) go_channel_close(a->halt);
    nsync_mu_lock(&a->listener_mu);
    if (a->listener >= 0) {
        int rc;
        do { rc = dup2(a->cancel_socket, a->listener); } while (rc < 0 && errno == EINTR);
        if (rc < 0) FIPSD_ERROR(MODULE, "cannot cancel listener: %s", strerror(errno));
        else {
            (void)fcntl(a->listener, F_SETFD, FD_CLOEXEC);
            if (a->accept_waiter) gof_hook_make_runnable(a->accept_waiter);
        }
    }
    nsync_mu_unlock(&a->listener_mu);
    /* Owner-directed cancellation of every parked wait: connections, flows
     * and listeners; each fiber then releases what it owns. */
    nsync_mu_lock(&a->mu);
    for (connection *c = a->conns; c; c = c->next) (void)gof_io_cancel_fd(c->fd);
    for (size_t i = 0; i < a->flow_cap; ++i) if (a->flows[i].used) (void)gof_io_cancel_fd(a->flows[i].sock);
    for (size_t i = 0; i < a->listener_cap; ++i) if (a->listeners[i].used) (void)gof_io_cancel_fd(a->listeners[i].sock);
    nsync_mu_unlock(&a->mu);
}

/* ---- provider ------------------------------------------------------------- */
typedef struct combined {
    fipsd_control_snapshot snapshot;
    GoRefPtr base;
    fipsd_control_native_flow *flows;
    fipsd_control_native_listener *listeners;
    char *strings;
    fipsd_control_counter native[32]; /* fipsd_metrics_snapshot's documented minimum */
} combined;
static void combined_free(void *arg)
{
    combined *c = arg;
    go_refptr_release(&c->base);
    free(c->flows); free(c->listeners); free(c->strings);
    free(c);
}
static size_t queued_of(void *sink, void *user) { (void)user; return go_channel_get_depth(sink); }
static GoRefPtr acquire(void *arg)
{
    fipsd_native_api *a = arg;
    combined *c = calloc(1, sizeof *c);
    if (!c) return (GoRefPtr){0};
    if (a->base.acquire) c->base = a->base.acquire(a->base.user);
    if (c->base.ptr) c->snapshot = *(const fipsd_control_snapshot *)c->base.ptr;
    uint64_t now_mono = mono_ms(), now_wall = c->snapshot.now_ms ? c->snapshot.now_ms : wall_ms();
    nsync_mu_lock(&a->mu);
    size_t n = fipsd_native_registry_flows(a->registry, NULL, 0, NULL, NULL);
    size_t m = fipsd_native_registry_listeners(a->registry, NULL, 0);
    fipsd_native_flow_view *views = calloc(n + 1, sizeof *views);
    fipsd_native_listener_view *lviews = calloc(m + 1, sizeof *lviews);
    if (views) n = fipsd_native_registry_flows(a->registry, views, n, queued_of, NULL);
    if (lviews) m = fipsd_native_registry_listeners(a->registry, lviews, m);
    nsync_mu_unlock(&a->mu);
    c->flows = calloc(n + 1, sizeof *c->flows);
    c->listeners = calloc(m + 1, sizeof *c->listeners);
    c->strings = calloc(n + 1, FIPS_NIP19_CAPACITY + 33);
    if (!views || !lviews || !c->flows || !c->listeners || !c->strings) { free(views); free(lviews); combined_free(c); return (GoRefPtr){0}; }
    for (size_t i = 0; i < n; ++i) {
        char *npub = c->strings + i * (FIPS_NIP19_CAPACITY + 33), *hex = npub + FIPS_NIP19_CAPACITY;
        (void)fips_encode_npub(&views[i].pubkey, npub, FIPS_NIP19_CAPACITY);
        (void)fips_node_addr_format(&views[i].key.peer, hex, 33);
        uint64_t age = now_mono > views[i].at ? now_mono - views[i].at : 0;
        c->flows[i] = (fipsd_control_native_flow){.flow_id = views[i].flow, .queued = views[i].queued,
            .since_ms = now_wall > age ? now_wall - age : 0, .peer = npub, .peer_addr = hex,
            .local_port = views[i].key.local, .remote_port = views[i].key.remote, .established = views[i].established};
    }
    for (size_t i = 0; i < m; ++i) c->listeners[i] = (fipsd_control_native_listener){.local_port = lviews[i].port, .backlog = lviews[i].backlog};
    free(views); free(lviews);
    c->snapshot.flows = c->flows; c->snapshot.flow_count = n;
    c->snapshot.listeners = c->listeners; c->snapshot.listener_count = m;
    /* This API bumps the native counters outside the dispatcher, so the base
     * snapshot's copy lags until its next publication; read them live beside
     * the live flows and listeners so show_native_flows agrees with itself.
     * (a->handlers is fixed at create; a->metrics is only set by start.) */
    c->snapshot.native = (fipsd_control_counters){c->native,
        fipsd_metrics_snapshot(fipsd_node_handlers_metrics(a->handlers), FIPSD_METRICS_native, c->native)};
    return make_go_refptr(c, combined_free); /* snapshot is the first member */
}
static json_t *command(void *arg, const char *name, const json_t *params, const fipsd_control_request_context *ctx)
{
    fipsd_native_api *a = arg;
    return a->base.command ? a->base.command(a->base.user, name, params, ctx) : NULL;
}
fipsd_control_provider fipsd_native_api_provider(fipsd_native_api *a, fipsd_control_provider base)
{
    a->base = base;
    return (fipsd_control_provider){.user = a, .acquire = acquire, .command = command};
}

fipsd_native_api *fipsd_native_api_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers)
{
    if (!configs || !node || !handlers) return NULL;
    fipsd_native_api *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->subsystem = (fipsd_subsystem){"native", start, stop, a};
    a->configs = configs; a->node = node; a->handlers = handlers;
    a->listener = a->cancel_socket = -1;
    atomic_init(&a->connections, 0); atomic_init(&a->stopping, false);
    GoRefPtr config = go_swap_load(configs);
    const fipsd_config *c = config.ptr;
    a->limits.per_flow = (size_t)fipsd_config_uint(fipsd_config_get(c, "node.native_api.pending_per_flow"));
    a->limits.backlog = (size_t)fipsd_config_uint(fipsd_config_get(c, "node.native_api.backlog"));
    a->limits.max_flows = (size_t)fipsd_config_uint(fipsd_config_get(c, "node.native_api.max_flows"));
    a->debug = fipsd_config_bool(fipsd_config_get(c, "node.native_api.debug_commands"));
    /* max_payload is the transport MTU less the FIPS encapsulation and the
     * port header: the smallest MTU of the transports the node registered,
     * or of the configured ones when none is registered yet. */
    uint32_t mtu = UINT32_MAX;
    const fipsd_transport_registry *registry = fipsd_node_transports(node);
    for (size_t i = 0; registry && i < registry->count; ++i) {
        const fipsd_transport *t = registry->items[i];
        uint16_t m = t && t->ops && t->ops->mtu ? t->ops->mtu(t, NULL) : 0;
        if (m && m < mtu) mtu = m;
    }
    if (mtu == UINT32_MAX) mtu = transport_mtu(c);
    a->max_payload = fips_framing_payload_limit(mtu > UINT16_MAX ? UINT16_MAX : (uint16_t)mtu);
    go_refptr_release(&config);
    if (!a->limits.per_flow) a->limits.per_flow = 1;
    if (!a->limits.backlog) a->limits.backlog = 1;
    if (!a->limits.max_flows) a->limits.max_flows = 1;
    a->flow_cap = a->limits.max_flows * 2 + 16;
    a->listener_cap = a->limits.max_flows + 16;
    a->flows = calloc(a->flow_cap, sizeof *a->flows);
    a->listeners = calloc(a->listener_cap, sizeof *a->listeners);
    a->registry = fipsd_native_registry_create(&a->limits, NULL, NULL);
    a->sink = go_channel_create(FIPSD_NATIVE_SINK_CAPACITY);
    if (!a->flows || !a->listeners || !a->registry || !a->sink) { fipsd_native_api_destroy(a); return NULL; }
    for (size_t i = 0; i < a->flow_cap; ++i) { a->flows[i].sock = -1; atomic_init(&a->flows[i].pin, -1); }
    for (size_t i = 0; i < a->listener_cap; ++i) a->listeners[i].sock = -1;
    return a;
}
fipsd_subsystem *fipsd_native_api_subsystem(fipsd_native_api *a) { return &a->subsystem; }
const char *fipsd_native_api_socket_path(const fipsd_native_api *a) { return a->path; }
const fipsd_native_limits *fipsd_native_api_limits(const fipsd_native_api *a) { return &a->limits; }
uint16_t fipsd_native_api_max_payload(const fipsd_native_api *a) { return a->max_payload; }
void fipsd_native_api_observe(fipsd_native_api *a, GoChannel *events) { a->events = events; }
void fipsd_native_api_before_flow_reap(fipsd_native_api *a, void (*hook)(void *, uint64_t), void *user)
{ a->before_flow_reap = hook; a->flow_reap_user = user; }
void fipsd_native_api_destroy(fipsd_native_api *a)
{
    if (!a) return;
    if (a->handlers) fipsd_node_handlers_set_native_sink(a->handlers, NULL);
    if (a->sink) {
        void *v;
        while (go_channel_try_receive(a->sink, &v) == 0) fipsd_node_notice_free(v);
        go_channel_free(a->sink);
    }
    if (a->halt) go_channel_free(a->halt);
    /* After the runtime joined, every fiber has dropped its references. */
    for (size_t i = 0; a->flows && i < a->flow_cap; ++i)
        if (a->flows[i].used) { a->flows[i].refs = 1; flow_unref_locked(a, &a->flows[i]); }
    for (size_t i = 0; a->listeners && i < a->listener_cap; ++i)
        if (a->listeners[i].used) { a->listeners[i].refs = 1; listener_unref_locked(a, &a->listeners[i]); }
    fipsd_native_registry_destroy(a->registry);
    free(a->flows); free(a->listeners);
    if (a->bound) {
        struct stat st;
        if (lstat(a->path, &st) == 0 && S_ISSOCK(st.st_mode) && (uint64_t)st.st_dev == a->socket_device &&
            (uint64_t)st.st_ino == a->socket_inode) (void)unlink(a->path);
    }
    free(a);
}
