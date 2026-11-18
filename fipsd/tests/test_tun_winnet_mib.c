/* The rows of tun/winnet.h as the MIB_* rows IP Helper takes (Windows), apart
 * from test_tun_winnet.c because <iphlpapi.h> brings ole2.h's macros. */
#if defined(_WIN32)
#include "tun/winnet_rows.h"
#include <assert.h>
#include <string.h>
int fipsd_test_winnet_mib(void);
int fipsd_test_winnet_mib(void)
{
    uint8_t node[16] = {0xfd, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 1, 2, 3, 4, 5, 6, 7};
    fipsd_winnet_address a;
    assert(fipsd_winnet_node_address(0x0600000001000000ull, node, &a));
    MIB_UNICASTIPADDRESS_ROW ar;
    memset(&ar, 0xa5, sizeof ar);
    fipsd_winnet_address_mib(&a, &ar);
    assert(ar.Address.si_family == AF_INET6 && ar.Address.Ipv6.sin6_family == AF_INET6);
    assert(!memcmp(&ar.Address.Ipv6.sin6_addr, node, 16) && ar.Address.Ipv6.sin6_port == 0 && ar.Address.Ipv6.sin6_scope_id == 0);
    assert(ar.InterfaceLuid.Value == 0x0600000001000000ull && ar.InterfaceIndex == 0);
    assert(ar.OnLinkPrefixLength == 128 && ar.PrefixOrigin == IpPrefixOriginManual && ar.SuffixOrigin == IpSuffixOriginManual);
    assert(ar.ValidLifetime == 0xffffffffu && ar.PreferredLifetime == 0xffffffffu && !ar.SkipAsSource);
    fipsd_winnet_route r;
    assert(fipsd_winnet_mesh_route(0x0600000001000000ull, &r));
    MIB_IPFORWARD_ROW2 rr;
    memset(&rr, 0xa5, sizeof rr);
    fipsd_winnet_route_mib(&r, &rr);
    static const uint8_t prefix[16] = {0xfd}, any[16] = {0};
    assert(rr.InterfaceLuid.Value == 0x0600000001000000ull && rr.InterfaceIndex == 0);
    assert(rr.DestinationPrefix.Prefix.si_family == AF_INET6 && rr.DestinationPrefix.PrefixLength == 8);
    assert(!memcmp(&rr.DestinationPrefix.Prefix.Ipv6.sin6_addr, prefix, 16));
    assert(rr.NextHop.si_family == AF_INET6 && !memcmp(&rr.NextHop.Ipv6.sin6_addr, any, 16));
    assert(rr.Metric == 256 && rr.Protocol == MIB_IPPROTO_NETMGMT);
    assert(rr.ValidLifetime == 0xffffffffu && rr.PreferredLifetime == 0xffffffffu);
    return 0;
}
#endif
