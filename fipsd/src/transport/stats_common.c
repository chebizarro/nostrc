#include "transport/stats_common.h"
#include <string.h>
void fipsd_transport_stats_init(fipsd_transport_stats *s)
{
#define INIT(name) atomic_init(&s->name, 0);
    FIPSD_STATS_ALL(INIT)
#undef INIT
}
void fipsd_transport_diagnostics_init(fipsd_transport_diagnostics *s)
{
#define INIT(name) atomic_init(&s->name, 0);
    FIPSD_TRANSPORT_DIAGNOSTICS(INIT)
#undef INIT
}
void fipsd_transport_stats_sent(fipsd_transport_stats *s, size_t n)
{ atomic_fetch_add_explicit(&s->packets_sent, 1, memory_order_relaxed); atomic_fetch_add_explicit(&s->bytes_sent, n, memory_order_relaxed); }
void fipsd_transport_stats_recv(fipsd_transport_stats *s, size_t n)
{ atomic_fetch_add_explicit(&s->packets_recv, 1, memory_order_relaxed); atomic_fetch_add_explicit(&s->bytes_recv, n, memory_order_relaxed); }
fipsd_transport_stats_view fipsd_transport_stats_read(const fipsd_transport_stats *s)
{
#define LOAD(name) .name = atomic_load_explicit(&s->name, memory_order_relaxed),
    return (fipsd_transport_stats_view){FIPSD_STATS_ALL(LOAD)};
#undef LOAD
}
fips_link_stats fipsd_transport_stats_snapshot(const fipsd_transport_stats *s)
{
    fipsd_transport_stats_view v = fipsd_transport_stats_read(s);
    return (fips_link_stats){.packets_sent = v.packets_sent, .packets_recv = v.packets_recv,
        .bytes_sent = v.bytes_sent, .bytes_recv = v.bytes_recv};
}
void fipsd_transport_stats_error(fipsd_transport_stats *s, bool send)
{ atomic_fetch_add_explicit(send ? &s->send_errors : &s->recv_errors, 1, memory_order_relaxed); }
void fipsd_transport_stats_pool(fipsd_transport_stats *s, fips_link_direction d, bool added)
{
    _Atomic uint64_t *n = d == FIPS_LINK_INBOUND ? &s->pool_inbound : &s->pool_outbound;
    if (added) atomic_fetch_add_explicit(n, 1, memory_order_relaxed);
    else atomic_fetch_sub_explicit(n, 1, memory_order_relaxed);
}
void fipsd_transport_stats_kernel_drops(fipsd_transport_stats *s, bool connected, uint32_t drops)
{ if (!connected) atomic_store_explicit(&s->kernel_drops, drops, memory_order_relaxed); }

const char *const *fipsd_transport_stats_fields(const char *type, size_t *count)
{
#define NAME(name) #name,
    static const char *const udp[] = {FIPSD_STATS_PACKET(NAME) FIPSD_STATS_UDP_ONLY(NAME)};
    static const char *const tcp[] = {FIPSD_STATS_PACKET(NAME) FIPSD_STATS_TCP_ONLY(NAME)};
    /* tor/stats.rs TorStatsSnapshot and nym/stats.rs NymStatsSnapshot, in
     * their serialized order. */
    static const char *const tor[] = {FIPSD_STATS_PACKET(NAME) "connections_established",
        "connect_timeouts", "connect_refused", "socks5_errors", "connections_accepted",
        "connections_rejected", "control_errors", "pool_inbound", "pool_outbound"};
    static const char *const nym[] = {FIPSD_STATS_PACKET(NAME) "connections_established",
        "connect_timeouts", "socks5_errors"};
    /* ble/stats.rs BleStatsSnapshot, in its serialized order. */
    static const char *const ble[] = {FIPSD_STATS_PACKET(NAME) "connections_established",
        "connections_accepted", "connections_rejected", "handshakes_aborted", "connect_timeouts",
        "connect_errors", "pubkey_exchange_failures", "pool_evictions", "advertisements_sent",
        "scan_results", "duplicate_node_declines", "duplicate_link_replacements"};
#undef NAME
    if (type && !strcmp(type, "udp")) { *count = sizeof udp / sizeof *udp; return udp; }
    if (type && !strcmp(type, "tcp")) { *count = sizeof tcp / sizeof *tcp; return tcp; }
    if (type && !strcmp(type, "tor")) { *count = sizeof tor / sizeof *tor; return tor; }
    if (type && !strcmp(type, "nym")) { *count = sizeof nym / sizeof *nym; return nym; }
    if (type && !strcmp(type, "ble")) { *count = sizeof ble / sizeof *ble; return ble; }
    *count = 0; return NULL;
}
