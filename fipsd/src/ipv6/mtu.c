#include "ipv6/mtu.h"
#include <fips/proto/framing.h>
/* Every node reports and clamps by this figure; a framing change that moves it
 * is a visible change and has to be made here on purpose. */
_Static_assert(FIPSD_IPV6_OVERHEAD == FIPS_OVERHEAD + FIPS_FRAMING_FSP_PORT_HEADER_SIZE - FIPSD_IPV6_HEADER_SAVING,
    "the IPv6 shim overhead is no longer 77 bytes");
_Static_assert(FIPSD_IPV6_OVERHEAD == FIPSD_TUN_IPV6_OVERHEAD, "device and shim disagree on the IPv6 overhead");
_Static_assert(FIPSD_IPV6_MIN_MTU == FIPSD_TUN_IPV6_MIN_MTU, "device and shim disagree on the IPv6 minimum MTU");
_Static_assert(FIPSD_IPV6_HEADER_SAVING == 33u, "the compressed header saves 33 bytes");
static uint16_t sub(uint16_t a, uint16_t b) { return a > b ? (uint16_t)(a - b) : 0; }
static uint16_t max_segment(uint16_t mtu) { return sub(sub(mtu, FIPSD_IPV6_HEADER_SIZE), FIPSD_IPV6_TCP_HEADER); }
uint16_t fipsd_ipv6_effective_mtu(uint16_t transport_mtu) { return sub(transport_mtu, FIPSD_IPV6_OVERHEAD); }
uint16_t fipsd_ipv6_mss_ceiling(uint16_t transport_mtu) { return max_segment(fipsd_ipv6_effective_mtu(transport_mtu)); }
uint16_t fipsd_ipv6_mtu_of_limit(uint16_t payload_limit)
{
    return payload_limit > UINT16_MAX - FIPSD_IPV6_HEADER_SAVING ? UINT16_MAX : (uint16_t)(payload_limit + FIPSD_IPV6_HEADER_SAVING);
}
uint16_t fipsd_ipv6_tcp_mss(uint16_t payload_limit) { return max_segment(fipsd_ipv6_mtu_of_limit(payload_limit)); }
