#include "nostr/driver.h"
#include "log.h"
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
            FIPSD_DEBUG("nostr", "NAT dial candidate awaits traversal driver (8hvs6.3)");
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
    GoRefPtr held = fipsd_node_config(d->node);
    const fipsd_config *config = held.ptr;
    const fipsd_config_value *udp = fipsd_config_get(config, "transports.udp");
    /* Concrete public/STUN endpoints are supplied by the transport/traversal
     * owner. Never advertise a wildcard bind address as a reachable endpoint. */
    for (size_t i = 0; i < d->transports->count; ++i) {
        fipsd_transport *t = d->transports->items[i];
        const fipsd_config_value *v = fipsd_config_member(udp, t->name);
        if (!v) v = udp;
        if (!strcmp(t->type, "udp") && t->ops->state(t) == FIPSD_TRANSPORT_BOUND &&
            fipsd_config_bool(fipsd_config_member(v, "advertise_on_nostr")) &&
            out->endpoint_count < FIPSD_NOSTR_ENDPOINTS_MAX) {
            out->endpoints[out->endpoint_count++] = (fipsd_nostr_endpoint){FIPSD_NOSTR_UDP, "nat"};
        }
    }
    go_refptr_release(&held);
    return out->endpoint_count != 0;
}
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *d,
    const fipsd_node_context *node, fipsd_node_handlers *handlers, const fipsd_transport_registry *transports)
{
    *d = (fipsd_nostr_node_driver){node, handlers, transports};
    return (fipsd_rendezvous_driver){.user = d, .dial_candidate = dial, .local_advert = local};
}
