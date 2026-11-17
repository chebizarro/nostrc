#ifndef FIPSD_TRANSPORT_STATS_H
#define FIPSD_TRANSPORT_STATS_H
#include <stdatomic.h>
#include <fips/transport/types.h>
/* Upstream transport/{udp,tcp}/stats.rs snapshots and stats_common.rs pool.
 * These lists own both storage and the control names. All counters are u64;
 * pool_* are current occupancy, not cumulative connection counts. */
#define FIPSD_STATS_PACKET(X) \
    X(packets_sent) X(bytes_sent) X(packets_recv) X(bytes_recv) \
    X(send_errors) X(recv_errors) X(mtu_exceeded)
#define FIPSD_STATS_UDP_ONLY(X) X(kernel_drops)
#define FIPSD_STATS_TCP_ONLY(X) \
    X(connections_established) X(connections_accepted) X(connections_rejected) \
    X(source_rejected) X(connect_timeouts) X(connect_refused) X(pool_inbound) X(pool_outbound)
/* Upstream socks5/stats.rs and tor/stats.rs: counted by the proxied (Tor,
 * Nym) owners only; appended last so earlier counter positions are stable. */
#define FIPSD_STATS_PROXY_ONLY(X) X(socks5_errors) X(control_errors)
/* Upstream ble/stats.rs counters the stream owners above lack; appended last
 * as well. connections_* and connect_timeouts are shared with TCP. */
#define FIPSD_STATS_BLE_ONLY(X) \
    X(handshakes_aborted) X(connect_errors) X(pubkey_exchange_failures) X(pool_evictions) \
    X(advertisements_sent) X(scan_results) X(duplicate_node_declines) X(duplicate_link_replacements)
#define FIPSD_STATS_ALL(X) FIPSD_STATS_PACKET(X) FIPSD_STATS_UDP_ONLY(X) FIPSD_STATS_TCP_ONLY(X) \
    FIPSD_STATS_PROXY_ONLY(X) FIPSD_STATS_BLE_ONLY(X)
#define FIPSD_STATS_DECLARE(name) _Atomic uint64_t name;
typedef struct fipsd_transport_stats { FIPSD_STATS_ALL(FIPSD_STATS_DECLARE) } fipsd_transport_stats;
#undef FIPSD_STATS_DECLARE
#define FIPSD_STATS_DECLARE(name) uint64_t name;
typedef struct fipsd_transport_stats_view { FIPSD_STATS_ALL(FIPSD_STATS_DECLARE) } fipsd_transport_stats_view;
#undef FIPSD_STATS_DECLARE
#define FIPSD_STATS_COUNT(name) + 1
enum { FIPSD_TRANSPORT_STATS_COUNT = 0 FIPSD_STATS_ALL(FIPSD_STATS_COUNT) };
#undef FIPSD_STATS_COUNT
/* Host-only diagnostics, deliberately NOT part of upstream show_transports. */
#define FIPSD_TRANSPORT_DIAGNOSTICS(X) \
    X(dropped) X(ecn_recv) X(ce_recv) X(pktinfo_recv) X(classifier_drops) X(malformed) X(pool_full) X(deadlines) X(socket_errors)
#define FIPSD_STATS_DECLARE(name) _Atomic uint64_t name;
typedef struct fipsd_transport_diagnostics { FIPSD_TRANSPORT_DIAGNOSTICS(FIPSD_STATS_DECLARE) } fipsd_transport_diagnostics;
#undef FIPSD_STATS_DECLARE
/* Exact output field set per type (udp, tcp, tor, nym, ble); loopback has Empty
 * stats upstream. Order follows each Rust snapshot struct. */
const char *const *fipsd_transport_stats_fields(const char *type, size_t *count);
void fipsd_transport_stats_init(fipsd_transport_stats *);
void fipsd_transport_diagnostics_init(fipsd_transport_diagnostics *);
void fipsd_transport_stats_sent(fipsd_transport_stats *, size_t);
void fipsd_transport_stats_recv(fipsd_transport_stats *, size_t);
fips_link_stats fipsd_transport_stats_snapshot(const fipsd_transport_stats *);
fipsd_transport_stats_view fipsd_transport_stats_read(const fipsd_transport_stats *);
void fipsd_transport_stats_error(fipsd_transport_stats *, bool send);
/* Called exactly once per successfully added/removed pool entry. */
void fipsd_transport_stats_pool(fipsd_transport_stats *, fips_link_direction, bool added);
/* SO_RXQ_OVFL is cumulative on the wildcard socket ONLY (Linux). */
void fipsd_transport_stats_kernel_drops(fipsd_transport_stats *, bool connected, uint32_t);
#endif
