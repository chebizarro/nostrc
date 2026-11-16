#include "transport/loopback.h"
#include "transport/transport.h"
#include "node/context.h"
#include <nsync.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct endpoint {
    fipsd_transport transport;
    fipsd_loopback_network *network;
    fips_transport_addr address;
    char name[64];
    uint16_t mtu;
    /* Dispatcher-owned channel/context, borrowed while BOUND; protected by the
     * network mutex because another node's fiber reads them to deliver. */
    GoChannel *events;
    GoContext *root;
    fipsd_transport_shared shared;
} endpoint;
struct fipsd_loopback_network {
    nsync_mu mu;
    endpoint *items[FIPSD_LOOPBACK_MAX_ENDPOINTS];
    size_t count;
    fipsd_loopback_filter filter;
    void *filter_user;
};

fipsd_loopback_network *fipsd_loopback_network_create(void)
{
    fipsd_loopback_network *n = calloc(1, sizeof(*n));
    if (n) nsync_mu_init(&n->mu);
    return n;
}
void fipsd_loopback_network_destroy(fipsd_loopback_network *n) { free(n); }
void fipsd_loopback_network_set_filter(fipsd_loopback_network *n, fipsd_loopback_filter f, void *user)
{
    nsync_mu_lock(&n->mu);
    n->filter = f; n->filter_user = user;
    nsync_mu_unlock(&n->mu);
}
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *c, GoChannel *events)
{
    (void)c;
    endpoint *e = t->user;
    nsync_mu_lock(&e->network->mu);
    e->events = events; e->root = fipsd_runtime_context(rt);
    nsync_mu_unlock(&e->network->mu);
    fipsd_presence_facts facts = {.valid = true, .exists = true, .up = true, .medium = FIPS_PATH_Unknown};
    fipsd_presence_bound(&e->shared.presence, facts, fipsd_transport_now_ms());
    atomic_store(&e->shared.state, FIPSD_TRANSPORT_BOUND);
    return 0;
}
static void stop(fipsd_transport *t)
{
    endpoint *e = t->user;
    atomic_store(&e->shared.state, FIPSD_TRANSPORT_STOPPED);
    fipsd_presence_transition(&e->shared.presence, FIPSD_ABSENT, fipsd_transport_now_ms());
    nsync_mu_lock(&e->network->mu);
    e->events = NULL; e->root = NULL;
    nsync_mu_unlock(&e->network->mu);
}
static fipsd_transport_state state(const fipsd_transport *t)
{ const endpoint *e = t->user; return (fipsd_transport_state)atomic_load(&e->shared.state); }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *a)
{ (void)a; const endpoint *e = t->user; return e->mtu; }
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{
    endpoint *e = t->user;
    fipsd_loopback_network *n = e->network;
    if (len > e->mtu || atomic_load(&e->shared.state) != FIPSD_TRANSPORT_BOUND) return -1;
    GoChannel *events = NULL; GoContext *root = NULL; fips_transport_id id = {0};
    endpoint *destination = NULL;
    fipsd_loopback_filter filter; void *user;
    nsync_mu_lock(&n->mu);
    for (size_t i = 0; i < n->count; ++i)
        if (fips_transport_addr_equal(&n->items[i]->address, to)) {
            destination = n->items[i];
            events = n->items[i]->events; root = n->items[i]->root; id = n->items[i]->transport.id;
        }
    filter = n->filter; user = n->filter_user;
    nsync_mu_unlock(&n->mu);
    if (!events || !root) return -1;
    if (filter && !filter(user, &e->address, to, bytes, len)) {
        atomic_fetch_add(&e->shared.stats.dropped, 1);
        return 0; /* A dropped datagram is a successful send, as on a lossy wire. */
    }
    if (fipsd_transport_emit(events, root, NULL, FIPSD_TRANSPORT_PACKET,
                             id, &e->address, bytes, len) != 0) return -1;
    fipsd_transport_stats_sent(&e->shared.stats, len);
    if (destination) fipsd_transport_stats_recv(&destination->shared.stats, len);
    return 0;
}
static int discover(fipsd_transport *t)
{
    endpoint *e = t->user;
    fipsd_loopback_network *n = e->network;
    nsync_mu_lock(&n->mu);
    GoChannel *events = e->events; GoContext *root = e->root;
    fips_transport_addr others[FIPSD_LOOPBACK_MAX_ENDPOINTS]; size_t count = 0;
    for (size_t i = 0; i < n->count; ++i) if (n->items[i] != e) others[count++] = n->items[i]->address;
    nsync_mu_unlock(&n->mu);
    if (!events || !root) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (fipsd_transport_emit(events, root, NULL, FIPSD_TRANSPORT_DISCOVERED,
                                 t->id, &others[i], NULL, 0) != 0) return -1;
    }
    return 0;
}
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e) { (void)t; (void)e; }
static const fipsd_transport_ops ops = {.start_async = start_async, .stop = stop, .state = state,
    .mtu = mtu, .send = send_frame, .discover = discover, .network_changed = network_changed};

fipsd_transport *fipsd_loopback_transport_create(fipsd_loopback_network *n, fips_transport_id id,
                                                 const char *name, const char *address, uint16_t mtu_bytes)
{
    if (!n || !name || !address || strlen(name) >= sizeof(((endpoint *)0)->name)) return NULL;
    endpoint *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (fips_transport_addr_from_string(address, strlen(address), &e->address) != FIPS_OK) { free(e); return NULL; }
    strcpy(e->name, name);
    e->network = n; e->mtu = mtu_bytes ? mtu_bytes : 1280;
    e->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "loopback", .name = e->name,
        .auto_connect = true, .accept_connections = true, .user = e};
    fipsd_transport_shared_init(&e->transport, &e->shared, false);
    nsync_mu_lock(&n->mu);
    bool ok = n->count < FIPSD_LOOPBACK_MAX_ENDPOINTS;
    for (size_t i = 0; ok && i < n->count; ++i)
        if (fips_transport_addr_equal(&n->items[i]->address, &e->address)) ok = false;
    if (ok) n->items[n->count++] = e;
    nsync_mu_unlock(&n->mu);
    if (!ok) { free(e); return NULL; }
    return &e->transport;
}
void fipsd_loopback_transport_destroy(fipsd_transport *t)
{
    if (!t) return;
    endpoint *e = t->user;
    fipsd_loopback_network *n = e->network;
    nsync_mu_lock(&n->mu);
    for (size_t i = 0; i < n->count; ++i) if (n->items[i] == e) {
        n->items[i] = n->items[--n->count]; break;
    }
    nsync_mu_unlock(&n->mu);
    free(e);
}
const fips_transport_addr *fipsd_loopback_transport_address(const fipsd_transport *t)
{ return &((const endpoint *)t->user)->address; }
uint64_t fipsd_loopback_transport_sent(const fipsd_transport *t)
{ return atomic_load(&((const endpoint *)t->user)->shared.stats.packets_sent); }
uint64_t fipsd_loopback_transport_dropped(const fipsd_transport *t)
{ return atomic_load(&((const endpoint *)t->user)->shared.stats.dropped); }
