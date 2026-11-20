#ifndef FIPSD_UDP_H
#define FIPSD_UDP_H
#include "transport/transport.h"
#include "transport/udp/classify.h"
#include "transport/udp/io.h"
#include "config/config.h"
#define FIPSD_UDP_MAX_PEERS 64u
struct fipsd_watcher_source;
typedef struct fipsd_udp_config {
    const char *bind_addr, *interface_name;
    size_t recv_buf_size, send_buf_size, peer_capacity;
    uint16_t mtu;
    bool accept_connections, optional;
    const struct fipsd_watcher_source *watcher_source; /* optional injected facts/clock; state outlives join */
} fipsd_udp_config;
void fipsd_udp_config_default(fipsd_udp_config *);
fipsd_transport *fipsd_udp_create(fips_transport_id, const char *, const fipsd_udp_config *);
void fipsd_udp_destroy(fipsd_transport *); /* after runtime join */
bool fipsd_udp_address(fipsd_transport *, fips_transport_addr *);
GoChannel *fipsd_udp_bound(fipsd_transport *); /* borrowed, one notification per bind */
/* A further observer of binds, for owners other than the one reading
 * fipsd_udp_bound (the mDNS responder advertises the bound port): each bind
 * try-sends the transport to ch, so give it capacity. Before the runtime
 * starts; ch must outlive the transport. -1 past FIPSD_UDP_BOUND_WATCHERS. */
#define FIPSD_UDP_BOUND_WATCHERS 4u
int fipsd_udp_watch_bound(fipsd_transport *, GoChannel *ch);
bool fipsd_udp_register_classifier(fipsd_transport *, const char *, fipsd_udp_classifier, void *);
size_t fipsd_udp_connected_count(fipsd_transport *);
/* Append configured singleton/named UDP instances to the node registry. The
 * caller owns successful instances, also on partial failure. */
int fipsd_udp_register_config(fipsd_transport_registry *, const fipsd_config *,
    fipsd_transport **owned, size_t *count);
bool fipsd_udp_register_datagram_classifier(fipsd_transport *, const char *, fipsd_udp_datagram_classifier, void *);
/* Resolve with the transport family/cache on the blocking executor. */
bool fipsd_udp_resolve_address(fipsd_transport *, const fips_transport_addr *, fips_transport_addr *);
bool fipsd_udp_unregister_datagram_classifier(fipsd_transport *, const char *, void *);
#endif
