#ifndef FIPSD_IPV6_OUTBOUND_H
#define FIPSD_IPV6_OUTBOUND_H
/* Host-side half of forwarding an IPv6 packet read from the TUN (upstream
 * outbound.rs). The decisions are synchronous functions that return what to
 * send rather than sending it, and read no clock: from_tun for a packet the
 * system TUN reader read (the fd00::/8 filter, the hairpin and the MSS clamp,
 * then the shared step), outbound for the shared step, and answer for a
 * refusal. The driver carries the result out, and only it consults the ICMPv6
 * limiters. can_split/split/reclamp are the node-side steps on a datagram it
 * holds; in this host the shim's driver takes them with its replica of the
 * node's limits, since the C node holds datagrams opaque. */
#include "ipv6/frag.h"
#include "ipv6/link.h"
typedef enum fipsd_ipv6_host_kind {
    FIPSD_IPV6_HOST_NOTHING,      /* dropped, or the answer it is owed cannot be built */
    FIPSD_IPV6_HOST_MESH,         /* hand the node this datagram */
    FIPSD_IPV6_HOST_UNREACHABLE,  /* send this Destination Unreachable if the bucket has a token */
    FIPSD_IPV6_HOST_TOO_BIG       /* send Packet Too Big reporting mtu if the limiter allows the source */
} fipsd_ipv6_host_kind;
typedef struct fipsd_ipv6_host {
    fipsd_ipv6_host_kind kind;
    fipsd_ipv6_datagram datagram;   /* MESH: payload points into the caller's scratch */
    const uint8_t *response;        /* UNREACHABLE: into the caller's scratch */
    size_t response_len;
    uint32_t mtu;                   /* TOO_BIG */
} fipsd_ipv6_host;
typedef enum fipsd_ipv6_from_tun_kind {
    FIPSD_IPV6_FROM_TUN_NOTHING,   /* not IPv6, or an off-mesh answer cannot be built */
    FIPSD_IPV6_FROM_TUN_HAIRPIN,   /* addressed to us: write it back, checksum completed */
    FIPSD_IPV6_FROM_TUN_OFF_MESH,  /* outside fd00::/8: this Destination Unreachable */
    FIPSD_IPV6_FROM_TUN_HOST       /* a mesh destination: the shared step's verdict, on the packet as clamped */
} fipsd_ipv6_from_tun_kind;
typedef struct fipsd_ipv6_from_tun {
    fipsd_ipv6_from_tun_kind kind;
    const uint8_t *response;
    size_t response_len;
    bool clamped;          /* a SYN was clamped, to clamped_mss */
    uint16_t clamped_mss;
    fipsd_ipv6_host host;
} fipsd_ipv6_from_tun;
typedef enum fipsd_ipv6_admit_kind { FIPSD_IPV6_ADMIT_DROP, FIPSD_IPV6_ADMIT_TOO_BIG, FIPSD_IPV6_ADMIT_FORWARD } fipsd_ipv6_admit_kind;
typedef struct fipsd_ipv6_admit { fipsd_ipv6_admit_kind kind; uint32_t mtu; fips_node_prefix prefix; } fipsd_ipv6_admit;
/* Check a packet against the node-wide IPv6 MTU and extract its destination prefix. */
fipsd_ipv6_admit fipsd_ipv6_admit_packet(const uint8_t *packet, size_t len, uint16_t ipv6_mtu);
/* scratch holds the datagram payload or the answer: at least max(len, 1280) bytes. */
void fipsd_ipv6_from_tun_decide(uint8_t *packet, size_t len, uint16_t ceiling, fipsd_ipv6_paths *paths,
                         fipsd_ipv6_mesh_state *state, const uint8_t our[16], uint8_t *scratch, size_t cap,
                         fipsd_ipv6_from_tun *out);
/* A packet whose source is not our address is answered code 5. A destination
 * whose prefix names a key goes to the node whatever its size. Any other is
 * answered here: Packet Too Big over the node-wide IPv6 MTU (1280 for a packet
 * over 1280 when that MTU is under it), else Destination Unreachable. */
void fipsd_ipv6_outbound(const uint8_t *packet, size_t len, fipsd_ipv6_mesh_state *state, const uint8_t our[16],
                         uint8_t *scratch, size_t cap, fipsd_ipv6_host *out);
typedef enum fipsd_ipv6_owed_kind { FIPSD_IPV6_OWED_UNREACHABLE, FIPSD_IPV6_OWED_TOO_BIG } fipsd_ipv6_owed_kind;
typedef struct fipsd_ipv6_owed {
    fipsd_ipv6_owed_kind kind;
    const uint8_t *packet;  /* UNREACHABLE: the response; TOO_BIG: the rebuilt packet */
    size_t len;
    uint32_t mtu;           /* TOO_BIG: the MTU to report */
} fipsd_ipv6_owed;
typedef void (*fipsd_ipv6_owed_fn)(void *user, const fipsd_ipv6_owed *owed);
/* The answers the host is owed for a refusal, in the order the payloads came,
 * each rebuilt with our address as its source; scratch (>= 2 * 2048) holds
 * each in turn. Returns how many were owed. */
size_t fipsd_ipv6_answer(const fipsd_ipv6_refusal *refusal, const uint8_t our[16], uint8_t *scratch, size_t cap,
                         fipsd_ipv6_owed_fn fn, void *user);
/* Whether split would send payload as fragments that each fit payload_limit. */
bool fipsd_ipv6_can_split(const uint8_t *payload, size_t len, const fips_node_addr *peer, const uint8_t our[16],
                          uint16_t payload_limit);
/* Compressed fragments of at most payload_limit bytes each, new ones under ident. */
bool fipsd_ipv6_split(const uint8_t *payload, size_t len, const fips_node_addr *peer, const uint8_t our[16],
                      uint16_t payload_limit, uint32_t ident, fipsd_ipv6_fragments *out);
/* Clamp a held SYN again to flow_ceiling(path_limit, ceiling); on a change the
 * payload is replaced by the clamped packet compressed again (cap bytes
 * available) and the MSS written is returned, else -1 and nothing changes. */
int fipsd_ipv6_reclamp(uint8_t *payload, size_t *len, size_t cap, const fips_node_addr *peer, const uint8_t our[16],
                       uint16_t ceiling, bool has_path_limit, uint16_t path_limit);
#endif
