#ifndef FIPSD_TCP_H
#define FIPSD_TCP_H
#include "transport/transport.h"
#include "config/config.h"
#define FIPSD_TCP_SEND_DEPTH 64u
struct fipsd_watcher_source;
typedef struct fipsd_tcp_config {
    const char *bind_addr; /* NULL: outbound only */
    uint16_t mtu;
    uint32_t connect_timeout_ms, keepalive_secs;
    size_t recv_buf_size, send_buf_size, max_inbound_connections, max_inbound_per_source;
    bool nodelay;
    /* Host bounds, not YAML keys. Pending connects count toward outbound. */
    size_t max_outbound_connections, send_queue_depth;
    uint64_t first_frame_ms, idle_ms;
    const struct fipsd_watcher_source *watcher_source;
} fipsd_tcp_config;
typedef struct fipsd_tcp_stats_view {
    uint64_t send_errors, recv_errors, mtu_exceeded, connections_established,
        connections_accepted, connections_rejected, source_rejected,
        connect_timeouts, connect_refused, pool_inbound, pool_outbound;
} fipsd_tcp_stats_view;
fipsd_tcp_stats_view fipsd_tcp_stats_snapshot(const fipsd_transport *);
void fipsd_tcp_config_default(fipsd_tcp_config *);
fipsd_transport *fipsd_tcp_create(fips_transport_id, const char *, const fipsd_tcp_config *);
void fipsd_tcp_destroy(fipsd_transport *); /* after runtime join */
GoChannel *fipsd_tcp_bound(fipsd_transport *); /* borrowed readiness notification */
bool fipsd_tcp_address(fipsd_transport *, fips_transport_addr *);
size_t fipsd_tcp_connected_count(fipsd_transport *);
int fipsd_tcp_register_config(fipsd_transport_registry *, const fipsd_config *,
    fipsd_transport **owned, size_t *count);
#endif
