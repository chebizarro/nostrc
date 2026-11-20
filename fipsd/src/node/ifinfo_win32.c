/* See ifinfo_win32.h: the only translation unit that includes IP Helper. */
#if defined(_WIN32)
#include "node/netmon.h" /* before <iphlpapi.h> and its macros */
#include "node/ifinfo_win32.h"
#include <stdlib.h>
#include <string.h>
#include <winsock2.h> /* IP Helper's address types need Winsock 2 first */
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

void fipsd_win_ifrow_read(const char *name, fipsd_win_ifrow *out)
{
    memset(out, 0, sizeof *out);
    NET_IFINDEX index = name ? if_nametoindex(name) : 0;
    if (!index) return;
    MIB_IF_ROW2 row;
    memset(&row, 0, sizeof row);
    row.InterfaceIndex = index;
    if (GetIfEntry2(&row) != NO_ERROR) return;
    out->exists = true;
    out->index = index;
    out->type = row.Type;
    out->admin_up = row.AdminStatus == NET_IF_ADMIN_STATUS_UP;
    out->connected = row.MediaConnectState == MediaConnectStateConnected;
    out->hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
    if (row.PhysicalAddressLength == 6) { memcpy(out->mac, row.PhysicalAddress, 6); out->has_mac = true; }
}

static const fipsd_interface *find_interface(const fipsd_network_snapshot *s, uint32_t index)
{
    for (size_t i = 0; i < s->interface_count; ++i)
        if (s->interfaces[i].index == index) return &s->interfaces[i];
    return NULL;
}

bool fipsd_win_network_read(fipsd_network_snapshot *s)
{
    ULONG size = 16384, rc = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *all = NULL;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        free(all);
        if (!(all = malloc(size))) return false;
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, all, &size);
    }
    bool ok = rc == NO_ERROR;
    for (const IP_ADAPTER_ADDRESSES *a = ok ? all : NULL; a; a = a->Next) {
        uint32_t index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
        char name[NDIS_IF_MAX_STRING_SIZE + 1];
        if (!index || !if_indextoname(index, name)) continue;
        if (!find_interface(s, index)) {
            if (s->interface_count == FIPSD_NETMON_MAX_INTERFACES ||
                strlen(name) >= FIPSD_INTERFACE_NAME_SIZE) { ok = false; break; }
            fipsd_interface *iface = &s->interfaces[s->interface_count++];
            strcpy(iface->name, name);
            iface->index = index;
            iface->up = a->OperStatus == IfOperStatusUp;
            iface->carrier = iface->up;
            iface->loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        }
        for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            const struct sockaddr *sa = u->Address.lpSockaddr;
            if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
            if (s->address_count == FIPSD_NETMON_MAX_ADDRESSES) { ok = false; break; }
            fipsd_interface_address *out = &s->addresses[s->address_count++];
            out->index = index;
            if (sa->sa_family == AF_INET) {
                out->ip.width = 4;
                memcpy(out->ip.bytes, &((const struct sockaddr_in *)sa)->sin_addr, 4);
            } else {
                out->ip.width = 16;
                memcpy(out->ip.bytes, &((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
            }
            out->prefix = u->OnLinkPrefixLength;
        }
        if (!ok) break;
    }
    free(all);
    return ok;
}
#endif /* _WIN32 */
