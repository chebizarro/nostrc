#include "ipv6/outbound.h"
#include "ipv6/icmp.h"
#include "ipv6/shim.h"
#include "ipv6/tcp_mss.h"
#include "log.h"
#include <string.h>
#define MOD "ipv6/outbound"
/* The largest packet a datagram the node carries rebuilds to (1400 + 33). */
#define REBUILT_MAX 2048u
fipsd_ipv6_admit fipsd_ipv6_admit_packet(const uint8_t *packet, size_t len, uint16_t ipv6_mtu)
{
    if (!packet || len < 40 || packet[0] >> 4 != 6) return (fipsd_ipv6_admit){.kind = FIPSD_IPV6_ADMIT_DROP};
    if (len > ipv6_mtu) return (fipsd_ipv6_admit){.kind = FIPSD_IPV6_ADMIT_TOO_BIG, .mtu = ipv6_mtu};
    return (fipsd_ipv6_admit){.kind = FIPSD_IPV6_ADMIT_FORWARD, .prefix = fipsd_ipv6_destination_prefix(packet)};
}
void fipsd_ipv6_outbound(const uint8_t *packet, size_t len, fipsd_ipv6_mesh_state *state, const uint8_t our[16],
                         uint8_t *scratch, size_t cap, fipsd_ipv6_host *out)
{
    *out = (fipsd_ipv6_host){.kind = FIPSD_IPV6_HOST_NOTHING};
    if (!packet || len < 40 || packet[0] >> 4 != 6) return;
    if (memcmp(packet + 8, our, 16)) {
        /* The receiver would rebuild it with our address as its source, so
         * its checksum would fail there. */
        FIPSD_DEBUG(MOD, "Refusing a packet from the host whose source is not our address");
        int n = fipsd_ipv6_source_refused(packet, len, our, scratch, cap);
        if (n > 0) { out->kind = FIPSD_IPV6_HOST_UNREACHABLE; out->response = scratch; out->response_len = (size_t)n; }
        return;
    }
    fips_node_prefix prefix = fipsd_ipv6_destination_prefix(packet);
    fips_node_addr peer;
    if (fipsd_ipv6_mesh_state_key(state, &prefix, &peer)) {
        int n = fipsd_ipv6_compress(packet, len, scratch, cap);
        if (n < 0) return;
        out->kind = FIPSD_IPV6_HOST_MESH;
        out->datagram = (fipsd_ipv6_datagram){.peer = peer, .payload = scratch, .len = (size_t)n};
        return;
    }
    fipsd_ipv6_admit admit = fipsd_ipv6_admit_packet(packet, len, fipsd_ipv6_mesh_state_ipv6_mtu(state));
    if (admit.kind == FIPSD_IPV6_ADMIT_TOO_BIG && packet[24] == FIPS_IPV6_ADDRESS_PREFIX) {
        out->kind = FIPSD_IPV6_HOST_TOO_BIG; out->mtu = fipsd_ipv6_packet_too_big_mtu(len, admit.mtu); return;
    }
    int n = fipsd_ipv6_unreachable(packet, len, our, scratch, cap);
    if (n > 0) { out->kind = FIPSD_IPV6_HOST_UNREACHABLE; out->response = scratch; out->response_len = (size_t)n; }
}
void fipsd_ipv6_from_tun_decide(uint8_t *packet, size_t len, uint16_t ceiling, fipsd_ipv6_paths *paths,
                         fipsd_ipv6_mesh_state *state, const uint8_t our[16], uint8_t *scratch, size_t cap,
                         fipsd_ipv6_from_tun *out)
{
    *out = (fipsd_ipv6_from_tun){.kind = FIPSD_IPV6_FROM_TUN_NOTHING};
    if (!packet || len < 40 || packet[0] >> 4 != 6) return;
    if (packet[24] != FIPS_IPV6_ADDRESS_PREFIX) {
        int n = fipsd_ipv6_unreachable(packet, len, our, scratch, cap);
        if (n > 0) { out->kind = FIPSD_IPV6_FROM_TUN_OFF_MESH; out->response = scratch; out->response_len = (size_t)n; }
        return;
    }
    /* A packet to our own mesh address is delivered locally, not pushed into
     * the mesh. In practice only macOS utun egresses self-addressed traffic
     * into the reader; the check is unconditional. The checksum macOS leaves
     * offloaded on self-traffic is finished first. */
    if (!memcmp(packet + 24, our, 16)) {
        fipsd_ipv6_recalculate_l4_checksum(packet, len);
        out->kind = FIPSD_IPV6_FROM_TUN_HAIRPIN; return;
    }
    out->kind = FIPSD_IPV6_FROM_TUN_HOST;
    out->clamped = fipsd_ipv6_clamp(packet, len, ceiling, paths, FIPSD_IPV6_REMOTE_DESTINATION, &out->clamped_mss);
    fipsd_ipv6_outbound(packet, len, state, our, scratch, cap, &out->host);
}
/* The IPv6 packet payload was compressed from, sent from our address to peer's. */
static int rebuild(const uint8_t *payload, size_t len, const fips_node_addr *peer, const uint8_t our[16], uint8_t *out, size_t cap)
{
    fips_ipv6_address dst = fipsd_ipv6_peer_address(peer);
    return fipsd_ipv6_decompress(payload, len, our, dst.bytes, out, cap);
}
size_t fipsd_ipv6_answer(const fipsd_ipv6_refusal *r, const uint8_t our[16], uint8_t *scratch, size_t cap,
                         fipsd_ipv6_owed_fn fn, void *user)
{
    if (cap < 2 * REBUILT_MAX) return 0;
    uint8_t *packet = scratch, *response = scratch + REBUILT_MAX;
    size_t owed = 0;
    for (size_t i = 0; i < r->count; ++i) {
        int n = rebuild(r->payloads[i], r->lens[i], &r->peer, our, packet, REBUILT_MAX);
        if (n < 0) continue;
        fipsd_ipv6_owed o;
        if (r->kind == FIPSD_IPV6_REFUSAL_TOO_BIG) {
            size_t mtu = fipsd_ipv6_mtu_of_limit(r->payload_limit);
            o = (fipsd_ipv6_owed){.kind = FIPSD_IPV6_OWED_TOO_BIG, .packet = packet, .len = (size_t)n,
                .mtu = fipsd_ipv6_packet_too_big_mtu((size_t)n, mtu)};
        } else {
            int m = fipsd_ipv6_unreachable(packet, (size_t)n, our, response, REBUILT_MAX);
            if (m < 0) continue;
            o = (fipsd_ipv6_owed){.kind = FIPSD_IPV6_OWED_UNREACHABLE, .packet = response, .len = (size_t)m};
        }
        ++owed;
        if (fn) fn(user, &o);
        if (r->kind != FIPSD_IPV6_REFUSAL_NO_ROUTE) break; /* one payload */
    }
    return owed;
}
bool fipsd_ipv6_can_split(const uint8_t *payload, size_t len, const fips_node_addr *peer, const uint8_t our[16],
                          uint16_t payload_limit)
{
    uint8_t packet[REBUILT_MAX];
    int n = rebuild(payload, len, peer, our, packet, sizeof packet);
    return n > 0 && fipsd_ipv6_can_fragment(packet, (size_t)n, fipsd_ipv6_mtu_of_limit(payload_limit));
}
/* Compress a fragment in place: the header becomes the format byte and the residual. */
static size_t compress_in_place(uint8_t *f, size_t len)
{
    uint8_t head[8]; memcpy(head, f, 8);
    memmove(f + 1 + FIPSD_IPV6_SHIM_RESIDUAL_SIZE, f + FIPSD_IPV6_HEADER_SIZE, len - FIPSD_IPV6_HEADER_SIZE);
    f[0] = FIPSD_IPV6_SHIM_FORMAT_COMPRESSED; memcpy(f + 1, head, 4); f[5] = head[6]; f[6] = head[7];
    return len - FIPSD_IPV6_HEADER_SAVING;
}
bool fipsd_ipv6_split(const uint8_t *payload, size_t len, const fips_node_addr *peer, const uint8_t our[16],
                      uint16_t payload_limit, uint32_t ident, fipsd_ipv6_fragments *out)
{
    uint8_t packet[REBUILT_MAX];
    int n = rebuild(payload, len, peer, our, packet, sizeof packet);
    if (n < 0 || !fipsd_ipv6_fragment_for_path(packet, (size_t)n, fipsd_ipv6_mtu_of_limit(payload_limit), ident, out)) return false;
    /* Each fragment compresses by the same 33 bytes conversion adds, so it fits the limit. */
    for (size_t i = 0; i < out->count; ++i) out->len[i] = compress_in_place(out->bytes[i], out->len[i]);
    return true;
}
int fipsd_ipv6_reclamp(uint8_t *payload, size_t *len, size_t cap, const fips_node_addr *peer, const uint8_t our[16],
                       uint16_t ceiling, bool has_path_limit, uint16_t path_limit)
{
    uint8_t packet[REBUILT_MAX];
    int n = rebuild(payload, *len, peer, our, packet, sizeof packet);
    if (n < 0) return -1;
    uint16_t mss = fipsd_ipv6_flow_ceiling(has_path_limit, path_limit, ceiling);
    if (!fipsd_ipv6_clamp_tcp_mss(packet, (size_t)n, mss)) return -1;
    int m = fipsd_ipv6_compress(packet, (size_t)n, payload, cap);
    if (m < 0) return -1;
    *len = (size_t)m;
    return mss;
}
