/* The Wintun adapter's host configuration (upstream ipv6tun/winnet.rs and the
 * windows_tun module of tun.rs) as plain data: the node's /128 address row,
 * the fd00::/8 on-link route row, how IP Helper's create/delete codes are
 * judged, the Win32 errors the Windows backend reports as errno, and the
 * UTF-16 adapter name. Pure and built on every host, so the computations are
 * tested everywhere; tun/winnet_win32.c turns the rows into MIB_* rows and
 * makes the calls (Windows only, declared at the end). */
#ifndef FIPSD_TUN_WINNET_H
#define FIPSD_TUN_WINNET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Win32 codes (winerror.h), so the vocabulary compiles everywhere;
 * tun/winnet_win32.c asserts each against the SDK. */
#define FIPSD_WIN_ERROR_FILE_NOT_FOUND 2u
#define FIPSD_WIN_ERROR_PATH_NOT_FOUND 3u
#define FIPSD_WIN_ERROR_ACCESS_DENIED 5u
#define FIPSD_WIN_ERROR_INVALID_HANDLE 6u
#define FIPSD_WIN_ERROR_NOT_ENOUGH_MEMORY 8u
#define FIPSD_WIN_ERROR_INVALID_DATA 13u
#define FIPSD_WIN_ERROR_OUTOFMEMORY 14u
#define FIPSD_WIN_ERROR_GEN_FAILURE 31u
#define FIPSD_WIN_ERROR_HANDLE_EOF 38u
#define FIPSD_WIN_ERROR_NOT_SUPPORTED 50u
#define FIPSD_WIN_ERROR_DUP_NAME 52u
#define FIPSD_WIN_ERROR_DEV_NOT_EXIST 55u
#define FIPSD_WIN_ERROR_INVALID_PARAMETER 87u
#define FIPSD_WIN_ERROR_BUFFER_OVERFLOW 111u
#define FIPSD_WIN_ERROR_MOD_NOT_FOUND 126u
#define FIPSD_WIN_ERROR_PROC_NOT_FOUND 127u
#define FIPSD_WIN_ERROR_BUSY 170u
#define FIPSD_WIN_ERROR_ALREADY_EXISTS 183u
#define FIPSD_WIN_WAIT_TIMEOUT 258u
#define FIPSD_WIN_ERROR_NO_MORE_ITEMS 259u
#define FIPSD_WIN_ERROR_ELEVATION_REQUIRED 740u
#define FIPSD_WIN_ERROR_NOT_FOUND 1168u
#define FIPSD_WIN_ERROR_TIMEOUT 1460u
#define FIPSD_WIN_ERROR_DEVICE_NOT_AVAILABLE 4319u
#define FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS 5010u

/* Wintun session ring, upstream WINTUN_RING_CAPACITY (2 MiB): a power of two
 * in wintun.h's [WINTUN_MIN_RING_CAPACITY, WINTUN_MAX_RING_CAPACITY]. */
#define FIPSD_WINTUN_RING_CAPACITY 0x200000u
#define FIPSD_WINTUN_MIN_RING_CAPACITY 0x20000u
#define FIPSD_WINTUN_MAX_RING_CAPACITY 0x4000000u
/* The adapter's tunnel type: Windows shows the adapter as "FIPS Tunnel". */
#define FIPSD_WINTUN_TUNNEL_TYPE "FIPS"
/* MAX_ADAPTER_NAME (128) less the terminating NUL, in UTF-16 units. */
#define FIPSD_WINTUN_NAME_MAX 127u
/* How long a new adapter may take to get its IPv6 interface (upstream IPV6_WAIT). */
#define FIPSD_WINNET_IPV6_WAIT_MS 5000u
/* The adapter's IPv6 interface sends no duplicate address detection probes,
 * so the node's /128 is preferred the moment it is added (nothing on a
 * point-to-point TUN could answer one), and no router solicitations into the
 * mesh. Not in winnet.rs; WireGuard for Windows configures Wintun the same.
 * Neighbor unreachability detection stays on: Windows refuses to turn it off
 * on a Wintun interface (ERROR_INVALID_PARAMETER), so it probes each mesh
 * address it talks to with a unicast Neighbor Solicitation now and then, which
 * crosses the mesh and is answered by the peer host. */
#define FIPSD_WINNET_DAD_TRANSMITS 0u
/* The metric `netsh interface ipv6 add route` uses when none is given. */
#define FIPSD_WINNET_ROUTE_METRIC 256u
/* MIB_IPPROTO_NETMGMT: a route a management program installed. */
#define FIPSD_WINNET_PROTO_NETMGMT 3u
/* IpPrefixOriginManual and IpSuffixOriginManual. */
#define FIPSD_WINNET_ORIGIN_MANUAL 1u

/* The node's fd00::/8 address on the adapter (CreateUnicastIpAddressEntry):
 * InitializeUnicastIpAddressEntry's defaults (infinite lifetimes) plus these. */
typedef struct fipsd_winnet_address {
    uint64_t luid;
    uint8_t address[16];
    uint8_t prefix_len;                  /* OnLinkPrefixLength: 128 */
    uint8_t prefix_origin, suffix_origin; /* manual */
} fipsd_winnet_address;
/* The mesh route through the adapter (CreateIpForwardEntry2): keyed on
 * prefix, next hop and interface, so one adapter's route never collides with
 * another adapter's route for the same prefix. */
typedef struct fipsd_winnet_route {
    uint64_t luid;
    uint8_t prefix[16], prefix_len;      /* fd00::/8 */
    uint8_t next_hop[16];                /* ::, on-link */
    uint32_t metric, protocol;
} fipsd_winnet_route;

/* The row for `address` (the identity's fd || NodeAddr[0..15]) on `luid`;
 * false for luid 0 or an address outside fd00::/8. */
bool fipsd_winnet_node_address(uint64_t luid, const uint8_t address[16], fipsd_winnet_address *out);
/* The fd00::/8 on-link route row through `luid`; false for luid 0. */
bool fipsd_winnet_mesh_route(uint64_t luid, fipsd_winnet_route *out);
/* Whether `address` falls in the route's destination prefix. */
bool fipsd_winnet_route_covers(const fipsd_winnet_route *route, const uint8_t address[16]);

/* What one create or delete call found, as winnet.rs judges it: a create
 * answered ERROR_OBJECT_ALREADY_EXISTS found the row present (Exists); a
 * delete answered ERROR_NOT_FOUND or ERROR_FILE_NOT_FOUND found nothing to
 * remove (the adapter may already be gone at stop). */
typedef enum fipsd_winnet_outcome {
    FIPSD_WINNET_DONE, FIPSD_WINNET_EXISTS, FIPSD_WINNET_ABSENT, FIPSD_WINNET_FAILED
} fipsd_winnet_outcome;
fipsd_winnet_outcome fipsd_winnet_outcome_of(uint32_t code, bool create);
/* errno for a Win32 code (0 for 0). Missing privilege is EPERM, as on Linux
 * without CAP_NET_ADMIN; a name already taken is EBUSY (the lifecycle's
 * bounded retry); an ending session ENODEV; a full ring ENOBUFS; a missing
 * wintun.dll ENOENT; unknown codes EIO. */
int fipsd_winnet_errno(uint32_t code);
/* Valid Wintun ring capacity (power of two within wintun.h's bounds). */
bool fipsd_winnet_ring_capacity_valid(uint32_t capacity);
/* UTF-8 `name` as NUL-terminated UTF-16 in `out` (cap units, NUL included);
 * returns the units before the NUL, or 0 for an empty, malformed, overlong
 * (over FIPSD_WINTUN_NAME_MAX units) or truncated name. */
size_t fipsd_winnet_wide(const char *name, uint16_t *out, size_t cap);

#if defined(_WIN32)
/* IP Helper calls (tun/winnet_win32.c). Each returns its Win32 code. */
uint32_t fipsd_winnet_alias_luid(const uint16_t *alias, uint64_t *luid);
/* Whether an interface in use carries `alias`: one with an IPv4 or IPv6
 * interface, or up. A closed Wintun adapter loses both IP interfaces before
 * WintunCloseAdapter returns, while Plug and Play removes its interface row
 * (and alias) about 100 ms later; that departing adapter is not in use. */
uint32_t fipsd_winnet_alias_in_use(const uint16_t *alias, bool *in_use);
uint32_t fipsd_winnet_luid_index(uint64_t luid, uint32_t *index);
/* Wait for `luid`'s IPv6 interface row (IP Helper's interface-change
 * notification, bounded by timeout_ms: ERROR_TIMEOUT), then turn duplicate
 * address detection and router discovery off on it (FIPSD_WINNET_DAD_TRANSMITS). */
uint32_t fipsd_winnet_ipv6_interface(uint64_t luid, uint32_t timeout_ms);
uint32_t fipsd_winnet_set_address(const fipsd_winnet_address *row, bool add);
uint32_t fipsd_winnet_set_route(const fipsd_winnet_route *row, bool add);
/* The interface's IPv6 MTU (NlMtu, GetIpInterfaceEntry/SetIpInterfaceEntry). */
uint32_t fipsd_winnet_set_mtu(uint64_t luid, uint32_t mtu);
uint32_t fipsd_winnet_get_mtu(uint64_t luid, uint32_t *mtu);
/* Read-backs (GetUnicastIpAddressEntry, GetIpForwardEntry2): 0 and *present. */
uint32_t fipsd_winnet_has_address(const fipsd_winnet_address *row, bool *present);
uint32_t fipsd_winnet_has_route(const fipsd_winnet_route *row, bool *present);
#endif
#endif
