#ifndef FIPSD_TRANSPORT_LOOPBACK_H
#define FIPSD_TRANSPORT_LOOPBACK_H
#include "node/transport.h"

/* In-process datagram medium behind the fipsd_transport seam (nostrc-9keoz.6).
 * Endpoints on one network deliver to each other by opaque text address with
 * owned PACKET events on the destination node's channel, bounded by that
 * channel and the destination's root context (never a raw blocking send).
 * This is test/interop infrastructure: main.c registers no loopback transport
 * and it fabricates no network readiness. An optional filter drops or
 * observes frames so tests can exercise loss and resends deterministically. */
#define FIPSD_LOOPBACK_MAX_ENDPOINTS 8u
typedef struct fipsd_loopback_network fipsd_loopback_network;
/* Return false to drop the frame. Runs on the sender's fiber under no lock. */
typedef bool (*fipsd_loopback_filter)(void *user, const fips_transport_addr *from,
                                      const fips_transport_addr *to, const uint8_t *bytes, size_t len);
fipsd_loopback_network *fipsd_loopback_network_create(void);
/* After every endpoint is destroyed. */
void fipsd_loopback_network_destroy(fipsd_loopback_network *network);
void fipsd_loopback_network_set_filter(fipsd_loopback_network *network, fipsd_loopback_filter filter, void *user);
/* address is the endpoint's own opaque text address (e.g. "loop:a"). NULL on
 * duplicate address, full network or allocation failure. */
fipsd_transport *fipsd_loopback_transport_create(fipsd_loopback_network *network, fips_transport_id id,
                                                 const char *name, const char *address, uint16_t mtu);
/* After the runtime joined the node (the endpoint is stopped). */
void fipsd_loopback_transport_destroy(fipsd_transport *transport);
const fips_transport_addr *fipsd_loopback_transport_address(const fipsd_transport *transport);
uint64_t fipsd_loopback_transport_sent(const fipsd_transport *transport);
uint64_t fipsd_loopback_transport_dropped(const fipsd_transport *transport);
#endif
