#include "nostr/driver.h"
#include "log.h"
#include "nostr/traversal_io.h"
#include "transport/udp/udp.h"
#include <sodium.h>
#include <string.h>
static bool dial(void *user, const char *hex, const fipsd_nostr_advert *advert)
{
    fipsd_nostr_node_driver *d = user;
    fips_xonly_pubkey key; fips_peer_identity peer;
    if (sodium_hex2bin(key.bytes, 32, hex, 64, NULL, NULL, NULL) ||
        fips_peer_identity_from_pubkey(fipsd_node_secp(d->node), &key, &peer) != FIPS_IDENTITY_OK) return false;
    if (fipsd_node_handlers_learn_identity(d->handlers, &peer)) return false;
    bool queued = false;
    for (size_t i = 0; i < advert->endpoint_count; ++i) {
        const fipsd_nostr_endpoint *e = &advert->endpoints[i];
        if (!strcmp(e->addr, "nat")) {
            if (e->transport == FIPSD_NOSTR_UDP && fipsd_traversal_io_connect(d->traversal, hex)) queued = true;
            continue;
        }
        const char *type = e->transport == FIPSD_NOSTR_UDP ? "udp" : e->transport == FIPSD_NOSTR_TCP ? "tcp" : "tor";
        for (size_t j = 0; j < d->transports->count; ++j) {
            fipsd_transport *t = d->transports->items[j]; fips_transport_addr addr;
            if (strcmp(t->type, type) || !t->auto_connect) continue;
            if (fips_transport_addr_from_string(e->addr, strlen(e->addr), &addr) == FIPS_OK &&
                fipsd_node_handlers_connect(d->handlers, &peer, t->id, &addr) == 0) queued = true;
        }
    }
    return queued;
}
static bool local(void *user, fipsd_nostr_advert *out)
{
    fipsd_nostr_node_driver *d = user;
    return fipsd_traversal_io_advert(d->traversal, out);
}
static bool prepare(void *user, const fipsd_config *c, struct fipsd_nostr_runtime *r)
{
    fipsd_nostr_node_driver *d = user;
    d->traversal = fipsd_traversal_io_create(c, d, r);
    return d->traversal != NULL;
}
static int start(void *user, fipsd_runtime *rt, const fips_identity *id)
{ return fipsd_traversal_io_start(((fipsd_nostr_node_driver *)user)->traversal, rt, id); }
static void stop(void *user)
{ fipsd_traversal_io_stop(((fipsd_nostr_node_driver *)user)->traversal); }
static void destroy(void *user)
{
    fipsd_nostr_node_driver *d = user; fipsd_traversal_io_destroy(d->traversal); d->traversal = NULL;
}
static bool traversal_signal(void *user, const fipsd_nostr_traversal *p)
{ return fipsd_traversal_io_signal(((fipsd_nostr_node_driver *)user)->traversal, p); }
static void published_signal(void *user, const char *id, bool ok, bool repair)
{ fipsd_traversal_io_published(((fipsd_nostr_node_driver *)user)->traversal, id, ok, repair); }
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *d,
    const fipsd_node_context *node, fipsd_node_handlers *handlers, const fipsd_transport_registry *transports)
{
    *d = (fipsd_nostr_node_driver){.node = node, .handlers = handlers, .transports = transports};
    return (fipsd_rendezvous_driver){.user = d, .dial_candidate = dial, .local_advert = local,
        .prepare = prepare, .start = start, .stop = stop, .destroy = destroy,
        .traversal_offer = traversal_signal, .traversal_answer = traversal_signal, .signal_result = published_signal};
}

bool fipsd_nostr_node_driver_established(fipsd_nostr_node_driver *d, const fipsd_established_traversal *e)
{
    fipsd_transport *t = fipsd_transport_find(d->transports, e->transport);
    fips_transport_addr current;
    if (!t || strcmp(t->type, "udp") || !fipsd_udp_address(t, &current) ||
        !fips_transport_addr_equal(&current, &e->local_addr)) return false;
    return !fipsd_node_handlers_learn_identity(d->handlers, &e->peer) &&
        !fipsd_node_handlers_connect(d->handlers, &e->peer, e->transport, &e->remote_addr);
}
