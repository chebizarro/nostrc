/* IP Helper calls for the Wintun adapter (upstream ipv6tun/winnet.rs): the
 * node's /128, the interface's IPv6 MTU and the fd00::/8 on-link route, all
 * keyed on the adapter's LUID. Each call returns its Win32 code so callers
 * judge benign answers by code (tun/winnet.h), never by netsh's localised
 * text. The only translation unit that includes IP Helper for the TUN. */
#if defined(_WIN32)
#include "tun/winnet_rows.h"
#include <string.h>

_Static_assert(sizeof(wchar_t) == sizeof(uint16_t), "adapter names are UTF-16");
_Static_assert(FIPSD_WIN_ERROR_FILE_NOT_FOUND == ERROR_FILE_NOT_FOUND && FIPSD_WIN_ERROR_PATH_NOT_FOUND == ERROR_PATH_NOT_FOUND &&
    FIPSD_WIN_ERROR_ACCESS_DENIED == ERROR_ACCESS_DENIED && FIPSD_WIN_ERROR_INVALID_HANDLE == ERROR_INVALID_HANDLE &&
    FIPSD_WIN_ERROR_NOT_ENOUGH_MEMORY == ERROR_NOT_ENOUGH_MEMORY && FIPSD_WIN_ERROR_INVALID_DATA == ERROR_INVALID_DATA &&
    FIPSD_WIN_ERROR_OUTOFMEMORY == ERROR_OUTOFMEMORY && FIPSD_WIN_ERROR_GEN_FAILURE == ERROR_GEN_FAILURE &&
    FIPSD_WIN_ERROR_HANDLE_EOF == ERROR_HANDLE_EOF && FIPSD_WIN_ERROR_NOT_SUPPORTED == ERROR_NOT_SUPPORTED &&
    FIPSD_WIN_ERROR_DUP_NAME == ERROR_DUP_NAME && FIPSD_WIN_ERROR_DEV_NOT_EXIST == ERROR_DEV_NOT_EXIST &&
    FIPSD_WIN_ERROR_INVALID_PARAMETER == ERROR_INVALID_PARAMETER && FIPSD_WIN_ERROR_BUFFER_OVERFLOW == ERROR_BUFFER_OVERFLOW &&
    FIPSD_WIN_ERROR_MOD_NOT_FOUND == ERROR_MOD_NOT_FOUND && FIPSD_WIN_ERROR_PROC_NOT_FOUND == ERROR_PROC_NOT_FOUND &&
    FIPSD_WIN_ERROR_BUSY == ERROR_BUSY && FIPSD_WIN_ERROR_ALREADY_EXISTS == ERROR_ALREADY_EXISTS &&
    FIPSD_WIN_WAIT_TIMEOUT == WAIT_TIMEOUT && FIPSD_WIN_ERROR_NO_MORE_ITEMS == ERROR_NO_MORE_ITEMS &&
    FIPSD_WIN_ERROR_ELEVATION_REQUIRED == ERROR_ELEVATION_REQUIRED && FIPSD_WIN_ERROR_NOT_FOUND == ERROR_NOT_FOUND &&
    FIPSD_WIN_ERROR_TIMEOUT == ERROR_TIMEOUT && FIPSD_WIN_ERROR_DEVICE_NOT_AVAILABLE == ERROR_DEVICE_NOT_AVAILABLE &&
    FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS == ERROR_OBJECT_ALREADY_EXISTS, "tun/winnet.h Win32 codes match winerror.h");
_Static_assert(FIPSD_WINNET_PROTO_NETMGMT == MIB_IPPROTO_NETMGMT, "MIB_IPPROTO_NETMGMT");
_Static_assert(FIPSD_WINNET_ORIGIN_MANUAL == IpPrefixOriginManual && FIPSD_WINNET_ORIGIN_MANUAL == IpSuffixOriginManual,
    "manual prefix/suffix origin");

static SOCKADDR_INET sockaddr6(const uint8_t address[16])
{
    SOCKADDR_INET s;
    memset(&s, 0, sizeof s);
    s.Ipv6.sin6_family = AF_INET6;
    memcpy(&s.Ipv6.sin6_addr, address, 16);
    return s;
}
void fipsd_winnet_address_mib(const fipsd_winnet_address *a, MIB_UNICASTIPADDRESS_ROW *row)
{
    InitializeUnicastIpAddressEntry(row); /* infinite lifetimes and the other defaults */
    row->Address = sockaddr6(a->address);
    row->InterfaceLuid.Value = a->luid;
    row->OnLinkPrefixLength = a->prefix_len;
    row->PrefixOrigin = (NL_PREFIX_ORIGIN)a->prefix_origin;
    row->SuffixOrigin = (NL_SUFFIX_ORIGIN)a->suffix_origin;
}
void fipsd_winnet_route_mib(const fipsd_winnet_route *r, MIB_IPFORWARD_ROW2 *row)
{
    InitializeIpForwardEntry(row); /* infinite lifetimes and the other defaults */
    row->InterfaceLuid.Value = r->luid;
    row->DestinationPrefix.Prefix = sockaddr6(r->prefix);
    row->DestinationPrefix.PrefixLength = r->prefix_len;
    row->NextHop = sockaddr6(r->next_hop); /* the unspecified address: on-link */
    row->Metric = r->metric;
    row->Protocol = (NL_ROUTE_PROTOCOL)r->protocol;
}
uint32_t fipsd_winnet_alias_luid(const uint16_t *alias, uint64_t *luid)
{
    NET_LUID out = {0};
    DWORD code = ConvertInterfaceAliasToLuid((const wchar_t *)alias, &out);
    if (!code) *luid = out.Value;
    return code;
}
/* Defined below with the IPv6 row helper. */
static DWORD ip_row(uint64_t luid, ADDRESS_FAMILY family, MIB_IPINTERFACE_ROW *row);
uint32_t fipsd_winnet_alias_in_use(const uint16_t *alias, bool *in_use)
{
    *in_use = false;
    NET_LUID luid = {0};
    if (ConvertInterfaceAliasToLuid((const wchar_t *)alias, &luid)) return NO_ERROR; /* no such alias */
    MIB_IPINTERFACE_ROW ip;
    if (!ip_row(luid.Value, AF_INET6, &ip) || !ip_row(luid.Value, AF_INET, &ip)) { *in_use = true; return NO_ERROR; }
    MIB_IF_ROW2 row;
    memset(&row, 0, sizeof row);
    row.InterfaceLuid = luid;
    DWORD code = GetIfEntry2(&row);
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_NOT_FOUND) return NO_ERROR; /* gone already */
    if (!code) *in_use = row.OperStatus == IfOperStatusUp;
    return code;
}
uint32_t fipsd_winnet_luid_index(uint64_t luid, uint32_t *index)
{
    NET_LUID in = {.Value = luid};
    NET_IFINDEX out = 0;
    DWORD code = ConvertInterfaceLuidToIndex(&in, &out);
    if (!code) *index = out;
    return code;
}
/* ERROR_NOT_FOUND and ERROR_FILE_NOT_FOUND: no such IP interface (yet). */
static DWORD ip_row(uint64_t luid, ADDRESS_FAMILY family, MIB_IPINTERFACE_ROW *row)
{
    InitializeIpInterfaceEntry(row);
    row->Family = family;
    row->InterfaceLuid.Value = luid;
    return GetIpInterfaceEntry(row);
}
static DWORD ipv6_row(uint64_t luid, MIB_IPINTERFACE_ROW *row) { return ip_row(luid, AF_INET6, row); }
typedef struct ipv6_wait { uint64_t luid; HANDLE event; } ipv6_wait;
static VOID WINAPI interface_changed(PVOID context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE type)
{
    (void)type;
    ipv6_wait *w = context;
    if (!row || row->InterfaceLuid.Value == w->luid) (void)SetEvent(w->event);
}
static uint32_t wait_ipv6(uint64_t luid, uint32_t timeout_ms)
{
    ipv6_wait w = {.luid = luid, .event = CreateEventW(NULL, FALSE, FALSE, NULL)};
    if (!w.event) return GetLastError();
    /* Registered before the first look, so an arrival between the look and
     * the wait still signals the event. */
    HANDLE note = NULL;
    DWORD code = NotifyIpInterfaceChange(AF_INET6, interface_changed, &w, FALSE, &note);
    if (code) { CloseHandle(w.event); return code; }
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    MIB_IPINTERFACE_ROW row;
    for (;;) {
        code = ipv6_row(luid, &row);
        if (code != ERROR_NOT_FOUND && code != ERROR_FILE_NOT_FOUND) break;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { code = ERROR_TIMEOUT; break; }
        if (WaitForSingleObject(w.event, (DWORD)(deadline - now)) == WAIT_FAILED) { code = GetLastError(); break; }
    }
    (void)CancelMibChangeNotify2(note); /* waits out a callback in progress */
    CloseHandle(w.event);
    return code;
}
uint32_t fipsd_winnet_ipv6_interface(uint64_t luid, uint32_t timeout_ms)
{
    DWORD code = wait_ipv6(luid, timeout_ms);
    if (code) return code;
    MIB_IPINTERFACE_ROW row;
    if ((code = ipv6_row(luid, &row))) return code;
    row.DadTransmits = FIPSD_WINNET_DAD_TRANSMITS;
    row.RouterDiscoveryBehavior = RouterDiscoveryDisabled;
    return SetIpInterfaceEntry(&row);
}
uint32_t fipsd_winnet_set_address(const fipsd_winnet_address *a, bool add)
{
    MIB_UNICASTIPADDRESS_ROW row;
    fipsd_winnet_address_mib(a, &row);
    return add ? CreateUnicastIpAddressEntry(&row) : DeleteUnicastIpAddressEntry(&row);
}
uint32_t fipsd_winnet_set_route(const fipsd_winnet_route *r, bool add)
{
    MIB_IPFORWARD_ROW2 row;
    fipsd_winnet_route_mib(r, &row);
    return add ? CreateIpForwardEntry2(&row) : DeleteIpForwardEntry2(&row);
}
uint32_t fipsd_winnet_set_mtu(uint64_t luid, uint32_t mtu)
{
    MIB_IPINTERFACE_ROW row;
    DWORD code = ipv6_row(luid, &row);
    if (code) return code;
    row.NlMtu = mtu; /* as `netsh interface ipv6 set subinterface <if> mtu=<mtu>` */
    return SetIpInterfaceEntry(&row);
}
uint32_t fipsd_winnet_get_mtu(uint64_t luid, uint32_t *mtu)
{
    MIB_IPINTERFACE_ROW row;
    DWORD code = ipv6_row(luid, &row);
    if (!code) *mtu = row.NlMtu;
    return code;
}
uint32_t fipsd_winnet_has_address(const fipsd_winnet_address *a, bool *present)
{
    MIB_UNICASTIPADDRESS_ROW row;
    fipsd_winnet_address_mib(a, &row);
    DWORD code = GetUnicastIpAddressEntry(&row);
    *present = code == NO_ERROR;
    return code == ERROR_NOT_FOUND || code == ERROR_FILE_NOT_FOUND ? NO_ERROR : code;
}
uint32_t fipsd_winnet_has_route(const fipsd_winnet_route *r, bool *present)
{
    MIB_IPFORWARD_ROW2 row;
    fipsd_winnet_route_mib(r, &row);
    DWORD code = GetIpForwardEntry2(&row);
    *present = code == NO_ERROR;
    return code == ERROR_NOT_FOUND || code == ERROR_FILE_NOT_FOUND ? NO_ERROR : code;
}
#endif
