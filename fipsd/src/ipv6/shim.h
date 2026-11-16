#ifndef FIPSD_IPV6_SHIM_H
#define FIPSD_IPV6_SHIM_H
/* The IPv6 shim (FSP port 256): header compression for mesh-internal traffic
 * (upstream ipv6_shim.rs) and the adapter that sits between the TUN device's
 * frames and the node's session datagrams (upstream tun.rs reader/writer and
 * lifecycle.rs). Source and destination addresses, version and payload length
 * are stripped on the wire and rebuilt from session context; traffic class,
 * flow label, next header and hop limit travel as residual fields:
 *
 *   [format:1][ver_tc_flow:4][next_header:1][hop_limit:1][upper_layer_payload...]
 *
 * The adapter owns three fibers: the reader (TUN frames -> from_tun -> node
 * datagrams, ICMPv6 answers, hairpins), the writer (the bounded host queue ->
 * TUN frames, clamping inbound SYNs) and the mesh receiver (the node's
 * notices: port-256 datagrams, path MTU changes, routable peers; everything
 * else is passed through to an optional consumer). It never blocks the node's
 * dispatcher: datagrams go in through the handlers' nonblocking command
 * mailbox and notices are read off their bounded channel. */
#include "ipv6/link.h"
#include "ipv6/listening.h"
#include "tun/tun.h"
#include "node/handlers/handlers.h"
#include "runtime.h"

#define FIPSD_IPV6_SHIM_FORMAT_COMPRESSED 0x00u
/* The compressed payload's length (len - 33), or -1 for a packet shorter than
 * its header or of another version, or when out is too small. */
int fipsd_ipv6_compress(const uint8_t *packet, size_t len, uint8_t *out, size_t cap);
/* The rebuilt packet's length (len + 33), or -1 for an unknown format byte, a
 * payload shorter than the residual, or when out is too small. */
int fipsd_ipv6_decompress(const uint8_t *payload, size_t len, const uint8_t src[16], const uint8_t dst[16],
                          uint8_t *out, size_t cap);

typedef struct fipsd_ipv6_shim fipsd_ipv6_shim;
typedef struct fipsd_ipv6_shim_config {
    fipsd_tun *tun;                   /* started by the TUN service before the shim */
    fipsd_node_handlers *handlers;    /* datagrams out; notices in */
    GoChannel *notices;               /* NULL: fipsd_node_handlers_notices(handlers) */
    GoChannel *passthrough;           /* notices the shim does not consume (owned by that consumer); NULL frees them */
    uint16_t payload_limit;           /* node-wide seed; 0: the limit of the default 1357-byte transport */
    size_t host_queue;                /* node.buffers.host_channel; 0: 4096 */
    size_t path_capacity, key_capacity;
    fips_clock clock;                 /* now == NULL: host monotonic clock */
    bool firewall_scope_check;        /* Linux: warn at start when inet fips polices another interface */
} fipsd_ipv6_shim_config;
typedef struct fipsd_ipv6_shim_counters {
    uint64_t read, hairpinned, off_mesh, unreachable, too_big, forwarded, fragments, send_refused;
    uint64_t delivered, undeliverable, written, write_errors, clamped_out, clamped_in, passed_through, path_updates, host_drops;
} fipsd_ipv6_shim_counters;
fipsd_ipv6_shim *fipsd_ipv6_shim_create(const fipsd_ipv6_shim_config *config);
/* On the starting thread after the device is up: spawns the fibers. */
int fipsd_ipv6_shim_start(fipsd_ipv6_shim *shim, fipsd_runtime *runtime);
/* On the stopping thread BEFORE the device stops: cancels the descriptor
 * waits, joins the three fibers and resets the descriptor. Idempotent. */
int fipsd_ipv6_shim_stop(fipsd_ipv6_shim *shim);
void fipsd_ipv6_shim_destroy(fipsd_ipv6_shim *shim);
fipsd_ipv6_mesh_state *fipsd_ipv6_shim_state(fipsd_ipv6_shim *shim);
fipsd_ipv6_host_link fipsd_ipv6_shim_host_link(fipsd_ipv6_shim *shim);
/* A name resolution handed the host peer's address (the DNS responder's seam). */
fipsd_ipv6_learned fipsd_ipv6_shim_learn(fipsd_ipv6_shim *shim, const fips_node_addr *peer);
fipsd_ipv6_shim_counters fipsd_ipv6_shim_counters_read(const fipsd_ipv6_shim *shim);
const _Atomic uint64_t *fipsd_ipv6_shim_drop_counter(const fipsd_ipv6_shim *shim);
#endif
