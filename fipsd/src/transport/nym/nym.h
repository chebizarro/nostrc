#ifndef FIPSD_NYM_H
#define FIPSD_NYM_H
#include "transport/transport.h"
#include "config/config.h"
#include "log.h"
/* Upstream transport/nym: the "mixnet as proxy" pattern. Outbound-only
 * streams through a local nym-socks5-client's SOCKS5 listener (no
 * authentication, no inbound service, no inbound deadlines); the start waits
 * for that client to answer on its port with exponential backoff, bounded by
 * startup_timeout_secs, and proceeds with a warning when it does not. Built
 * only with -DFIPSD_ENABLE_NYM=ON; a build without it refuses a configured
 * transports.nym rather than silently ignoring it. */
struct fipsd_watcher_source;
typedef struct fipsd_nym_config {
    const char *socks5_addr;      /* nym-socks5-client host:port */
    uint32_t connect_timeout_ms;  /* mixnet circuits take minutes: 300 s upstream */
    uint16_t mtu;
    uint64_t startup_timeout_secs;
    /* Host bounds, not YAML keys. */
    size_t max_outbound_connections, send_queue_depth;
    const struct fipsd_watcher_source *watcher_source;
} fipsd_nym_config;
int fipsd_nym_register_config(fipsd_transport_registry *, const fipsd_config *,
    fipsd_transport **owned, size_t *count);
void fipsd_nym_destroy(fipsd_transport *); /* after runtime join; a no-op without the build flag */
#ifdef FIPSD_ENABLE_NYM
void fipsd_nym_config_default(fipsd_nym_config *);
fipsd_transport *fipsd_nym_create(fips_transport_id, const char *name, const fipsd_nym_config *);
bool fipsd_nym_is(const fipsd_transport *);
GoChannel *fipsd_nym_bound(fipsd_transport *);  /* borrowed: the stream owner's readiness */
GoChannel *fipsd_nym_ready(fipsd_transport *);  /* borrowed: closed once the readiness probe ended */
bool fipsd_nym_proxy_ready(const fipsd_transport *); /* the probe reached the client */
size_t fipsd_nym_connected_count(fipsd_transport *);
fipsd_transport *fipsd_nym_stream(fipsd_transport *);
#endif
#endif
