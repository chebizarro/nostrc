#ifndef FIPSD_TCP_H
#define FIPSD_TCP_H
#include "transport/transport.h"
#include "config/config.h"
#define FIPSD_TCP_SEND_DEPTH 64u
struct fipsd_watcher_source;
/* Host-only stream dialer for the proxied owners (Tor, Nym): the connect
 * fiber resolves and connects `via` (the SOCKS5 proxy) instead of the peer,
 * then `upgrade` performs the proxy handshake on the connected socket under
 * the same connect deadline (absolute, fipsd_transport_now_ms). On failure it
 * returns -1 with *error: ETIMEDOUT (deadline), ECONNREFUSED (the proxy says
 * the peer refused, SOCKS5 REP 0x05) or another errno for every other proxy or
 * protocol failure; the owner counts those as socks5_errors, and a failed
 * connect to the proxy itself the same way, never as connect_refused. */
typedef struct fipsd_stream_dialer {
    const fips_transport_addr *via;
    int (*upgrade)(void *user, int fd, const fips_transport_addr *peer, uint64_t deadline_ms, int *error);
    void *user;
} fipsd_stream_dialer;
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
    const fipsd_stream_dialer *dialer; /* NULL: native TCP connect to the peer */
} fipsd_tcp_config;
typedef fipsd_transport_stats_view fipsd_tcp_stats_view;
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
